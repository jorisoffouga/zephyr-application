#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Build SWUpdate for the host (x86_64 Linux) with the mcumgr handler.
#
# usage: build-host-swupdate.sh <swupdate-src> [build-dir]
#
# Host packages: build-essential pkg-config libconfig-dev libjson-c-dev libubootenv-dev libssl-dev zlib1g-dev
# Libraries in a non standard prefix: set EXTRA_CFLAGS / EXTRA_LDFLAGS in the
# environment, they are stored in CONFIG_EXTRA_CFLAGS / CONFIG_EXTRA_LDFLAGS.
set -eu

SRC=$(cd "${1:?usage: $0 <swupdate-src> [build-dir]}" && pwd)
OUT=${2:-$SRC/build-host}
HERE=$(cd "$(dirname "$0")" && pwd)

if [ -z "${EXTRA_CFLAGS:-}" ] && ! pkg-config --exists libconfig json-c libubootenv; then
	echo "missing libraries: sudo apt install libconfig-dev libjson-c-dev libubootenv-dev libssl-dev zlib1g-dev" >&2
	exit 1
fi

# Optional libraries SWUpdate assumes present by default: turn them off
# so Kconfig hides the features that need them.
for lib in LUA LIBARCHIVE LIBCURL LIBFDISK LIBBLKID LIBGPIOD LIBMTD LIBUBI \
	   LIBEBGENV LIBTEGRABOOT_TOOLS LIBZEROMQ LIBZSTD LIBRSYNC \
	   LIBZCK LIBBTRFS LIBWEBSOCKETS URIPARSER LIBEXT2FS LIBSYSTEMD \
	   MBEDTLS WOLFSSL GPGME P11KIT LIBUDEV; do
	export HAVE_$lib=n
done

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)	# make -C changes directory: use an absolute path
# start from a clean configuration (a stale auto.conf can re-enable features)
rm -rf "$OUT/include/config" "$OUT/include/generated"
cp "$HERE/host_defconfig" "$OUT/.config"
[ -n "${EXTRA_CFLAGS:-}" ] && echo "CONFIG_EXTRA_CFLAGS=\"$EXTRA_CFLAGS\"" >> "$OUT/.config"
[ -n "${EXTRA_LDFLAGS:-}" ] && echo "CONFIG_EXTRA_LDFLAGS=\"$EXTRA_LDFLAGS\"" >> "$OUT/.config"
# do not let them override the per-directory EXTRA_CFLAGS (-fPIC in ipc/)
unset EXTRA_CFLAGS EXTRA_LDFLAGS
make -C "$SRC" O="$OUT" olddefconfig
grep -q '^CONFIG_MCUMGRHANDLER=y' "$OUT/.config" || {
	echo "CONFIG_MCUMGRHANDLER not enabled (handler missing in $SRC ?)" >&2
	exit 1
}
make -C "$SRC" O="$OUT" -j"$(nproc)" V=1 swupdate 

echo
echo "SWUpdate built: $OUT/swupdate"
