#!/bin/bash
# Bring up a private, headless PipeWire stack that the audio backend's tests
# can drive without touching (or needing) a desktop session.
#
# Why a whole stack and not a mock: every claim this backend makes -- that a
# volume was written, that a link moved, that a default changed -- is only
# worth anything if a real pipewire daemon and a real wireplumber session
# manager agreed to it. The container has no session bus and no sound card, so
# the script makes both: its own dbus-daemon, its own pipewire with
# `support.null-audio-sink` nodes standing in for hardware, and pipewire-pulse
# so the libpulse fallback can be tested against the same graph.
#
# Layout it creates under $VS_AUDIO_STACK_DIR (default /tmp/vs-audio-stack):
#   run/          XDG_RUNTIME_DIR for the pipewire sockets
#   state/        private XDG_STATE_HOME, so WirePlumber's remembered stream
#                 volumes and routes cannot leak between runs
#   config/ data/ private XDG_CONFIG_HOME / XDG_DATA_HOME for the same reason
#   env           sourceable environment (the four XDG dirs and the bus address)
#   pids          one pid per line, newest first, for `stop`
#   *.log         daemon logs
#
# Nodes it creates:
#   vs-sink-a, vs-sink-b   two Audio/Sink null sinks (routing needs two)
#   vs-source-a            one Audio/Source null source (mute-inputs needs one)
#
# usage:
#   scripts/run-stack.sh start     bring the stack up, print the env
#   scripts/run-stack.sh stop      tear it down
#   scripts/run-stack.sh env       print the env of a running stack
#   scripts/run-stack.sh status    report whether the stack answers
#   scripts/run-stack.sh exec CMD...   run CMD inside the stack's environment
set -u

DIR=${VS_AUDIO_STACK_DIR:-/tmp/vs-audio-stack}
RUNDIR="$DIR/run"
ENVFILE="$DIR/env"
PIDFILE="$DIR/pids"

die() { echo "run-stack: $*" >&2; exit 1; }

# The daemons are started with the config PipeWire ships; the extra nodes are
# added afterwards with pw-cli so the script does not have to carry (and keep
# in sync with) a private copy of pipewire.conf.
make_null_node() {
    local name=$1 desc=$2 class=$3
    pw-cli create-node adapter \
        "{ factory.name=support.null-audio-sink
           node.name=$name
           node.description=\"$desc\"
           media.class=$class
           object.linger=true
           audio.position=[FL FR] }" >/dev/null 2>&1
}

wait_for() {   # wait_for <seconds> <command...>
    local limit=$1; shift
    local i=0
    while [ "$i" -lt "$((limit * 10))" ]; do
        if "$@" >/dev/null 2>&1; then return 0; fi
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

start() {
    stop >/dev/null 2>&1
    # A stale state directory is exactly the non-hermeticity the private
    # XDG_STATE_HOME exists to prevent, so start() clears it rather than
    # reusing whatever the last run left.
    rm -rf "$DIR/state" "$DIR/config" "$DIR/data" "$RUNDIR"
    mkdir -p "$RUNDIR" || die "cannot create $RUNDIR"
    chmod 700 "$RUNDIR"
    : > "$PIDFILE"

    export XDG_RUNTIME_DIR="$RUNDIR"
    # WirePlumber's stream-restore module remembers a stream's volume and its
    # target sink under $XDG_STATE_HOME, keyed by application name, and applies
    # it to the next stream of the same name. Without a private state directory
    # the stack is not hermetic: a test that sets a volume to 150 % hands that
    # 150 % to the *next* run's fresh stream, which was observed while writing
    # this backend and would have made the volume test pass before it ran.
    mkdir -p "$DIR/state" "$DIR/config" "$DIR/data"
    export XDG_STATE_HOME="$DIR/state"
    export XDG_CONFIG_HOME="$DIR/config"
    export XDG_DATA_HOME="$DIR/data"

    # A private session bus: wireplumber wants one, and the host may have none.
    local busout addr buspid
    busout=$(dbus-daemon --session --fork --print-address=1 --print-pid=1) \
        || die "dbus-daemon failed"
    addr=$(printf '%s\n' "$busout" | sed -n 1p)
    buspid=$(printf '%s\n' "$busout" | sed -n 2p)
    export DBUS_SESSION_BUS_ADDRESS="$addr"
    echo "$buspid" >> "$PIDFILE"

    printf 'export XDG_RUNTIME_DIR=%s\nexport DBUS_SESSION_BUS_ADDRESS=%s\n' \
        "$RUNDIR" "$addr" > "$ENVFILE"
    printf 'export XDG_STATE_HOME=%s\nexport XDG_CONFIG_HOME=%s\nexport XDG_DATA_HOME=%s\n' \
        "$DIR/state" "$DIR/config" "$DIR/data" >> "$ENVFILE"

    pipewire > "$DIR/pipewire.log" 2>&1 &
    echo "$!" >> "$PIDFILE"
    wait_for 15 pw-cli info 0 || {
        sed 's/^/  /' "$DIR/pipewire.log" >&2
        die "pipewire did not come up"
    }

    wireplumber > "$DIR/wireplumber.log" 2>&1 &
    echo "$!" >> "$PIDFILE"

    pipewire-pulse > "$DIR/pipewire-pulse.log" 2>&1 &
    echo "$!" >> "$PIDFILE"
    # pipewire-pulse publishes its socket under XDG_RUNTIME_DIR/pulse/native;
    # waiting for the file is more honest than sleeping a guessed interval.
    wait_for 15 test -S "$RUNDIR/pulse/native" \
        || echo "run-stack: warning: pipewire-pulse socket never appeared" >&2

    make_null_node vs-sink-a "Vorssaint Test Sink A" Audio/Sink
    make_null_node vs-sink-b "Vorssaint Test Sink B" Audio/Sink
    make_null_node vs-source-a "Vorssaint Test Source A" Audio/Source

    wait_for 10 node_present vs-sink-a || die "test sinks never appeared"
    wait_for 10 node_present vs-sink-b || die "vs-sink-b never appeared"
    wait_for 10 node_present vs-source-a || die "vs-source-a never appeared"

    # WirePlumber picks a default on its own once the sinks exist, but which
    # one is its choice; the tests want a known starting point.
    wpctl set-default "$(node_id vs-sink-a)" >/dev/null 2>&1

    cat "$ENVFILE"
}

node_present() { pw-dump Node 2>/dev/null | grep -q "\"node.name\": \"$1\""; }

node_id() {
    pw-dump Node 2>/dev/null | python3 -c '
import json,sys
want=sys.argv[1]
for o in json.load(sys.stdin):
    if o.get("info",{}).get("props",{}).get("node.name")==want:
        print(o["id"]); break
' "$1"
}

stop() {
    [ -f "$PIDFILE" ] || return 0
    # Reverse order: pulse and wireplumber before the daemon they attach to.
    tac "$PIDFILE" | while read -r pid; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null
    done
    sleep 0.5
    tac "$PIDFILE" | while read -r pid; do
        [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null
    done
    rm -f "$PIDFILE" "$ENVFILE"
    return 0
}

case "${1:-start}" in
    start) start ;;
    stop) stop ;;
    env)
        [ -f "$ENVFILE" ] || die "no stack running (no $ENVFILE)"
        cat "$ENVFILE" ;;
    status)
        [ -f "$ENVFILE" ] || die "no stack running"
        # shellcheck disable=SC1090
        . "$ENVFILE"
        pw-cli info 0 >/dev/null 2>&1 \
            && echo "pipewire: up" || { echo "pipewire: down"; exit 1; }
        pw-dump Node | grep -c '"media.class"' | sed 's/^/nodes with a media.class: /'
        ;;
    exec)
        shift
        [ -f "$ENVFILE" ] || die "no stack running"
        # shellcheck disable=SC1090
        . "$ENVFILE"
        exec "$@" ;;
    *) die "usage: $0 {start|stop|env|status|exec CMD...}" ;;
esac
