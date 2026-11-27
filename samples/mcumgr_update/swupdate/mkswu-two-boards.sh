#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Package the signed images of the two boards into one .swu with
# swugenerator (sw-description-two-boards).
#
# usage: mkswu-two-boards.sh <frdm_k64f zephyr.signed.bin>
#            <stm32f4_disco zephyr.signed.bin> <out.swu>
#            [k64f uart] [f4 uart] [confirm] [version]
#   k64f uart  default /dev/ttyACM0 (OpenSDA)
#   f4 uart    default /dev/ttyUSB0 (MikroE shield FT232RL)
#   confirm    mcu (default, app confirms itself) | host
#   version    default 1.0.0
#
# Both builds name their image zephyr.signed.bin: they are copied as
# frdm_k64f.signed.bin and stm32f4_disco.signed.bin, the names used in
# sw-description-two-boards.
set -eu

USAGE="usage: $0 <k64f image> <f4 image> <out.swu> [k64f uart] [f4 uart] [confirm] [version]"
K64F_IMG=${1:?$USAGE}
F4_IMG=${2:?$USAGE}
OUT=${3:?$USAGE}
K64F_DEVICE=${4:-/dev/ttyACM0}
F4_DEVICE=${5:-/dev/ttyUSB0}
CONFIRM=${6:-mcu}
VERSION=${7:-1.0.0}
HERE=$(cd "$(dirname "$0")" && pwd)

command -v swugenerator >/dev/null || {
	echo "swugenerator not found: pip install swugenerator" >&2
	exit 1
}
case "$CONFIRM" in mcu|host) ;; *) echo "confirm must be mcu or host" >&2; exit 1 ;; esac

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp "$K64F_IMG" "$WORK/frdm_k64f.signed.bin"
cp "$F4_IMG" "$WORK/stm32f4_disco.signed.bin"
cat > "$WORK/vars.conf" <<EOV
variables:
{
	VERSION = "$VERSION";
	K64F_DEVICE = "$K64F_DEVICE";
	F4_DEVICE = "$F4_DEVICE";
	CONFIRM = "$CONFIRM";
};
EOV

OUT_ABS=$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")
swugenerator -s "$HERE/sw-description-two-boards" -c "$WORK/vars.conf" \
	-a "$WORK" -o "$OUT_ABS" create
echo "$OUT: frdm_k64f=$K64F_DEVICE stm32f4_disco=$F4_DEVICE confirm=$CONFIRM version=$VERSION"
