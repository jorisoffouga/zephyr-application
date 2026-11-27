#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Package a signed MCUboot image into a .swu with swugenerator (>= 0.9).
#
# usage: mkswu.sh [-z zlib|zstd] [-k <aes key file>]
#                 <zephyr.signed.bin> <out.swu> [uart] [confirm] [version]
#   -z         compress the image in the .swu
#   -k         encrypt the image (AES-256-CBC), with the SWUpdate key file
#              given to swupdate -K ("<key hex> <iv hex>" on one line).
#              A random IV per image is stored in sw-description (ivt).
#   uart       default /dev/ttyACM0 (frdm_k64f OpenSDA)
#   confirm    mcu (default, app confirms itself) | host
#   version    default 1.0.0
#
# swugenerator adds the sha256, and decompressed-size / decrypted-size,
# needed by the mcumgr handler to know the image size before the upload.
#
# Equivalent direct call, with the variables in swugenerator.conf:
#   swugenerator -s sw-description -c swugenerator.conf \
#                -a <dir of zephyr.signed.bin> -o out.swu create
set -eu

USAGE="usage: $0 [-z zlib|zstd] [-k <aes key file>] <zephyr.signed.bin> <out.swu> [uart] [confirm] [version]"
COMPRESS=
KEYFILE=
while getopts "z:k:" opt; do
	case "$opt" in
	z) COMPRESS=$OPTARG ;;
	k) KEYFILE=$OPTARG ;;
	*) echo "$USAGE" >&2; exit 1 ;;
	esac
done
shift $((OPTIND - 1))

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
case "$COMPRESS" in ""|zlib|zstd) ;; *) echo "compression must be zlib or zstd" >&2; exit 1 ;; esac
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

# options of the image, added after its "type" line
COMPRESS_LINE=
[ -n "$COMPRESS" ] && COMPRESS_LINE="compressed = \"$COMPRESS\";"
ENCRYPT_LINE=
GENOPTS=
if [ -n "$KEYFILE" ]; then
	# SWUpdate key file: "<key> <iv>"; swugenerator: key=/iv= lines
	read -r KEY IV < "$KEYFILE"
	[ ${#KEY} -eq 64 ] && [ ${#IV} -eq 32 ] || {
		echo "$KEYFILE: expected \"<64 hex key> <32 hex iv>\"" >&2
		exit 1
	}
	printf 'key=%s\niv=%s\n' "$KEY" "$IV" > "$WORK/aes.key"
	GENOPTS="-K $WORK/aes.key"
	ENCRYPT_LINE="encrypted = true;"
fi
awk -v c="$COMPRESS_LINE" -v e="$ENCRYPT_LINE" '
	{ print }
	/type = "mcumgr";/ {
		if (c != "") print "\t\t\t" c
		if (e != "") print "\t\t\t" e
	}' "$HERE/sw-description" > "$WORK/sw-description"

swugenerator -s "$WORK/sw-description" -c "$WORK/vars.conf" $GENOPTS \
	-a "$(cd "$(dirname "$IMG")" && pwd)" -o "$OUT" create
echo "$OUT: device=$DEVICE confirm=$CONFIRM version=$VERSION" \
     "compressed=${COMPRESS:-no} encrypted=$([ -n "$KEYFILE" ] && echo yes || echo no)"
