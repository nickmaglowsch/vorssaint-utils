#!/usr/bin/env bash
# WP-02 spike: bring up a headless wlroots + PipeWire + xdg-desktop-portal
# stack inside a container that has no login session, no seat and no display.
#
#   ./run-stack.sh start     bring the stack up, write $STATE/env.sh
#   ./run-stack.sh paint     put non-uniform content on the headless output
#   ./run-stack.sh status    show what is running
#   ./run-stack.sh stop      tear it all down
#
# After `start`, source "$STATE/env.sh" in any shell that wants to talk to the
# stack (it exports XDG_RUNTIME_DIR, DBUS_SESSION_BUS_ADDRESS, WAYLAND_DISPLAY,
# XDG_CURRENT_DESKTOP and friends).
set -u

STATE=${VS_CAPTURE_STATE:-/tmp/vorssaint-capture}
RUNTIME="$STATE/run"
LOGS="$STATE/logs"
CFG="$STATE/config"
OUTPUT_NAME=${VS_CAPTURE_OUTPUT:-HEADLESS-1}
OUTPUT_MODE=${VS_CAPTURE_MODE:-1280x720@60Hz}
SINK_NAME=${VS_CAPTURE_SINK:-vorssaint-null-sink}
SOURCE_NAME=${VS_CAPTURE_SOURCE:-vorssaint-virtual-mic}
HERE=$(cd "$(dirname "$0")" && pwd)

# Two things this container forces on us, both documented in docs/linux-port/CAPTURE_ENGINE.md:
#
# VS_CAPTURE_XDPW: which xdg-desktop-portal-wlr binary to run. Stock 0.7.1 refuses
#   to start a cast when the compositor advertises zwlr_screencopy v3 but the
#   renderer has no DMA-BUF (no /dev/dri). `build-patched-xdpw.sh` produces a
#   fixed build; set VS_CAPTURE_XDPW=/usr/libexec/xdg-desktop-portal-wlr to see the
#   stock failure instead.
#
# VS_CAPTURE_ACCESS_SHIM: xdg-desktop-portal will not export the Screenshot portal
#   (nor Camera/Device/Location) unless some backend implements
#   org.freedesktop.impl.portal.Access. xdpw implements none, so on a plain
#   wlroots session the Screenshot portal is simply absent. Set this to 0 to
#   reproduce that; 1 loads mock-screenshot-impl.py, which supplies Access
#   plus a grim-backed Screenshot impl.
PATCHED_XDPW=${VS_CAPTURE_XDPW_PREFIX:-/tmp/vorssaint-capture/xdpw-patched}/libexec/xdg-desktop-portal-wlr
XDPW_BIN=${VS_CAPTURE_XDPW:-$([ -x "$PATCHED_XDPW" ] && echo "$PATCHED_XDPW" || echo /usr/libexec/xdg-desktop-portal-wlr)}
ACCESS_SHIM=${VS_CAPTURE_ACCESS_SHIM:-1}

log() { printf '[capture-stack] %s\n' "$*" >&2; }
die() { log "FATAL: $*"; exit 1; }

# Wait until CMD succeeds, up to $1 tenths of a second.
wait_for() {
  local tries=$1; shift
  local i=0
  while [ "$i" -lt "$tries" ]; do
    if "$@" >/dev/null 2>&1; then return 0; fi
    sleep 0.1; i=$((i + 1))
  done
  return 1
}

write_configs() {
  mkdir -p "$CFG/pipewire/pipewire.conf.d" \
           "$CFG/xdg-desktop-portal-wlr" \
           "$CFG/sway"

  # A null sink, so there is a playback device and -- the point here -- a
  # *monitor* source to capture "system audio" from. object.linger keeps it
  # alive after the creating client goes away.
  cat > "$CFG/pipewire/pipewire.conf.d/10-vorssaint-capture-audio.conf" <<EOF
context.objects = [
  { factory = adapter
    args = {
      factory.name            = support.null-audio-sink
      node.name               = "$SINK_NAME"
      node.description        = "Vorssaint capture test sink"
      media.class             = Audio/Sink
      object.linger           = true
      audio.position          = [ FL FR ]
      monitor.channel-volumes = true
    }
  }
]

# A real Audio/Source, so the microphone path is tested against a source node
# rather than assumed. A loopback from the null sink's monitor makes it carry
# whatever the tone player plays, so one tone exercises both audio paths and a
# silent microphone capture is a failure rather than the expected result.
context.modules = [
  { name = libpipewire-module-loopback
    args = {
      node.description = "Vorssaint capture test microphone"
      capture.props = {
        node.name           = "${SOURCE_NAME}-capture"
        node.target         = "$SINK_NAME"
        stream.capture.sink = true
        node.passive        = true
      }
      playback.props = {
        node.name        = "$SOURCE_NAME"
        media.class      = Audio/Source
        node.description = "Vorssaint capture test microphone"
      }
    }
  }
]
EOF

  # xdg-desktop-portal-wlr normally pops a chooser (slurp/wofi//bin/sh -c ...)
  # to pick an output. Nobody can click it here, so pin the output and turn the
  # chooser off: `chooser_type=none` makes xdpw use `output_name` directly.
  cat > "$CFG/xdg-desktop-portal-wlr/config" <<EOF
[screencast]
output_name=$OUTPUT_NAME
max_fps=30
chooser_type=none
EOF

  # Minimal sway: one headless output, no bar, no keybinds, no input devices,
  # and a solid colour background so captured frames are at least not black.
  cat > "$CFG/sway/config" <<EOF
# headless spike config
output $OUTPUT_NAME mode $OUTPUT_MODE position 0 0 background #1e5fb4 solid_color
default_border none
focus_follows_mouse no
xwayland disable
EOF
}

start() {
  mkdir -p "$RUNTIME" "$LOGS"
  chmod 700 "$RUNTIME"
  write_configs

  export XDG_RUNTIME_DIR="$RUNTIME"
  export XDG_CONFIG_HOME="$CFG"
  export XDG_DATA_HOME="$STATE/share"
  export XDG_CACHE_HOME="$STATE/cache"
  export XDG_STATE_HOME="$STATE/state"
  mkdir -p "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME"

  # 1. private session bus -------------------------------------------------
  if [ -f "$STATE/dbus.addr" ] && \
     DBUS_SESSION_BUS_ADDRESS=$(cat "$STATE/dbus.addr") \
     dbus-send --session --dest=org.freedesktop.DBus --print-reply \
               /org/freedesktop/DBus org.freedesktop.DBus.ListNames >/dev/null 2>&1
  then
    log "session bus already up"
  else
    rm -f "$STATE/dbus.addr"
    dbus-daemon --session --fork --print-address=3 3>"$STATE/dbus.addr" \
      || die "dbus-daemon failed"
    wait_for 50 test -s "$STATE/dbus.addr" || die "no bus address"
  fi
  DBUS_SESSION_BUS_ADDRESS=$(cat "$STATE/dbus.addr")
  export DBUS_SESSION_BUS_ADDRESS
  log "session bus: $DBUS_SESSION_BUS_ADDRESS"

  # 2. pipewire + wireplumber ---------------------------------------------
  if [ -S "$RUNTIME/pipewire-0" ]; then
    log "pipewire socket already present"
  else
    pipewire >"$LOGS/pipewire.log" 2>&1 &
    echo $! > "$STATE/pipewire.pid"
    wait_for 100 test -S "$RUNTIME/pipewire-0" || die "pipewire made no socket"
  fi
  pgrep -x wireplumber >/dev/null 2>&1 || {
    wireplumber >"$LOGS/wireplumber.log" 2>&1 &
    echo $! > "$STATE/wireplumber.pid"
  }
  pgrep -x pipewire-pulse >/dev/null 2>&1 || {
    pipewire-pulse >"$LOGS/pipewire-pulse.log" 2>&1 &
    echo $! > "$STATE/pipewire-pulse.pid"
  }
  wait_for 100 sh -c "pw-dump 2>/dev/null | grep -q '$SINK_NAME'" \
    || log "WARNING: null sink '$SINK_NAME' not yet visible in pw-dump"
  wait_for 100 sh -c "pw-dump 2>/dev/null | grep -q '$SOURCE_NAME'" \
    || log "WARNING: virtual source '$SOURCE_NAME' not yet visible in pw-dump"
  log "pipewire up, sink '$SINK_NAME', source '$SOURCE_NAME'"

  # 3. sway, headless ------------------------------------------------------
  if pgrep -x sway >/dev/null 2>&1 && [ -S "$RUNTIME/wayland-1" ]; then
    log "sway already running"
  else
    WLR_BACKENDS=headless \
    WLR_RENDERER=pixman \
    WLR_LIBINPUT_NO_DEVICES=1 \
    XDG_SESSION_TYPE=wayland \
    XDG_CURRENT_DESKTOP=sway \
    WAYLAND_DISPLAY=wayland-1 \
    sway -c "$CFG/sway/config" >"$LOGS/sway.log" 2>&1 &
    echo $! > "$STATE/sway.pid"
    wait_for 150 test -S "$RUNTIME/wayland-1" || die "sway made no wayland-1"
  fi
  export WAYLAND_DISPLAY=wayland-1
  export XDG_CURRENT_DESKTOP=sway
  export XDG_SESSION_TYPE=wayland
  # sway names its IPC socket after its own pid; swaymsg needs SWAYSOCK.
  SWAYSOCK=$(ls -t "$RUNTIME"/sway-ipc.*.sock 2>/dev/null | head -1)
  export SWAYSOCK
  log "sway up on WAYLAND_DISPLAY=$WAYLAND_DISPLAY SWAYSOCK=$SWAYSOCK"

  # 4. portal backends, then the frontend ----------------------------------
  # xdg-desktop-portal-wlr must be on the bus (and must see the compositor)
  # before xdg-desktop-portal probes backends, or ScreenCast comes back
  # unimplemented.
  log "xdpw binary: $XDPW_BIN"
  pgrep -f 'libexec/xdg-desktop-portal-wlr' >/dev/null 2>&1 || {
    "$XDPW_BIN" -l DEBUG >"$LOGS/xdpw.log" 2>&1 &
    echo $! > "$STATE/xdpw.pid"
  }
  wait_for 100 dbus-send --session --dest=org.freedesktop.DBus --print-reply \
      /org/freedesktop/DBus org.freedesktop.DBus.GetNameOwner \
      string:org.freedesktop.impl.portal.desktop.wlr \
    || die "xdg-desktop-portal-wlr never took its name (see $LOGS/xdpw.log)"

  # The portal search dir: a private copy so we can add the Access shim.
  PDIR="$STATE/portals"
  rm -rf "$PDIR"; mkdir -p "$PDIR"
  cp /usr/share/xdg-desktop-portal/portals/wlr.portal "$PDIR/"
  if [ "$ACCESS_SHIM" = "1" ]; then
    sed -i 's/^Interfaces=.*/Interfaces=org.freedesktop.impl.portal.ScreenCast;/' \
      "$PDIR/wlr.portal"
    cat > "$PDIR/vsmock.portal" <<EOF
[portal]
DBusName=org.freedesktop.impl.portal.desktop.vsmock
Interfaces=org.freedesktop.impl.portal.Screenshot;org.freedesktop.impl.portal.Access;
UseIn=sway;wlroots;
EOF
    pgrep -f mock-screenshot-impl >/dev/null 2>&1 || {
      "$HERE/mock-screenshot-impl.py" --version 2 >"$LOGS/mock.log" 2>&1 &
      echo $! > "$STATE/mock.pid"
    }
    wait_for 100 dbus-send --session --dest=org.freedesktop.DBus --print-reply \
        /org/freedesktop/DBus org.freedesktop.DBus.GetNameOwner \
        string:org.freedesktop.impl.portal.desktop.vsmock \
      || die "Access/Screenshot shim never took its name (see $LOGS/mock.log)"
    log "Access + Screenshot shim up"
  fi

  pgrep -f 'libexec/xdg-desktop-portal( |$)' >/dev/null 2>&1 || {
    XDG_DESKTOP_PORTAL_DIR="$PDIR" /usr/libexec/xdg-desktop-portal -v \
      >"$LOGS/xdp.log" 2>&1 &
    echo $! > "$STATE/xdp.pid"
  }
  wait_for 150 dbus-send --session --dest=org.freedesktop.DBus --print-reply \
      /org/freedesktop/DBus org.freedesktop.DBus.GetNameOwner \
      string:org.freedesktop.portal.Desktop \
    || die "xdg-desktop-portal never took its name (see $LOGS/xdp.log)"
  log "portal frontend + backends up"

  cat > "$STATE/env.sh" <<EOF
export XDG_RUNTIME_DIR=$RUNTIME
export XDG_CONFIG_HOME=$CFG
export XDG_DATA_HOME=$STATE/share
export XDG_CACHE_HOME=$STATE/cache
export XDG_STATE_HOME=$STATE/state
export DBUS_SESSION_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS
export WAYLAND_DISPLAY=wayland-1
export XDG_CURRENT_DESKTOP=sway
export XDG_SESSION_TYPE=wayland
export SWAYSOCK=$SWAYSOCK
export VS_CAPTURE_OUTPUT=$OUTPUT_NAME
export VS_CAPTURE_SINK=$SINK_NAME
export VS_CAPTURE_SOURCE=$SOURCE_NAME
EOF
  log "wrote $STATE/env.sh -- source it to use the stack"
}

# Put something non-uniform on screen so captured frames are not one colour:
# sway paints a solid blue background and `foot` draws a terminal printing a
# changing timestamp on top of it.
paint() {
  # shellcheck disable=SC1090
  . "$STATE/env.sh"
  swaymsg -- output "$OUTPUT_NAME" background '#1e5fb4' solid_color >/dev/null
  pgrep -x foot >/dev/null 2>&1 || {
    foot -f 'monospace:size=28' \
      sh -c 'while :; do date "+%H:%M:%S.%N"; sleep 0.01; done' \
      >"$LOGS/foot.log" 2>&1 &
    echo $! > "$STATE/foot.pid"
    sleep 2
  }
  swaymsg -t get_tree -r | head -c 600; echo
}

status() {
  [ -f "$STATE/env.sh" ] || { echo "not started"; return 1; }
  # shellcheck disable=SC1090
  . "$STATE/env.sh"
  for p in dbus-daemon pipewire wireplumber pipewire-pulse sway foot; do
    printf '%-20s %s\n' "$p" "$(pgrep -x "$p" | tr '\n' ' ')"
  done
  printf '%-20s %s\n' xdg-desktop-portal \
    "$(pgrep -f 'libexec/xdg-desktop-portal( |$)' | tr '\n' ' ')"
  printf '%-20s %s\n' xdg-desktop-portal-wlr \
    "$(pgrep -f 'libexec/xdg-desktop-portal-wlr' | tr '\n' ' ')"
  echo "--- sway outputs ---"
  swaymsg -t get_outputs -r 2>&1 | head -c 500; echo
  echo "--- portal ScreenCast version / SourceTypes ---"
  gdbus call --session -d org.freedesktop.portal.Desktop \
    -o /org/freedesktop/portal/desktop \
    -m org.freedesktop.DBus.Properties.Get \
    org.freedesktop.portal.ScreenCast version 2>&1
  gdbus call --session -d org.freedesktop.portal.Desktop \
    -o /org/freedesktop/portal/desktop \
    -m org.freedesktop.DBus.Properties.Get \
    org.freedesktop.portal.ScreenCast AvailableSourceTypes 2>&1
  echo "--- pipewire sink + monitor ---"
  pw-cli ls Node 2>/dev/null | grep -A1 -i "$VS_CAPTURE_SINK" | head -20
}

stop() {
  for n in foot xdp xdpw mock sway pipewire-pulse wireplumber pipewire; do
    [ -f "$STATE/$n.pid" ] && kill "$(cat "$STATE/$n.pid")" 2>/dev/null
    rm -f "$STATE/$n.pid"
  done
  pkill -f 'libexec/xdg-desktop-portal' 2>/dev/null
  pkill -f mock-screenshot-impl 2>/dev/null
  pkill -x sway 2>/dev/null; pkill -x foot 2>/dev/null
  pkill -x wireplumber 2>/dev/null; pkill -x pipewire-pulse 2>/dev/null
  pkill -x pipewire 2>/dev/null
  pkill -f 'dbus-daemon --session' 2>/dev/null
  rm -f "$STATE/dbus.addr" "$STATE/env.sh"
  log "stopped"
}

case "${1:-start}" in
  start)  start ;;
  paint)  paint ;;
  status) status ;;
  stop)   stop ;;
  *) echo "usage: $0 {start|paint|status|stop}" >&2; exit 2 ;;
esac
