#!/bin/sh
# tools/build-cross.sh — REFERENCE record of how /opt/cross was built 2026-10-09.
# Do not re-run unless /opt/cross is deleted. Kept for reproducibility.
# Per https://wiki.osdev.org/GCC_Cross-Compiler
set -e
export PREFIX=/opt/cross TARGET=x86_64-elf PATH=$PREFIX/bin:$PATH
BINUTILS=2.47
GCCV=16.2.0
pacman -S --needed mingw-w64-ucrt-x86_64-gmp mingw-w64-ucrt-x86_64-mpfr mingw-w64-ucrt-x86_64-mpc mingw-w64-ucrt-x86_64-isl wget
mkdir -p ~/cross-src && cd ~/cross-src
[ -f binutils-$BINUTILS.tar.xz ] || wget https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS.tar.xz
[ -f gcc-$GCCV.tar.xz ] || wget https://ftp.gnu.org/gnu/gcc/gcc-$GCCV/gcc-$GCCV.tar.xz
tar -xf binutils-$BINUTILS.tar.xz; tar -xf gcc-$GCCV.tar.xz
mkdir -p build-binutils && cd build-binutils
../binutils-$BINUTILS/configure --target=$TARGET --prefix=$PREFIX --with-sysroot --disable-nls --disable-werror
make -j$(nproc); make install; cd ..
mkdir -p build-gcc && cd build-gcc
../gcc-$GCCV/configure --target=$TARGET --prefix=$PREFIX --disable-nls --enable-languages=c,c++ --without-headers
make -j$(nproc) all-gcc all-target-libgcc; make install-gcc install-target-libgcc; cd ..
x86_64-elf-gcc --version; x86_64-elf-ld --version
