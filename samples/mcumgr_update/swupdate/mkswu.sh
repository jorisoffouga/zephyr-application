#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Package a signed MCUboot image into a .swu with swugenerator.
#
# usage: mkswu.sh <zephyr.signed.bin> <out.swu> [uart] [confirm] [version]
#   uart     default /dev/ttyACM0 (frdm_k64f OpenSDA)
#   confirm  mcu (default, app confirms itself) | host
#   version  default 1.0.0
#
# Equivalent direct call, with the variables in swugenerator.conf:
#   swugenerator -s sw-description -c swugenerator.conf \
#                -a <dir of zephyr.signed.bin> -o out.swu create
set -eu

USAGE="usage: $0 <zephyr.signed.bin> <out.swu> [uart] [confirm] [version]"
IMG=${1:?$USAGE}
OUT=${2:?$USAGE}
DEVICE=${3:-/dev/ttyACM0}
CONFIRM=${4:-mcu}
VERSION=${5:-1.0.0}
HERE=$(cd "$(dirname "$0")" && pwd)

command -v swugenerator >/dev/null || {
	echo "swugenerator not found: pip install swugenerator" >&2
	exit 1
}
case "$CONFIRM" in mcu|host) ;; *) echo "confirm must be mcu or host" >&2; exit 1 ;; esac
[ "$(basename "$IMG")" = zephyr.signed.bin ] || {
	echo "expected a zephyr.signed.bin (name used in sw-description)" >&2
	exit 1
}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cat > "$WORK/vars.conf" <<EOV
variables:
{
	VERSION = "$VERSION";
	DEVICE = "$DEVICE";
	CONFIRM = "$CONFIRM";
};
EOV

swugenerator -s "$HERE/sw-description" -c "$WORK/vars.conf" \
	-a "$(cd "$(dirname "$IMG")" && pwd)" -o "$OUT" create
echo "$OUT: device=$DEVICE confirm=$CONFIRM version=$VERSION"
