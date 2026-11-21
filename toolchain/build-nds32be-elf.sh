#!/bin/sh
# Bootstrap nds32be-elf GCC 9.5 + binutils 2.35 (real NDS32 ISA, not Andes RISC-V v5).
# Prefix is fw/toolchain/nds32be-elf; do not commit the prefix or build trees.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
PREFIX=$ROOT/nds32be-elf
SRC=$ROOT/src
BUILD=$ROOT/build
TARGET=nds32be-elf
NPROC=$(nproc 2>/dev/null || echo 4)
GNU_MIRROR=${GNU_MIRROR:-https://ftpmirror.gnu.org/gnu}

BINUTILS_VER=2.35.2
GCC_VER=9.5.0
GMP_VER=6.2.1
MPFR_VER=4.1.0
MPC_VER=1.2.1

mkdir -p "$SRC" "$BUILD"
cd "$SRC"

fetch() {
	url=$1
	out=$2
	if [ -f "$out" ]; then
		return 0
	fi
	echo "fetch $url"
	wget -O "$out.part" "$url"
	mv "$out.part" "$out"
}

fetch "$GNU_MIRROR/binutils/binutils-${BINUTILS_VER}.tar.xz" "binutils-${BINUTILS_VER}.tar.xz"
fetch "$GNU_MIRROR/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz" "gcc-${GCC_VER}.tar.xz"
fetch "$GNU_MIRROR/gmp/gmp-${GMP_VER}.tar.xz" "gmp-${GMP_VER}.tar.xz"
fetch "$GNU_MIRROR/mpfr/mpfr-${MPFR_VER}.tar.xz" "mpfr-${MPFR_VER}.tar.xz"
fetch "$GNU_MIRROR/mpc/mpc-${MPC_VER}.tar.gz" "mpc-${MPC_VER}.tar.gz"

[ -d "binutils-${BINUTILS_VER}" ] || tar -xf "binutils-${BINUTILS_VER}.tar.xz"
[ -d "gcc-${GCC_VER}" ] || tar -xf "gcc-${GCC_VER}.tar.xz"

if [ ! -d "gcc-${GCC_VER}/gmp" ]; then
	tar -xf "gmp-${GMP_VER}.tar.xz"
	mv "gmp-${GMP_VER}" "gcc-${GCC_VER}/gmp"
fi
if [ ! -d "gcc-${GCC_VER}/mpfr" ]; then
	tar -xf "mpfr-${MPFR_VER}.tar.xz"
	mv "mpfr-${MPFR_VER}" "gcc-${GCC_VER}/mpfr"
fi
if [ ! -d "gcc-${GCC_VER}/mpc" ]; then
	tar -xzf "mpc-${MPC_VER}.tar.gz"
	mv "mpc-${MPC_VER}" "gcc-${GCC_VER}/mpc"
fi

# Host GCC 15 is stricter than GCC 9's sources; keep the compiler in C++14.
export CC="${CC:-gcc}"
export CXX="${CXX:-g++}"
export CFLAGS="${CFLAGS:--O2}"
export CXXFLAGS="${CXXFLAGS:--O2 -std=gnu++14}"

if [ ! -x "$PREFIX/bin/${TARGET}-as" ]; then
	echo "=== binutils ${BINUTILS_VER} (${TARGET}) ==="
	rm -rf "$BUILD/binutils"
	mkdir -p "$BUILD/binutils"
	cd "$BUILD/binutils"
	"$SRC/binutils-${BINUTILS_VER}/configure" \
		--prefix="$PREFIX" \
		--target="$TARGET" \
		--disable-nls \
		--disable-werror \
		--disable-gdb \
		--disable-sim \
		--disable-libdecnumber \
		--disable-readline \
		--disable-gprof \
		--disable-gold \
		--disable-lto
	make -j"$NPROC"
	make install
	cd "$ROOT"
fi

if [ ! -x "$PREFIX/bin/${TARGET}-gcc" ]; then
	echo "=== gcc ${GCC_VER} (${TARGET}) all-gcc ==="
	rm -rf "$BUILD/gcc"
	mkdir -p "$BUILD/gcc"
	cd "$BUILD/gcc"
	"$SRC/gcc-${GCC_VER}/configure" \
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
		--disable-libstdcxx \
		--disable-bootstrap \
		--disable-multilib \
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
"$PREFIX/bin/${TARGET}-gcc" -dumpmachine
"$PREFIX/bin/${TARGET}-gcc" -dM -E - </dev/null | grep -E '__NDS32__|__nds32|__riscv' || true
