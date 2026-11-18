#!/bin/sh
# Build the toolchain-side pieces this port needs: libdragon (preview branch)
# with the emulator fixes in stak64-emulator-fixes.patch, installed to $N64_INST,
# and the patched IPL3 (copied next to this script).
#
# Needs the libdragon MIPS toolchain in $N64_INST (see
# https://github.com/DragonMinded/libdragon/wiki/Installing-libdragon).
set -e
: "${N64_INST:?set N64_INST to the toolchain prefix}"
HERE=$(cd "$(dirname "$0")" && pwd)
REV=39d0d6096130836a65710da6730290701d060272
DIR=${LIBDRAGON_DIR:-$HERE/src}
if [ ! -d "$DIR" ]; then
  git clone --branch preview https://github.com/DragonMinded/libdragon "$DIR"
fi
cd "$DIR"
git checkout -q "$REV"
git apply --check "$HERE/stak64-emulator-fixes.patch" 2>/dev/null && git apply "$HERE/stak64-emulator-fixes.patch"
export PATH="$N64_INST/bin:$PATH"
make -j"$(nproc)" install
make -C boot
cp boot/bin/ipl3_dev.z64 "$HERE/ipl3_dev.z64"
echo "libdragon installed to $N64_INST; IPL3 copied to $HERE/ipl3_dev.z64"
