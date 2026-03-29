#!/bin/bash
# Phase 2: Rebuild Qt/Embedded 2.3.10 against Buildroot musl sysroot
# Phase 3: Build hello app against musl
# Run inside mips-dev container:
#   docker compose run --rm mips-dev bash /work/opie-build/build_qte_musl.sh
set -e

BUILDDIR=/work/opie-build
BRDIR=$BUILDDIR/buildroot/output/host
TCBIN=$BRDIR/bin
SYSROOT=$BRDIR/mipsel-buildroot-linux-musl/sysroot
export QTDIR=$BUILDDIR/qte-opie
export OPIEDIR=$BUILDDIR/opie
export QPEDIR=$BUILDDIR/opie

# Verify toolchain exists
if [ ! -f "$TCBIN/mipsel-linux-gcc" ]; then
    echo "ERROR: Buildroot toolchain not found. Run build_musl_toolchain.sh first."
    exit 1
fi

echo "=== Phase 2: Rebuild Qt/E 2.3.10 with musl toolchain ==="

# --- Step 1: Create musl toolchain config for Qt/E ---
echo "--- Creating linux-mips-musl-g++-shared config ---"
MUSL_CONFIG=$QTDIR/configs/linux-mips-musl-g++-shared

cat > "$MUSL_CONFIG" << 'QTECONFIG'
# Compiling
INTERFACE_DECL_PATH 	= .
SYSCONF_CXX		= mipsel-linux-g++
SYSCONF_CC		= mipsel-linux-gcc
DASHCROSS		= -mips

# Compiling with support libraries
SYSCONF_CXXFLAGS_X11	=
SYSCONF_CXXFLAGS_QT	= -I$(QTDIR)/include
SYSCONF_CXXFLAGS_QTOPIA	= -I$(QPEDIR)/include
SYSCONF_CXXFLAGS_OPENGL	= -I/usr/X11R6/include

# Compiling YACC output
SYSCONF_CXXFLAGS_YACC     = -Wno-unused -Wno-parentheses

# Linking with support libraries
SYSCONF_RPATH_X11	=
SYSCONF_RPATH_QT	= -Wl,-rpath,$(QTDIR)/lib
SYSCONF_RPATH_QTOPIA	= -Wl,-rpath,$(QPEDIR)/lib
SYSCONF_RPATH_OPENGL	= -Wl,-rpath,/usr/X11R6/lib

# Linking with support libraries
# X11
SYSCONF_LFLAGS_X11	=
SYSCONF_LIBS_X11	=
# Qt, Qt+OpenGL
SYSCONF_LFLAGS_QT	= -L$(QTDIR)/lib
SYSCONF_LFLAGS_QTOPIA	= -L$(QPEDIR)/lib
SYSCONF_LIBS_QT		= -lqte$(QT_THREAD_SUFFIX)
SYSCONF_LIBS_QT_OPENGL	=
SYSCONF_LIBS_QTOPIA	= -lqtopia
# OpenGL
SYSCONF_LFLAGS_OPENGL	= -L/usr/X11R6/lib
SYSCONF_LIBS_OPENGL	=
# Yacc
SYSCONF_LIBS_YACC	=

# Linking applications
SYSCONF_LINK		= mipsel-linux-gcc
SYSCONF_LFLAGS		=
SYSCONF_LIBS		=

# Link flags for shared objects
SYSCONF_LFLAGS_SHOBJ	= -shared

# Flags for threading
SYSCONF_CFLAGS_THREAD	= -D_REENTRANT
SYSCONF_CXXFLAGS_THREAD	= -D_REENTRANT
SYSCONF_LFLAGS_THREAD	=
SYSCONF_LIBS_THREAD	=  -lpthread

# Meta-object compiler
SYSCONF_MOC		= $(QTDIR)/bin/moc

# UI compiler
SYSCONF_UIC		= $(QTDIR)/bin/uic

# Linking shared libraries
SYSCONF_LINK_SHLIB	= mipsel-linux-gcc
SYSCONF_LINK_TARGET_SHARED	= lib$(TARGET).so.$(VER_MAJ).$(VER_MIN).$(VER_PATCH)
SYSCONF_LINK_LIB_SHARED	=  $(SYSCONF_LINK_SHLIB) -shared -Wl,-soname,lib$(TARGET).so.$(VER_MAJ) \
				     $(LFLAGS) -o $(SYSCONF_LINK_TARGET_SHARED) \
				     $(OBJECTS) $(OBJMOC) $(LIBS) && \
				 mv $(SYSCONF_LINK_TARGET_SHARED) $(DESTDIR); \
				 cd $(DESTDIR) && \
				 rm -f lib$(TARGET).so lib$(TARGET).so.$(VER_MAJ) lib$(TARGET).so.$(VER_MAJ).$(VER_MIN); \
				 ln -s $(SYSCONF_LINK_TARGET_SHARED) lib$(TARGET).so; \
				 ln -s $(SYSCONF_LINK_TARGET_SHARED) lib$(TARGET).so.$(VER_MAJ); \
				 ln -s $(SYSCONF_LINK_TARGET_SHARED) lib$(TARGET).so.$(VER_MAJ).$(VER_MIN)

# Linking static libraries
SYSCONF_AR		= mipsel-linux-ar cqs
SYSCONF_LINK_TARGET_STATIC = lib$(TARGET).a
SYSCONF_LINK_LIB_STATIC	= rm -f $(DESTDIR)$(SYSCONF_LINK_TARGET_STATIC) ; \
				 $(SYSCONF_AR) $(DESTDIR)$(SYSCONF_LINK_TARGET_STATIC) $(OBJECTS) $(OBJMOC)
# Compiling application source
SYSCONF_CXXFLAGS	= -pipe -march=mips2 -DQWS -fno-exceptions -fno-rtti -O2 -w -DNO_DEBUG -fpermissive -std=gnu++98 -Wno-narrowing -DQT_NO_SOUND
SYSCONF_CFLAGS		= -pipe -march=mips2 -O2 -w -Wno-implicit-function-declaration -Wno-implicit-int -Wno-int-conversion -Wno-incompatible-pointer-types
# Default link type (static linking is still be used where required)
SYSCONF_LINK_LIB	= $(SYSCONF_LINK_LIB_SHARED)
SYSCONF_LINK_TARGET	= $(SYSCONF_LINK_TARGET_SHARED)
# Compiling library source
SYSCONF_CXXFLAGS_LIB	= -fPIC
SYSCONF_CFLAGS_LIB	= -fPIC
# Compiling shared-object source
SYSCONF_CXXFLAGS_SHOBJ	= -fPIC
SYSCONF_CFLAGS_SHOBJ	= -fPIC
# Linking Qt
SYSCONF_LIBS_QTLIB	= $(SYSCONF_LFLAGS_X11) $(QT_LIBS_MT) $(QT_LIBS_OPT)
# Linking Qt applications
SYSCONF_LIBS_QTAPP	=
QTECONFIG

echo "Config created: $MUSL_CONFIG"

# --- Step 2: Create toolchain symlinks pointing to Buildroot ---
echo "--- Creating toolchain symlinks to Buildroot musl ---"
for tool in gcc g++ ar strip ld nm objcopy ranlib; do
    ln -sf "$TCBIN/mipsel-linux-$tool" /usr/local/bin/mipsel-linux-$tool
done
echo "mipsel-linux-gcc -> $(readlink -f /usr/local/bin/mipsel-linux-gcc)"
mipsel-linux-gcc --version | head -1

# --- Step 3: Install Opie's qconfig-qpe.h ---
echo "--- Installing qconfig-qpe.h ---"
cp $OPIEDIR/qt/qconfig-qpe.h $QTDIR/src/tools/

# --- Step 4: Clean and configure for musl MIPS cross-compile ---
echo "--- Deep-cleaning Qt/E build tree ---"
cd $QTDIR
make clean 2>/dev/null || true
# Remove ALL .o files to prevent stale glibc-linked objects from persisting
find src/ -name "*.o" -delete 2>/dev/null || true
rm -f lib/libqte* 2>/dev/null || true
echo "--- Configuring Qt/E for MIPS musl cross-compilation ---"

echo "yes" | ./configure \
    -qconfig qpe \
    -depths 16 \
    -xplatform linux-mips-musl-g++ \
    -no-xft \
    -no-qvfb \
    2>&1 | tail -20

# --- Step 5: Restore native moc ---
echo "--- Restoring native moc ---"
cp $BUILDDIR/native-tools/moc $QTDIR/bin/moc
chmod +x $QTDIR/bin/moc
echo "Native moc restored"

# --- Step 5a: Remove ALSA from link flags (not available in musl sysroot) ---
cd $QTDIR/src
sed -i 's/-lasound//' Makefile
echo "Removed -lasound from link flags"

# --- Step 5b: Patch Makefile to filter allmoc.h for moc compatibility ---
# The CXX -E preprocessor expands GCC 14 C++ headers containing
# 'inline namespace' and __attribute__ syntax that Qt 2.x moc can't parse.
# Insert a python filter step between preprocessing and moc invocation.
echo "--- Patching Makefile for moc filter ---"
cd $QTDIR/src
# Replace the allmoc generation: preprocess with CXX -E, then strip system header
# content that confuses moc, keeping Qt declarations intact
sed -i '/$(MOC) -o allmoc.cpp allmoc.h/i\\tpython3 /work/opie-build/moc_strip_syshdrs.py allmoc.h' Makefile
echo "Makefile patched for moc filter"

# --- Step 6: Build Qt/E ---
echo "--- Building Qt/Embedded for MIPS (musl) ---"
cd $QTDIR/src
make -j$(nproc) 2>&1 | tee $BUILDDIR/qte_musl_build.log | tail -50

BUILD_STATUS=${PIPESTATUS[0]}
if [ $BUILD_STATUS -eq 0 ]; then
    echo ""
    echo "=== Qt/Embedded (musl) build SUCCEEDED ==="
    ls -lh $QTDIR/lib/libqte* 2>/dev/null
    file $QTDIR/lib/libqte.so* 2>/dev/null | head -3
    # Verify MIPS-II and musl linkage
    echo "--- ELF flags ---"
    mipsel-linux-gnu-readelf -h $QTDIR/lib/libqte.so.2.3.10 | grep Flags
    echo "--- Dynamic dependencies ---"
    mipsel-linux-gnu-readelf -d $QTDIR/lib/libqte.so.2.3.10 | grep NEEDED
else
    echo ""
    echo "=== Qt/Embedded (musl) build FAILED (exit code $BUILD_STATUS) ==="
    echo "Last 30 error lines:"
    grep -E "error:|Error:|undefined reference" $BUILDDIR/qte_musl_build.log | tail -30
    echo ""
    echo "Full build log at: $BUILDDIR/qte_musl_build.log"
    exit 1
fi

# === Phase 3: Build hello app ===
echo ""
echo "=== Phase 3: Build hello app against musl ==="
cd $BUILDDIR

mipsel-linux-g++ -march=mips2 -o hello_musl \
    -I$QTDIR/include -L$QTDIR/lib \
    -Wl,-rpath,/opt/QtPalmtop/lib \
    -DQWS hello.cpp -lqte -lm

echo "--- hello_musl built ---"
file hello_musl
mipsel-linux-gnu-readelf -h hello_musl | grep Flags
mipsel-linux-gnu-readelf -d hello_musl | grep NEEDED
ls -lh hello_musl

echo ""
echo "=== Phase 2+3 COMPLETE ==="
