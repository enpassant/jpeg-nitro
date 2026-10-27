#!/bin/bash
# Downloads and builds the optional dependencies into third_party/:
#   - libjpeg-turbo 3.1.2 (static, with SIMD): reference for the tests and benchmarks,
#     optional fallback decoder for nitroview (make TURBOJPEG=1; macOS: x86_64 + arm64 in one library)
#   - nasm (needed to build libjpeg-turbo's SIMD code) and cmake (via pip), locally
#   - stb_image.h and wuffs (only for the decoder comparison in bench/bench)
#   - lunasvg (MIT, with its plutovg): SVG rendering in nitroview
#   - libcdr (our fork with fixes, github.com/gereoffy/libcdr) + librevenge (MPL-2.0),
#     lcms2 (MIT), the boost headers and (for Windows) zlib: CorelDRAW
#     drawings in nitroview (sources only; the Makefile compiles them)
# Nothing is installed system-wide.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p third_party && cd third_party
TP=$PWD
LJT_VER=3.1.2
NASM_VER=2.16.03
LUNASVG_COMMIT=90d6ff333862acf3f5551ffbbcb1e45957661881   # 2026-10-15 (also in packaging/flatpak/*.json, with its sha256)
LIBCDR_COMMIT=551bab4aef5b2541c88180b01070f9c4ec9f3539    # 2026-10-16, github.com/gereoffy/libcdr (and in packaging/flatpak/*.json)
LIBREVENGE_VER=0.0.6   # (these three: also in packaging/flatpak/*.json, with their sha256)
LCMS2_VER=2.19.1
ZLIB_VER=1.3.1      # (only for the Windows build: macOS and Linux have it)
MIMALLOC_VER=2.1.7
BOOST_VER=1.89.0
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8)

if [ ! -f stb_image.h ]; then
  curl -fsSL -o stb_image.h https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
fi
if [ ! -f wuffs.c ]; then
  curl -fsSL -o wuffs.c https://raw.githubusercontent.com/google/wuffs/main/release/c/wuffs-unsupported-snapshot.c
fi

# + patches/lunasvg.patch (fixes not upstream yet; re-applied when the patch changes)
LUNASVG_STAMP=$LUNASVG_COMMIT-$(cksum < ../patches/lunasvg.patch | cut -d' ' -f1)
if [ ! -f lunasvg/.stamp-$LUNASVG_STAMP ]; then
  rm -rf lunasvg lunasvg-$LUNASVG_COMMIT
  curl -fsSL -o lunasvg.tar.gz https://github.com/sammycage/lunasvg/archive/$LUNASVG_COMMIT.tar.gz
  tar xf lunasvg.tar.gz && rm lunasvg.tar.gz
  (cd lunasvg-$LUNASVG_COMMIT && patch -p1 -s < ../../patches/lunasvg.patch)
  mv lunasvg-$LUNASVG_COMMIT lunasvg
  touch lunasvg/.stamp-$LUNASVG_STAMP
fi

# CorelDRAW: libcdr from our fork (LibreOffice's libcdr with the fixes nitroview needs, one
# commit each: github.com/gereoffy/libcdr), librevenge + patches/librevenge.patch (re-applied
# when the patch changes), lcms2, and the boost headers (only boost/: libcdr and librevenge use
# header-only parts)
if [ ! -f libcdr/.stamp-$LIBCDR_COMMIT ]; then
  rm -rf libcdr libcdr-$LIBCDR_COMMIT
  curl -fsSL -o libcdr.tar.gz https://github.com/gereoffy/libcdr/archive/$LIBCDR_COMMIT.tar.gz
  tar xf libcdr.tar.gz && rm libcdr.tar.gz
  mv libcdr-$LIBCDR_COMMIT libcdr
  touch libcdr/.stamp-$LIBCDR_COMMIT
fi
LIBREVENGE_STAMP=$LIBREVENGE_VER-$(cksum < ../patches/librevenge.patch | cut -d' ' -f1)
if [ ! -f librevenge/.stamp-$LIBREVENGE_STAMP ]; then
  rm -rf librevenge librevenge-$LIBREVENGE_VER
  curl -fsSL -o librevenge.tar.bz2 https://dev-www.libreoffice.org/src/librevenge-$LIBREVENGE_VER.tar.bz2
  tar xf librevenge.tar.bz2 && rm librevenge.tar.bz2
  (cd librevenge-$LIBREVENGE_VER && patch -p1 -s < ../../patches/librevenge.patch)
  mv librevenge-$LIBREVENGE_VER librevenge
  touch librevenge/.stamp-$LIBREVENGE_STAMP
fi
if [ ! -f lcms2/.version-$LCMS2_VER ]; then
  rm -rf lcms2 lcms2-$LCMS2_VER
  curl -fsSL -o lcms2.tar.gz https://dev-www.libreoffice.org/src/lcms2-$LCMS2_VER.tar.gz
  tar xf lcms2.tar.gz && rm lcms2.tar.gz
  mv lcms2-$LCMS2_VER lcms2
  touch lcms2/.version-$LCMS2_VER
fi
if [ ! -f zlib/.version-$ZLIB_VER ]; then
  rm -rf zlib zlib-$ZLIB_VER
  curl -fsSL -o zlib.tar.gz https://github.com/madler/zlib/releases/download/v$ZLIB_VER/zlib-$ZLIB_VER.tar.gz
  tar xf zlib.tar.gz && rm zlib.tar.gz
  mv zlib-$ZLIB_VER zlib
  touch zlib/.version-$ZLIB_VER
fi
# mimalloc: the allocator of the SVG / CorelDRAW code (C++ new / delete)
if [ ! -f mimalloc/.version-$MIMALLOC_VER ]; then
  rm -rf mimalloc mimalloc-$MIMALLOC_VER
  curl -fsSL -o mimalloc.tar.gz https://github.com/microsoft/mimalloc/archive/refs/tags/v$MIMALLOC_VER.tar.gz
  tar xf mimalloc.tar.gz && rm mimalloc.tar.gz
  mv mimalloc-$MIMALLOC_VER mimalloc
  touch mimalloc/.version-$MIMALLOC_VER
fi
if [ ! -f boost/.version-$BOOST_VER ]; then
  B=boost_${BOOST_VER//./_}
  rm -rf boost $B
  curl -fsSL -o boost.tar.bz2 https://archives.boost.io/release/$BOOST_VER/source/$B.tar.bz2
  tar xf boost.tar.bz2 $B/boost && rm boost.tar.bz2
  mkdir boost && mv $B/boost boost/boost && rm -rf $B
  touch boost/.version-$BOOST_VER
fi

if [ ! -x tools/nasm/bin/nasm ]; then
  curl -fsSL -o nasm.tar.xz https://www.nasm.us/pub/nasm/releasebuilds/$NASM_VER/nasm-$NASM_VER.tar.xz
  tar xf nasm.tar.xz
  (cd nasm-$NASM_VER && ./configure -q --prefix="$TP/tools/nasm" && make -j"$JOBS" -s && make install -s)
fi

CMAKE=$(command -v cmake || true)
if [ -z "$CMAKE" ]; then
  [ -x tools/py/bin/cmake ] || python3 -m pip install -q --target tools/py cmake
  CMAKE=$(find "$TP/tools/py" -path '*data/bin/cmake' -type f | head -1)
fi

# libjpeg-turbo for one architecture into $TP/$2 (static, with its SIMD code)
build_ljt() {   # arch prefix
  local A=$1 P=$2 CPU PROC
  case $A in
    x86_64) PROC=x86_64; CPU=$([ "$(uname -s)" = Darwin ] && echo "-march=x86-64-v3" || echo "-march=native") ;;
    arm64|aarch64) PROC=aarch64; CPU=$([ "$(uname -s)" = Darwin ] && echo "-mcpu=apple-m1" || echo "-mcpu=native") ;;
    *) PROC=$A; CPU="" ;;
  esac
  local MAC=()
  [ "$(uname -s)" = Darwin ] && MAC=(-DCMAKE_OSX_ARCHITECTURES="$A" -DCMAKE_SYSTEM_PROCESSOR="$PROC" -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0)
  [ -d libjpeg-turbo-$LJT_VER ] || {
    curl -fsSL -o ljt.tar.gz https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$LJT_VER/libjpeg-turbo-$LJT_VER.tar.gz
    tar xf ljt.tar.gz
  }
  (cd libjpeg-turbo-$LJT_VER &&
   "$CMAKE" -S . -B build-$A -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF "${MAC[@]}" \
     -DCMAKE_ASM_NASM_COMPILER="$TP/tools/nasm/bin/nasm" -DCMAKE_INSTALL_PREFIX="$TP/$P" -DCMAKE_INSTALL_LIBDIR=lib \
     -DCMAKE_C_FLAGS="-O3 $CPU" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null &&
   "$CMAKE" --build build-$A -j"$JOBS" >/dev/null &&
   "$CMAKE" --install build-$A >/dev/null)
}

if [ "$(uname -s)" = Darwin ]; then
  # macOS: both architectures in one (universal) library, for the universal nitroview.app
  if [ ! -f ljt/.universal-macos12 ]; then   # (older versions built only the host architecture)
    build_ljt x86_64 ljt-x86_64
    build_ljt arm64 ljt-arm64
    rm -rf ljt && cp -R ljt-x86_64 ljt
    for L in libturbojpeg.a libjpeg.a; do
      lipo -create ljt-x86_64/lib/$L ljt-arm64/lib/$L -output ljt/lib/$L
    done
    touch ljt/.universal-macos12
  fi
elif [ ! -f ljt/lib/libturbojpeg.a ]; then
  build_ljt "$(uname -m)" ljt
fi
echo "dependencies ready in third_party/ (libjpeg-turbo $LJT_VER, stb_image, wuffs, lunasvg, libcdr)"
