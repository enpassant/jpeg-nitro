# jpeg-nitro: nitroview (fast image viewer for macOS) + nitrojpeg (parallel JPEG decoder)
#
#   make                 nitroview; JPEGs nitrojpeg can't decode (damaged, CMYK ...) go to ImageIO
#   make TURBOJPEG=1     with libjpeg-turbo (third_party/ljt, see scripts/get-deps.sh) as their
#                        decoder before ImageIO, and --selftest
#   make DAV1D=0         no dav1d: AVIF is decoded by ImageIO (with dav1d - scripts/get-deps.sh - nitroavif is ~3x faster)
#   make NITROHEVC=1     with nitrohevc, our own HEVC decoder, for HEIC photos. Off by default: HEVC is
#                        covered by patents that need a licence, which the binaries we distribute don't
#                        have. Without it HEIC goes to the system: VideoToolbox on macOS (nitroheic),
#                        WIC on Windows (Microsoft's HEVC extension), GTK's loaders in the GNOME viewer;
#                        the SDL viewer has none. With it every viewer uses nitrohevc (macOS too,
#                        VideoToolbox left out)
#   make WUFFS=1         with Wuffs (third_party/wuffs.c) for interlaced PNGs before ImageIO (nitropng
#                        decodes all others)
#   make SVG=0           no SVG support (lunasvg, from scripts/get-deps.sh)
#   make CDR=0           no CorelDRAW support (libcdr, from scripts/get-deps.sh; needs SVG)
#   make MIMALLOC=0      SVG / CorelDRAW code with the system malloc, not mimalloc
#   make app             nitroview.app bundle, so the Finder can open images with it: universal
#                        (x86_64 + arm64, macOS 12+); MAC_ARCHS=x86_64 for one architecture only
#   make zip / make dmg  nitroview.zip / nitroview.dmg (drag to Applications) of the app
#   make dmg notarize SIGN_ID="Developer ID Application: Name (TEAMID)" NOTARY_PROFILE=name
#                        signed with a Developer ID and notarized by Apple: opens without a
#                        Gatekeeper warning (once: xcrun notarytool store-credentials name ...)
#   make tools           benchmarks and tests (need scripts/get-deps.sh first)
#   make lib             build/libnitro.a: the decoders (JPEG, PNG, PSD, HEIC, DNG, WebP, TIFF; AVIF with
#                        dav1d) and nitro_load (file -> decoded image) as one static library. The viewers
#                        link it (built again per target: the app's architectures, Windows, the portable
#                        Linux build); any other program can too, with nitro_os.h's blocks runtime
#   make examples/nitroconvert
#                        the example program on the library: any image it decodes -> a 24-bit BMP
#   make linux-lib       Linux: build/lib/libnitro.so (exports: src/libnitro.map) and a PIC libnitro.a
#   make gnome           GNOME/libadwaita viewer: nitroview-gnome (the decoders linked in statically)
#   make sdl             SDL3 + OpenGL viewer for Linux: nitroview-sdl (SDL3 from scripts/get-deps.sh;
#                        builds on macOS too, for testing)
#   make sdl-dist        nitroview-linux-x64.tar.gz: it as one portable file (x86-64-v3, no libstdc++);
#                        on 64-bit ARM (e.g. Asahi Linux on Apple M1 / M2): nitroview-linux-arm64.tar.gz (armv8-a)
#   make flatpak         GNOME 50 Flatpak bundle: nitroview-gnome.flatpak
#   make linux           Linux (also the default there): library + portable tools - nbench,
#                        pngverify, psdverify, the ASan robustness tests, and verify / robust
#                        once scripts/get-deps.sh has built libjpeg-turbo. Needs clang.
#   make windows         cross-compile nitroview.exe (the viewer), bench/nbench.exe and
#                        bench/psdverify.exe for Windows (x64, static, AVX2) with llvm-mingw:
#                        WINCC=path/to/x86_64-w64-mingw32-clang (AVIF with dav1d once
#                        WINCC=... scripts/get-deps.sh built it into third_party/dav1d-win)

LJT        := third_party/ljt
TURBOJPEG  ?= 0
CC         := clang
AR         ?= ar
ARCH       := $(shell uname -m)
# Skylake-family Intel CPUs lose their decoded-uop cache for code where a jump crosses or ends
# at a 32-byte boundary (JCC erratum microcode): the Huffman loops became up to 25% slower
# after edits elsewhere on a Xeon E3-1245 v5. This keeps branches clear; no effect elsewhere.
X86JCC     := -mbranches-within-32B-boundaries
ifeq ($(ARCH),x86_64)
CPUFLAGS   := -march=native $(X86JCC)
else
CPUFLAGS   := -mcpu=native
endif
# extra compiler flags for every build (decoders, viewers, benchmarks), e.g. make -B DEFS=-DLOOK=13
DEFS       ?=
CPUFLAGS   += $(DEFS)
UNAME_S    ?= $(shell uname -s)
COMMA      := ,
ifeq ($(UNAME_S),Darwin)
PORTFLAGS  :=
THREADLIBS :=
else   # the decoders' loop bodies are blocks (see src/nitro_os.h): clang -fblocks
PORTFLAGS  := -fblocks
THREADLIBS := -lpthread
endif
CFLAGS     := -O3 $(CPUFLAGS) $(PORTFLAGS) -Wall -Wextra -Wno-unused-parameter
ASAN       := -O1 -g $(PORTFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer
# VideoToolbox: nitroheic's HEVC decoding (left out with nitrohevc: make NITROHEVC=1)
VT_FW      := $(if $(filter 1,$(NITROHEVC)),,-framework VideoToolbox -framework CoreMedia -framework CoreVideo)
FRAMEWORKS := -framework Cocoa -framework Metal -framework MetalKit -framework QuartzCore -framework UniformTypeIdentifiers \
              $(VT_FW)
# libnitro: the decoders and nitro_load as one static library, built per target into <dir>/libnitro.a
# (from <dir>/libobj/): build/ for this machine, build/app-<arch>/ for the app, build/dist/ for the
# portable Linux build, build/win/ for Windows, build/lib/ as PIC for the Linux shared library. With
# dav1d (DAV1D=1) nitroavif is in it, compiled with -DNV_DAV1D; libdav1d.a is linked beside it.
NITROHEVC  ?= 0
LIB_SRC    := src/nitrojpeg.c src/nitropng.c src/nitropsd.c src/nitroheic.c src/nitrodng.c src/nitrowebp.c \
              src/nitrotiff.c src/nitro_load.c src/nitro_convert.c $(if $(filter 1,$(NITROHEVC)),src/nitrohevc.c)
LIB_HDR    := src/nitrojpeg.h src/nitropng.h src/nitropsd.h src/nitroheic.h src/nitrohevc.h src/nitrodng.h src/nitrowebp.h \
              src/nitrotiff.h src/nitroavif.h src/nitro_load.h src/nitro_convert.h src/nitro_os.h src/heif.h src/tiff.h
LIB_AVIF    = $(if $(filter 1,$(DAV1D)),src/nitroavif.c)
LIB_DEFS    = $(if $(filter 1,$(DAV1D)),-DNV_DAV1D -I$(DAV1D_DIR)/include)
HEVC_DEFS  := $(if $(filter 1,$(NITROHEVC)),-DNV_NITROHEVC)
HEVC_NAME  := $(if $(filter 1,$(NITROHEVC)),nitrohevc,$(if $(filter Darwin,$(UNAME_S)),VideoToolbox,the system's))
LIB_WARN   := -Wall -Wextra -Wno-unused-parameter
define nitro_lib   # $(1): the compiler flags (optimisation, CPU, -fblocks, -fPIC ...)
	@rm -rf $(@D)/libobj && mkdir -p $(@D)/libobj
	@echo "compiling libnitro into $@"
	@printf '%s\n' $(LIB_SRC) $(LIB_AVIF) | xargs -n1 -P$(JOBS) sh -c '$(CC) $(1) $(LIB_WARN) $(LIB_DEFS) $(HEVC_DEFS) -Isrc -c "$$0" -o $(@D)/libobj/$$(basename "$$0" .c).o'
	@rm -f $@ && $(AR) rcs $@ $(@D)/libobj/*.o
endef
# what the four viewers share beside the library (compiled into each)
VIEWER_SRC := src/nitro_prefetch.c
VIEWER_HDR := src/nitro_prefetch.h src/nitro_view.h
LIBDIR     := build/lib
LIB_SHARED := $(LIBDIR)/libnitro.so
LIB_STATIC := $(LIBDIR)/libnitro.a

# GNOME frontend: GtkBuilder template + libadwaita, with the decoders from the static library
# (one self-contained binary: nothing to find at run time).
GNOME_PKGS     := gtk4 libadwaita-1
# (= not :=: pkg-config runs only when the GNOME viewer is built, not on every make)
GNOME_CFLAGS    = $(shell pkg-config --cflags $(GNOME_PKGS) 2>/dev/null)
GNOME_LIBS      = $(shell pkg-config --libs $(GNOME_PKGS) 2>/dev/null)
GNOME_DIR      := src/gnome
GNOME_RES      := build/gnome/nitroview-resources.c
GNOME_SRC      := $(GNOME_DIR)/main.c $(GNOME_DIR)/nitro-window.c $(GNOME_DIR)/nitro-view.c $(GNOME_DIR)/nitro-image.c \
                  $(GNOME_DIR)/nitro-loader.c $(VIEWER_SRC) $(GNOME_RES)
GNOME_HDR      := $(GNOME_DIR)/nitro-window.h $(GNOME_DIR)/nitro-view.h $(GNOME_DIR)/nitro-image.h $(GNOME_DIR)/nitro-loader.h \
                  $(LIB_HDR) $(VIEWER_HDR)
GNOME_BIN      := nitroview-gnome
GNOME_APP_ID   := com.github.gereoffy.nitroview
GNOME_DESKTOP  := build/gnome/$(GNOME_APP_ID).desktop
GNOME_DESKTOP_SOURCE := packaging/gnome/$(GNOME_APP_ID).desktop
GNOME_USER_DATA := $(HOME)/.local/share

FLATPAK_MANIFEST := packaging/flatpak/$(GNOME_APP_ID).json
FLATPAK_BUILD    := build/flatpak
FLATPAK_REPO     := build/flatpak-repo
FLATPAK_BUNDLE   := nitroview-gnome.flatpak

WUFFS      ?= 0
LUNA       := third_party/lunasvg
SVG        ?= $(if $(wildcard $(LUNA)/include/lunasvg.h),1,0)
LIBCDR     := third_party/libcdr
LIBREVENGE := third_party/librevenge
LCMS       := third_party/lcms2
BOOST      := third_party/boost
# zlib for CorelDRAW in the Windows and the portable Linux build (macOS / Linux have it)
ZLIB       := third_party/zlib
# dav1d (VideoLAN, BSD-2-Clause) for AVIF in the Linux viewers (nitroavif), once scripts/get-deps.sh built it
DAV1D_DIR  := third_party/dav1d
DAV1D      ?= $(if $(wildcard $(DAV1D_DIR)/lib/libdav1d.a),1,0)
DAV1D_LIB  := $(DAV1D_DIR)/lib/libdav1d.a
CDR        ?= $(if $(and $(filter 1,$(SVG)),$(wildcard $(LIBCDR)/inc/libcdr/libcdr.h),$(wildcard $(LIBREVENGE)/inc/librevenge/librevenge.h),$(wildcard $(LCMS)/include/lcms2.h),$(wildcard $(BOOST)/boost/optional.hpp)),1,0)
MI         := third_party/mimalloc
MIMALLOC   ?= $(if $(and $(filter 1,$(SVG)),$(wildcard $(MI)/include/mimalloc-new-delete.h)),1,0)
CXX        := clang++
JOBS       := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8)

ifeq ($(TURBOJPEG),1)
VIEWER_DEFS += -DNV_TURBOJPEG -I$(LJT)/include
VIEWER_LIBS += $(LJT)/lib/libturbojpeg.a -Wl,-w
endif
ifeq ($(WUFFS),1)
VIEWER_DEFS += -DNV_WUFFS
VIEWER_OBJS += build/png_wuffs.o
endif
ifeq ($(CDR),1)   # (before SVG: it uses the plutovg in libnitrosvg.a)
VIEWER_DEFS += -DNV_CDR
VIEWER_OBJS += build/libnitrocdr.a
VIEWER_LIBS += -liconv -lz
endif
ifeq ($(SVG),1)
VIEWER_DEFS += -DNV_SVG
VIEWER_OBJS += build/libnitrosvg.a
VIEWER_LIBS += -lc++
endif
ifeq ($(DAV1D),1)   # AVIF: nitroavif (in libnitro) with dav1d
VIEWER_DEFS += -DNV_DAV1D -I$(DAV1D_DIR)/include
VIEWER_LIBS += $(DAV1D_LIB)
endif
ifeq ($(MIMALLOC),1)   # C++ new / delete (the SVG / CorelDRAW code) through mimalloc
VIEWER_OBJS += build/mimalloc.o build/nitroalloc.o
endif

REF_CFLAGS := $(CFLAGS) -DNJ_REFERENCE -I$(LJT)/include
LDQUIET    := $(if $(filter Darwin,$(UNAME_S)),-Wl$(COMMA)-w)   # Apple ld: no duplicate-library warnings
REF_LIBS   := $(LJT)/lib/libturbojpeg.a $(LDQUIET)

ifeq ($(UNAME_S),Darwin)
all: nitroview
else
all: linux
endif

# a failed recipe leaves no half-written target behind (e.g. an empty build/icon.inc without xxd)
.DELETE_ON_ERROR:

# rebuild the viewer when the optional parts change
CONFIG := tj$(TURBOJPEG)-wuffs$(WUFFS)-svg$(SVG)-cdr$(CDR)-mi$(MIMALLOC)-dav1d$(DAV1D)-hevc$(NITROHEVC)
build/.config-$(CONFIG):
	@mkdir -p build
	@rm -f build/.config-* nitroview
	@touch $@

# the library for this machine (the viewers, the tools)
build/libnitro.a: $(LIB_SRC) $(LIB_AVIF) $(LIB_HDR) build/.config-$(CONFIG)
	$(call nitro_lib,$(CFLAGS))
lib: build/libnitro.a
	@echo "built build/libnitro.a (AVIF: $(if $(filter 1,$(DAV1D)),dav1d,no), HEIC: $(HEVC_NAME))"

# reference build of nitrojpeg: + old libjpeg-turbo engine and BGRX output (tests, benchmarks)
build/nitrojpeg_ref.o: src/nitrojpeg.c src/nitrojpeg.h src/nitro_os.h src/tiff.h | ljt
	@mkdir -p build
	$(CC) $(REF_CFLAGS) -c $< -o $@

# Linux: the shared library (its exports: src/libnitro.map) and a PIC static one (the GNOME and
# SDL viewers link that)
$(LIB_STATIC): $(LIB_SRC) $(LIB_AVIF) $(LIB_HDR) build/.config-$(CONFIG)
	$(call nitro_lib,$(CFLAGS) -fPIC)
$(LIB_SHARED): $(LIB_STATIC) src/libnitro.map
	$(CC) -shared -Wl,-soname,libnitro.so -Wl,--version-script=src/libnitro.map -o $@ $(LIBDIR)/libobj/*.o \
	  $(if $(filter 1,$(DAV1D)),$(DAV1D_LIB)) $(THREADLIBS)

linux-lib: $(LIB_SHARED) $(LIB_STATIC)
	@echo "built $(LIB_SHARED) $(LIB_STATIC) (AVIF: $(if $(filter 1,$(DAV1D)),dav1d,no), HEIC: $(HEVC_NAME))"

$(GNOME_RES): $(GNOME_DIR)/nitroview.gresource.xml $(GNOME_DIR)/nitro-window.ui packaging/icon.png
	@mkdir -p $(@D)
	glib-compile-resources $< --sourcedir=$(GNOME_DIR) --sourcedir=. --target=$@ --generate-source

# SVG in the GNOME viewer: the same nitrosvg (lunasvg) as on macOS / Windows, as PIC objects
GNOME_SVG := $(if $(filter 1,$(SVG)),$(LIBDIR)/libnitrosvg.a)
$(LIBDIR)/libnitrosvg.a: src/nitrosvg.cpp src/nitrosvg.h $(SVG_STAMP)
	$(call svg_lib,$(CPUFLAGS) -fPIC)
# ... and CorelDRAW: nitrocdr (libcdr + librevenge + lcms2, drawn with the plutovg of nitrosvg)
GNOME_CDR := $(if $(and $(GNOME_SVG),$(filter 1,$(CDR))),$(LIBDIR)/libnitrocdr.a)
$(LIBDIR)/libnitrocdr.a: src/nitrocdr.cpp src/nitrocdr.h $(CDR_STAMP)
	$(call cdr_lib,$(CPUFLAGS) -fPIC)

GNOME_AVIF := $(if $(filter 1,$(DAV1D)),$(DAV1D_LIB))
$(GNOME_BIN): $(GNOME_SRC) $(GNOME_HDR) $(LIB_STATIC) $(GNOME_SVG) $(GNOME_CDR) $(GNOME_AVIF)
	@pkg-config --exists $(GNOME_PKGS) || { echo "missing GNOME development packages: gtk4 libadwaita-1"; exit 1; }
	@pkg-config --atleast-version=4.6 gtk4 || { echo "GNOME frontend requires GTK 4.6 or newer"; exit 1; }
	@pkg-config --atleast-version=1.1 libadwaita-1 || { echo "GNOME frontend requires libadwaita 1.1 or newer"; exit 1; }
	$(CC) -std=gnu11 -O2 $(CPUFLAGS) $(PORTFLAGS) -Wall -Wextra -Wno-unused-parameter -Isrc -I$(GNOME_DIR) $(GNOME_CFLAGS) \
	  $(if $(GNOME_SVG),-DNV_SVG) $(if $(GNOME_CDR),-DNV_CDR) $(if $(GNOME_AVIF),-DNV_DAV1D) $(GNOME_SRC) $(GNOME_CDR) $(GNOME_SVG) \
	  $(LIB_STATIC) $(GNOME_AVIF) \
	  $(GNOME_LIBS) $(if $(GNOME_CDR),-lz) $(if $(GNOME_SVG),-lstdc++) $(THREADLIBS) -lm -o $@

# GNOME viewer: command-line open plus a native Open/Quit application shell.
gnome: $(GNOME_BIN)
	@echo "built $(GNOME_BIN) (SVG: $(if $(GNOME_SVG),nitrosvg,GTK's loaders), CorelDRAW: $(if $(GNOME_CDR),yes,no), AVIF: $(if $(GNOME_AVIF),dav1d,GTK's loaders), HEIC: $(if $(filter 1,$(NITROHEVC)),nitrohevc,GTK's loaders))"

# SDL3 viewer (Linux; builds on macOS too, for testing): SDL3 and the decoders linked in
# statically, OpenGL and the window system (Wayland / X11) loaded at run time
SDL3_DIR  := third_party/sdl3
SDL_BIN   := nitroview-sdl
SDL_LIBS   = $(SDL3_DIR)/lib/libSDL3.a $(shell sed -n 's/^Libs:.*-lSDL3//p' $(SDL3_DIR)/lib/pkgconfig/sdl3.pc 2>/dev/null)
ifeq ($(UNAME_S),Darwin)
SDL_DEC   := build/libnitro.a
SDL_VEC   := $(if $(filter 1,$(CDR)),build/libnitrocdr.a) $(if $(filter 1,$(SVG)),build/libnitrosvg.a)
SDL_VLIBS := $(if $(filter 1,$(CDR)),-liconv -lz) $(if $(filter 1,$(SVG)),-lc++) -framework CoreFoundation $(VT_FW)   # (HEIC)
else
SDL_DEC   := $(LIB_STATIC)
SDL_VEC   := $(GNOME_CDR) $(GNOME_SVG) $(if $(and $(GNOME_SVG),$(filter 1,$(MIMALLOC))),build/mimalloc.o build/nitroalloc.o)
SDL_VLIBS := $(if $(GNOME_CDR),-lz) $(if $(GNOME_SVG),-lstdc++)
endif
SDL_AVIF  := $(if $(filter 1,$(DAV1D)),$(DAV1D_LIB))
SDL_VDEFS  = $(if $(filter %libnitrosvg.a,$(SDL_VEC)),-DNV_SVG) $(if $(filter %libnitrocdr.a,$(SDL_VEC)),-DNV_CDR) $(if $(SDL_AVIF),-DNV_DAV1D)

# the window icon, embedded as a byte array
build/icon.inc: packaging/icon.png
	@mkdir -p build
	cd packaging && xxd -i icon.png > ../build/icon.inc

$(SDL_BIN): src/nitroview_sdl.c $(VIEWER_SRC) $(VIEWER_HDR) build/icon.inc $(SDL_DEC) $(SDL_VEC) $(SDL_AVIF) $(LIB_HDR) src/nitrosvg.h src/nitrocdr.h
	@test -f $(SDL3_DIR)/lib/libSDL3.a -a -f third_party/stb_image.h || { echo "missing SDL3 / stb_image: run scripts/get-deps.sh first"; exit 1; }
	$(CC) $(CFLAGS) $(SDL_VDEFS) -Isrc -Ibuild -Ithird_party -I$(SDL3_DIR)/include src/nitroview_sdl.c $(VIEWER_SRC) $(SDL_DEC) $(SDL_AVIF) \
	  $(SDL_VEC) $(SDL_LIBS) $(SDL_VLIBS) $(THREADLIBS) -lm -o $@

sdl: $(SDL_BIN)
	@echo "built $(SDL_BIN) (SVG: $(if $(filter -DNV_SVG,$(SDL_VDEFS)),yes,no), CorelDRAW: $(if $(filter -DNV_CDR,$(SDL_VDEFS)),yes,no), AVIF: $(if $(SDL_AVIF),dav1d,no), HEIC: $(if $(filter 1,$(NITROHEVC)),nitrohevc,$(if $(filter Darwin,$(UNAME_S)),VideoToolbox,no)))"

# Portable Linux build of it (make sdl-dist): x86-64-v3 (AVX2; DIST_ARCH=x86-64-v2 for older CPUs)
# instead of -march=native, libstdc++ / libgcc and zlib linked in. One file that needs only the build
# system's glibc (or newer) and, at run time, OpenGL and Wayland / X11; --register adds it to the
# desktop. nitroview-linux-x64.tar.gz: just that file (keeping its execute permission). Built on
# 64-bit ARM: armv8-a (any of them: Apple M1 / M2 under Asahi Linux, Raspberry Pi 4 / 5, ARM servers;
# NEON is part of it), nitroview-linux-arm64.tar.gz.
DIST_DIR   := build/dist
ifeq ($(ARCH),aarch64)
DIST_ARCH  ?= armv8-a
DIST_CPU    = -march=$(DIST_ARCH)
DIST_TGZ   := nitroview-linux-arm64.tar.gz
else
DIST_ARCH  ?= x86-64-v3
DIST_CPU    = -march=$(DIST_ARCH) $(X86JCC)
DIST_TGZ   := nitroview-linux-x64.tar.gz
endif
DIST_VEC   := $(if $(filter 1,$(CDR)),$(DIST_DIR)/libnitrocdr.a) $(if $(filter 1,$(SVG)),$(DIST_DIR)/libnitrosvg.a)
# their new / delete through mimalloc, as on macOS (CorelDRAW on the M1 under Linux: ~10% faster than glibc's malloc)
DIST_MI    := $(if $(and $(DIST_VEC),$(filter 1,$(MIMALLOC))),$(DIST_DIR)/mimalloc.o $(DIST_DIR)/nitroalloc.o)
$(DIST_DIR)/mimalloc.o $(DIST_DIR)/nitroalloc.o: src/nitroalloc.cpp $(wildcard $(MI)/.version-*)
	$(call mimalloc_objs,-march=$(DIST_ARCH))
$(DIST_DIR)/libnitrosvg.a: src/nitrosvg.cpp src/nitrosvg.h $(SVG_STAMP)
	$(call svg_lib,-march=$(DIST_ARCH))
$(DIST_DIR)/libnitrocdr.a: CDR_C += $(addprefix $(ZLIB)/,adler32.c crc32.c inflate.c inffast.c inftrees.c zutil.c)
$(DIST_DIR)/libnitrocdr.a: CDR_DEFS += -I$(ZLIB)
$(DIST_DIR)/libnitrocdr.a: src/nitrocdr.cpp src/nitrocdr.h $(CDR_STAMP)
	$(call cdr_lib,-march=$(DIST_ARCH) -I$(ZLIB))
$(DIST_DIR)/libnitro.a: $(LIB_SRC) $(LIB_AVIF) $(LIB_HDR) build/.config-$(CONFIG)
	$(call nitro_lib,-O3 $(DIST_CPU) $(DEFS) -fblocks)
$(DIST_DIR)/nitroview-sdl: src/nitroview_sdl.c $(VIEWER_SRC) $(VIEWER_HDR) build/icon.inc $(DIST_DIR)/libnitro.a $(LIB_HDR) src/nitrosvg.h \
                           src/nitrocdr.h $(DIST_VEC) $(DIST_MI)
	@test -f $(SDL3_DIR)/lib/libSDL3.a -a -f third_party/stb_image.h || { echo "missing SDL3 / stb_image: run scripts/get-deps.sh first"; exit 1; }
	@mkdir -p $(@D)
	$(CC) -O3 $(DIST_CPU) $(DEFS) -fblocks -Wall -Wextra -Wno-unused-parameter \
	  $(if $(filter %libnitrosvg.a,$(DIST_VEC)),-DNV_SVG) $(if $(filter %libnitrocdr.a,$(DIST_VEC)),-DNV_CDR) \
	  $(if $(filter 1,$(DAV1D)),-DNV_DAV1D -I$(DAV1D_DIR)/include) \
	  -Isrc -Ibuild -Ithird_party -I$(SDL3_DIR)/include src/nitroview_sdl.c $(VIEWER_SRC) $(DIST_DIR)/libnitro.a \
	  $(if $(filter 1,$(DAV1D)),$(DAV1D_LIB)) $(DIST_VEC) $(DIST_MI) $(SDL_LIBS) $(if $(DIST_VEC),-Wl$(COMMA)-Bstatic -lstdc++ -Wl$(COMMA)-Bdynamic) -static-libgcc \
	  -lpthread -lm -o $@
	@echo "$@ needs: $$(objdump -p $@ | sed -n 's/ *NEEDED *//p' | tr '\n' ' ')(glibc $$(objdump -T $@ | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 | cut -d_ -f2))"
$(DIST_TGZ): $(DIST_DIR)/nitroview-sdl
	tar -C $(DIST_DIR) -czf $@ nitroview-sdl
sdl-dist: $(DIST_TGZ)
	@echo "built $(DIST_TGZ) (HEIC: $(if $(filter 1,$(NITROHEVC)),nitrohevc,none: make NITROHEVC=1))"

# Development desktop registration for GNOME Shell. The generated desktop entry uses
# the absolute path of this source-tree binary so it also works without installing it.
$(GNOME_DESKTOP): $(GNOME_DESKTOP_SOURCE) $(GNOME_BIN)
	@mkdir -p $(@D)
	sed 's|^Exec=.*|Exec=$(abspath $(GNOME_BIN)) %F|' $< > $@

# Register the development build for the current user so GNOME Shell can resolve the
# Wayland application id to the matching desktop entry and application icon.
gnome-register: $(GNOME_DESKTOP) packaging/icon.png
	install -Dm644 $(GNOME_DESKTOP) $(GNOME_USER_DATA)/applications/$(GNOME_APP_ID).desktop
	install -Dm644 packaging/icon.png $(GNOME_USER_DATA)/icons/hicolor/512x512/apps/$(GNOME_APP_ID).png
	@command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database $(GNOME_USER_DATA)/applications || true
	@command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -f -t $(GNOME_USER_DATA)/icons/hicolor >/dev/null 2>&1 || true
	@echo "registered $(GNOME_APP_ID) for the current user"

# Self-contained Flatpak bundle built against the GNOME 50 runtime.
# flatpak-builder: Flathub's org.flatpak.Builder if installed (older distributions' own, e.g. Ubuntu
# 22.04's 1.2, still call appstream-compose, which the GNOME 50 SDK no longer has)
FLATPAK_BUILDER ?= $(if $(shell flatpak info org.flatpak.Builder >/dev/null 2>&1 && echo y),flatpak run --filesystem=$(CURDIR) org.flatpak.Builder,flatpak-builder)
flatpak: $(FLATPAK_MANIFEST) packaging/flatpak/$(GNOME_APP_ID).metainfo.xml $(GNOME_DESKTOP_SOURCE) packaging/icon.png
	@command -v flatpak >/dev/null 2>&1 || { echo "missing flatpak"; exit 1; }
	@case "$(FLATPAK_BUILDER)" in flatpak\ run*) ;; *) command -v flatpak-builder >/dev/null 2>&1 || { echo "missing flatpak-builder"; exit 1; } ;; esac
	rm -rf $(FLATPAK_BUILD) $(FLATPAK_REPO) $(FLATPAK_BUNDLE)
	@if flatpak remote-list --user --columns=name | grep -qx flathub; then \
		scope=--user; \
	elif flatpak remote-list --system --columns=name | grep -qx flathub; then \
		scope=--system; \
	else \
		echo "missing Flathub remote; add it with:"; \
		echo "  flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo"; \
		exit 1; \
	fi; \
	$(FLATPAK_BUILDER) $$scope --install-deps-from=flathub --force-clean --repo=$(CURDIR)/$(FLATPAK_REPO) $(CURDIR)/$(FLATPAK_BUILD) \
	  $(CURDIR)/$(FLATPAK_MANIFEST)
	flatpak build-bundle --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo $(FLATPAK_REPO) $(FLATPAK_BUNDLE) \
	  $(GNOME_APP_ID) stable   # (installing it fetches the GNOME runtime from Flathub if needed)
	@echo "built $(FLATPAK_BUNDLE)"

# optional Wuffs PNG decoder (third_party/wuffs.c, Apache-2.0), compiled once
build/png_wuffs.o: src/png_wuffs.c src/png_wuffs.h
	@mkdir -p build
	$(CC) -O3 $(CPUFLAGS) -w -c $< -o $@

# optional SVG rendering: lunasvg + plutovg (third_party/lunasvg, MIT) + src/nitrosvg.cpp,
# one static library, compiled once (in parallel); $(1): compiler flags (architecture, CPU)
SVG_C    = $(wildcard $(LUNA)/plutovg/source/*.c)
SVG_CXX  = $(wildcard $(LUNA)/source/*.cpp) src/nitrosvg.cpp
SVG_DEFS := -DLUNASVG_BUILD -DLUNASVG_BUILD_STATIC -DPLUTOVG_BUILD -DPLUTOVG_BUILD_STATIC \
            -DLUNASVG_DISABLE_EXTERNAL_RESOURCES -I$(LUNA)/include -I$(LUNA)/source -I$(LUNA)/plutovg/include \
            -I$(LUNA)/plutovg/source
define svg_lib
	@rm -rf $(@D)/svgobj && mkdir -p $(@D)/svgobj
	@echo "compiling lunasvg + plutovg into $@"
	@printf '%s\n' $(SVG_C) | xargs -n1 -P$(JOBS) sh -c '$(CC) $(1) -O3 -w $(SVG_DEFS) -c "$$0" -o $(@D)/svgobj/$$(basename "$$0").o'
	@printf '%s\n' $(SVG_CXX) | xargs -n1 -P$(JOBS) sh -c '$(CXX) -std=c++17 -fno-exceptions -fno-rtti $(1) -O3 -w $(SVG_DEFS) -c "$$0" -o $(@D)/svgobj/$$(basename "$$0").o'
	@rm -f $@ && $(AR) rcs $@ $(@D)/svgobj/*.o
endef
SVG_STAMP = $(wildcard $(LUNA)/.stamp-*)   # changes with the lunasvg commit / patches/lunasvg.patch
build/libnitrosvg.a: src/nitrosvg.cpp src/nitrosvg.h $(SVG_STAMP)
	$(call svg_lib,$(CPUFLAGS))

# optional CorelDRAW rendering: libcdr + librevenge (its core and streams; the SVG generator is
# part of the core: libcdr renders full colour pattern fills with it) +
# lcms2 + src/nitrocdr.cpp in one static library, compiled once (in parallel); drawn with
# the plutovg of libnitrosvg.a. $(1): compiler flags (architecture, CPU)
CDR_C    = $(wildcard $(LCMS)/src/*.c)
CDR_CXX  = $(wildcard $(LIBCDR)/src/lib/*.cpp) src/nitrocdr.cpp \
           $(addprefix $(LIBREVENGE)/src/lib/,RVNGBinaryData.cpp RVNGMemoryStream.cpp RVNGProperty.cpp RVNGPropertyList.cpp \
             RVNGPropertyListVector.cpp RVNGString.cpp RVNGStringVector.cpp RVNGSVGDrawingGenerator.cpp \
             RVNGStreamImplementation.cpp \
             RVNGDirectoryStream.cpp RVNGOLEStream.cpp RVNGZipStream.cpp)
CDR_DEFS := -DBOOST_ALL_NO_LIB -DBOOST_ERROR_CODE_HEADER_ONLY -DLIBREVENGE_BUILD -DLIBREVENGE_STREAM_BUILD -DPLUTOVG_BUILD_STATIC \
            -I$(LIBCDR)/inc -I$(LIBREVENGE)/inc -I$(LCMS)/include -I$(BOOST) -I$(LUNA)/plutovg/include
CDR_STAMP = $(wildcard $(LIBCDR)/.stamp-* $(LIBREVENGE)/.stamp-* $(LCMS)/.version-*)
define cdr_lib
	@rm -rf $(@D)/cdrobj && mkdir -p $(@D)/cdrobj
	@echo "compiling libcdr + librevenge + lcms2 into $@"
	@printf '%s\n' $(CDR_C) | xargs -n1 -P$(JOBS) sh -c '$(CC) $(1) -O2 -w -I$(LCMS)/include -c "$$0" -o $(@D)/cdrobj/$$(basename "$$0").o'
	@printf '%s\n' $(CDR_CXX) | xargs -n1 -P$(JOBS) sh -c '$(CXX) -std=c++17 $(1) -O2 -w $(CDR_DEFS) -c "$$0" -o $(@D)/cdrobj/$$(basename "$$0").o'
	@rm -f $@ && $(AR) rcs $@ $(@D)/cdrobj/*.o
endef
build/libnitrocdr.a: src/nitrocdr.cpp src/nitrocdr.h $(CDR_STAMP)
	$(call cdr_lib,$(CPUFLAGS))

# mimalloc and the new / delete that use it; $(1): compiler flags (architecture, CPU)
define mimalloc_objs
	@mkdir -p $(@D)
	$(CC) $(1) -O3 -DNDEBUG -w -I$(MI)/include -c $(MI)/src/static.c -o $(@D)/mimalloc.o
	$(CXX) -std=c++17 $(1) -O2 -I$(MI)/include -c src/nitroalloc.cpp -o $(@D)/nitroalloc.o
endef
build/mimalloc.o build/nitroalloc.o: src/nitroalloc.cpp $(wildcard $(MI)/.version-*)
	$(call mimalloc_objs,$(CPUFLAGS))


# the Metal shader source, embedded as a byte array
build/shaders.inc: src/shaders.metal
	@mkdir -p build
	cd src && xxd -i shaders.metal > ../$@

nitroview: src/nitroview.m $(VIEWER_SRC) $(VIEWER_HDR) $(LIB_HDR) build/shaders.inc build/libnitro.a $(VIEWER_OBJS) build/.config-$(CONFIG) Makefile
	$(CC) $(CFLAGS) $(VIEWER_DEFS) -Ibuild -Isrc -fobjc-arc src/nitroview.m $(VIEWER_SRC) build/libnitro.a $(VIEWER_OBJS) $(VIEWER_LIBS) \
	  $(FRAMEWORKS) -framework ImageIO -o $@
	@echo "built nitroview (libjpeg-turbo fallback: $(if $(filter 1,$(TURBOJPEG)),yes,no), Wuffs PNG: $(if $(filter 1,$(WUFFS)),yes,no), SVG: $(if $(filter 1,$(SVG)),yes,no), CDR: $(if $(filter 1,$(CDR)),yes,no), AVIF: $(if $(filter 1,$(DAV1D)),dav1d,ImageIO), HEIC: $(if $(filter 1,$(NITROHEVC)),nitrohevc,VideoToolbox), ImageIO: always)"

# Dock / Finder icon: every iconset size scaled from packaging/icon.png
build/nitroview.icns: packaging/icon.png
	@mkdir -p build
	rm -rf build/nitroview.iconset && mkdir build/nitroview.iconset
	for s in 16 32 128 256 512; do \
	  sips -z $$s $$s $< --out build/nitroview.iconset/icon_$${s}x$${s}.png >/dev/null; \
	  sips -z $$((s*2)) $$((s*2)) $< --out build/nitroview.iconset/icon_$${s}x$${s}@2x.png >/dev/null; \
	done
	iconutil -c icns build/nitroview.iconset -o $@

# macOS app bundle (for the Finder: "Open With" / "Change All"), ad-hoc signed
# The app is universal: the viewer is built once per architecture with portable CPU flags
# (not -march=native: it runs on other Macs) and joined with lipo. With TURBOJPEG=1 the
# libjpeg-turbo fallback goes into each slice whose architecture the library has.
MAC_ARCHS    ?= x86_64 arm64
MAC_MIN      := -mmacosx-version-min=12.0
APPCPU_x86_64 := -march=x86-64-v3 $(X86JCC) $(DEFS)
APPCPU_arm64  := -mcpu=apple-m1 $(DEFS)
ljt_has = $(if $(filter 1,$(TURBOJPEG)),$(filter $(1),$(shell lipo -archs $(LJT)/lib/libturbojpeg.a 2>/dev/null)))
dav1d_has = $(if $(filter 1,$(DAV1D)),$(filter $(1),$(shell lipo -archs $(DAV1D_LIB) 2>/dev/null)))
build/app-%/png_wuffs.o: src/png_wuffs.c src/png_wuffs.h
	@mkdir -p $(@D)
	$(CC) -arch $* $(MAC_MIN) -O3 $(APPCPU_$*) -w -c $< -o $@
build/app-%/libnitrosvg.a: src/nitrosvg.cpp src/nitrosvg.h $(SVG_STAMP)
	$(call svg_lib,-arch $* $(MAC_MIN) $(APPCPU_$*))
build/app-%/libnitrocdr.a: src/nitrocdr.cpp src/nitrocdr.h $(CDR_STAMP)
	$(call cdr_lib,-arch $* $(MAC_MIN) $(APPCPU_$*))
build/app-%/mimalloc.o: src/nitroalloc.cpp $(wildcard $(MI)/.version-*)
	$(call mimalloc_objs,-arch $* $(MAC_MIN) $(APPCPU_$*))
# the library per architecture (AVIF in the slices whose architecture dav1d has)
app_arch = $(patsubst build/app-%/libnitro.a,%,$@)
build/app-%/libnitro.a: LIB_AVIF = $(if $(call dav1d_has,$(app_arch)),src/nitroavif.c)
build/app-%/libnitro.a: LIB_DEFS = $(if $(call dav1d_has,$(app_arch)),-DNV_DAV1D -I$(DAV1D_DIR)/include)
build/app-%/libnitro.a: $(LIB_SRC) src/nitroavif.c $(LIB_HDR) build/.config-$(CONFIG)
	$(call nitro_lib,-arch $* $(MAC_MIN) -O3 $(APPCPU_$*))
build/app-%/nitroview: src/nitroview.m $(VIEWER_SRC) $(VIEWER_HDR) build/shaders.inc build/app-%/libnitro.a $(LIB_HDR) $(if $(filter 1,$(WUFFS)),build/app-%/png_wuffs.o) \
                       $(if $(filter 1,$(SVG)),build/app-%/libnitrosvg.a) $(if $(filter 1,$(CDR)),build/app-%/libnitrocdr.a) \
                       $(if $(filter 1,$(MIMALLOC)),build/app-%/mimalloc.o) \
                       build/.config-$(CONFIG) Makefile
	@mkdir -p $(@D)
	$(CC) -arch $* $(MAC_MIN) -O3 $(APPCPU_$*) -Wall -Wextra -Wno-unused-parameter -Ibuild -Isrc \
	  $(if $(call ljt_has,$*),-DNV_TURBOJPEG -I$(LJT)/include) $(if $(filter 1,$(WUFFS)),-DNV_WUFFS) $(if $(filter 1,$(SVG)),-DNV_SVG) $(if $(filter 1,$(CDR)),-DNV_CDR) \
	  $(if $(call dav1d_has,$*),-DNV_DAV1D -I$(DAV1D_DIR)/include) \
	  -fobjc-arc src/nitroview.m $(VIEWER_SRC) $(@D)/libnitro.a $(if $(call dav1d_has,$*),$(DAV1D_LIB)) $(if $(filter 1,$(WUFFS)),$(@D)/png_wuffs.o) $(if $(filter 1,$(CDR)),$(@D)/libnitrocdr.a -liconv -lz) $(if $(filter 1,$(SVG)),$(@D)/libnitrosvg.a -lc++) \
	  $(if $(filter 1,$(MIMALLOC)),$(@D)/mimalloc.o $(@D)/nitroalloc.o) $(if $(call ljt_has,$*),$(LJT)/lib/libturbojpeg.a -Wl$(COMMA)-w) $(FRAMEWORKS) -framework ImageIO -o $@
build/nitroview-universal: $(foreach a,$(MAC_ARCHS),build/app-$(a)/nitroview)
	lipo -create $^ -output $@

# Signing: ad-hoc by default ("-": runs here, other Macs ask once, see README). With a Developer
# ID certificate: hardened runtime + secure timestamp, as notarization requires.
SIGN_ID    ?= -
SIGN_FLAGS := $(if $(filter -,$(SIGN_ID)),,--options runtime --timestamp)

app: build/nitroview-universal packaging/Info.plist build/nitroview.icns
	rm -rf nitroview.app
	mkdir -p nitroview.app/Contents/MacOS nitroview.app/Contents/Resources
	cp build/nitroview-universal nitroview.app/Contents/MacOS/nitroview
	cp packaging/Info.plist nitroview.app/Contents/
	cp build/nitroview.icns nitroview.app/Contents/Resources/
	codesign --force $(SIGN_FLAGS) --sign "$(SIGN_ID)" nitroview.app
	@echo "built nitroview.app ($(foreach a,$(MAC_ARCHS),$(a)$(if $(call ljt_has,$(a)),+turbojpeg)$(if $(call dav1d_has,$(a)),+dav1d)) ; copy it to /Applications, then Finder: Get Info > Open with > nitroview > Change All)"

deps:
	@test -f $(LJT)/lib/libturbojpeg.a -a -f third_party/stb_image.h -a -f third_party/wuffs.c || \
	  { echo "missing dependencies: run scripts/get-deps.sh first"; exit 1; }
ljt:
	@test -f $(LJT)/lib/libturbojpeg.a || { echo "missing libjpeg-turbo: run scripts/get-deps.sh first"; exit 1; }

bench/bench: bench/bench.m build/nitrojpeg_ref.o | deps
	$(CC) $(REF_CFLAGS) -w -fobjc-arc $^ $(REF_LIBS) -framework Foundation -framework ImageIO \
	  -framework CoreGraphics -framework VideoToolbox -framework CoreMedia -framework CoreVideo -o $@

# the library's JPEG -> RGB conversion (nitro_convert, the GNOME viewer's) vs libjpeg-turbo
bench/rgbverify: bench/rgbverify.c build/libnitro.a | ljt
	$(CC) $(CFLAGS) -I$(LJT)/include -Isrc bench/rgbverify.c build/libnitro.a $(LJT)/lib/libturbojpeg.a \
	  $(LDQUIET) $(THREADLIBS) -o $@

# the example program on the library (examples/nitroconvert.c): any image it decodes -> a 24-bit BMP
EXAMPLE_LIBS := $(if $(filter 1,$(DAV1D)),$(DAV1D_LIB)) $(THREADLIBS) \
                $(if $(filter Darwin,$(UNAME_S)),-framework CoreFoundation $(VT_FW),-lm)
examples/nitroconvert: examples/nitroconvert.c build/libnitro.a
	$(CC) $(CFLAGS) -Isrc $^ $(EXAMPLE_LIBS) $(LDQUIET) -o $@

bench/verify: bench/verify.c build/nitrojpeg_ref.o | ljt
	$(CC) $(REF_CFLAGS) -w $^ $(REF_LIBS) $(THREADLIBS) -o $@

# robustness test on damaged files (production decoder + TurboJPEG fallback, like
# the viewer), with AddressSanitizer + UBSan
bench/robust: bench/robust.c src/nitrojpeg.c src/nitrojpeg.h | ljt
	$(CC) $(ASAN) -I$(LJT)/include bench/robust.c src/nitrojpeg.c $(REF_LIBS) $(THREADLIBS) -o $@

bench/pngbench: bench/pngbench.m build/libnitro.a | deps
	$(CC) -O3 $(CPUFLAGS) -w -Isrc -fobjc-arc $^ -framework Foundation -framework ImageIO -framework CoreGraphics -o $@

# nitrotiff / nitropng / nitroavif's BGRX output against ImageIO: pixels and time (macOS)
bench/bgrxverify: bench/bgrxverify.m build/libnitro.a
	$(CC) -O3 $(CPUFLAGS) -w -Isrc $(if $(filter 1,$(DAV1D)),-DNV_DAV1D) -fobjc-arc $^ $(if $(filter 1,$(DAV1D)),$(DAV1D_LIB)) \
	  -framework Foundation -framework ImageIO -framework CoreGraphics $(VT_FW) -o $@

bench/pngverify: bench/pngverify.c build/libnitro.a
	$(CC) -O3 $(CPUFLAGS) $(PORTFLAGS) -w -Isrc $^ -lz $(THREADLIBS) -o $@

# nitropng robustness test on damaged PNGs, with AddressSanitizer + UBSan
bench/pngrobust: bench/pngrobust.c src/nitropng.c src/nitropng.h
	$(CC) $(ASAN) bench/pngrobust.c src/nitropng.c $(THREADLIBS) -o $@

bench/psdverify: bench/psdverify.c build/libnitro.a
	$(CC) -O3 $(CPUFLAGS) $(PORTFLAGS) -w -Isrc $^ $(THREADLIBS) -o $@

# portable benchmark of the three decoders (also builds on Linux, see README)
bench/nbench: bench/nbench.c build/libnitro.a
	$(CC) $(CFLAGS) -Isrc $^ $(THREADLIBS) -o $@

# nitropsd robustness test on damaged PSDs, with AddressSanitizer + UBSan
bench/psdrobust: bench/psdrobust.c src/nitropsd.c src/nitropsd.h src/nitropng.c
	$(CC) $(ASAN) bench/psdrobust.c src/nitropsd.c src/nitropng.c $(THREADLIBS) -o $@

bench/pngsplit: bench/pngsplit.c
	$(CC) -O3 $(CPUFLAGS) $< -lz -o $@

bench/freqprobe: bench/freqprobe.c
	$(CC) -O2 $< -o $@

bench/sustain: bench/sustain.c build/libnitro.a
	$(CC) -O2 -Isrc $^ -o $@

tools: bench/bench bench/verify bench/robust bench/freqprobe bench/sustain bench/pngbench bench/pngsplit bench/pngverify bench/pngrobust bench/psdverify bench/psdrobust bench/bgrxverify \
       bench/nbench bench/rgbverify examples/nitroconvert

# Linux (or any clang + pthreads system): the tools without macOS APIs; verify and robust
# only once scripts/get-deps.sh has built libjpeg-turbo
LINUX_TOOLS := bench/nbench bench/pngverify bench/psdverify bench/pngrobust bench/psdrobust examples/nitroconvert \
               $(if $(wildcard $(LJT)/lib/libturbojpeg.a),bench/verify bench/robust bench/rgbverify)
linux: linux-lib $(LINUX_TOOLS)
	@echo "built $(LIB_SHARED) $(LIB_STATIC) $(LINUX_TOOLS)$(if $(wildcard $(LJT)/lib/libturbojpeg.a),, (verify / robust: run scripts/get-deps.sh first))"

clean:
	rm -rf build nitroview nitroview-gnome nitroview-sdl nitroview-linux-x64.tar.gz nitroview-linux-arm64.tar.gz nitroview-gnome.flatpak nitroview.app nitroview.exe nitroview.zip nitroview.dmg \
	  bench/bench bench/verify bench/robust bench/freqprobe bench/sustain bench/pngbench bench/pngsplit \
	  bench/pngverify bench/pngrobust bench/psdverify bench/psdrobust bench/nbench bench/rgbverify bench/bgrxverify bench/*.exe

# Windows (cross, llvm-mingw): self-contained .exe files, only system DLLs (UCRT: Windows 10 /
# Server 2016 and later). WINARCH=x86-64-v2 for CPUs without AVX2.
WINCC   ?= x86_64-w64-mingw32-clang
WINARCH ?= x86-64-v3
WINFLAGS = -O3 -march=$(WINARCH) $(X86JCC) $(DEFS) -fblocks -Wall -Wextra -Wno-unused-parameter -static
# everything Windows is built again when WINARCH changes (the v2 and v3 builds share build/win/)
WIN_STAMP := build/win/.arch-$(WINARCH)
$(WIN_STAMP):
	@mkdir -p build/win
	@rm -f build/win/.arch-*
	@touch $@
bench/nbench.exe: bench/nbench.c build/win/libnitro.a $(LIB_HDR) $(WIN_STAMP)
	$(WINCC) $(WINFLAGS) -Isrc bench/nbench.c build/win/libnitro.a -lpthread -o $@
bench/psdverify.exe: bench/psdverify.c build/win/libnitro.a $(LIB_HDR) $(WIN_STAMP)
	$(WINCC) $(WINFLAGS) -w -Isrc bench/psdverify.c build/win/libnitro.a -lpthread -o $@
WINRES  ?= $(patsubst %clang,%windres,$(WINCC))
WINLIBS := -ld3d11 -ldxgi -ld3dcompiler -lwindowscodecs -lole32 -loleaut32 -lshlwapi -lshell32 -luser32 \
           -lgdi32 -ladvapi32 -luuid -ldxguid -lwinmm -lpthread
build/nitroview_res.o: packaging/nitroview.rc packaging/nitroview.ico
	@mkdir -p build
	$(WINRES) -I packaging $< -O coff -o $@
# SVG and CorelDRAW in the Windows viewer too (same sources, cross-compiled into build/win/;
# zlib, which Windows lacks, from third_party/zlib into libnitrocdr.a)
WINCXX  ?= $(patsubst %clang,%clang++,$(WINCC))
WINAR   ?= $(if $(findstring /,$(WINCC)),$(dir $(WINCC)))llvm-ar
WIN_OBJS :=
WIN_DEFS :=
ifeq ($(CDR),1)
WIN_DEFS += -DNV_CDR
WIN_OBJS += build/win/libnitrocdr.a
endif
ifeq ($(SVG),1)
WIN_DEFS += -DNV_SVG
WIN_OBJS += build/win/libnitrosvg.a
endif
WIN_CXXLIBS := $(if $(WIN_OBJS),-lc++)
# AVIF: nitroavif with dav1d cross-compiled by scripts/get-deps.sh (WINCC=... set there too)
DAV1D_WIN := third_party/dav1d-win
WIN_DAV1D := $(if $(and $(filter-out 0,$(DAV1D)),$(wildcard $(DAV1D_WIN)/lib/libdav1d.a)),1)
WIN_AVIF  := $(if $(WIN_DAV1D),-DNV_DAV1D -I$(DAV1D_WIN)/include)
# the library, cross-compiled (nitroavif in it with dav1d-win)
build/win/libnitro.a: CC = $(WINCC)
build/win/libnitro.a: AR = $(WINAR)
build/win/libnitro.a: LIB_AVIF = $(if $(WIN_DAV1D),src/nitroavif.c)
build/win/libnitro.a: LIB_DEFS = $(WIN_AVIF)
build/win/libnitro.a: $(LIB_SRC) src/nitroavif.c $(LIB_HDR) build/.config-$(CONFIG) $(WIN_STAMP)
	$(call nitro_lib,$(WINFLAGS))
build/win/libnitrosvg.a: CC = $(WINCC)
build/win/libnitrosvg.a: CXX = $(WINCXX)
build/win/libnitrosvg.a: AR = $(WINAR)
build/win/libnitrosvg.a: src/nitrosvg.cpp src/nitrosvg.h $(SVG_STAMP) $(WIN_STAMP)
	$(call svg_lib,-march=$(WINARCH))
build/win/libnitrocdr.a: CC = $(WINCC)
build/win/libnitrocdr.a: CXX = $(WINCXX)
build/win/libnitrocdr.a: AR = $(WINAR)
build/win/libnitrocdr.a: CDR_C += $(addprefix $(ZLIB)/,adler32.c crc32.c inflate.c inffast.c inftrees.c zutil.c)
build/win/libnitrocdr.a: CDR_DEFS += -I$(ZLIB)
build/win/libnitrocdr.a: src/nitrocdr.cpp src/nitrocdr.h $(CDR_STAMP) $(WIN_STAMP)
	$(call cdr_lib,-march=$(WINARCH) -I$(ZLIB))
nitroview.exe: src/nitroview_win.c $(VIEWER_SRC) $(VIEWER_HDR) build/win/libnitro.a $(LIB_HDR) build/nitroview_res.o $(WIN_OBJS) \
               src/nitrosvg.h src/nitrocdr.h $(WIN_STAMP)
	$(WINCC) $(WINFLAGS) $(WIN_DEFS) $(WIN_AVIF) -Ibuild -Isrc -municode src/nitroview_win.c $(VIEWER_SRC) build/win/libnitro.a \
	  $(if $(WIN_DAV1D),$(DAV1D_WIN)/lib/libdav1d.a) build/nitroview_res.o $(WIN_OBJS) $(WINLIBS) $(WIN_CXXLIBS) -o $@
windows: nitroview.exe bench/nbench.exe bench/psdverify.exe
	@echo "built nitroview.exe bench/nbench.exe bench/psdverify.exe (Windows x64, $(WINARCH), SVG: $(if $(filter 1,$(SVG)),yes,no), CDR: $(if $(filter 1,$(CDR)),yes,no), AVIF: $(if $(WIN_AVIF),yes,no), HEIC: $(if $(filter 1,$(NITROHEVC)),nitrohevc,WIC))"

# Distribution. zip: ditto keeps the signature and the bundle's attributes (like Finder > Compress).
zip: app
	rm -f nitroview.zip
	ditto -c -k --keepParent nitroview.app nitroview.zip
	@echo "built nitroview.zip"

# dmg: the app + a link to /Applications, compressed
dmg: app
	rm -rf build/dmg nitroview.dmg
	mkdir -p build/dmg
	ditto nitroview.app build/dmg/nitroview.app
	ln -s /Applications build/dmg/Applications
	hdiutil create -volname NitroView -srcfolder build/dmg -ov -format UDZO -quiet nitroview.dmg
	$(if $(filter -,$(SIGN_ID)),,codesign --force --timestamp --sign "$(SIGN_ID)" nitroview.dmg)
	@echo "built nitroview.dmg"

# Apple's notarization of the signed dmg, then the ticket stapled to it (works offline too).
NOTARY_PROFILE ?=
notarize:
	@test "$(SIGN_ID)" != "-" -a -n "$(NOTARY_PROFILE)" || \
	  { echo "needs SIGN_ID=\"Developer ID Application: ...\" and NOTARY_PROFILE=... (see the Makefile header)"; exit 1; }
	xcrun notarytool submit nitroview.dmg --keychain-profile "$(NOTARY_PROFILE)" --wait
	xcrun stapler staple nitroview.dmg
	@echo "nitroview.dmg is notarized"

.PHONY: all app zip dmg notarize tools lib linux linux-lib gnome gnome-register sdl sdl-dist flatpak windows clean deps ljt
