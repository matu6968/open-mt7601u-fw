#!/bin/sh
# Bootstrap nds32be-elf GCC 16 + binutils 2.47 (real NDS32 ISA, not Andes RISC-V v5)
# from local source checkouts; nothing is downloaded.
#
#   toolchain/gcc          GCC source tree (16.2.1 was used; may be a symlink)
#   toolchain/binutils-gdb binutils-gdb source tree (2.47 release)
#
# Override with GCC_SRC=... and BINUTILS_SRC=.... GMP, MPFR and MPC come from
# the host (install their development packages). Prefix is
# toolchain/nds32be-elf; do not commit the prefix, build trees, or source links.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
PREFIX=${PREFIX:-$ROOT/nds32be-elf}
BUILD=${BUILD:-$ROOT/build}
GCC_SRC=${GCC_SRC:-$ROOT/gcc}
BINUTILS_SRC=${BINUTILS_SRC:-$ROOT/binutils-gdb}
TARGET=nds32be-elf
NPROC=$(nproc 2>/dev/null || echo 4)

[ -f "$GCC_SRC/gcc/BASE-VER" ] || {
	echo "GCC source tree not found at $GCC_SRC (set GCC_SRC)" >&2; exit 1; }
[ -f "$BINUTILS_SRC/bfd/config.bfd" ] || {
	echo "binutils-gdb source tree not found at $BINUTILS_SRC (set BINUTILS_SRC)" >&2; exit 1; }
grep -q nds32 "$GCC_SRC/gcc/config.gcc" || {
	echo "$GCC_SRC has no nds32 backend" >&2; exit 1; }
grep -q nds32 "$BINUTILS_SRC/bfd/config.bfd" || {
	echo "$BINUTILS_SRC has no nds32 support" >&2; exit 1; }
echo "GCC $(cat "$GCC_SRC/gcc/BASE-VER"), binutils-gdb at $BINUTILS_SRC"

export CC="${CC:-gcc}"
export CXX="${CXX:-g++}"
export CFLAGS="${CFLAGS:--O2}"
export CXXFLAGS="${CXXFLAGS:--O2}"
mkdir -p "$BUILD"

if [ ! -x "$PREFIX/bin/${TARGET}-as" ]; then
	echo "=== binutils (${TARGET}) ==="
	rm -rf "$BUILD/binutils"
	mkdir -p "$BUILD/binutils"
	cd "$BUILD/binutils"
	"$BINUTILS_SRC/configure" \
		--prefix="$PREFIX" \
		--target="$TARGET" \
		--disable-nls \
		--disable-werror \
		--disable-gdb \
		--disable-gdbserver \
		--disable-sim \
		--disable-readline \
		--disable-gprof \
		--disable-gold \
		--enable-plugins \
		--with-system-zlib
	make -j"$NPROC" all-binutils all-gas all-ld
	make install-binutils install-gas install-ld
	cd "$ROOT"
fi

# GCC finds the new assembler and linker through PATH while it configures.
PATH="$PREFIX/bin:$PATH"
export PATH

if [ ! -x "$PREFIX/bin/${TARGET}-gcc" ]; then
	echo "=== gcc (${TARGET}) all-gcc ==="
	rm -rf "$BUILD/gcc"
	mkdir -p "$BUILD/gcc"
	cd "$BUILD/gcc"
	"$GCC_SRC/configure" \
		--prefix="$PREFIX" \
		--target="$TARGET" \
		--enable-languages=c \
		--disable-nls \
		--disable-shared \
		--disable-threads \
		--disable-tls \
		--disable-libssp \
		--disable-libgomp \
		--disable-libquadmath \
		--disable-libatomic \
		--disable-bootstrap \
		--disable-multilib \
		--enable-lto \
		--without-headers \
		--with-newlib \
		--with-gnu-as \
		--with-gnu-ld \
		--with-endian=big
	make -j"$NPROC" all-gcc
	make install-gcc
	cd "$ROOT"
fi

echo "installed $PREFIX"
"$PREFIX/bin/${TARGET}-gcc" --version | head -n 1
"$PREFIX/bin/${TARGET}-gcc" -dumpmachine
"$PREFIX/bin/${TARGET}-gcc" -dM -E - </dev/null | grep -E '__NDS32__|__nds32|__riscv' || true
