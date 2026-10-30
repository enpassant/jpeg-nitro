<p align="center"><img src="docs/logo.webp" alt="NitroView – fast image viewer for macOS, Windows and Linux" width="800"></p>

# jpeg-nitro

**English** | [Magyar](README.hu.md)

**NitroView** is a very fast image viewer for **macOS, Windows and Linux**. It opens photos, scans,
Photoshop files, raw images and vector drawings, and it is built on its own decoders, which decode
every image on multiple CPU cores: a 24-megapixel JPEG opens in about 11 ms on a desktop Intel CPU, ten
times faster than the system's decoder, so a photo series pages as fast as you press the key. SVG
and CorelDRAW drawings are rendered sharp at any zoom level.

There is no user interface to speak of: a window with the picture in it, use keys and the mouse to
page and zoom.

| platform                                          | viewer                                                    |
| ------------------------------------------------- | --------------------------------------------------------- |
| macOS 12+, Intel and Apple Silicon                | `nitroview.app` (Cocoa + Metal, universal)                |
| Windows 10+ with Direct3D 11+                     | `nitroview.exe` (one self-contained file)                 |
| Linux with GNOME                                  | `nitroview-gnome` (GTK 4 + libadwaita), also as a Flatpak |
| Linux, any desktop (Wayland / X11), x64 and arm64 | `nitroview-sdl` (SDL3 + OpenGL, one self-contained file)  |

All four behave the same: the same keys, mouse and trackpad gestures, zoom and window behaviour, full
screen, slideshow and prefetching. The differences are listed under [Platform notes](#platform-notes).

## Download and install

Ready-made builds are available on GitHub: [Releases](https://github.com/gereoffy/jpeg-nitro/releases).

**macOS:** open the dmg and drag `nitroview.app` to Applications. The dmg is notarized, so it opens
without a Gatekeeper warning. To make it the default viewer: in the Finder, Get Info on an image →
Open with → nitroview → Change All. Images or folders can also be dropped on its Dock icon or its
window, or opened with ⌘O; a running viewer takes the new files.

**Windows:** unzip `nitroview-windows-x64.zip` anywhere, there is nothing to install.
`nitroview.exe --register` offers it for all image types in Explorer (current user, no admin rights)
and opens Settings → Default apps, where you pick it (Windows lets only the user choose the default);
`--unregister` removes it again. Files opened while it runs appear in the running window (`-n`: a new
one). The `_noavx2` zip is for CPUs without AVX2 (AMD FX, Intel before 2013). HEIC photos need
Microsoft's "HEVC Video Extensions" from the Store.

**Linux with GNOME:** `flatpak install --user nitroview-gnome.flatpak`. HEIC photos need the
`org.freedesktop.Platform.codecs-extra` extension from Flathub.

**Linux, any desktop:** unpack `nitroview-linux-x64.tar.gz` (or `-arm64` for Asahi Linux on an
Apple M1 / M2 and the Raspberry Pi 4 / 5): one file, which needs only glibc 2.35 or newer.
`nitroview-sdl --register` adds it to the desktop's applications for the current user ("Open with"
in the file manager), `--unregister` removes it. This viewer opens no HEIC files (see
[HEIC](#heic)).

Building it yourself, the make options, and using the decoders in your own program:
[BUILDING.md](BUILDING.md).

## Usage

```
nitroview [-f] [-s ms] [-j n] image.jpg ... | directory/
```

Give it files, a folder, or a single file: paging then moves through the other images of its
folder, in the file manager's order (the folder is listed only on the first paging key, so macOS
asks for folder access only if you actually page). `-f` starts in full screen, `-s ms` a slideshow
`-j n` limits cpu threads. Run without arguments (or with `-h`) for all options; the Windows and
SDL viewers open a file dialog when started without files.

### Keys

| key                           | action                                                                                                         |
| ----------------------------- | -------------------------------------------------------------------------------------------------------------- |
| PgDn / Space                  | next image                                                                                                     |
| PgUp / Backspace              | previous image                                                                                                 |
| Home / End                    | first / last image                                                                                             |
| [ / ]                         | previous / next page of a multi-page CorelDRAW drawing (title: [page 2/5])                                     |
| + / −                         | zoom in / out (√2 steps, stops exactly at 100% and at fit)                                                     |
| 0                             | fit to the screen: small images are enlarged too (stays on while paging, until W)                              |
| 1                             | actual size: one image pixel = one screen pixel                                                                |
| 2 … 8                         | 200% … 800%, pixel-exact: one image pixel = N×N screen pixels                                                  |
| ← → ↑ ↓                       | move a zoomed image (Shift: half a screen per step)                                                            |
| W                             | back to the image's own size (small images at 100%), the window follows image sizes again                      |
| E / Shift + E                 | DNG raw images: brighter / darker by 1/3 EV (up to ±3 EV; kept while paging, shown in the title)               |
| Del (Mac: also ⌘ + Backspace) | delete: the file is moved to the `.delete` folder next to it (hidden), the next image follows; nothing is lost |
| U / Ctrl + Z (Mac: ⌘ Z)       | undo the last delete: the file goes back to its place                                                          |
| F / Enter                     | toggle full screen                                                                                             |
| P                             | pause / resume the slideshow                                                                                   |
| Esc / Q                       | quit (Esc in full screen: back to a window)                                                                    |

### Mouse and trackpad

| gesture                             | action                                                                                      |
| ----------------------------------- | ------------------------------------------------------------------------------------------- |
| drag                                | move a zoomed image                                                                         |
| scroll wheel                        | zoom around the point under the cursor (one √2 step per notch; trackpad: smooth, pinch too) |
| double click                        | fit → 100% at the clicked point; again → back to fit                                        |
| right click / Shift + right click   | next / previous image                                                                       |
| Ctrl + scroll wheel                 | previous / next image (trackpad: one image per 40 px)                                       |
| mouse side buttons (back / forward) | previous / next image                                                                       |

### The window and the zoom

- The window takes the image's size: exactly 100% if it fits the screen, otherwise as large as fits
  with the image's aspect ratio (no black bars). Zooming in grows the window with the image up to the
  screen size, zooming out shrinks it back; 0 restores it.
- When paging, the window follows each image's size, keeping its centre. After you resize the window
  yourself (edge drag, green button, tiling) it keeps that size; W brings it back to the image size
  and turns the automatic sizing back on.
- **Zoom and position are kept when paging**: in a burst or series you can zoom into a detail (the
  eyes, say) and page through to find the sharpest shot. While zoomed in, the window, zoom and
  position stay as they are.
- Integer zoom levels (200%, 300%, 400%, …, also when reached with the wheel or +/−) show the pixels
  as exact square blocks; other levels are smoothly interpolated.
- Wheel zoom keeps the point under the cursor in place; while the image is narrower than the window
  along an axis it stays centred there until it fills the window.
- The title shows the file name, size, zoom and decode time. EXIF orientation is honoured. Paging
  stops at the first and last image.

### Slideshow

`-s <ms>` (or `--slideshow <ms>`): the next image comes when the current one has been on screen for
`ms` milliseconds (`-s 2000` is a classic slideshow, `-s 40` is ~25 images/s). If an image takes
longer to load, it waits, so no image is skipped. It stops at the last image; P pauses; the paging
keys keep working and the slideshow continues from there. Timing follows the display refresh (±8 ms
at 60 Hz).

### Tips

- If Ctrl + scroll zooms the whole screen on a Mac, macOS Accessibility zoom is set to use Ctrl with
  the scroll gesture (System Settings → Accessibility → Zoom). If pinching does nothing, the gesture
  is turned off (System Settings → Trackpad → Scroll & Zoom → Zoom in or out).
- The zoom keys work by character, so they work on any keyboard layout and on the numeric keypad.
- Delete only moves the file into the hidden `.delete` folder next to it; U undoes it, and the folder
  can be emptied (or restored) by hand at any time.

### Platform notes

|                       | macOS                          | Windows                        | GNOME                                              | SDL (Linux)                                                    |
| --------------------- | ------------------------------ | ------------------------------ | -------------------------------------------------- | -------------------------------------------------------------- |
| open files / a folder | ⌘O, drop on the icon or window | O / Shift + O, drag & drop     | Open dialog, drag & drop                           | O / Shift + O                                                  |
| default viewer        | Finder: Change All             | `--register`                   | the Flatpak, or `make gnome-register`              | `--register`                                                   |
| HEIC                  | yes (VideoToolbox)             | with the HEVC Video Extensions | with codecs-extra                                  | no                                                             |
| colour management     | ICC profiles honoured          | sRGB                           | sRGB                                               | sRGB                                                           |
| formats not ours      | Apple ImageIO                  | WIC                            | GTK's loaders                                      | stb_image (GIF, BMP, interlaced PNG, damaged / CMYK JPEG only) |
| window                | follows the image              | follows the image              | keeps its size while paging (Wayland), 0 maximizes | follows the image                                              |

The GNOME viewer was contributed by Ferenc Czirok. Windows touchpad gestures are untested.

## Supported formats

| format                                   | what opens                                                                                                                                                                                                           | not supported                                                                                                                        |
| ---------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------ |
| **JPEG**                                 | baseline and progressive, any chroma subsampling, greyscale; bit-exact with libjpeg-turbo                                                                                                                            | CMYK, arithmetic coding, damaged or cut-off files                                                                                    |
| **PNG**                                  | all non-interlaced PNGs: 8 / 16-bit, palette, 1 / 2 / 4-bit, transparency key, alpha (shown over black)                                                                                                              | interlaced PNGs (rare)                                                                                                               |
| **HEIC**                                 | iPhone photos (8-bit 4:2:0 HEVC, also the tiled ones), with the system's HEVC decoder: see [HEIC](#heic)                                                                                                             |                                                                                                                                      |
| **AVIF**                                 | 8 / 10 / 12-bit, 4:2:0 / 4:2:2 / 4:4:4 / grey, alpha, grids, crop / rotation / mirroring (AV1 by [dav1d](https://code.videolan.org/videolan/dav1d))                                                                  | a grid of layered tiles (two Apple test files)                                                                                       |
| **WebP**                                 | lossy (VP8)                                                                                                                                                                                                          | lossless and transparent WebP                                                                                                        |
| **TIFF**                                 | uncompressed, PackBits, LZW, Deflate, CCITT G3 / G4; 1–16-bit grey, palette, RGB, CMYK, alpha; strips or tiles, chunky or planar                                                                                     | JPEG-compressed TIFF                                                                                                                 |
| **PSD / PSB**                            | the merged (composite) image of 8-bit RGB and greyscale files, all four compressions; merged transparency shown over black                                                                                           | other colour modes and depths; files saved without "Maximize Compatibility" have no merged image and show white (as in every viewer) |
| **DNG**                                  | developed at full resolution from the raw data (Bayer, lossless JPEG or uncompressed): a quick look at what is in the picture, not a raw converter; E / Shift+E for the exposure control                             | no noise reduction, lens corrections or other DNG opcodes                                                                            |
| **camera raw** (CR2, NEF, NRW, ARW, RAF) | the full-size JPEG the camera stored in the file (only that part of the file is read)                                                                                                                                |                                                                                                                                      |
| **SVG**                                  | shapes, paths, gradients, clipping, masks, patterns, `<style>` sheets, text with the system's fonts, embedded images                                                                                                 | filters (drop shadows and blurs are left out), HTML in `<foreignObject>` (draw.io), SVG fonts and symbol-only sprites draw nothing   |
| **CorelDRAW** (CDR 5 – X6, CMX)          | drawings, text (with similar fonts where the original is missing), embedded bitmaps, multi-page documents ([ / ]), hidden layers skipped as in CorelDRAW; the shown area is the drawing's bounding box, not the page | arrowheads, Corel's own vector and bitmap pattern fills                                                                              |
| **everything else** (GIF, BMP, …)        | through the operating system's decoder (ImageIO, WIC, GTK's loaders; stb_image in the SDL viewer)                                                                                                                    |                                                                                                                                      |

Vector drawings are rendered first in full, so paging, fitting and zooming out work as for photos.
Zoomed in beyond that, the visible part is rendered again at screen resolution once the zoom or
position has been still for a moment, so pinching stays smooth and the picture sharpens when it
stops.

### HEIC

HEIC photos are HEVC (H.265) pictures, and HEVC is covered by patents that need a licence: the
operating systems' decoders come with one, our own implementation does not. So the binaries we
distribute decode HEIC only with the system's decoder: VideoToolbox on macOS (nitroheic hands it an
iPhone photo's tiles in parallel), WIC on Windows (Microsoft's HEVC Video Extensions must be
installed), GTK's loaders in the GNOME viewer (the Flatpak needs the codecs-extra extension); the
SDL viewer then shows no HEIC files at all. Our own HEVC decoder, nitrohevc (intra-only, written
from the H.265 specification; it leaves out the deblocking and SAO filters, which cannot be seen on
photos), is only in a build you make yourself with `make NITROHEVC=1` ([BUILDING.md](BUILDING.md#macos)),
for your own use: the patents are then your concern.

### Damaged files

When one of our decoders detects an error (an invalid Huffman code, data ending early, a wrong
number of restart markers, …) it gives up, and the viewer falls back to the system's decoder, which
shows whatever is recoverable (the top part of a half-downloaded file, say). If nothing can decode
it (unknown format, broken header, empty file), the title shows `CANNOT DECODE` and paging continues.

### Colour management

On macOS embedded ICC profiles are honoured for every format (Display P3 from iPhones and Mac
screenshots, Adobe RGB scans, …); images without a profile are treated as sRGB, and wide-gamut
colours are kept on a wide-gamut display. The Windows, GNOME and SDL viewers show every image as
sRGB (the profile is ignored).

## Why it is fast

Every format is decoded by our own decoder, which decodes a single image on several CPU cores, even for
formats whose compression is sequential by design: JPEG's and DNG's huffman stream and PNG's deflate stream
are decoded speculatively in parallel pieces. The inverse DCT uses AVX2 on Intel and NEON on ARM, the
colour conversion and the scaling run on the GPU, and the viewer decodes the next images ahead while
you look at the current one.

| format           | test image                | libnitro   | reference decoder                                |
| ---------------- | ------------------------- | ---------- | ------------------------------------------------ |
| JPEG             | 24 MP photo               | **11 ms**  | libjpeg-turbo 107 ms (1 thread)                  |
| progressive JPEG | 24 MP photo               | **79 ms**  | libjpeg-turbo 336 ms                             |
| PNG              | 38 real PNGs, average     | **43 ms**  | ImageIO 318 ms, Wuffs 171 ms                     |
| PSD              | 24 MP, RLE                | **5 ms**   | ImageIO 170 ms                                   |
| HEIC             | 12 MP iPhone photo        | **33 ms**  | ImageIO 200 ms                                   |
| DNG              | 20 MP raw                 | **8 ms**   | ImageIO 210–510 ms                               |
| WebP             | 42 MP                     | **100 ms** | ImageIO 360 ms                                   |
| TIFF             | 13 MP 16-bit scan (73 MB) | **2 ms**   | ImageIO 31 ms                                    |
| AVIF             | 41 web AVIFs, average     | **10 ms**  | ImageIO 32 ms                                    |
| SVG              | 38 MB drawing             | **0.53 s** | WebKit 1.4 s, CoreSVG 2.4 s                      |
| CorelDRAW        | 17.7 MB all-vector plan   | **1.8 s**  | libcdr's `cdr2xhtml` 5.4 s (SVG conversion only) |

Measured on an Intel i9 desktop. On a 2020 M1 MacBook Air a 24 MP JPEG decodes in ~30 ms, three
times faster than Apple's hardware JPEG decoder on the same machine. A folder of 24 MP photos plays
back at the display's refresh rate, 60 images/s, so a timelapse can be watched at full resolution in
real time. How each decoder does it, and all the measurements: [BENCHMARKING.md](BENCHMARKING.md).

## Building and the library

[BUILDING.md](BUILDING.md): building the viewers on every platform, the make options, and
`libnitro`, the decoders as one library for your own program (with a hundred-line example that
converts any image it decodes to a BMP).

## License

MIT, see [LICENSE](LICENSE). The optional third-party components (libjpeg-turbo, dav1d, SDL3,
lunasvg and plutovg, libcdr and librevenge, lcms2, boost, mimalloc, stb_image, Wuffs) have their
own licences: [BUILDING.md](BUILDING.md#third-party-licences). This software is based in part on
the work of the Independent JPEG Group.
