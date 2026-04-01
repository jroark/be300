#!/bin/bash
set -e

# Use the patched kernel from the x86 directory
KERNEL_SOURCE_URL="http://distro.ibiblio.org/tinycorelinux/7.x/x86/release/src/kernel/linux-4.2.9-patched.tar.xz"

echo "--- Downloading Patched Kernel Source ---"
if [ ! -f linux-4.2.9-patched.tar.xz ]; then
    wget $KERNEL_SOURCE_URL
fi

echo "--- Extracting Kernel ---"
if [ ! -d linux-4.2.9 ]; then
    tar xf linux-4.2.9-patched.tar.xz
fi

cd linux-4.2.9

# FIX 1: multiple definition of 'yylloc' (GCC 10+ issue)
if [ -f scripts/dtc/dtc-lexer.lex.c_shipped ]; then
    sed -i 's/^YYLTYPE yylloc;/extern YYLTYPE yylloc;/' scripts/dtc/dtc-lexer.lex.c_shipped
fi

# FIX 2: GCC 11+ noreturn/const conflict in log2.h
find . -name "log2.h" -exec sed -i 's/____ilog2_NaN(void) __attribute__((noreturn, const))/____ilog2_NaN(void) __attribute__((noreturn))/g' {} +

# FIX 3: Disable -Werror aggressively
grep -rl "\-Werror" . | xargs sed -i 's/\-Werror\([[:space:]]\|$\)/ /g' || true
grep -rl "\-Werror" . | xargs sed -i 's/=\-Werror/=/g' || true

echo "--- Preparing Config ---"
make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- malta_defconfig

echo "--- Starting Kernel Build (vmlinux) ---"
# Using -j1 to avoid "Too many open files" which is persistent in this environment
make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- vmlinux -j1 || true

if [ -f vmlinux ]; then
    echo "--- SUCCESS: vmlinux created ---"
    ls -l vmlinux
else
    echo "--- FAILURE: vmlinux NOT created ---"
    exit 1
fi
