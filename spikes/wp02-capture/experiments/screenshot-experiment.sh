#!/usr/bin/env bash
# Isolate the Screenshot gate: run a mock org.freedesktop.impl.portal.Screenshot
# backend at a chosen `version`, alongside the stock wlr.portal, and see whether
# xdg-desktop-portal exports org.freedesktop.portal.Screenshot.
#
#   screenshot-experiment.sh <spike-dir> <impl-version>
set -u
SPIKE=${1:?spike dir}
VER=${2:-2}
. /tmp/wp02/env.sh

pkill -f 'libexec/xdg-desktop-portal( |$)' 2>/dev/null
pkill -f mock-screenshot 2>/dev/null
sleep 1

PDIR=/tmp/wp02/portals-v$VER
rm -rf "$PDIR"; mkdir -p "$PDIR"
cp /usr/share/xdg-desktop-portal/portals/wlr.portal "$PDIR/"
# Keep wlr for ScreenCast only, so exactly one variable changes.
sed -i 's/^Interfaces=.*/Interfaces=org.freedesktop.impl.portal.ScreenCast;/' \
  "$PDIR/wlr.portal"
cat > "$PDIR/wp02mock.portal" <<EOF
[portal]
DBusName=org.freedesktop.impl.portal.desktop.wp02mock
Interfaces=org.freedesktop.impl.portal.Screenshot;org.freedesktop.impl.portal.Access;
UseIn=sway;wlroots;
EOF

"$SPIKE/mock-screenshot-impl.py" --version "$VER" \
  >/tmp/wp02/logs/mock-v$VER.log 2>&1 &
sleep 2

XDG_DESKTOP_PORTAL_DIR="$PDIR" \
  G_MESSAGES_DEBUG=all /usr/libexec/xdg-desktop-portal -v \
  >"/tmp/wp02/logs/xdp-mock-v$VER.log" 2>&1 &
sleep 4

echo "### mock impl backend advertises:"
gdbus call --session -d org.freedesktop.impl.portal.desktop.wp02mock \
  -o /org/freedesktop/portal/desktop \
  -m org.freedesktop.DBus.Properties.Get \
  org.freedesktop.impl.portal.Screenshot version 2>&1
echo "### xdp lines mentioning Screenshot:"
grep -iE 'screenshot' "/tmp/wp02/logs/xdp-mock-v$VER.log" | head -10
echo "### frontend interfaces:"
gdbus introspect --session -d org.freedesktop.portal.Desktop \
  -o /org/freedesktop/portal/desktop 2>&1 | grep -E '^  interface org.freedesktop.portal'
echo "### frontend Screenshot version:"
gdbus call --session -d org.freedesktop.portal.Desktop \
  -o /org/freedesktop/portal/desktop \
  -m org.freedesktop.DBus.Properties.Get \
  org.freedesktop.portal.Screenshot version 2>&1
