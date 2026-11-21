#!/bin/sh
# Build the companion driver from a clean revision of a local Linux repository.
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "Usage: $0 LINUX_GIT_CHECKOUT [OUTPUT_DIRECTORY]" >&2
    exit 2
fi

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source=$(CDPATH= cd -- "$1" && pwd)
output=${2:-"$here/../build/linux-ap"}
kdir=${KDIR:-"/lib/modules/$(uname -r)/build"}
revision=${LINUX_REF:-v7.0}
tree=drivers/net/wireless/mediatek/mt7601u

mkdir -p -- "$output"
output=$(CDPATH= cd -- "$output" && pwd)
stage=$(mktemp -d "$output/.module.XXXXXXXX")
trap 'rm -rf -- "$stage"' EXIT
trap 'exit 1' HUP INT TERM

git -C "$source" archive --output="$stage/source.tar" "$revision" "$tree"
tar -xf "$stage/source.tar" -C "$stage"
git -C "$stage" init -q
git -C "$stage" apply "$here/mt7601u-ap.patch"
git -C "$stage" apply "$here/mt7601u-wowlan.patch"
git -C "$stage" apply "$here/mt7601u-led.patch"
make -C "$kdir" M="$stage/$tree" CONFIG_MT7601U=m modules
install -m 0644 "$stage/$tree/mt7601u.ko" "$output/mt7601u.ko"
printf '%s\n' "$output/mt7601u.ko"
