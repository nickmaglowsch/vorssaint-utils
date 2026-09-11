# WP-04: self-contained packaging proof

Everything here packages the WP-01 winner (Qt 6 Quick,
`spikes/wp01-toolkit/qt-quick`, C bridge stub in
`spikes/wp01-toolkit/corebridge-stub`). The findings are in
`docs/linux-port/spikes/04-packaging.md`.

```
appdir/
  build-appdir.sh           builds a complete AppDir by hand (no linuxdeploy)
  excludelist               SONAMEs deliberately left to the host
  verify-selfcontained.sh   runs the AppDir in a Qt-free chroot, X11 + Wayland
  icu-probe.cpp             does trimming libicudata change behaviour?
flatpak/
  com.example.vorssaint-linux-spike.yml          full permission set
  com.example.vorssaint-linux-spike.flathub.yml  reduced permission set
  sandbox-probe.sh          what the privileged paths see inside the sandbox
  *.desktop, *.svg          placeholder metadata (branding undecided)
```

The app id and icon are placeholders: branding is the lead's decision
(`PLAN.md` § 10, `TRADEMARKS.md`).

## Reproduce: AppDir

```sh
# 1. build the spike in Release
cmake -S spikes/wp01-toolkit/qt-quick -B /tmp/wp04/build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/wp04/build -j"$(nproc)"

# 2. build the AppDir (needs patchelf; qmake6 + qmlimportscanner come from Qt 6)
apt-get install -y patchelf squashfs-tools
./spikes/wp04-packaging/appdir/build-appdir.sh \
    /tmp/wp04/build/vorssaint-qt-spike /tmp/wp04/AppDir

# 3. squashfs the payload (this is what an AppImage carries)
mksquashfs /tmp/wp04/AppDir /tmp/wp04/payload.squashfs \
    -root-owned -noappend -comp zstd -Xcompression-level 19 -b 128K
```

`build-appdir.sh` prints the total size, the file count, the fifteen largest
files, the libraries it left to the host, and the main binary's `RUNPATH`.

## Reproduce: run it on this host

The WP-01 harness is reused unchanged, so the screenshots are comparable with
the WP-01 ones. Both invocations unset `LD_LIBRARY_PATH` first.

```sh
mkdir -p /tmp/wp01
env -u LD_LIBRARY_PATH DISPLAY_NUM=:94 SHOT_AFTER=5 RUN_FOR=9 \
  bash spikes/wp01-toolkit/run-xvfb.sh /tmp/wp04/appdir-xvfb.png \
  /tmp/wp04/AppDir/AppRun --screen panel --quit-after 8000

env -u LD_LIBRARY_PATH SHOT_AFTER=8 RUN_FOR=16 \
  bash spikes/wp01-toolkit/run-sway.sh /tmp/wp04/appdir-sway.png \
  /tmp/wp04/AppDir/AppRun --screen panel --quit-after 14000
```

## Reproduce: prove it is self-contained

A chroot, not a `bwrap` overlay: hiding `/usr/lib/*/qt6` still leaves
`libQt6*.so.6` in `/usr/lib/x86_64-linux-gnu`, so only a root filesystem that
never had Qt installed is real evidence.

```sh
apt-get install -y debootstrap
debootstrap --variant=minbase --include=ca-certificates noble /tmp/wp04/chroot \
    http://archive.ubuntu.com/ubuntu/
printf 'deb http://archive.ubuntu.com/ubuntu/ noble main universe\n' \
    > /tmp/wp04/chroot/etc/apt/sources.list
cp /etc/resolv.conf /tmp/wp04/chroot/etc/resolv.conf
for d in proc sys dev dev/pts; do mount --bind "/$d" "/tmp/wp04/chroot/$d"; done
chroot /tmp/wp04/chroot apt-get update
chroot /tmp/wp04/chroot apt-get install -y --no-install-recommends \
    xvfb x11-utils imagemagick dbus-x11 fonts-dejavu-core \
    libgl1 libegl1 libopengl0 libgl1-mesa-dri libglx-mesa0 libegl-mesa0 \
    libglib2.0-0t64 libfontconfig1 libfreetype6 libharfbuzz0b \
    libice6 libsm6 libx11-xcb1 libxcb-glx0 libxcb-shm0 libxcb-sync1 \
    libxcb-xfixes0 libselinux1 libgpg-error0 libp11-kit0 libcom-err2

./spikes/wp04-packaging/appdir/verify-selfcontained.sh \
    /tmp/wp04/AppDir /tmp/wp04/chroot /tmp/wp04/out both
```

The script refuses to run if any `libQt*` file exists in the chroot outside
the AppDir. `both` runs X11 (Xvfb inside the chroot) and Wayland (sway on the
host, its `XDG_RUNTIME_DIR` bind-mounted into the chroot, `grim` on the host).

## Reproduce: the ICU trimming experiment

```sh
apt-get install -y icu-devtools
objcopy -O binary --only-section=.rodata \
    /tmp/wp04/AppDir/usr/lib/libicudata.so.74 /tmp/wp04/icu/icudt74l.dat
icupkg -l /tmp/wp04/icu/icudt74l.dat > items.txt            # 4083 items
# build a removal list keeping root + the thirteen shipped languages, then:
icupkg -r remove-locales.txt icudt74l.dat trim/icudt74l.dat
# wrap the trimmed .dat back into a .so the way ICU's own pkgdata does:
printf '.globl icudt74_dat\n.section .rodata\n.balign 16\nicudt74_dat:\n.incbin "trim/icudt74l.dat"\n' > trimdata.S
gcc -shared -nostdlib -o libicudata.so.74 -Wl,-soname,libicudata.so.74 trimdata.S
```

Then swap it into a copy of the AppDir and compare `icu-probe` output:

```sh
g++ -std=c++17 -O2 spikes/wp04-packaging/appdir/icu-probe.cpp \
    -I/usr/include/x86_64-linux-gnu/qt6 -I/usr/include/x86_64-linux-gnu/qt6/QtCore \
    -fPIC -lQt6Core -o icu-probe
diff <(AppDir/usr/bin/icu-probe) <(AppDir-trimicu/usr/bin/icu-probe)
```

## Reproduce: Flatpak

`flatpak-builder` needs Flathub, which the spike container cannot reach, so
this runs in CI (`.github/workflows/linux-spike-wp04.yml`, job `flatpak`).
Locally on a machine with internet:

```sh
flatpak remote-add --user --if-not-exists flathub \
    https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user -y flathub org.kde.Platform//6.8 org.kde.Sdk//6.8
cd spikes/wp04-packaging/flatpak
flatpak-builder --user --install --force-clean build-full \
    com.example.vorssaint-linux-spike.yml
flatpak info --show-permissions com.example.vorssaint-linux-spike
flatpak run --command=sandbox-probe com.example.vorssaint-linux-spike
# then the same three commands with the .flathub.yml manifest
```

YAML-only validation (what the spike container can do):

```sh
python3 -c "import yaml,sys;[yaml.safe_load(open(f)) for f in sys.argv[1:]]" \
    spikes/wp04-packaging/flatpak/*.yml
```
