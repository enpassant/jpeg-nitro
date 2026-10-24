# jpeg-nitro: nitroview (fast image viewer for macOS) + nitrojpeg (parallel JPEG decoder)
#
#   make                 nitroview; uses libjpeg-turbo as fallback decoder if it is present
#                        in third_party/ljt (see scripts/get-deps.sh), standalone otherwise
#   make TURBOJPEG=0     no libjpeg-turbo: JPEGs nitrojpeg can't decode go to Apple ImageIO
#   make WUFFS=0         no Wuffs: PNG is decoded by Apple ImageIO (~1.8x slower)
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
#   make linux-lib       Linux shared + static decoder library: build/lib/libnitro.so/.a
#   make gnome           GNOME/libadwaita viewer: nitroview-gnome (uses libnitro.so)
#   make flatpak         GNOME 50 Flatpak bundle: nitroview-gnome.flatpak
#   make linux           Linux (also the default there): library + portable tools - nbench,
#                        pngverify, psdverify, the ASan robustness tests, and verify / robust
#                        once scripts/get-deps.sh has built libjpeg-turbo. Needs clang.
#   make windows         cross-compile nitroview.exe (the viewer), bench/nbench.exe and
#                        bench/psdverify.exe for Windows (x64, static, AVX2) with llvm-mingw:
#                        WINCC=path/to/x86_64-w64-mingw32-clang

LJT        := third_party/ljt
TURBOJPEG  ?= $(if $(wildcard $(LJT)/lib/libturbojpeg.a),1,0)
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
FRAMEWORKS := -framework Cocoa -framework Metal -framework MetalKit -framework QuartzCore -framework UniformTypeIdentifiers \
              -framework VideoToolbox -framework CoreMedia -framework CoreVideo
DEC_SRC    := src/nitrojpeg.c src/nitropng.c src/nitropsd.c
DEC_HDR    := src/nitrojpeg.h src/nitropng.h src/nitropsd.h src/nitro_os.h

# Linux library: the decoders in one native C library (JPEG, PNG, PSD, and HEIC, DNG, WebP).
# Keep dedicated PIC objects so the existing benchmark/test objects stay unchanged.
LIBDIR     := build/lib
LIB_OBJS   := $(LIBDIR)/nitrojpeg.o $(LIBDIR)/nitropng.o $(LIBDIR)/nitropsd.o $(LIBDIR)/nitroheic.o $(LIBDIR)/nitrohevc.o \
              $(LIBDIR)/nitrodng.o $(LIBDIR)/nitrowebp.o
LIB_SHARED := $(LIBDIR)/libnitro.so
LIB_STATIC := $(LIBDIR)/libnitro.a
LIB_CFLAGS := $(CFLAGS) -fPIC

# GNOME frontend: GtkBuilder template + libadwaita, linked against the Linux shared library.
GNOME_PKGS     := gtk4 libadwaita-1
# (= not :=: pkg-config runs only when the GNOME viewer is built, not on every make)
GNOME_CFLAGS    = $(shell pkg-config --cflags $(GNOME_PKGS) 2>/dev/null)
GNOME_LIBS      = $(shell pkg-config --libs $(GNOME_PKGS) 2>/dev/null)
GNOME_DIR      := src/gnome
GNOME_RES      := build/gnome/nitroview-resources.c
GNOME_SRC      := $(GNOME_DIR)/main.c $(GNOME_DIR)/nitro-window.c $(GNOME_DIR)/nitro-view.c $(GNOME_DIR)/nitro-image.c \
                  $(GNOME_DIR)/nitro-convert.c $(GNOME_DIR)/nitro-loader.c $(GNOME_RES)
GNOME_HDR      := $(GNOME_DIR)/nitro-window.h $(GNOME_DIR)/nitro-view.h $(GNOME_DIR)/nitro-image.h $(GNOME_DIR)/nitro-convert.h \
                  $(GNOME_DIR)/nitro-loader.h \
                  src/nitro_os.h
GNOME_BIN      := nitroview-gnome
GNOME_APP_ID   := com.github.gereoffy.nitroview
GNOME_RPATH    ?= $$ORIGIN/build/lib
GNOME_DESKTOP  := build/gnome/$(GNOME_APP_ID).desktop
GNOME_DESKTOP_SOURCE := packaging/gnome/$(GNOME_APP_ID).desktop
GNOME_USER_DATA := $(HOME)/.local/share

FLATPAK_MANIFEST := packaging/flatpak/$(GNOME_APP_ID).json
FLATPAK_BUILD    := build/flatpak
FLATPAK_REPO     := build/flatpak-repo
FLATPAK_BUNDLE   := nitroview-gnome.flatpak

WUFFS      ?= $(if $(wildcard third_party/wuffs.c),1,0)
LUNA       := third_party/lunasvg
SVG        ?= $(if $(wildcard $(LUNA)/include/lunasvg.h),1,0)
LIBCDR     := third_party/libcdr
LIBREVENGE := third_party/librevenge
LCMS       := third_party/lcms2
BOOST      := third_party/boost
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

# production decoder: no dependencies
build/nitrojpeg.o: src/nitrojpeg.c src/nitrojpeg.h src/nitro_os.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

# reference build: + old libjpeg-turbo engine and BGRX output (tests, benchmarks)
build/nitrojpeg_ref.o: src/nitrojpeg.c src/nitrojpeg.h src/nitro_os.h | ljt
	@mkdir -p build
	$(CC) $(REF_CFLAGS) -c $< -o $@

build/nitropsd.o: src/nitropsd.c src/nitropsd.h src/nitropng.h src/nitro_os.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@
build/nitroheic.o: src/nitroheic.c src/nitroheic.h src/nitrohevc.h   # (macOS: VideoToolbox)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@
build/nitrohevc.o: src/nitrohevc.c src/nitrohevc.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@
build/nitrodng.o: src/nitrodng.c src/nitrodng.h src/nitro_os.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@
build/nitrowebp.o: src/nitrowebp.c src/nitrowebp.h src/nitro_os.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/nitropng.o: src/nitropng.c src/nitropng.h src/nitro_os.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBDIR)/nitrojpeg.o: src/nitrojpeg.c src/nitrojpeg.h src/nitro_os.h
	@mkdir -p $(LIBDIR)
	$(CC) $(LIB_CFLAGS) -c $< -o $@

$(LIBDIR)/nitropng.o: src/nitropng.c src/nitropng.h src/nitro_os.h
	@mkdir -p $(LIBDIR)
	$(CC) $(LIB_CFLAGS) -c $< -o $@

$(LIBDIR)/nitropsd.o: src/nitropsd.c src/nitropsd.h src/nitropng.h src/nitro_os.h
	@mkdir -p $(LIBDIR)
	$(CC) $(LIB_CFLAGS) -c $< -o $@

$(LIBDIR)/nitroheic.o: src/nitroheic.c src/nitroheic.h src/nitrohevc.h src/nitro_os.h
	@mkdir -p $(LIBDIR)
	$(CC) $(LIB_CFLAGS) -c $< -o $@

$(LIBDIR)/nitrohevc.o: src/nitrohevc.c src/nitrohevc.h src/nitro_os.h
	@mkdir -p $(LIBDIR)
	$(CC) $(LIB_CFLAGS) -c $< -o $@

$(LIBDIR)/nitrodng.o: src/nitrodng.c src/nitrodng.h src/nitro_os.h
	@mkdir -p $(LIBDIR)
	$(CC) $(LIB_CFLAGS) -c $< -o $@

$(LIBDIR)/nitrowebp.o: src/nitrowebp.c src/nitrowebp.h src/nitro_os.h
	@mkdir -p $(LIBDIR)
	$(CC) $(LIB_CFLAGS) -c $< -o $@

$(LIB_SHARED): $(LIB_OBJS) src/libnitro.map
	$(CC) -shared -Wl,-soname,libnitro.so -Wl,--version-script=src/libnitro.map -o $@ $(LIB_OBJS) $(THREADLIBS)

$(LIB_STATIC): $(LIB_OBJS)
	$(AR) rcs $@ $^

linux-lib: $(LIB_SHARED) $(LIB_STATIC)
	@echo "built $(LIB_SHARED) $(LIB_STATIC)"

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

$(GNOME_BIN): $(GNOME_SRC) $(GNOME_HDR) $(LIB_SHARED) $(GNOME_SVG) $(GNOME_CDR)
	@pkg-config --exists $(GNOME_PKGS) || { echo "missing GNOME development packages: gtk4 libadwaita-1"; exit 1; }
	@pkg-config --atleast-version=4.6 gtk4 || { echo "GNOME frontend requires GTK 4.6 or newer"; exit 1; }
	@pkg-config --atleast-version=1.1 libadwaita-1 || { echo "GNOME frontend requires libadwaita 1.1 or newer"; exit 1; }
	$(CC) -std=gnu11 -O2 $(CPUFLAGS) $(PORTFLAGS) -Wall -Wextra -Wno-unused-parameter -Isrc -I$(GNOME_DIR) $(GNOME_CFLAGS) \
	  $(if $(GNOME_SVG),-DNV_SVG) $(if $(GNOME_CDR),-DNV_CDR) $(GNOME_SRC) $(GNOME_CDR) $(GNOME_SVG) -L$(LIBDIR) -lnitro \
	  -Wl,-rpath,'$(GNOME_RPATH)' $(GNOME_LIBS) $(if $(GNOME_CDR),-lz) $(if $(GNOME_SVG),-lstdc++) $(THREADLIBS) -lm -o $@

# GNOME viewer: command-line open plus a native Open/Quit application shell.
gnome: $(GNOME_BIN)
	@echo "built $(GNOME_BIN) (SVG: $(if $(GNOME_SVG),nitrosvg,GTK's loaders), CorelDRAW: $(if $(GNOME_CDR),yes,no))"

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

# rebuild the viewer when the optional parts change
CONFIG := tj$(TURBOJPEG)-wuffs$(WUFFS)-svg$(SVG)-cdr$(CDR)-mi$(MIMALLOC)
build/.config-$(CONFIG):
	@mkdir -p build
	@rm -f build/.config-* nitroview
	@touch $@

# the Metal shader source, embedded as a byte array
build/shaders.inc: src/shaders.metal
	@mkdir -p build
	cd src && xxd -i shaders.metal > ../$@

nitroview: src/nitroview.m build/shaders.inc build/nitrojpeg.o build/nitropng.o build/nitropsd.o build/nitroheic.o build/nitrohevc.o build/nitrodng.o build/nitrowebp.o $(VIEWER_OBJS) build/.config-$(CONFIG) Makefile
	$(CC) $(CFLAGS) $(VIEWER_DEFS) -Ibuild -fobjc-arc src/nitroview.m build/nitrojpeg.o build/nitropng.o build/nitropsd.o build/nitroheic.o build/nitrohevc.o build/nitrodng.o build/nitrowebp.o $(VIEWER_OBJS) $(VIEWER_LIBS) \
	  $(FRAMEWORKS) -framework ImageIO -o $@
	@echo "built nitroview (libjpeg-turbo fallback: $(if $(filter 1,$(TURBOJPEG)),yes,no), Wuffs PNG: $(if $(filter 1,$(WUFFS)),yes,no), SVG: $(if $(filter 1,$(SVG)),yes,no), CDR: $(if $(filter 1,$(CDR)),yes,no), ImageIO: always)"

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
# (not -march=native: it runs on other Macs) and joined with lipo. The libjpeg-turbo fallback
# goes into each slice whose architecture the library has (scripts/get-deps.sh builds both).
MAC_ARCHS    ?= x86_64 arm64
MAC_MIN      := -mmacosx-version-min=12.0
APPCPU_x86_64 := -march=x86-64-v3 $(X86JCC) $(DEFS)
APPCPU_arm64  := -mcpu=apple-m1 $(DEFS)
ljt_has = $(filter $(1),$(shell lipo -archs $(LJT)/lib/libturbojpeg.a 2>/dev/null))
build/app-%/png_wuffs.o: src/png_wuffs.c src/png_wuffs.h
	@mkdir -p $(@D)
	$(CC) -arch $* $(MAC_MIN) -O3 $(APPCPU_$*) -w -c $< -o $@
build/app-%/libnitrosvg.a: src/nitrosvg.cpp src/nitrosvg.h $(SVG_STAMP)
	$(call svg_lib,-arch $* $(MAC_MIN) $(APPCPU_$*))
build/app-%/libnitrocdr.a: src/nitrocdr.cpp src/nitrocdr.h $(CDR_STAMP)
	$(call cdr_lib,-arch $* $(MAC_MIN) $(APPCPU_$*))
build/app-%/mimalloc.o: src/nitroalloc.cpp $(wildcard $(MI)/.version-*)
	$(call mimalloc_objs,-arch $* $(MAC_MIN) $(APPCPU_$*))
build/app-%/nitroview: src/nitroview.m build/shaders.inc $(DEC_SRC) $(DEC_HDR) src/nitroheic.c src/nitroheic.h src/nitrohevc.c src/nitrohevc.h src/nitrodng.c src/nitrodng.h src/nitrowebp.c src/nitrowebp.h $(if $(filter 1,$(WUFFS)),build/app-%/png_wuffs.o) \
                       $(if $(filter 1,$(SVG)),build/app-%/libnitrosvg.a) $(if $(filter 1,$(CDR)),build/app-%/libnitrocdr.a) \
                       $(if $(filter 1,$(MIMALLOC)),build/app-%/mimalloc.o) \
                       build/.config-$(CONFIG) Makefile
	@mkdir -p $(@D)
	$(CC) -arch $* $(MAC_MIN) -O3 $(APPCPU_$*) -Wall -Wextra -Wno-unused-parameter -Ibuild \
	  $(if $(call ljt_has,$*),-DNV_TURBOJPEG -I$(LJT)/include) $(if $(filter 1,$(WUFFS)),-DNV_WUFFS) $(if $(filter 1,$(SVG)),-DNV_SVG) $(if $(filter 1,$(CDR)),-DNV_CDR) \
	  -fobjc-arc src/nitroview.m $(DEC_SRC) src/nitroheic.c src/nitrohevc.c src/nitrodng.c src/nitrowebp.c $(if $(filter 1,$(WUFFS)),$(@D)/png_wuffs.o) $(if $(filter 1,$(CDR)),$(@D)/libnitrocdr.a -liconv -lz) $(if $(filter 1,$(SVG)),$(@D)/libnitrosvg.a -lc++) \
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
	@echo "built nitroview.app ($(foreach a,$(MAC_ARCHS),$(a)$(if $(call ljt_has,$(a)),+turbojpeg)) ; copy it to /Applications, then Finder: Get Info > Open with > nitroview > Change All)"

deps:
	@test -f $(LJT)/lib/libturbojpeg.a -a -f third_party/stb_image.h -a -f third_party/wuffs.c || \
	  { echo "missing dependencies: run scripts/get-deps.sh first"; exit 1; }
ljt:
	@test -f $(LJT)/lib/libturbojpeg.a || { echo "missing libjpeg-turbo: run scripts/get-deps.sh first"; exit 1; }

bench/bench: bench/bench.m build/nitrojpeg_ref.o | deps
	$(CC) $(REF_CFLAGS) -w -fobjc-arc $^ $(REF_LIBS) -framework Foundation -framework ImageIO \
	  -framework CoreGraphics -framework VideoToolbox -framework CoreMedia -framework CoreVideo -o $@

# the GNOME viewer's JPEG -> RGB conversion vs libjpeg-turbo (no GTK needed)
bench/rgbverify: bench/rgbverify.c src/gnome/nitro-convert.c src/gnome/nitro-convert.h build/nitrojpeg.o | ljt
	$(CC) $(CFLAGS) -I$(LJT)/include bench/rgbverify.c src/gnome/nitro-convert.c build/nitrojpeg.o $(LJT)/lib/libturbojpeg.a \
	  $(LDQUIET) $(THREADLIBS) -o $@

bench/verify: bench/verify.c build/nitrojpeg_ref.o | ljt
	$(CC) $(REF_CFLAGS) -w $^ $(REF_LIBS) $(THREADLIBS) -o $@

# robustness test on damaged files (production decoder + TurboJPEG fallback, like
# the viewer), with AddressSanitizer + UBSan
bench/robust: bench/robust.c src/nitrojpeg.c src/nitrojpeg.h | ljt
	$(CC) $(ASAN) -I$(LJT)/include bench/robust.c src/nitrojpeg.c $(REF_LIBS) $(THREADLIBS) -o $@

bench/pngbench: bench/pngbench.m build/nitropng.o | deps
	$(CC) -O3 $(CPUFLAGS) -w -fobjc-arc $^ -framework Foundation -framework ImageIO -framework CoreGraphics -o $@

bench/pngverify: bench/pngverify.c build/nitropng.o
	$(CC) -O3 $(CPUFLAGS) $(PORTFLAGS) -w $^ -lz $(THREADLIBS) -o $@

# nitropng robustness test on damaged PNGs, with AddressSanitizer + UBSan
bench/pngrobust: bench/pngrobust.c src/nitropng.c src/nitropng.h
	$(CC) $(ASAN) bench/pngrobust.c src/nitropng.c $(THREADLIBS) -o $@

bench/psdverify: bench/psdverify.c build/nitropsd.o build/nitropng.o
	$(CC) -O3 $(CPUFLAGS) $(PORTFLAGS) -w $^ $(THREADLIBS) -o $@

# portable benchmark of the three decoders (also builds on Linux, see README)
bench/nbench: bench/nbench.c build/nitrojpeg.o build/nitropng.o build/nitropsd.o
	$(CC) $(CFLAGS) $^ $(THREADLIBS) -o $@

# nitropsd robustness test on damaged PSDs, with AddressSanitizer + UBSan
bench/psdrobust: bench/psdrobust.c src/nitropsd.c src/nitropsd.h src/nitropng.c
	$(CC) $(ASAN) bench/psdrobust.c src/nitropsd.c src/nitropng.c $(THREADLIBS) -o $@

bench/pngsplit: bench/pngsplit.c
	$(CC) -O3 $(CPUFLAGS) $< -lz -o $@

bench/freqprobe: bench/freqprobe.c
	$(CC) -O2 $< -o $@

bench/sustain: bench/sustain.c build/nitrojpeg.o
	$(CC) -O2 $^ -o $@

tools: bench/bench bench/verify bench/robust bench/freqprobe bench/sustain bench/pngbench bench/pngsplit bench/pngverify bench/pngrobust bench/psdverify bench/psdrobust \
       bench/nbench bench/rgbverify

# Linux (or any clang + pthreads system): the tools without macOS APIs; verify and robust
# only once scripts/get-deps.sh has built libjpeg-turbo
LINUX_TOOLS := bench/nbench bench/pngverify bench/psdverify bench/pngrobust bench/psdrobust \
               $(if $(wildcard $(LJT)/lib/libturbojpeg.a),bench/verify bench/robust bench/rgbverify)
linux: linux-lib $(LINUX_TOOLS)
	@echo "built $(LIB_SHARED) $(LIB_STATIC) $(LINUX_TOOLS)$(if $(wildcard $(LJT)/lib/libturbojpeg.a),, (verify / robust: run scripts/get-deps.sh first))"

clean:
	rm -rf build nitroview nitroview-gnome nitroview-gnome.flatpak nitroview.app nitroview.exe nitroview.zip nitroview.dmg \
	  bench/bench bench/verify bench/robust bench/freqprobe bench/sustain bench/pngbench bench/pngsplit \
	  bench/pngverify bench/pngrobust bench/psdverify bench/psdrobust bench/nbench bench/rgbverify bench/*.exe

# Windows (cross, llvm-mingw): self-contained .exe files, only system DLLs (UCRT: Windows 10 /
# Server 2016 and later). WINARCH=x86-64-v2 for CPUs without AVX2.
WINCC   ?= x86_64-w64-mingw32-clang
WINARCH ?= x86-64-v3
WINFLAGS = -O3 -march=$(WINARCH) $(X86JCC) $(DEFS) -fblocks -Wall -Wextra -Wno-unused-parameter -static
bench/nbench.exe: bench/nbench.c $(DEC_SRC) $(DEC_HDR)
	$(WINCC) $(WINFLAGS) bench/nbench.c $(DEC_SRC) -lpthread -o $@
bench/psdverify.exe: bench/psdverify.c src/nitropsd.c src/nitropng.c $(DEC_HDR)
	$(WINCC) $(WINFLAGS) -w bench/psdverify.c src/nitropsd.c src/nitropng.c -lpthread -o $@
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
ZLIB    := third_party/zlib
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
build/win/libnitrosvg.a: CC = $(WINCC)
build/win/libnitrosvg.a: CXX = $(WINCXX)
build/win/libnitrosvg.a: AR = $(WINAR)
build/win/libnitrosvg.a: src/nitrosvg.cpp src/nitrosvg.h $(SVG_STAMP)
	$(call svg_lib,-march=$(WINARCH))
build/win/libnitrocdr.a: CC = $(WINCC)
build/win/libnitrocdr.a: CXX = $(WINCXX)
build/win/libnitrocdr.a: AR = $(WINAR)
build/win/libnitrocdr.a: CDR_C += $(addprefix $(ZLIB)/,adler32.c crc32.c inflate.c inffast.c inftrees.c zutil.c)
build/win/libnitrocdr.a: CDR_DEFS += -I$(ZLIB)
build/win/libnitrocdr.a: src/nitrocdr.cpp src/nitrocdr.h $(CDR_STAMP)
	$(call cdr_lib,-march=$(WINARCH) -I$(ZLIB))
nitroview.exe: src/nitroview_win.c $(DEC_SRC) $(DEC_HDR) src/nitroheic.c src/nitroheic.h src/nitrohevc.c src/nitrohevc.h src/nitrodng.c src/nitrodng.h src/nitrowebp.c src/nitrowebp.h \
               build/nitroview_res.o $(WIN_OBJS) src/nitrosvg.h src/nitrocdr.h
	$(WINCC) $(WINFLAGS) $(WIN_DEFS) -Ibuild -Isrc -municode src/nitroview_win.c $(DEC_SRC) src/nitroheic.c src/nitrohevc.c src/nitrodng.c src/nitrowebp.c \
	  build/nitroview_res.o $(WIN_OBJS) $(WINLIBS) $(WIN_CXXLIBS) -o $@
windows: nitroview.exe bench/nbench.exe bench/psdverify.exe
	@echo "built nitroview.exe bench/nbench.exe bench/psdverify.exe (Windows x64, $(WINARCH), SVG: $(if $(filter 1,$(SVG)),yes,no), CDR: $(if $(filter 1,$(CDR)),yes,no))"

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

.PHONY: all app zip dmg notarize tools linux linux-lib gnome gnome-register flatpak windows clean deps ljt
