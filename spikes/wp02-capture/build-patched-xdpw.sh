#!/usr/bin/env bash
# Fetch the Ubuntu source of xdg-desktop-portal-wlr 0.7.1, apply
# xdpw-shm-only.patch, and build it into /tmp/wp02/xdpw-patched.
#
# Needed because stock xdpw 0.7.1 refuses to screencast a wlroots session that
# has no DMA-BUF capable renderer (see the patch header and 02-capture.md).
set -eux

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${WP02_SRC:-/tmp/wp02/src}
PREFIX=${WP02_XDPW_PREFIX:-/tmp/wp02/xdpw-patched}

if ! grep -rqs 'Types: deb-src' /etc/apt/sources.list.d/wp02-src.sources; then
  cat > /etc/apt/sources.list.d/wp02-src.sources <<'EOF'
Types: deb-src
URIs: http://archive.ubuntu.com/ubuntu/
Suites: noble noble-updates
Components: main universe
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF
  apt-get update -o DPkg::Lock::Timeout=900
fi

DEBIAN_FRONTEND=noninteractive apt-get install -y -o DPkg::Lock::Timeout=900 \
  meson libwayland-dev wayland-protocols libdrm-dev libgbm-dev \
  libsystemd-dev libinih-dev scdoc

mkdir -p "$SRC"
cd "$SRC"
[ -d xdg-desktop-portal-wlr-0.7.1 ] || apt-get source xdg-desktop-portal-wlr
cd xdg-desktop-portal-wlr-0.7.1
patch -N -p1 --batch < "$HERE/xdpw-shm-only.patch" || true
rm -rf build
meson setup build --prefix="$PREFIX" -Dsd-bus-provider=libsystemd
meson compile -C build
meson install -C build
ls -la "$PREFIX/libexec/xdg-desktop-portal-wlr"
