#!/usr/bin/env bash
# Run a command inside the environment created by run-stack.sh.
#   ./with-stack.sh grim /tmp/shot.png
set -eu
STATE=${VS_CAPTURE_STATE:-/tmp/vorssaint-capture}
[ -f "$STATE/env.sh" ] || { echo "stack not started: no $STATE/env.sh" >&2; exit 1; }
# shellcheck disable=SC1090
. "$STATE/env.sh"
exec "$@"
