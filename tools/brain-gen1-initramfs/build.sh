#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: $0 OUTPUT.cpio" >&2
	exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output=$1
work=$(mktemp -d /tmp/brain-gen1-initramfs.XXXXXX)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM

mkdir -p "$work/root/bin" "$work/root/dev" "$work/root/proc" \
	"$work/root/sys"
arm-linux-gnueabi-gcc -Os -static -s -Wall -Wextra \
	-o "$work/root/bin/sh" "$script_dir/rescue.c"
cp "$work/root/bin/sh" "$work/root/init"
find "$work/root" -exec touch -h -d @0 {} +

mkdir -p "$(dirname -- "$output")"
(cd "$work/root" && find . -print0 | sort -z | \
	cpio --null --create --format=newc --owner=0:0 --reproducible) > "$output"
echo "wrote $output"
