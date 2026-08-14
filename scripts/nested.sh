# Boot a throwaway fenriz and find its control socket. Sourced by mem-stress.sh and
# wltest.sh; not executable on its own.
#
# Two rules that this file exists to keep in one place:
#
#   * The socket path is read out of THAT instance's own log. Globbing
#     $XDG_RUNTIME_DIR/fenriz-*.sock finds the user's live session and the test then drives
#     their real desktop.
#   * FENRIZ_LOG is always set to the run's own file. Otherwise the throwaway instance
#     rotates and clobbers ~/.local/state/fenriz/fenriz.log on startup.
#   * XDG_CONFIG_HOME is always set to a directory this function fills in. Otherwise the
#     instance reads the user's ~/.config/fenriz/fenriz.conf — and hot-reloads it mid-run —
#     so a test's result depends on whose machine it runs on. A user windowrule that floats
#     or unfocuses a window is enough to fail an unrelated scenario.
#
# fenriz_boot <log> <backend> <launcher...>
#   backend: headless (no host session needed) | nested (host wayland session)
#   launcher: the command to run, with the fenriz binary LAST (so `valgrind ... fenriz` works)
#   $FENRIZ_TEST_CONFIG: optional config file to run with; unset means the compiled-in
#     defaults, which is the baseline every scenario is written against.
# Sets: FENRIZ_SOCK, FENRIZ_DISPLAY, FENRIZ_PID, FENRIZ_LAUNCH_PID

fenriz_boot() {
    local log=$1 backend=$2; shift 2
    local launch=("$@")
    local bin=${launch[${#launch[@]}-1]}

    case $backend in
        headless)
            # No host compositor and no GPU: the pixman renderer draws into main memory, so
            # this works over ssh and in a container.
            export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=2
            unset WLR_WL_OUTPUTS
            ;;
        nested)
            [ -n "${WAYLAND_DISPLAY:-}" ] || { echo "backend=nested needs a host WAYLAND_DISPLAY"; return 1; }
            export WLR_BACKENDS=wayland WLR_WL_OUTPUTS=2
            unset WLR_RENDERER WLR_HEADLESS_OUTPUTS
            ;;
        *) echo "unknown backend: $backend"; return 1 ;;
    esac
    export FENRIZ_LOG=$log

    # An EMPTY config dir is not isolation: fenriz falls back to the installed
    # /usr/share/fenriz/fenriz.conf, which on a machine with fenriz installed is a second
    # source of truth. The file has to exist, even if it is empty — an empty one parses to the
    # struct defaults in config.hpp.
    local cfgdir=${log%.log}.config
    mkdir -p "$cfgdir/fenriz"
    if [ -n "${FENRIZ_TEST_CONFIG:-}" ]; then
        cp "$FENRIZ_TEST_CONFIG" "$cfgdir/fenriz/fenriz.conf"
    else
        : >"$cfgdir/fenriz/fenriz.conf"
    fi
    export XDG_CONFIG_HOME=$cfgdir

    "${launch[@]}" >"${log%.log}.stdout" 2>"${log%.log}.stderr" &
    FENRIZ_LAUNCH_PID=$!

    FENRIZ_SOCK=""
    local tries=${FENRIZ_BOOT_TRIES:-60}
    for _ in $(seq "$tries"); do
        [ -s "$log" ] && FENRIZ_SOCK=$(grep -o 'FENRIZ_SOCKET=[^ ]*' "$log" | tail -1 | cut -d= -f2)
        [ -n "$FENRIZ_SOCK" ] && [ -S "$FENRIZ_SOCK" ] && break
        kill -0 $FENRIZ_LAUNCH_PID 2>/dev/null || break
        sleep 0.5
    done
    if [ -z "$FENRIZ_SOCK" ] || [ ! -S "$FENRIZ_SOCK" ]; then
        echo "fenriz never came up; see ${log%.log}.stderr"
        kill $FENRIZ_LAUNCH_PID 2>/dev/null
        return 1
    fi
    FENRIZ_DISPLAY=$(grep -o 'WAYLAND_DISPLAY=[^ ]*' "$log" | tail -1 | cut -d= -f2)
    FENRIZ_PID=$(pgrep -f "^${bin}$" | head -1)
    [ -n "$FENRIZ_PID" ] || FENRIZ_PID=$FENRIZ_LAUNCH_PID
}

# Commands are never acked, so this only reports that the line went out.
fenriz_ipc() {
    printf '%s\n' "$1" | timeout 2 socat -T1 - "UNIX-CONNECT:$FENRIZ_SOCK" >/dev/null 2>&1
}

# Ask nicely, then insist.
fenriz_kill() {
    fenriz_ipc '{"cmd":"exit"}'
    for _ in $(seq 20); do
        kill -0 "$FENRIZ_LAUNCH_PID" 2>/dev/null || return 0
        sleep 0.25
    done
    kill -TERM "$FENRIZ_LAUNCH_PID" 2>/dev/null
    sleep 1
    kill -KILL "$FENRIZ_LAUNCH_PID" 2>/dev/null
    return 0
}

# Refuse to run alongside another throwaway instance: both land on the same control socket
# and silently drive each other's commands. Anchored so it can't match /usr/bin/fenriz.
fenriz_refuse_if_running() {
    if pgrep -f "^$1/build/[a-z]*/fenriz$" >/dev/null; then
        echo "a test fenriz is already running — stop it first (pkill -f '$1/build/.*/fenriz')"
        return 1
    fi
}
