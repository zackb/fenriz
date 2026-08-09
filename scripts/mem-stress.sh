#!/bin/bash
# Drive a NESTED fenriz through the state transitions that allocate and free, then report
# whether RSS settled. This is the harness the memory audit's findings are measured against:
# reading the code tells you a buffer is retained, this tells you by how much.
#
#   ./scripts/mem-stress.sh [cycles]            # plain debug build, RSS delta
#   ./scripts/mem-stress.sh 20 sanitize         # ASan/LSan/UBSan build
#   ./scripts/mem-stress.sh 5 valgrind          # memcheck (slow; use few cycles)
#   ./scripts/mem-stress.sh 20 heaptrack        # allocation profile
#
# Never run against the live session: it always starts its own nested compositor on the
# wayland backend, and always reads that instance's socket out of its OWN log rather than
# globbing $XDG_RUNTIME_DIR (which would find the real one).

set -u

CYCLES=${1:-10}
MODE=${2:-plain}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/scripts/nested.sh"
OUT=$(mktemp -d /tmp/fenriz-memstress.XXXXXX)
LOG=$OUT/fenriz.log

case $MODE in
    sanitize) BIN=$ROOT/build/sanitize/fenriz; PRESET=sanitize ;;
    *)        BIN=$ROOT/build/debug/fenriz;    PRESET=debug ;;
esac

[ -x "$BIN" ] || { echo "missing $BIN — run: cmake --preset $PRESET && cmake --build --preset $PRESET"; exit 1; }
[ -n "${WAYLAND_DISPLAY:-}" ] || { echo "no host WAYLAND_DISPLAY; this must run inside a Wayland session"; exit 1; }
command -v socat >/dev/null || { echo "socat required"; exit 1; }
# Two nested instances land on the same WAYLAND_DISPLAY, hence the same control socket, and
# silently drive each other's commands. Refuse to start rather than produce a bogus number.
fenriz_refuse_if_running "$ROOT" || exit 1

export ASAN_OPTIONS="detect_leaks=1:halt_on_error=0:abort_on_error=0:detect_stack_use_after_return=1"
export LSAN_OPTIONS="suppressions=$ROOT/scripts/lsan-suppressions.txt:print_suppressions=0"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0"

case $MODE in
    valgrind)  LAUNCH=(valgrind --leak-check=full --show-leak-kinds=definite,indirect
                       --track-origins=yes --error-limit=no --log-file="$OUT/valgrind.txt" "$BIN") ;;
    # --record-only or heaptrack pops its GUI open on the user's live session when the
    # trace finishes, and the wrapper then never exits. Analyze with heaptrack_print.
    heaptrack) LAUNCH=(heaptrack --record-only -o "$OUT/heaptrack" "$BIN") ;;
    *)         LAUNCH=("$BIN") ;;
esac

echo "mode=$MODE cycles=$CYCLES out=$OUT"
# Startup under valgrind is ~50x slower (GL/EGL init dominates), so wait much longer there.
[ "$MODE" = valgrind ] && export FENRIZ_BOOT_TRIES=600
fenriz_boot "$LOG" nested "${LAUNCH[@]}" || exit 1
SOCK=$FENRIZ_SOCK
NESTED_DISPLAY=$FENRIZ_DISPLAY
LAUNCH_PID=$FENRIZ_LAUNCH_PID
echo "nested display=$NESTED_DISPLAY sock=$SOCK pid=$FENRIZ_PID"

ipc() { fenriz_ipc "$1"; }
rss() { awk '/^VmRSS:/{print $2}' "/proc/$FENRIZ_PID/status" 2>/dev/null || echo 0; }

# Real clients, so map/unmap, popups and the tiling tree are actually exercised. Prefer our
# own test client: it's always built alongside fenriz, where foot may not be installed at
# all — in which case this loop used to silently produce a client-free run.
spawn_clients() {
    local ours=$ROOT/build/$PRESET/fenriz-test
    if [ -x "$ours" ]; then
        WAYLAND_DISPLAY=$NESTED_DISPLAY "$ours" popup --hold >/dev/null 2>&1 &
        WAYLAND_DISPLAY=$NESTED_DISPLAY "$ours" subsurface --hold >/dev/null 2>&1 &
    elif command -v foot >/dev/null; then
        WAYLAND_DISPLAY=$NESTED_DISPLAY foot >/dev/null 2>&1 &
        WAYLAND_DISPLAY=$NESTED_DISPLAY foot >/dev/null 2>&1 &
    elif command -v weston-terminal >/dev/null; then
        WAYLAND_DISPLAY=$NESTED_DISPLAY weston-terminal >/dev/null 2>&1 &
    else
        echo "warning: no test client available; map/unmap paths will not be exercised"
    fi
    sleep 1
}

spawn_clients
sleep 2
BASE=$(rss)
echo "baseline RSS: ${BASE} kB"

for i in $(seq "$CYCLES"); do
    ipc '{"cmd":"workspace","n":2}'
    ipc '{"cmd":"workspace","n":1}'
    # zoom in/out: the offscreen swapchain path (F1)
    ipc '{"cmd":"dispatch","action":"fullscreen"}'
    ipc '{"cmd":"dispatch","action":"fullscreen"}'
    # output churn: scene outputs, layer surfaces, workspace evacuation/restore
    ipc '{"cmd":"output","name":"WL-2","enabled":false}'
    ipc '{"cmd":"output","name":"WL-2","enabled":true}'
    # lid policy, same funnel from a different entry point
    ipc '{"cmd":"lid","closed":true}'
    ipc '{"cmd":"lid","closed":false}'
    # config reload: rebuilds every Bind/WindowRule/OutputCfg and re-places every view
    ipc '{"cmd":"reload"}'
    # float/tile transitions move views in and out of the BSP tree
    ipc '{"cmd":"dispatch","action":"togglefloating"}'
    ipc '{"cmd":"dispatch","action":"togglefloating"}'
    sleep 0.4
    printf 'cycle %2d/%s  RSS %s kB\n' "$i" "$CYCLES" "$(rss)"
done

sleep 2
END=$(rss)
echo "---"
echo "RSS: ${BASE} -> ${END} kB (delta $((END - BASE)) kB over $CYCLES cycles)"

ipc '{"cmd":"exit"}'
wait $LAUNCH_PID 2>/dev/null

case $MODE in
    sanitize)
        echo "--- sanitizer report ---"
        grep -E "ERROR: (Address|Leak)Sanitizer|runtime error|SUMMARY" "$OUT/fenriz.stderr" || echo "clean"
        ;;
    valgrind)  echo "--- valgrind ---"; grep -E "definitely lost|indirectly lost|ERROR SUMMARY" "$OUT/valgrind.txt" ;;
    heaptrack)
        echo "--- heaptrack: leaked at exit (top 10 by size) ---"
        heaptrack_print --print-leaks 1 --print-flamegraph "" "$OUT"/heaptrack.zst 2>/dev/null \
            | sed -n '/LEAKED ALLOCATIONS/,/^$/p' | head -60
        echo "(full profile: heaptrack_gui $OUT/heaptrack.zst)"
        ;;
esac
echo "artifacts in $OUT"
