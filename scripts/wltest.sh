#!/bin/bash
# Run the fenriz-test clients against a throwaway compositor, one fresh instance per
# scenario so a crash in one can't poison the next.
#
#   ./scripts/wltest.sh                          # every scenario, headless
#   ./scripts/wltest.sh popup evil               # named ones
#   ./scripts/wltest.sh --backend nested popup   # visible, for watching
#   ./scripts/wltest.sh --build-dir build/sanitize
#
# The client is the oracle: exit 0 pass, 1 protocol error or lost compositor, 2 watchdog.
# Nothing here inspects pixels or compositor state.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/scripts/nested.sh"

BACKEND=headless
BUILD=$ROOT/build/debug
TIMEOUT=30
LOOP=1
VERBOSE=""
SCENARIOS=()

while [ $# -gt 0 ]; do
    case $1 in
        --backend)   BACKEND=$2; shift 2 ;;
        --build-dir) BUILD=$2; shift 2 ;;
        --timeout)   TIMEOUT=$2; shift 2 ;;
        --loop)      LOOP=$2; shift 2 ;;
        --verbose|-v) VERBOSE="--verbose"; shift ;;
        -h|--help)   sed -n '2,11p' "$0"; exit 0 ;;
        -*)          echo "unknown option: $1"; exit 1 ;;
        *)           SCENARIOS+=("$1"); shift ;;
    esac
done

case $BUILD in /*) ;; *) BUILD=$ROOT/$BUILD ;; esac
FENRIZ=$BUILD/fenriz
CLIENT=$BUILD/fenriz-test

for b in "$FENRIZ" "$CLIENT"; do
    [ -x "$b" ] || { echo "missing $b — run: make debug"; exit 1; }
done
command -v socat >/dev/null || { echo "socat required"; exit 1; }
fenriz_refuse_if_running "$ROOT" || exit 1

[ ${#SCENARIOS[@]} -gt 0 ] || mapfile -t SCENARIOS < <("$CLIENT" --list)

OUT=$(mktemp -d /tmp/fenriz-wltest.XXXXXX)
echo "backend=$BACKEND build=$BUILD out=$OUT"
echo

# Sanitizer settings are harmless on a plain build and are what makes --build-dir
# build/sanitize useful without any extra flags here.
export ASAN_OPTIONS="detect_leaks=1:halt_on_error=0:abort_on_error=0:detect_stack_use_after_return=1"
export LSAN_OPTIONS="suppressions=$ROOT/scripts/lsan-suppressions.txt:print_suppressions=0"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0"

failed=0
results=()

for s in "${SCENARIOS[@]}"; do
    log=$OUT/$s.log
    printf '%-16s ' "$s"

    # A scenario that needs compositor config (a windowrule to match against) ships it next to
    # the scenario code; everything else runs on the compiled-in defaults.
    conf=$ROOT/tests/config/$s.conf
    [ -f "$conf" ] && export FENRIZ_TEST_CONFIG=$conf || unset FENRIZ_TEST_CONFIG

    if ! fenriz_boot "$log" "$BACKEND" "$FENRIZ" >"$OUT/$s.boot" 2>&1; then
        results+=("$s BOOT-FAILED")
        echo "BOOT FAILED (see $OUT/$s.boot)"
        failed=1
        continue
    fi

    WAYLAND_DISPLAY=$FENRIZ_DISPLAY FENRIZ_SOCKET=$FENRIZ_SOCK FENRIZ_EVENT_SOCKET=$FENRIZ_EVENT_SOCK \
        "$CLIENT" "$s" --timeout "$TIMEOUT" --loop "$LOOP" $VERBOSE \
        >"$OUT/$s.client" 2>&1
    rc=$?

    # A dead compositor is the interesting half of a client failure; note it either way.
    alive=yes
    kill -0 "$FENRIZ_PID" 2>/dev/null || alive=no
    fenriz_kill

    case $rc in
        0) if [ $alive = yes ]; then results+=("$s ok"); echo "ok"
           else results+=("$s COMPOSITOR-DIED"); echo "FAIL (client ok, compositor died)"; failed=1; fi ;;
        2) results+=("$s TIMEOUT"); echo "TIMEOUT"; failed=1 ;;
        *) results+=("$s FAIL($rc)"); echo "FAIL (exit $rc)"; failed=1 ;;
    esac
    if [ $rc -ne 0 ] || [ $alive = no ]; then
        sed 's/^/    | /' "$OUT/$s.client" | tail -15
    fi
done

echo
printf '%s\n' "${results[@]}" | column -t
if [ $failed -eq 0 ]; then
    echo "all passed"
    rm -rf "$OUT"
else
    echo "artifacts in $OUT"
fi
exit $failed
