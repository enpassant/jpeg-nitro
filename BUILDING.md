# Building nitroview and the decoders

[README](README.md) | **Building** | [Benchmarking](BENCHMARKING.md)

Ready-made builds are in the [Releases](https://github.com/gereoffy/jpeg-nitro/releases). This is
for building the viewers yourself, and for using the decoders in your own program.

Everything is built with **clang**: the decoders' parallel loops are clang blocks
(`^(size_t i) {…}`), which GCC does not support. Nothing else is needed for a basic build; the
optional dependencies below add formats.

| platform | needs |
|---|---|
| macOS | Xcode Command Line Tools |
| Linux | clang and make; the GNOME viewer: the GTK 4 (4.6+) and libadwaita (1.1+) development packages; the SDL viewer: the window systems' development packages listed in `scripts/get-deps.sh` |
| Windows, cross-compiled on macOS or Linux | [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (clang + MinGW-w64; MSYS2's CLANG64 works too) |

## Optional dependencies

`scripts/get-deps.sh` downloads and builds them into `third_party/` (about a minute; nothing is
installed system-wide). The Makefile uses what it finds there: SVG, CorelDRAW and AVIF are built in
automatically once their libraries are present.

| dependency | for | option |
|---|---|---|
| [lunasvg](https://github.com/sammycage/lunasvg) with plutovg | SVG | `SVG=0` leaves it out |
| [libcdr](https://github.com/gereoffy/libcdr) (our fork), librevenge, lcms2, the boost headers; mimalloc | CorelDRAW (needs SVG) | `CDR=0` leaves it out; `MIMALLOC=0`: the C++ code with the system malloc |
| [dav1d](https://code.videolan.org/videolan/dav1d) | AVIF with nitroavif; without it AVIF goes to ImageIO / WIC | `DAV1D=0` |
| SDL3 | the SDL viewer's window | `SDL=0 scripts/get-deps.sh` skips it |
| libjpeg-turbo | the reference of the tests and benchmarks; an optional fallback decoder in the viewer | `TURBOJPEG=1` |
| stb_image, Wuffs | the decoder comparison in `bench/bench`; Wuffs also as the viewer's interlaced-PNG decoder | `WUFFS=1` |

dav1d is built with meson + ninja in a Python virtual environment (`third_party/tools/venv`),
universal (x86_64 + arm64) on macOS; `WINCC=… scripts/get-deps.sh` also cross-compiles it for the
Windows viewer (`third_party/dav1d-win`). The script applies two patches: `patches/lunasvg.patch`
(a repeated closepath lost the rest of the path; masks zoomed in far allocated layers the size of
the whole element) and `patches/librevenge.patch` (embedded images pass through without base64).

## macOS

```
make                 # nitroview, for the CPU it is built on (-march=native)
make app             # nitroview.app: universal (Intel + Apple Silicon), macOS 12+, ad-hoc signed
make zip / make dmg  # nitroview.zip / nitroview.dmg of the app, for passing it on
make dmg notarize SIGN_ID="Developer ID Application: Name (TEAMID)" NOTARY_PROFILE=name
```

Options on the make command line (the defaults depend on what `scripts/get-deps.sh` has built):

| option | effect |
|---|---|
| `TURBOJPEG=1` | the JPEGs nitrojpeg does not decode (damaged or cut-off, CMYK, arithmetic coding) go to libjpeg-turbo before ImageIO: about 2× faster on them, 1.2 MB more; `--selftest` needs it |
| `WUFFS=1` | interlaced PNGs through Wuffs before ImageIO |
| `DAV1D=0` | AVIF through ImageIO instead of nitroavif + dav1d |
| `SVG=0`, `CDR=0` | without SVG / CorelDRAW |
| `MIMALLOC=0` | the SVG / CorelDRAW code with the system malloc instead of mimalloc |
| `NITROHEVC=1` | HEIC with our own HEVC decoder, nitrohevc, instead of the system's (off by default, see [HEIC](README.md#heic)) |
| `MAC_ARCHS=x86_64` | `make app` for one architecture only |

Without any optional dependency the viewer needs only macOS frameworks.

**Passing the app on:** the app is ad-hoc signed, so another Mac refuses it the first time:
right-click → Open (up to macOS 14), System Settings → Privacy & Security → Open Anyway (macOS 15),
or `xattr -dr com.apple.quarantine nitroview.app`. With an Apple Developer ID, `make dmg notarize
SIGN_ID=… NOTARY_PROFILE=name` signs it (hardened runtime), has Apple notarize the dmg and staples
the ticket to it (once before: `xcrun notarytool store-credentials name`). The dmg in the Releases
is made this way by the workflow in `.github/workflows/macos-release.yml`, run in
[mgelei's fork](https://github.com/mgelei/jpeg-nitro) with their Developer ID
(`gh attestation verify nitroview-vN.dmg --repo mgelei/jpeg-nitro` checks the provenance).

## Windows

```
make windows WINCC=…/llvm-mingw/bin/x86_64-w64-mingw32-clang
```

cross-compiles `nitroview.exe` (with its icon), `bench/nbench.exe` and `bench/psdverify.exe`:
static, only system DLLs, Windows 10 or later, built for AVX2 (x86-64-v3). `WINARCH=x86-64-v2` builds
for CPUs without AVX2 (switching it rebuilds everything Windows, the SVG / CorelDRAW libraries too).
AVIF needs dav1d built for Windows first: `WINCC=… scripts/get-deps.sh` with the same compiler. The
`SVG`, `CDR` and `NITROHEVC` options apply here too.

## Linux

### Nix development environment

Run `nix-shell` in the repository root to load `shell.nix`, which provides the
compilation environment. Then follow [Optional dependencies](#optional-dependencies)
and the Linux build instructions below.

A configured `nixpkgs` channel is required; alternatively, use
`nix-shell -I nixpkgs=/path/to/nixpkgs`.

### Build commands

```
make                 # = make linux: build/lib/libnitro.so + libnitro.a, and the portable tools
make gnome           # nitroview-gnome (GTK 4 + libadwaita), the decoders linked in statically
make gnome-register  # its desktop file and icon for the current user
make flatpak         # nitroview-gnome.flatpak on the GNOME 50 runtime
make sdl             # nitroview-sdl (SDL3 + OpenGL)
make sdl-dist        # nitroview-linux-x64.tar.gz (on ARM: -arm64): the viewer as one portable file
```

**GNOME:** needs GTK 4.6+ and libadwaita 1.1+ (Ubuntu 22.04 is enough; there clang also needs
`libstdc++-12-dev` for nitrosvg). Before GTK 4.10 textures are drawn only with a linear filter:
large images get mipmaps made on the CPU, and the pixel-exact 200–800% zoom is smoothed.
`make flatpak` needs Flathub's `org.flatpak.Builder` where the distribution's own flatpak-builder
is too old for a GNOME 50 build (Ubuntu 22.04's is).

**SDL:** SDL3 is linked in statically (built by `scripts/get-deps.sh`); OpenGL and Wayland / X11 are
loaded at run time, so the viewer needs no GTK. `make sdl-dist` builds for x86-64-v3 instead of the
build machine's CPU, with libstdc++, zlib and dav1d linked in, so the tarball needs only glibc
(2.35+ when built on Ubuntu 22.04). On 64-bit ARM Linux (Asahi Linux on an Apple M1 / M2, Raspberry
Pi 4 / 5) the same makes `nitroview-linux-arm64.tar.gz`, built for armv8-a (tested on an M1 MacBook
Air under Fedora Asahi Remix 44, KDE Plasma on Wayland). It builds on macOS too, only for testing:
there OpenGL is slow, the Metal viewer is the one to use.

## Using the decoders in your own program

`make lib` builds `build/libnitro.a` (on Linux `make linux-lib`: `build/lib/libnitro.a` and
`libnitro.so`, exports in `src/libnitro.map`; the viewers' builds make their own copy per target):
all the decoders and `nitro_load`, the viewers' common path from a file to a decoded image.

- `nv_read_image()` reads what is needed of the file: a PSD's merged image only, a camera raw's
  embedded JPEG only, otherwise the whole file.
- `nv_decode()` recognises the format and decodes it with its decoder into one buffer, and fills
  an `NvImage` that says what is in it: YUV planes (JPEG, HEIC, AVIF, WebP), a PSD's planes, 8-bit
  BGRX pixels (TIFF, DNG, palette / 16-bit PNG) or a PNG's unfiltered rows, plus the EXIF
  orientation and the ICC profile.
- `nitro_convert` (`nc_yuv_to_rgb()`, `nc_psd_to_rgb()`) makes RGB of the planes, in parallel, for
  a program without a GPU shader for them.

`examples/nitroconvert.c` is the whole thing in a hundred lines: any image the library decodes to a
24-bit BMP, with its orientation applied. `make examples/nitroconvert` builds it; by hand:

```
clang -O3 -fblocks -Isrc examples/nitroconvert.c build/libnitro.a -lpthread -lm
```

plus `third_party/dav1d/lib/libdav1d.a` when the library was built with dav1d; on macOS
`-framework CoreFoundation` instead of `-lm`, and `-framework VideoToolbox -framework CoreMedia
-framework CoreVideo` for HEIC (unless built with `NITROHEVC=1`).

**The decoders one by one** are self-contained too (C, pthreads, optional AVX2 / NEON), all of the
same shape: `*_read_info()` says whether the decoder takes the file and gives its size and
orientation, `*_decode()` writes the pixels.

| decoder | files | decodes | output |
|---|---|---|---|
| nitrojpeg | `src/nitrojpeg.c/.h` | baseline and progressive JPEG: 8-bit Huffman, 1 or 3 components, any chroma subsampling | planar Y/Cb/Cr (`nj_decode_planes()`); `nj_find_embedded()` finds a camera raw's full-size JPEG, with the orientation the JPEG itself lacks |
| nitropng | `src/nitropng.c/.h` | non-interlaced PNG | the unfiltered rows, each after its filter byte (row y at `out + y * (stride + 1) + 1`); palette, 1 / 2 / 4 / 16-bit and transparency-key PNGs (`info.convertible`) to BGRX with `np_to_bgrx()`; `np_zlib_decompress()` is the parallel inflate on its own, for any zlib stream of known size |
| nitropsd | `src/nitropsd.c/.h` (needs nitropng) | a PSD / PSB's merged image, 8-bit RGB or gray | the planes one after the other: R, G, B or gray, then transparency if `info.alpha` |
| nitrotiff | `src/nitrotiff.c/.h` (needs nitropng) | the first full-size image of a TIFF | 8-bit BGRX; `nt_dir_size()` gives the size from the directory alone (a Photoshop TIFF has it at the end of the file) |
| nitrodng | `src/nitrodng.c/.h` | a DNG's raw image: 2×2 Bayer, uncompressed 8 / 16-bit or lossless JPEG | 8-bit sRGB BGRX, developed (the default crop) |
| nitroheic, nitrowebp, nitroavif | `src/nitroheic.c/.h`, `src/nitrowebp.c/.h`, `src/nitroavif.c/.h` (with `src/heif.h`) | HEIC (through the system's HEVC decoder, or nitrohevc), lossy WebP, AVIF (with dav1d) | planar Y/Cb/Cr |

nitrojpeg, for example:

```c
#include "nitrojpeg.h"

nj_info info;
if (nj_read_info(data, len, &info) == 0 && info.supported) {
    uint8_t *planes[3];
    size_t pitch[3];
    for (int c = 0; c < info.ncomp; c++) {
        pitch[c] = info.plane_w[c];                       // any pitch >= plane_w[c]
        planes[c] = malloc(pitch[c] * info.plane_h[c]);   // padded to whole MCUs
    }
    if (nj_decode_planes(data, len, &info, planes, pitch, 0 /* all CPUs */, NULL) == 0) {
        // info.width x info.height visible pixels; chroma subsampled by
        // info.h[c] / info.hmax horizontally and info.v[c] / info.vmax vertically
    } else {
        // damaged or unsupported stream: use another decoder
    }
}
```

nitrojpeg and nitropng keep their work buffers for the next image; `nj_free_buffers()` /
`np_free_buffers()` free them. All the decoders need from the OS is in `src/nitro_os.h`: a parallel
loop (a small pthread pool, the same on every system), a clock and the CPU count. They need clang
with `-fblocks`; no blocks runtime library is needed.

## Tests and benchmarks

`make tools` (macOS) builds `bench/bench`, `verify`, `robust`, `freqprobe`, `sustain`, the PNG,
PSD and BGRX checks, `rgbverify` and `nbench`; on Linux `make` builds the portable ones (`nbench`,
`pngverify`, `psdverify`, the ASan robustness tests, and `verify`, `robust`, `rgbverify` once
`scripts/get-deps.sh` has built libjpeg-turbo 3: the TurboJPEG 3 API is needed, older system
packages don't have it). They look for images in `samples/`: put your own there, or a symlink. The
measurements in [BENCHMARKING.md](BENCHMARKING.md) were made on 50 private photos that are not in the
repo.

`bench/nbench` is the portable benchmark: it reads each JPEG, PNG or PSD file into memory and decodes
it several times (`-r`, default 5); `-j N` limits the threads, `-1` also runs single-threaded.
`nbench.exe` runs on Windows with the same results (wildcards are expanded by the program,
directories work as arguments too). By hand:

```
clang -O3 -march=native -fblocks bench/nbench.c src/nitrojpeg.c src/nitropng.c src/nitropsd.c -lpthread -o nbench
clang -O3 -fblocks bench/pngverify.c src/nitropng.c -lz -lpthread -o pngverify   # byte-exact check vs zlib
```

The parts of nitrojpeg that need libjpeg-turbo are behind `#ifdef NJ_REFERENCE`: the old engine
where libjpeg-turbo decodes the bands, the CPU BGRX output and the single-threaded pass experiment
(`build/nitrojpeg_ref.o`, only the tests and benchmarks use them); in the viewer the TurboJPEG
fallback is behind `#ifdef NV_TURBOJPEG`. The viewer's own test modes (`--bench`, `--selftest`,
`--zoomtest`, …): [BENCHMARKING.md](BENCHMARKING.md#the-viewers-test-and-benchmark-modes).

## Files

**Decoders** (`src/`)
- `nitrojpeg.c/.h`: parallel JPEG decoder (own Huffman decoding, AVX2 / NEON IDCT); with `NJ_REFERENCE` also the libjpeg-turbo based comparison code
- `nitropng.c/.h`: parallel PNG decoder; `png_wuffs.c/.h`: the optional Wuffs path for interlaced PNGs
- `nitropsd.c/.h`: parallel PSD / PSB merged-image decoder
- `nitroheic.c/.h`: HEIC (HEIF) reading, the tiles handled in parallel; `nitrohevc.c/.h`: our optional HEVC intra decoder (`NITROHEVC=1`)
- `nitroavif.c/.h`: AVIF (AV1 through dav1d, our colour conversion); `heif.h`: the HEIF structure, shared with nitroheic
- `nitrodng.c/.h`: DNG raw development (lossless JPEG, demosaicing, colour)
- `nitrowebp.c/.h`: lossy WebP (VP8) decoder
- `nitrotiff.c/.h`, `tiff.h`: TIFF decoder (strips / tiles in parallel)
- `nitrosvg.cpp/.h`: SVG with lunasvg; `nitrocdr.cpp/.h`: CorelDRAW with libcdr, drawn with plutovg; `nitroalloc.cpp`: their mimalloc hookup
- `nitro_os.h`: what the decoders need from the OS (parallel loop, clock, CPU count)

**The library**
- `src/nitro_load.c/.h`: file → decoded image with the decoders above (the library's face, what the viewers use); `src/nitro_convert.c/.h`: its planes → RGB
- `src/libnitro.map`: the exported API of the Linux shared library
- `examples/nitroconvert.c`: the example program on the library, any image → BMP
- `src/nitro_prefetch.c/.h`, `src/nitro_view.h`: the viewers' shared prefetching loader and zoom arithmetic

**The viewers**
- `src/nitroview.m`: Cocoa + Metal; `src/shaders.metal`: its GPU shaders (colour conversion, drawing), embedded at build time; `packaging/Info.plist`, `packaging/icon.png`
- `src/nitroview_win.c`: Win32 + Direct3D 11 + WIC; `packaging/nitroview.ico/.rc`: its icon
- `src/gnome/`: GTK 4 + libadwaita; `packaging/gnome/`: desktop integration; `packaging/flatpak/`: the Flatpak manifest
- `src/nitroview_sdl.c`: SDL3 + OpenGL
- `scripts/get-deps.sh`: downloads and builds the optional dependencies; `patches/`: the lunasvg and librevenge fixes it applies

**Tests and benchmarks** (`bench/`)
- `bench.m`: decoder benchmark (macOS, all decoders); `nbench.c`: the portable one (nitrojpeg / nitropng / nitropsd)
- `verify.c`: bit-exactness and sync statistics vs libjpeg-turbo; `rgbverify.c`: nitro_convert's RGB vs libjpeg-turbo's; `bgrxverify.m`: nitrotiff's and nitropng's BGRX vs ImageIO
- `robust.c`, `pngrobust.c`, `psdrobust.c`: damaged files under ASan
- `pngbench.m`, `pngsplit.c`: PNG decoder comparison and time split; `pngverify.c`: nitropng byte-exactness vs zlib
- `mkpsd.py`: test PSD writer (every compression); `psdverify.c`: all compressions vs raw
- `freqprobe.c`: clock measurement; `sustain.c`: sustained load; `ab.sh`: noise-resistant A/B comparison
- `results/`: measured results

## Third-party licences

The viewer is MIT ([LICENSE](LICENSE)); the optional dependencies downloaded by
`scripts/get-deps.sh` have their own licences: libjpeg-turbo (BSD-style / IJG / zlib), Wuffs
(Apache-2.0), stb_image (public domain / MIT), SDL3 (zlib), dav1d (BSD-2-Clause), lunasvg and
plutovg (MIT; plutovg's rasterizer is FreeType's smooth rasterizer, FreeType License: portions of
this software are copyright © 2014 The FreeType Project (www.freetype.org), all rights reserved),
libcdr and librevenge (MPL-2.0; our libcdr changes are in github.com/gereoffy/libcdr, the librevenge
one in patches/), lcms2 (MIT), boost (Boost Software License), mimalloc (MIT). The inverse DCT
implements the algorithm of jidctint.c from the Independent JPEG Group's libjpeg: this software is
based in part on the work of the Independent JPEG Group.
