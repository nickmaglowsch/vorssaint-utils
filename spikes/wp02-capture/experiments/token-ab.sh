#!/usr/bin/env bash
# Prove the restore token actually drives source selection, not just that the
# same UUID comes back: point xdpw's config at an output that does not exist.
#   - a fresh session then has nothing to capture and must fail
#   - a session carrying the token must still capture HEADLESS-1, because the
#     output name comes from the stored restore_data
set -u
. /tmp/wp02/env.sh
CAP=/tmp/wp02/build/capture

echo "### baseline: capture a token with the correct config"
rm -f /tmp/wp02/token-ab.txt
$CAP screencast -o /tmp/wp02/out/ab0.mp4 -d 1 --restore-token /tmp/wp02/token-ab.txt \
  2>&1 | grep -E 'Start_response|restore_token_received|^STAT frames='

echo
echo "### break the config: output_name=NO-SUCH-OUTPUT"
cat > "$XDG_CONFIG_HOME/xdg-desktop-portal-wlr/config" <<'EOF'
[screencast]
output_name=NO-SUCH-OUTPUT
max_fps=30
chooser_type=none
EOF
pkill -f 'libexec/xdg-desktop-portal-wlr' 2>/dev/null
sleep 1
"${WP02_XDPW:-/tmp/wp02/xdpw-patched/libexec/xdg-desktop-portal-wlr}" -l DEBUG \
  >/tmp/wp02/logs/xdpw-ab.log 2>&1 &
sleep 2

echo "--- A: fresh session, no token (expect failure) ---"
$CAP screencast -o /tmp/wp02/out/abA.mp4 -d 1 \
  2>&1 | grep -E 'SelectSources_response|Start_response|ERROR'

echo "--- B: same broken config, WITH the stored token (expect success) ---"
$CAP screencast -o /tmp/wp02/out/abB.mp4 -d 1 --restore-token /tmp/wp02/token-ab.txt \
  2>&1 | grep -E 'restore_token_sent|SelectSources_response|Start_response|^STAT frames='

echo
echo "--- what xdpw logged for B ---"
grep -E 'restore|output_name|capturable output|unable' /tmp/wp02/logs/xdpw-ab.log | tail -12

# restore the good config
cat > "$XDG_CONFIG_HOME/xdg-desktop-portal-wlr/config" <<'EOF'
[screencast]
output_name=HEADLESS-1
max_fps=30
chooser_type=none
EOF
