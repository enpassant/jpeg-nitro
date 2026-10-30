# How the decoders work, and the measurements

[README](README.md) | [Building](BUILDING.md) | **Benchmarking**

Developed and measured on an Intel x86-64 Mac (i9 13th gen, 8 cores / 16 threads, AMD RX 580).
Also runs on Apple Silicon (M1 MacBook Air, see [below](#apple-silicon-m1)); there the IDCT uses NEON instead of AVX2.

## The decoders at a glance

| decoder | format | how | time | usual decoders |
|---|---|---|---|---|
| **nitrojpeg** | baseline and progressive JPEG | speculative parallel Huffman decoding, AVX2 IDCT; progressive: the scans as a pipeline, row by row; bit-exact with libjpeg-turbo | 24 MP photo: **~11 ms**; progressive: **~79 ms** | libjpeg-turbo ~107 ms (1 thread); progressive ~336 ms |
| **nitropng** | PNG (non-interlaced; palette, 1–16-bit, transparency key too) | speculative parallel inflate (back-references into not yet known data resolved later, Adler-32 verified) + "wavefront" filter reversal | 38 real PNGs: **~43 ms** avg. | Wuffs ~171 ms, ImageIO ~318 ms |
| **nitropsd** | Photoshop PSD/PSB (8-bit RGB / gray) | merged image: RLE rows in parallel, ZIP via nitropng's inflate; reads only the merged image of huge layered files | 24 MP RLE: **~5 ms** | ImageIO ~170 ms |
| **nitroheic** (macOS) | HEIC (8-bit 4:2:0 HEVC, iPhone grids) | own HEIF parsing; the 48 independent HEVC tiles of an iPhone photo decoded in parallel by VideoToolbox into Y/Cb/Cr for the GPU | 12 MP iPhone photo: **~33 ms** | ImageIO ~200 ms |
| **nitrodng** | DNG raw (Bayer; lossless JPEG or uncompressed) | own raw development: lossless JPEG in parallel (a single strip too: speculative Huffman decoding, as in nitrojpeg), black / white levels, white balance, Malvar–He–Cutler demosaicing, colour matrix, tone curve | 20 MP DNG: **~8 ms**; 25 MP in one lossless JPEG strip: ~22 ms | ImageIO 210–510 ms / 490–630 ms |
| **nitrowebp** | lossy WebP (VP8) | own VP8 decoder (RFC 6386): the coefficient tokens (one serial stream) on one thread, the modes and the reconstruction on others; Y/Cb/Cr for the GPU | 42 MP WebP: **~100 ms**; 137 everyday WebPs: **27 ms** avg. | ImageIO ~360 ms; 54 ms |
| **nitrotiff** | TIFF (none, PackBits, LZW, Deflate, CCITT G3 / G4; 1–16-bit gray, palette, RGB, CMYK, alpha; strips / tiles, chunky / planar) | strips and tiles in parallel (Deflate also through nitropng's parallel inflate); uncompressed: converted straight from the file, in parallel | 13 MP 16-bit scan (73 MB): **~2 ms**; 17 MP LZW: **~6 ms** | ImageIO ~31 ms; ~46 ms |
| **nitroavif** | AVIF (AV1 in HEIF: 8–12-bit, 4:2:0 – 4:4:4, gray, alpha, grids, crop / rotation / mirroring) | own HEIF parsing (shared with nitroheic), AV1 by [dav1d](https://code.videolan.org/videolan/dav1d); grid tiles decoded in parallel, our own parallel colour conversion (chroma upsampled linearly) | 41 web AVIFs: **~10 ms** avg. (M1: 10.8 ms; Windows i5: 13 ms) | ImageIO ~32 ms (M1: 22 ms); WIC: only with a Store extension |
| **nitrosvg** | SVG (without filters) | lunasvg parser + plutovg renderer, with font lookup in the system and user font folders | 38 MB SVG: **~0.53 s** | WebKit ~1.4 s, CoreSVG ~2.4 s |
| **nitrocdr** | CorelDRAW 5 – X6 (CDR), CMX | our [libcdr fork](https://github.com/gereoffy/libcdr) with many fixes (text styles, line spacing, clipping, bitmap transparency) and a 2× faster librevenge; draws directly with plutovg | 17.7 MB all-vector CDR: **~1.8 s**, ~3× faster | original libcdr `cdr2xhtml` ~5.4 s (only converting to SVG) |

Times measured on the Intel i9. The decoders are fast on ARM too (NEON): on a 2020 M1 MacBook Air a
24 MP JPEG decodes in ~30 ms, ~3× faster than Apple's hardware JPEG decoder on the same machine
([Apple Silicon](#apple-silicon-m1)). The SDL and Windows viewers share one GPU path: the planes as
textures, YCbCr→RGB in a shader, mipmaps, trilinear drawing, pixel-exact 200–800%.

## The viewer's test and benchmark modes

The window title shows the file name, size, zoom and decode time; stdout gets one line per image
with the read time, the decode time and the key → on-screen latency.

Other modes:
- `--bench files…`: load everything without a window (read, decode, GPU) and print timings.
- `--selftest files…`: compare the GPU colour conversion with libjpeg-turbo's output.
- `--zoomtest files…`: replays zoom/pan key sequences offscreen and checks that at 100% every
  drawn pixel equals the decoded image (plus edge clamping and zoom snapping).
- `--inputtest files…`: sends synthesized mouse and wheel events and checks paging and zoom.
- `--auto <ms>`: test mode: pages every `ms` (even if the image isn't shown yet) and quits
  at the end. For latency measurements.

## Results (50 photos, mostly 6000×4000, 17 MB on average)

**Full load in the viewer: ~23 ms/image** (file read 3.5 + decode 12 + GPU 7).
When paging, a prefetched image reaches the screen in **7–20 ms** (one frame at 60 Hz).
A folder of 24 MP images plays back continuously at **~60 images/s** (the display refresh
rate), also from an SSD with nothing cached.

### Decoder comparison (`bench/bench`, decode only, from memory)

| decoder | ms/image |
|---|---|
| Apple ImageIO (CGImageSource) → BGRA | 274 |
| Apple ImageIO, native buffer | 249 |
| stb_image | 175 |
| Wuffs | 159 |
| ImageIO thumbnail 3000 px (DCT scaling) | 159 |
| VideoToolbox (JPEG; no HW JPEG on this machine, runs in software) | 157 |
| libjpeg-turbo 3.1.2 (AVX2), BGRX | 124 |
| libjpeg-turbo, fast DCT + fast upsampling | 112 |
| libjpeg-turbo → YUV planes (no colour conversion) | 107 |
| libjpeg-turbo, 1/2 scale | 100 |
| libjpeg-turbo, 1/4 scale | 96 |
| nitrojpeg, own engine, **1 thread** → YUV | 103 |
| nitrojpeg, old engine (libjpeg-turbo decodes the bands) → YUV | 20.6 |
| **nitrojpeg, own engine, parallel → YUV planes** (used by the viewer) | **11.7** |

1/4 scale barely helps, so most of the time goes into Huffman decoding, which is
inherently sequential in JPEG. Most camera JPEGs have no restart markers, so libjpeg-turbo
can't parallelise within one image. The RX 580 has no hardware JPEG decoder.

### Apple Silicon (M1)

A 2020 M1 MacBook Air (4 performance + 4 efficiency cores, no fan), `bench/bench` on 43 of the
photos (`samples/*.JPG`, 765 MB), ms/image. Raw output, also with the i9 on the same 43 files:
`bench/results/m1.txt`.

| decoder | ms/image |
|---|---|
| stb_image | 384 |
| Wuffs | 217 |
| libjpeg-turbo, BGRX | 185 |
| libjpeg-turbo → YUV planes | 168 |
| Apple ImageIO → BGRA | 108 |
| Apple ImageIO, native buffer | 89 |
| VideoToolbox | 88 |
| ImageIO thumbnail 3000 px | 67 |
| nitrojpeg, **1 thread** → YUV | 154 |
| **nitrojpeg, parallel → YUV** | **30.4** |

On the M1, ImageIO and VideoToolbox decode JPEG in hardware: 2.5–3× faster than the software
path on the i9 (table above). nitrojpeg (NEON IDCT, 8 threads) is still ~3× faster than that
hardware at full size and 5.5× faster than libjpeg-turbo. One M1 core is ~1.5× slower than an i9
core on this work (1 thread: 154 ms, the i9 103 ms in the table above); the threads scale well: 2 → 81 ms, 4 → 46 ms, from 6 on 33–36 ms (the four
efficiency cores add roughly one performance core). The NEON IDCT brought 8 threads from 43 to
31 ms. The default (one thread per core, 8) is the best setting there.

PNG (`bench/pngbench samples_png/real/*.png`: 34 real PNGs, 938 MB, 805 Mpixel — screenshots,
scans, AI images, maps; ms/image):

| decoder | M1 | i9 |
|---|---|---|
| Apple ImageIO, native buffer | 364 | 345 |
| Apple ImageIO → BGRA | 392 | 376 |
| Wuffs → BGRA | 272 | 180 |
| nitropng, 1 thread | 326 | 223 |
| **nitropng, parallel** | **90** | **45.5** |

There is no hardware PNG decoder, so ImageIO is equally slow on both. On the M1 nitropng is 3×
faster than Wuffs and 4× faster than ImageIO. Its Adler-32 and back-reference resolving use NEON
there (AVX2 on x86): that took it from ~100 to 90 ms; most of the time is Huffman decoding
and the row filters, which are plain C on both.

HEIC (a small test program, not in the repository: the 48 tiles of 20 iPhone photos, 12 MP, through VideoToolbox in
different ways, decoder sessions included; the last row: `nitroview --bench` on all 155; ms/image):

| decoder | M1 | i9 |
|---|---|---|
| Apple ImageIO | 58–60 | 176 |
| VideoToolbox hardware, 1 session | 52 | 135 |
| VideoToolbox hardware, 4 (M1) / 8 (i9) sessions | **24–29** | 224 |
| VideoToolbox software, 4 / 8 sessions | 62–75 | **42** |
| **nitroheic** (in the viewer) | **25** | **33** |

The M1's media engine serves 4 tiles at once, the i9's Radeon RX 580 one at a time (it is slower
with more sessions), while the software decoder scales with the cores: so nitroheic uses the
hardware on Apple Silicon and one software decoder per core on Intel.

WebP (`nitroview --bench` on everyday lossy WebPs from the web, without synthetic noise and quality 90
photos: 119 files on the M1, 137 on the i9; decode, ms/image):

| decoder | M1 | i9 |
|---|---|---|
| Apple ImageIO | 42.9 | 54.2 |
| **nitrowebp** | **25.5** | **27.2** |

WebP has no hardware decoder either. nitrowebp's pace is set by its one serial stream of
coefficient tokens, where a single M1 core does well, and so does ImageIO there: 1.7× on the M1,
2× on the i9.

DNG (`nitroview --bench` on the 14 DNGs of [DNG raw images](#dng-raw-images-nitrodng); decode, ms/image):

| decoder | M1 | i9 |
|---|---|---|
| Apple ImageIO | 401 | 360 |
| **nitrodng** | **34.4** | **22.5** |

ImageIO shows the three Leica M9 DNGs only as their 320×216 previews, which makes its average
look better; nitrodng develops all of them at full size, 12× faster on the M1, 16× on the i9.

Progressive JPEG (`nitroview --bench` on 100 progressive web JPEGs over 1 MB, 5.8 MP on average;
decode / with the GPU upload and conversion, ms/image):

| decoder | M1 | i9 | i5-9600K, Windows |
|---|---|---|---|
| libjpeg-turbo (TurboJPEG, the viewer's earlier path on macOS) | 43.4 / 47.1 | 41.9 / 47.9 | |
| WIC (the viewer's earlier path on Windows) | | | 118 / 128 |
| **nitrojpeg** (the scans as a pipeline) | **10.6 / 15.8** | **9.5 / 14.3** | **11.0 / 18.3** |

4× faster on the M1, 4.4× on the i9, 10.7× faster than WIC on the i5 (6 cores without
hyper-threading; the Windows viewer also builds mipmaps, 1.5 ms). On the i5 one thread takes
29.3 ms (`bench/nbench -1`), 4 workers 11.0, 6 workers (`-j 6`) 11.8. The 16-bit IDCT (pmaddwd / vmlal) made a difference only on
one thread (its 32-bit vector multiplies are slow on the i9); 4 workers instead of all cores took
the M1 from 11.3 to 10.6 ms and the i9 from 11.7 to 9.5 ms (more can't be kept busy, see
[Progressive JPEG](#progressive-jpeg); `-j` sets more).

### Three machines, macOS and Linux

`bench/nbench` (portable, same code on every system), best of 5 decodes, ms/image; JPEG: the 50
sample photos, PNG: the 34 real PNGs above. The M1 row is from `bench/bench` / `bench/pngbench`
(JPEG: 43 of the photos).

| machine | JPEG, parallel | JPEG, 1 thread | PNG, parallel | PNG, 1 thread |
|---|---|---|---|---|
| Intel i9 13th gen, 8 cores / 16 threads, macOS | **8.7** | 96 | **37.6** | 216 |
| Apple M1, 4 + 4 cores, macOS | **30.4** | 154 | **90** | 326 |
| Intel Xeon E3-1245 v5 (2015), 4 cores / 8 threads, Linux | **33.6** | 161 | **122** | 390 |

On Linux the decoders run on the pthread pool of `src/nitro_os.h` and are bit-exact there too
(`bench/verify`, `bench/pngverify`). A ten-year-old 4-core server decodes a 24 MP photo in
~34 ms, like the M1; hyper-threading adds ~20% for JPEG (4.8× on 4 cores), little for PNG (3.1×).

Intel Core i7-1260P (4 P + 8 E cores / 16 threads, Arch Linux, 64 GB RAM), `bench/nbench`,
best of 5 decodes on a different image set (4 JPEGs, 5 PNGs): JPEG **5.4 ms** parallel /
32.3 ms on 1 thread; PNG **13.4 ms** / 35.4 ms. A 12000×12000 (144 MP) RLE PSD:
**68.2 ms** / 112.4 ms on 1 thread. The image set differs from the table above, so these
numbers are not directly comparable with its three rows.

On the same machine, best of 5 on one 6000×6000 JPEG and one 2000×1970 PNG:

| decoder | JPEG | PNG |
|---|---:|---:|
| **Nitro** | **14.8 ms** | **12.9 ms** |
| Glycin 2.1.0 load + frame | 184.5 ms | 37.6 ms |

That is **12.5×** for JPEG and **2.9×** for PNG. Nitro is `bench/nbench` decode time;
the Glycin number includes `gly_loader_load()` + `gly_image_next_frame()`.

### DNG raw images (nitrodng)

`nitroview --bench` on 14 DNGs of 6 cameras, decode (the raw data read and developed) in ms/image:
the i9 on macOS and a 6-core Intel i5-9600K (no hyper-threading) on Windows; the last column is
the viewer before nitrodng, through Apple ImageIO, on the i9.

| camera, raw data | image | file | i9 | i5-9600K | ImageIO (i9) |
|---|---|---|---|---|---|
| DJI L2D-20c, uncompressed 16-bit | 20.8 MP | 41 MB | **8** | **31** | 210–510 |
| DJI FC4382, one lossless JPEG tile | 12.1 MP | 16–19 MB | **21–26** | **42** | 270–300 |
| Ricoh GR IV, one lossless JPEG strip | 25.6 MP | 30–32 MB | **22** | **69–77** | 490–630 |
| Leica M9, 8-bit with a linearization table | 18.1 MP | 17.5 MB | **6–9** | **24–27** | only a 320×216 preview |
| Oppo Find X6 Pro, uncompressed 16-bit | 12.6 MP | 27.5 MB | **11** | **19.5** | ~150 |
| Canon EOS 350D via Adobe DNG Converter, 140 lossless JPEG tiles | 8 MP | 6 MB | **13–16** | **18** | 170–190 |

On the i5 the average went from 119 to 44 ms/image: the development (demosaicing, colour matrix,
curve) vectorised 2–3×, and a single lossless JPEG strip or tile is decoded in parallel like
nitrojpeg's Huffman data (the Ricoh from ~210 to ~70 ms, the DJI FC4382 from ~125 to 42 ms).

### TIFF (nitrotiff)

`src/nitrotiff.c`, written from the TIFF 6.0 specification: uncompressed, PackBits, LZW and Deflate
(with the horizontal predictor), CCITT G3 (MH, T.4 1D / 2D) and G4 for bilevel images; 1–16-bit gray,
palette, RGB and CMYK (a naive conversion, no profile), alpha over black, strips or tiles, chunky or
planar. Strips and tiles are decoded in parallel, then the rows converted to BGRX in parallel; an
uncompressed image (scanners write those: 16-bit RGB, 70 MB in one strip) is converted straight from
the file. JPEG-compressed and YCbCr TIFFs go to the system's decoder. On macOS an RGB TIFF's ICC
profile is used, as ImageIO did.

`bench/bgrxverify` (macOS) compares it with ImageIO: 63 scans, faxes, icons and documents plus 33
variants made with ImageMagick and libtiff (every compression, tiles, planar, 16-bit, alpha, fill
order) are pixel-identical (palettes ±1: the 16-bit colour map's rounding; CMYK differs: ImageIO
converts with the profile). The output is the same on Linux and with any thread count; damaged
files (truncated, mutated directories) run clean under AddressSanitizer. In the viewer (i9, the 63
real TIFFs) the decode went from 14.0 to 3.1 ms/image: a 13 MP 16-bit scan from ~58 to ~2.3 ms,
a 17 MP LZW drawing from ~46 to ~6 ms; single-strip G4 faxes stay sequential (~2 ms).

### How nitrojpeg parallelises a single image

The Huffman stream is decoded **once**, without libjpeg-turbo:

1. **Preparation (parallel, ~1 ms):** find the end of the entropy-coded data, remove byte
   stuffing (0xFF 0x00) and restart markers, giving a "clean" bit stream.
2. **Files without restart markers: speculative full decoding.** The bit stream is cut into
   ~512 chunks (`32 × CPUs`, ~200 KB of coefficients each, which fits in L2). Every chunk is
   decoded by a thread that starts at an arbitrary byte offset, writing coefficients right
   away. Huffman codes self-synchronise with the true decoding path after a few MCUs.
3. **Stitching, in order, pipelined.** When the true path starts an MCU at a bit position
   that the next chunk also recorded as an MCU start, both paths are identical from there.
   This gives the chunk's MCU index and per-component DC offset; the few MCUs before the
   sync point are discarded.
4. **Dequantisation + IDCT** with AVX2. The algorithm is libjpeg's "ISLOW" IDCT (jidctint.c),
   so the output is bit-identical. As soon as a chunk is stitched, the thread that decoded it
   runs its IDCT while the coefficients are still in cache.
5. **Files with restart markers:** no speculation; the intervals are decoded in parallel with
   the IDCT done per MCU right away.
6. The output is planar Y/Cb/Cr, written directly into a Metal (shared) buffer. Upsampling
   and YCbCr→RGB run in a GPU compute kernel, the GPU builds mipmaps, and the image is drawn
   with trilinear filtering.

Measured synchronisation (`bench/verify`, 512 chunks, first 256 sync points per image,
40 images = 10240 cases): on average a chunk joins the true path after 2.40 "garbage" MCUs
(~922 bits, ~115 bytes). In 40% of the cases after 1 MCU; the worst case was 21 MCUs
(8754 bits), and synchronisation never failed.

**Accuracy:** the parallel output is **bit-identical** to single-threaded libjpeg-turbo on all
50 test images (`bench/verify`, with both the scalar and the AVX2 IDCT). The GPU colour
conversion differs by at most ±1 (rounding, `--selftest`). Unsupported files (CMYK, RGB,
arithmetic coding, >16384 px) and damaged or cut-off files go to the system's decoder in the viewer
(ImageIO, WIC, GTK). Of ~4100 JPEGs downloaded over the years none does.

### How it got there (ms/image, 16 threads)

| version | ms/image |
|---|---|
| libjpeg-turbo, 1 thread | 107 |
| speculative skip pass + libjpeg-turbo decodes 128 re-emitted "band" JPEGs (Huffman twice) | 19.9 |
| same, but a single-threaded Huffman pass instead of speculation (`NJ_SEQ=1`, old engine) | 67.2 |
| own engine: Huffman once, coefficients kept in memory, then IDCT | 15.7 |
| + single-lookup AC decoding, pipeline: IDCT while the chunk is in cache | **11.7** |

The single-threaded Huffman pass is slow because it is a serial dependency chain: the length
of each code must be known before the next one can be read (~12 cycles/symbol). Without the
pipeline, the IDCT was memory-bound: writing and re-reading the 96 MB coefficient array
saturated at ~45 GB/s and did not scale beyond 8 threads.

Chunk count (`NJ_CHUNKS`), ms/image:

| chunks | 64 | 128 | 256 | 512 | 1024 | 2048 |
|---|---|---|---|---|---|---|
| ms/image | 12.3 | 12.0 | 11.9 | **11.4** | 12.1 | 14.7 |

Measurements of the old engine (libjpeg-turbo bands, `nj_set_engine(1)`, `NJ_ENGINE=1`) are in
`bench/results/matrix.txt` (chunks × threads) and `bench/results/bands.txt` (band count).

### Measurement method: clock speed and temperature

The development machine's clock jumps between 4 and 5.5 GHz and the CPU passes 90 °C within
seconds under 16-thread load, so a single measurement can be off by ±10%.

- `bench/freqprobe [threads] [seconds]`: measures the real core clock without root (a chain
  of dependent adds runs at one add per cycle). Measured: 4.48 GHz single-threaded from idle
  (the scheduler raises the clock slowly), ~4.95 GHz with 16 threads, ~5.3 GHz with 8.
- `bench/sustain SECONDS THREADS files…`: continuous decoding, speed + clock every 0.5 s.
  Over 30 s with 16 threads there is no downward trend (10.1–11.9 ms/image), so the cooling
  holds; the spread comes from clock jumps, not throttling.
- `bench/ab.sh REPS PAUSE -- "name:ENV=value" …`: runs variants interleaved, with a cool-down
  pause and a clock measurement before each run; prints median / min / max. This keeps the
  min–max spread within about ±3%.

Re-measured this way (5 repetitions, 3 s pause, ms/image, `bench/results/ab_results.txt`):

| comparison | median (min–max) |
|---|---|
| own engine / old engine (libjpeg-turbo bands) | **11.8** (11.8–11.9) / 19.8 (19.5–20.8) |
| 4 / 8 / 12 / 16 threads | 27.0 / 15.4 / 12.3 / **11.8** |
| 256 / 512 / 1024 chunks | 11.9 / **11.7** / 12.2 (overlapping ranges: within noise) |

### Progressive JPEG

A progressive JPEG comes in many scans (usually 9–10): the DC coefficients, bands of the AC
coefficients, then refinement scans that add one more bit to every coefficient. Each scan is its
own Huffman stream, decoded one after the other by the usual decoders. Measured scan by scan
(libjpeg-turbo) on 1521 progressive web images and 50 24 MP photos converted with
`jpegtran -progressive`, the Y AC refinements take ~60% of the time and the output (IDCT, colour)
6–15%. Decoding the independent scans in parallel would give only 1.3–1.6×: the Y scans depend on
each other. But a scan only needs *the same block rows* of the earlier scans of the same
coefficients, so nitrojpeg runs the scans as a pipeline (a wavefront): every scan advances by a few
MCU rows as soon as the scans before it are far enough, and the IDCT follows the last ones. The
time is then bounded by the longest single scan instead of the sum of all scans. The refinement
scans are also decoded differently: libjpeg walks the band one coefficient at a time (a correction
bit for each nonzero one, counting the zero ones), nitrojpeg lists a block's zero coefficients once
and jumps to the end of a run, which makes the decoding 1.5× faster on one thread too. 4 workers by default (the longest scan is a
quarter to a third of the work, more threads can't be kept busy and only slow it down; `-j` sets
more):

| ms / MP | nitrojpeg | nitrojpeg, 1 thread | libjpeg-turbo (Y/Cb/Cr) | libjpeg-turbo (BGRX) |
|---|---|---|---|---|
| 50 × 24 MP photos (jpegtran) | **3.31** (~79 ms) | 9.35 | 14.02 (~336 ms) | 14.32 |
| 1509 web images (2.7 MP avg.) | **1.66** | 3.50 | 5.33 | 5.61 |

The output is bit-identical to libjpeg-turbo on all of them (`bench/verify` checks progressive
files against its Y/Cb/Cr planes, with all threads and with one). 12 more web images (not in the
table) are low-quality previews without the last scans, some with only the DC (8×8 blocks of one
colour each): nitrojpeg decodes what is there, as Apple ImageIO does. libjpeg-turbo also estimates
the missing low AC coefficients from the neighbouring blocks ("block smoothing", softer but
blurred); nitrojpeg doesn't, `bench/verify` compares these with libjpeg-turbo without smoothing
(identical, also on 270 test files cut back to their first scans). Damaged or cut-off files go to
the system's decoder.

### nitropng

PNG = deflate (LZ77 + Huffman) + per-row filters, both sequential by design:

1. **Parallel inflate (speculative, like *pugz*):** the compressed stream is cut into one chunk
   per thread. Every thread finds the first valid deflate block header in its chunk (Huffman
   tables must be exactly complete, so false positives are rare and get rejected anyway) and
   decodes from there. Back-references into the still unknown 32 KB before the chunk are written
   as placeholders (16-bit symbols). The chunks are chained in order; only the last 32 KB of each
   need to be resolved sequentially, the rest in parallel (AVX2). The Adler-32 of the whole
   output (combined from per-chunk sums, AVX2) must match, otherwise the viewer falls back.
2. **Parallel filter reversal ("wavefront"):** None / Sub rows are independent; Up / Average /
   Paeth rows follow the previous row segment by segment (1 KB), on 12 of the 16 threads
   (waiting hyper-threads would slow down their working siblings). With performance and efficiency
   cores (Apple M1, Intel 12th generation) only on the performance cores: a row on a slower core
   holds up every row after it (the M1 under Linux: 4 threads instead of 6, about +20%). Under
   Linux every parallel loop of the decoders with no more items than performance cores runs on
   those (`src/nitro_os.h`; also WebP's three jobs: +15% on the M1).
3. The unfiltered rows go straight to the GPU, which reads them in place (skipping the filter
   byte), premultiplies alpha and converts to RGBA — no CPU colour conversion.

`bench/pngverify` checks the output byte-for-byte against zlib + a straightforward filter
reversal (all 33 supported test PNGs identical), `bench/pngrobust` runs damaged PNGs with ASan +
UBSan (no errors), `--selftest` checks the whole path against ImageIO (pixel-exact).

### PNG: why it was not parallel

`bench/pngbench` (decoders) and `bench/pngsplit` (where the time goes), on 24 MP photos saved
as PNG:

| decoder | ms/image |
|---|---|
| Apple ImageIO | 339 |
| Wuffs | 193 |
| zlib inflate alone | 91 |
| filter reversal alone (plain C) | 67–134 |

PNG is deflate (LZ77 + Huffman with per-block tables) plus per-row filters. A thread starting
in the middle of the stream does not know the previous 32 KB of output that back-references
point to, and 97% of the rows used the Paeth filter, which depends on the previous row. Both
can be parallelised in principle: speculative inflate with placeholder back-references resolved
later (as in *pugz*), and a "wavefront" filter reversal where row *r* trails row *r−1*. That
could bring a 24 MP PNG to perhaps 20–40 ms (the estimate at the time — see nitropng above
for what it became).


### AVIF (nitroavif)

AV1 stills in a HEIF container, as webshops and CDNs send them more and more. The HEIF structure is
read by our own code (`src/heif.h`, shared with nitroheic), AV1 is decoded by
[dav1d](https://code.videolan.org/videolan/dav1d) (VideoLAN, BSD-2-Clause), the colour conversion is
ours, in parallel: 8 / 10 / 12-bit, 4:2:0 / 4:2:2 / 4:4:4 / gray, BT.601 / 709 / 2020 / RGB
matrices, full or limited range, alpha over black, grids of tiles decoded in parallel. Both Linux
viewers use it (GTK has no AVIF loader without an extra package), and so do the macOS and Windows
viewers when built with dav1d (otherwise ImageIO, and WIC with the AV1 Store extension). Not handled:
a grid of layered tiles (two Apple test files).

Decoding 41 AVIFs saved from
webshops and news sites (300×200 … 3000×3000), ms/image: i9 (macOS) ImageIO 31.7 → nitroavif 10.0, M1
22.4 → 10.8, i5-9600K (Windows) 13.0 (WIC without the Store extension: none of them). The same pixels within ±1 for 4:4:4 and for luma,
4:2:0 chroma differs at colour edges because ImageIO repeats chroma samples where we interpolate
linearly (as for JPEG). Checked on the AOM test files ([av1-avif](https://github.com/AOMediaCodec/av1-avif))
and [Link-U's samples](https://github.com/link-u/avif-sample-images) against their PNG references:
every bit depth, chroma format, monochrome, limited-range alpha, grids, layered images (a1op, data
in several pieces), crops (also after a rotation, in the order the properties give) and mirroring
match them; ImageIO gets several of these wrong (limited-range gray and alpha, the crop after a
rotation, profile 2 files it cannot decode). The output is identical on Linux and with any thread count; damaged files run clean under
AddressSanitizer. (This also fixed nitroheic's `imir` axis: 0 exchanges top and bottom.)

### PSD and PNG in the viewer

**PNG** (nitropng, `src/nitropng.c`): 8-bit gray / RGB / RGBA rows go to the GPU as they are;
palette, 1 / 2 / 4 / 16-bit and transparency-key PNGs are converted to BGRX in parallel. The rare
interlaced ones go to the system's decoder (on macOS with `make WUFFS=1` to
[Wuffs](https://github.com/google/wuffs) first). Transparent images are shown over black.

**PSD** (nitropsd, `src/nitropsd.c`): the merged (composite) image of 8-bit RGB and grayscale files
(all four compressions: raw, RLE, ZIP, ZIP with prediction); other PSDs go to the system's decoder
(Apple ImageIO cannot read ZIP-compressed PSDs at all). Big layered PSDs are mostly layer data (the
merged image is often only 13–23% of the file), so the viewer reads just the header, the image
resources (ICC profile) and the merged image. Merged transparency (negative layer count): Photoshop
stores the colour matted with white — in a test file every partially transparent pixel had every
channel ≥ 255 − alpha — so nitropsd shows it over black as max(0, colour + alpha − 255). ImageIO
does not remove the matte (light fringes on soft edges). A 4th channel with a positive layer count
is a saved selection, not transparency. Files saved without "Maximize Compatibility" contain only a
white placeholder instead of the merged image; the layers are not composited, so they show white (as
in ImageIO). On 55 real PSDs (Photoshop work, maps, architecture) the result matches ImageIO except
for these two points.

A 24 MP RLE PSD decodes in ~5 ms
(ImageIO: ~170 ms), ZIP in ~46 ms: the RLE row table gives every row's position, so all rows
are unpacked in parallel; ZIP uses nitropng's parallel inflate.
A 482 MB layered PSD loads in ~20 ms instead of ~155 ms (only the header, the image resources and
the merged image are read).

On 38 real PNGs
(screenshots, scans, AI images, upscaled textures, Photoshop exports) nitropng averages
**~43 ms/image** vs ~171 ms for Wuffs and ~318 ms for ImageIO; see [nitropng](#nitropng). On 25
palette / 1–16-bit / transparency-key PNGs the viewer's decode went from 53 (Wuffs) to 18 ms/image,
pixel-identical with ImageIO (`bench/bgrxverify`; a 26 MP 16-bit RGB from ~940 to ~195 ms).

### SVG and CorelDRAW

**SVG** (nitrosvg, with [lunasvg](https://github.com/sammycage/lunasvg) fetched by
`scripts/get-deps.sh`): rendered by lunasvg over white, first in full with the long side 1024–4096
pixels (small icons are enlarged, huge drawings reduced; fitted to the screen, in full screen or
after key 0, at least the screen's size, the prefetched images too, so they are sharp right away),
so paging, fitting and zooming out work as for photos. Zoomed in beyond that, the visible part is
rendered again at screen resolution once the zoom or position has been still for ~80 ms (typically
5–40 ms); until then the previous rendering is scaled, so pinching stays smooth and the picture
sharpens when it stops. Supported: shapes, paths, gradients, clipping, masks, patterns, `<style>`
style sheets, text (with the system fonts), embedded images; before parsing, nitrosvg also fixes
three things lunasvg would get wrong: `svg:`-prefixed elements (LibreOffice), CSS
`var(--x, fallback)` colours (VS Code's images: the fallback, or without one the inherited value;
black squares otherwise) and SVG 1.1 line breaks in text (old exporters split words over lines). Not
supported: filters (drop shadows and blurs are left out), HTML labels in `<foreignObject>` (draw.io
diagrams), SVG fonts and symbol-only sprite files, which draw nothing. Of 883 SVGs collected from a
Mac (icons, logos, diagrams, Illustrator exports) 867 open; the 16 others draw nothing in a browser
either.

**CorelDRAW** (nitrocdr, `src/nitrocdr.cpp`, with libcdr fetched by `scripts/get-deps.sh`): libcdr
(from LibreOffice, used by Inkscape and LibreOffice Draw too) reads the drawing; nitrocdr records
what it reports as a display list and draws it with plutovg directly, without an SVG step, so
embedded scans and photos stay decoded bitmaps in memory. The shown area is the drawing's bounding
box (with a small margin), not the page size set in the file, so a small logo fills the window; the
pages of a multi-page document share one area (the bounding box of all of them), so what lies above
each other on them, like the floors of a building, stays in place when paging. Paging, zooming and
the sharp detail work as for SVG ([ and ] switch between the pages, keeping the zoom and the
position). libcdr comes from our fork, [gereoffy/libcdr](https://github.com/gereoffy/libcdr), which
has the fixes this needed, one commit each: open paths are not filled; text keeps its rotation, its
horizontal stretch, bold and italic, its exact size and the shifts that place the words of imported
PDF text, and is placed at its real origin; `CMYK100` colours (CorelDRAW 2022) are not white;
objects on hidden layers are skipped (an imported PDF keeps its switched-off layers as hidden
layers); symbol definitions do not make an extra page; bitmaps keep their transparent background and
are cut to their own outline (the wall hatch of imported plans); texts lost in CorelDRAW 5 and 7–X5
files show; and ICU is replaced by iconv (code pages; without a charset, windows-1250 when the text
has ő/ű). Fonts come from /Library/Fonts, /System/Library/Fonts and ~/Library/Fonts (Bitstream names
such as "Futura Lt BT" map to the family); a missing font is replaced by a similar one, so its text
may run a little longer or shorter. Lines are never thinner than one device pixel, as in CorelDRAW.
Not supported: arrowheads, Corel's own vector and bitmap pattern fills.

On 216
CDRs collected from a Mac (architectural plans from ArchiCAD PDFs, network maps, logos) every file
opens; on an Intel i9 the median takes 20 ms, the 76 files over 1 MB 230 ms on average, the
biggest plan 1.8 s (mostly libcdr's parsing). On macOS and in the Linux SDL viewer the C++ code
(libcdr, librevenge, lunasvg) allocates through [mimalloc](https://github.com/microsoft/mimalloc):
loading a big plan makes millions of small allocations, and with it plans load 20–25% faster on
Intel and Apple Silicon too (`make MIMALLOC=0`: the system malloc); 193 CDRs on the M1 under Linux,
16% faster than with glibc's malloc.

### Damaged files

`make bench/robust && bench/robust samples/*` creates ~90 damaged variants of every image
(truncation from 0 bytes to length−1, random byte and bit errors, zeroed or 0xFF-filled
blocks, markers inserted into the data, header damage, cut or duplicated sections) plus
garbage and text files, and runs them through the viewer's decode path built with
AddressSanitizer and UBSan: 4372 cases, 0 errors. The pipeline's synchronisation was also
checked with ThreadSanitizer (on valid and damaged files).

### Display and prefetching

- A background thread decodes 3 images ahead in the paging direction and 2 behind, and
  uploads/converts the neighbours on the GPU in advance.
- Metal buffers and textures are reused (allocating the 128 MB texture per image cost ~6 ms).
- A shared (system memory) buffer read directly by the conversion kernel was the fastest
  (8 ms vs 10 ms with managed buffers / textures).
- The decoder's large buffers (clean bit stream, coefficients) are reused from image to image.
- The GNOME viewer decodes on a background thread with the same prefetch (3 ahead, 2 behind); its
  JPEG colour conversion is multi-threaded and gives exactly libjpeg-turbo's RGB (`bench/rgbverify`),
  PNG rows go to GTK without a copy, and the EXIF orientation is applied when drawing.
- Colour management on macOS costs nothing: the decoded pixel values are left as they are and the
  display layer is tagged with the image's colour space (the embedded ICC profile, sRGB without one),
  so macOS converts to the monitor's profile itself and wide-gamut colours are kept.

### The parallel loop: from GCD to a pthread pool

On macOS the decoders used Grand Central Dispatch until October 2026; the pthread pool replaced
it so that every system runs the same code. It is as fast: the viewer's `--bench` on 10 formats,
on the M1 PNG 7% faster, HEIC (through VideoToolbox) 5% slower, the rest within 2%; on the Intel
i9 no difference. It passes all checks (`verify`, `pngverify`, `psdverify`, the ASan robustness
tests; ThreadSanitizer with four threads decoding every format at once: clean, apart from reads of
VideoToolbox's own output buffers, whose writes it cannot see, the same with GCD). Tested on Linux (see
[Three machines](#three-machines-macos-and-linux)).

## Further optimisation ideas (measured)

**Huffman tables are mostly the same.** Cameras and phones use the example tables of the JPEG
standard (Annex K): all 43 Sony A6300 photos, all 92 iPhone photos and 168 of the 178 Canon 50D
photos (the other 10 were re-saved in Photoshop, which writes optimised tables); of 2341 JPEGs
from the web, 1506 (~2/3) too, the rest mostly have their own optimised tables. Caching the
decoding tables would gain nothing, though: building them takes ~8 µs per image, 0.1–0.4% of the
decode. The time goes into *using* them.

**Multi-symbol tables** (possible with fixed tables, built once): single-threaded Huffman decoding
of 27 Sony photos, ms/image, all outputs identical:

| lookup table | table size | ms/image |
|---|---|---|
| **one symbol, 11 bits (nitrojpeg)** | 8 KB | **104.7** |
| one symbol, 12–16 bits | 16–256 KB | 105–141 |
| up to 3 symbols, 11–16 bits | 32 KB – 1 MB | 110–120 |
| two value symbols, 11–14 bits, else one | 16–128 KB | 103–105 (±1%) |

A 16-bit multi-symbol table yields 2.3 AC symbols per lookup, but it is not faster: the serial
chain (the next code starts where the previous one ended) stays, and the bigger table misses the
L1 cache. AC coefficients are 95.6% of the bits (29.7 non-zero per block in these photos).

**Lookup size in nitrojpeg** (`LOOK`, 8 bytes per entry, 4 tables per image), 43 Sony photos,
average of two interleaved runs, ms/image:

| `LOOK` | tables | parallel (16 threads) | 1 thread |
|---|---|---|---|
| 8 | 8 KB | 11.0 | 110.0 |
| 9 | 16 KB | 10.6 | 106.5 |
| 10 | 32 KB | 10.0 | 98.9 |
| **11** | **64 KB** | **10.0** | **98.3** |
| 12 | 128 KB | 10.1 | 99.4 |
| 13 | 256 KB | 10.5 | 101.0 |
| 14 | 512 KB | 11.2 | 112.4 |
| 15 | 1 MB | 12.4 | 122.4 |
| 16 | 2 MB | 14.4 | 140.4 |

11 bits is the optimum (10–12 is flat): with fewer bits more codes (and code + value pairs) miss
the lookup and take the slow path; with more, the two tables a block uses (DC + AC) no longer
fit the i9's 48 KB L1 cache. The output is bit-exact at every size. With a smaller L1 cache
(Xeon E3-1245 v5, Skylake, 32 KB, Linux) 10, 11 and 12 bits are equal within the noise (1 thread:
~174, ~171, ~173 ms/image), so 11 stays.

A trap on that Xeon: after a few edits that did not touch the Huffman code the parallel decode went from 34 to 40–43 ms,
and 13 bits looked 18% faster. It was code layout, not the table: Skylake-family CPUs (JCC
erratum microcode) drop code from the decoded-uop cache when a jump crosses or ends at a 32-byte
boundary. With clang's `-mbranches-within-32B-boundaries` (now in the Makefile for x86) every
version measures 34 ms at 11 bits (13 bits: 33, but 3% slower on the i9, where the flag changes
nothing).

The best size depends on the CPU (L1 cache, branch layout), so on your machine, especially an
older one, it is worth measuring 8 to 16 instead of the default 11. The size is a compile-time
option; each build stays bit-exact:

```sh
for L in 8 9 10 11 12 13 14 15 16; do make -B DEFS=-DLOOK=$L bench/nbench >/dev/null && printf "LOOK=$L " && bench/nbench -q -r 3 samples/*.jpg | tail -1; done
make -B DEFS=-DLOOK=13       # then build with the winner (DEFS goes into every target)
```

**Lookup size in nitropng** (literal/length table `LBITS`, distance table `DBITS`, 4 bytes per
entry, longer codes in sub-tables), 34 real PNGs, average of two interleaved runs, ms/image:

| `LBITS` | `DBITS` | parallel (16 threads) | 1 thread |
|---|---|---|---|
| 9 | 8 | 39.0 | 217.5 |
| 10 | 8 | 38.7 | 215.6 |
| **11** | **8** | **39.1** | **214.7** |
| 12 | 8 | 38.9 | 214.2 |
| 13 | 8 | 40.3 | 217.7 |
| 11 | 6 | 38.7 | 214.9 |
| 11 | 7 | 38.8 | 214.4 |
| 11 | 9 | 38.6 | 215.2 |
| 11 | 10 | 38.7 | 215.3 |

All within the noise (±1.5%), only 13 bits is slightly slower; the output is byte-exact at every
size. Unlike JPEG, the size hardly matters: the tables are small (the 11-bit literal table is 8 KB),
the common deflate codes are shorter than 11 bits, and much of the time goes into copying
back-references and reversing the row filters, which don't depend on the tables.
