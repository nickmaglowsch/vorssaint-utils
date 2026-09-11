#!/bin/sh
# WP-04: what the Linux port's privileged paths actually see inside a Flatpak
# sandbox. Installed into the Flatpak as /app/bin/sandbox-probe and run with
# `flatpak run --command=sandbox-probe <app-id>` from the CI job, once per
# permission set, so the full and reduced manifests can be diffed.
#
# Each probe names the feature that depends on it (FEATURE_TRIAGE.md).
echo "=== sandbox identity"
echo "FLATPAK_ID=${FLATPAK_ID:-<unset>}"
[ -r /.flatpak-info ] && sed -n '1,40p' /.flatpak-info

echo
echo "=== /dev/uinput  (input relay: quitWindowProtection, scrollInverter,"
echo "    smoothScroll, textSnippets, superKey, cleaningMode, debounce)"
ls -l /dev/uinput 2>&1
if [ -e /dev/uinput ]; then
  if : > /dev/uinput 2>/dev/null; then echo "uinput: WRITABLE"; else echo "uinput: open for write FAILED: $(: > /dev/uinput 2>&1)"; fi
else
  echo "uinput: ABSENT"
fi

echo
echo "=== /dev/input  (evdev grab, the other half of the relay)"
ls -l /dev/input 2>&1 | head -20
echo "event devices visible: $(ls /dev/input/event* 2>/dev/null | wc -l)"

echo
echo "=== /dev/i2c-*  (brightness: DDC/CI on external monitors)"
ls -l /dev/i2c-* 2>&1 | head -10

echo
echo "=== /sys/class/hwmon  (monitorCPU temperatures, fanControl pwm writes)"
ls -l /sys/class/hwmon 2>&1 | head -10
for h in /sys/class/hwmon/hwmon*; do
  [ -d "$h" ] || continue
  echo "--- $h name=$(cat "$h/name" 2>/dev/null)"
  ls "$h" 2>/dev/null | tr '\n' ' '; echo
  for p in "$h"/pwm[0-9]; do
    [ -e "$p" ] || continue
    echo "pwm candidate $p perms=$(stat -c %A "$p" 2>/dev/null) value=$(cat "$p" 2>/dev/null)"
    if printf '%s' "$(cat "$p" 2>/dev/null)" > "$p" 2>/dev/null; then
      echo "  write: SUCCEEDED (read back $(cat "$p" 2>/dev/null))"
    else
      echo "  write: FAILED ($(printf 'x' > "$p" 2>&1))"
    fi
  done
done

echo
echo "=== /sys/class/backlight  (brightness: internal panel via logind)"
ls -l /sys/class/backlight 2>&1 | head -5

echo
echo "=== /sys/class/power_supply  (monitorPower fallback when UPower is denied)"
ls -l /sys/class/power_supply 2>&1 | head -5

echo
echo "=== /proc visibility  (killProcess, per-process CPU/memory)"
echo "pids visible in /proc: $(ls -d /proc/[0-9]* 2>/dev/null | wc -l)"
echo "host pid 1 comm: $(cat /proc/1/comm 2>/dev/null || echo '<denied>')"

echo
echo "=== system bus: login1 (keepAwake inhibitors, session lock)"
if command -v busctl >/dev/null 2>&1; then
  busctl --system list 2>&1 | head -20
  echo "--- login1 Inhibit call:"
  busctl --system call org.freedesktop.login1 /org/freedesktop/login1 \
    org.freedesktop.login1.Manager Inhibit ssss \
    "idle" "vorssaint-probe" "WP-04 probe" "block" 2>&1 | head -3
else
  echo "busctl not present in the runtime"
fi

echo
echo "=== system bus: UPower / BlueZ / PackageKit"
for n in org.freedesktop.UPower org.bluez org.freedesktop.PackageKit; do
  if command -v gdbus >/dev/null 2>&1; then
    printf '%s -> ' "$n"
    gdbus call --system -d "$n" -o / -m org.freedesktop.DBus.Peer.Ping 2>&1 | head -1
  fi
done

echo
echo "=== session bus: portals and the tray watcher"
if command -v gdbus >/dev/null 2>&1; then
  for n in org.freedesktop.portal.Desktop org.kde.StatusNotifierWatcher org.freedesktop.Notifications; do
    printf '%s -> ' "$n"
    gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
      -m org.freedesktop.DBus.NameHasOwner "$n" 2>&1 | head -1
  done
fi

echo
echo "=== host filesystem reach (cleaner, uninstaller)"
for p in /run/host /var/run/host "$HOME/.cache" "$HOME/.local/share" "$HOME/.var/app" /etc/os-release; do
  printf '%-28s ' "$p"
  if [ -e "$p" ]; then echo "present"; else echo "ABSENT"; fi
done
echo "probe done"
