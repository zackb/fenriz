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
# Anchored at both ends so it matches only the nested binary itself — not the user's live
# /usr/bin/fenriz, and not a shell whose command line happens to contain the path.
if pgrep -f "^${ROOT}/build/[a-z]*/fenriz$" >/dev/null; then
    echo "a nested fenriz is already running — stop it first (pkill -f '$ROOT/build/.*/fenriz')"; exit 1
fi

export WLR_BACKENDS=wayland WLR_WL_OUTPUTS=2 FENRIZ_LOG=$LOG
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
"${LAUNCH[@]}" >"$OUT/stdout.txt" 2>"$OUT/stderr.txt" &
LAUNCH_PID=$!

# The compositor's own log is the only trustworthy source for its socket path.
# Startup under valgrind is ~50x slower (GL/EGL init dominates), so wait much longer there.
SOCK=""
WAIT=60
[ "$MODE" = valgrind ] && WAIT=600
for _ in $(seq $WAIT); do
    [ -s "$LOG" ] && SOCK=$(grep -o 'FENRIZ_SOCKET=[^ ]*' "$LOG" | tail -1 | cut -d= -f2)
    [ -n "$SOCK" ] && [ -S "$SOCK" ] && break
    sleep 0.5
done
[ -n "$SOCK" ] || { echo "nested fenriz never came up; see $OUT/stderr.txt"; kill $LAUNCH_PID 2>/dev/null; exit 1; }
NESTED_DISPLAY=$(grep -o 'WAYLAND_DISPLAY=[^ ]*' "$LOG" | tail -1 | cut -d= -f2)
FENRIZ_PID=$(pgrep -f "^${BIN}$" | head -1)
[ -n "$FENRIZ_PID" ] || FENRIZ_PID=$LAUNCH_PID
echo "nested display=$NESTED_DISPLAY sock=$SOCK pid=$FENRIZ_PID"

ipc() { printf '%s\n' "$1" | timeout 2 socat -T1 - "UNIX-CONNECT:$SOCK" >/dev/null 2>&1; }
rss() { awk '/^VmRSS:/{print $2}' "/proc/$FENRIZ_PID/status" 2>/dev/null || echo 0; }

# A couple of real clients so map/unmap, popups and the tiling tree are actually exercised.
spawn_clients() {
    for _ in 1 2; do
        WAYLAND_DISPLAY=$NESTED_DISPLAY foot >/dev/null 2>&1 &
    done 2>/dev/null
    command -v foot >/dev/null || WAYLAND_DISPLAY=$NESTED_DISPLAY weston-terminal >/dev/null 2>&1 &
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
        grep -E "ERROR: (Address|Leak)Sanitizer|runtime error|SUMMARY" "$OUT/stderr.txt" || echo "clean"
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
