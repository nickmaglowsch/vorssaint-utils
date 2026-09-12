#!/bin/bash
# install.sh and uninstall.sh against a DESTDIR fake root.
#
# What this proves, without touching the machine it runs on: the scripts put
# exactly seven files in exactly the paths PRIVILEGES.md documents, with the
# documented modes; a second install is a no-op and says so; a modified file
# is repaired; and uninstall removes every one of them and leaves the tree
# empty. What it does not prove is the reload half -- systemctl, udevadm and
# busctl are skipped under DESTDIR by design, and there is no systemd as pid 1
# here to run them against.
set -u

DIST=${HELPER_DIST:?HELPER_DIST must point at linux/helper/dist}
BIN=${HELPER_BIN:?HELPER_BIN must point at a built vorssaint-helper}
ROOT=$(mktemp -d /tmp/vorssaint-install-test-XXXXXX)
failures=0

ok()   { printf '  %-52s ok\n' "$1"; }
fail() { printf '  FAIL %s: %s\n' "$1" "$2"; failures=$((failures + 1)); }

FILES=(
  "usr/libexec/vorssaint-helper:755"
  "usr/share/dbus-1/system.d/org.vorssaint.Helper1.conf:644"
  "usr/share/dbus-1/system-services/org.vorssaint.Helper1.service:644"
  "usr/share/polkit-1/actions/org.vorssaint.helper.policy:644"
  "usr/lib/systemd/system/vorssaint-helper.service:644"
  "usr/lib/udev/rules.d/70-vorssaint-uinput.rules:644"
  "usr/lib/udev/rules.d/71-vorssaint-i2c.rules:644"
)

run_install()   { DESTDIR="$ROOT" HELPER_BIN="$BIN" SRC="$DIST" sh "$DIST/install.sh"   2>&1; }
run_uninstall() { DESTDIR="$ROOT" sh "$DIST/uninstall.sh" 2>&1; }

echo "first install into a fake root"
out=$(run_install); rc=$?
echo "$out" | sed 's/^/    /'
[ $rc -eq 0 ] && ok "install.sh exits 0" || fail "install.sh exits 0" "exit $rc"

all=1
for entry in "${FILES[@]}"; do
  path="$ROOT/${entry%%:*}"
  mode="${entry##*:}"
  if [ ! -f "$path" ]; then
    fail "every documented file is present" "missing ${entry%%:*}"
    all=0
  elif [ "$(stat -c '%a' "$path")" != "$mode" ]; then
    fail "every file has its documented mode" "${entry%%:*} is $(stat -c '%a' "$path"), want $mode"
    all=0
  fi
done
[ $all -eq 1 ] && ok "all seven files present with the documented modes"

# Nothing may be written outside the seven paths: a privileged installer that
# also drops a file somewhere unlisted is exactly what the uninstall cannot
# then remove.
n=$(find "$ROOT" -type f | wc -l)
[ "$n" -eq 7 ] && ok "nothing else was written into the root" \
               || fail "nothing else was written into the root" "$n files found"

echo
echo "second install: must be a no-op"
out=$(run_install); rc=$?
echo "$out" | sed 's/^/    /'
[ $rc -eq 0 ] && ok "a repeat install exits 0" || fail "a repeat install exits 0" "exit $rc"
if echo "$out" | grep -q "0 installed, 0 updated, 7 unchanged"; then
  ok "a repeat install changes nothing and says so"
else
  fail "a repeat install changes nothing and says so" "$(echo "$out" | grep 'unchanged$')"
fi
if echo "$out" | grep -q "Nothing changed; skipping the reloads"; then
  ok "with nothing changed, the reloads are skipped"
else
  fail "with nothing changed, the reloads are skipped" "no such line"
fi

echo
echo "a tampered file is repaired"
POLICY="$ROOT/usr/share/polkit-1/actions/org.vorssaint.helper.policy"
echo '<!-- tampered -->' >> "$POLICY"
out=$(run_install)
if cmp -s "$DIST/org.vorssaint.helper.policy" "$POLICY"; then
  ok "a modified policy file is restored to the shipped one"
else
  fail "a modified policy file is restored to the shipped one" "still differs"
fi
echo "$out" | grep -q "updated    $POLICY" &&
  ok "and the repair is reported as an update" ||
  fail "and the repair is reported as an update" "$(echo "$out" | grep policy)"

echo
echo "a wrong mode is repaired"
chmod 666 "$POLICY"
out=$(run_install)
[ "$(stat -c '%a' "$POLICY")" = "644" ] &&
  ok "a world-writable policy file has its mode put back" ||
  fail "a world-writable policy file has its mode put back" "$(stat -c '%a' "$POLICY")"

echo
echo "uninstall"
out=$(run_uninstall); rc=$?
echo "$out" | sed 's/^/    /'
[ $rc -eq 0 ] && ok "uninstall.sh exits 0" || fail "uninstall.sh exits 0" "exit $rc"
n=$(find "$ROOT" -type f | wc -l)
[ "$n" -eq 0 ] && ok "every installed file is gone" || fail "every installed file is gone" "$n left"

echo
echo "uninstall again: must be a no-op"
out=$(run_uninstall); rc=$?
[ $rc -eq 0 ] && ok "a repeat uninstall exits 0" || fail "a repeat uninstall exits 0" "exit $rc"
echo "$out" | grep -q "0 removed, 7 already absent" &&
  ok "a repeat uninstall reports everything already absent" ||
  fail "a repeat uninstall reports everything already absent" "$(echo "$out" | grep absent$)"

echo
echo "install.sh refuses to half-install"
out=$(DESTDIR="$ROOT" HELPER_BIN=/nonexistent SRC="$DIST" sh "$DIST/install.sh" 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$out" | grep -q "missing source file"; then
  ok "a missing binary is a hard failure, named"
else
  fail "a missing binary is a hard failure, named" "exit $rc: $out"
fi
n=$(find "$ROOT" -type f | wc -l)
[ "$n" -eq 0 ] && ok "and nothing was installed before it failed" \
               || fail "and nothing was installed before it failed" "$n files"

rm -rf "$ROOT"
echo
if [ "$failures" -eq 0 ]; then
  echo "all install-script tests passed"
else
  echo "FAILURES"
fi
exit $((failures > 0))
