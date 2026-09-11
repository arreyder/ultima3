# Linux foundation build

This builds the portable bitmap library and exports the game assets. It does
not yet build a playable game or open a window. The existing Xcode build is
unchanged.

## Build and test

On Ubuntu 24.04 or a derivative:

```sh
sudo apt-get install build-essential cmake python3 python3-pil
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build/linux --parallel
ctest --test-dir build/linux --output-on-failure
```

Python 3.10+ and Pillow are build-time dependencies. Specifying the interpreter
avoids accidentally selecting a virtual environment without Pillow. The
build does not download assets or dependencies.

For sanitizer checks, configure a separate directory with
`-DU3_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug`. Run CTest normally there. Under
ptrace-based environments where LeakSanitizer cannot run, use
`ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/linux-sanitize --output-on-failure`.
This disables leak checking only, not address or undefined behavior checks.

## Generated bundle

`build/linux/assets/manifest.json` is the authoritative file index and export
completion marker. It contains format version 1, resource type/ID/name/attribute
metadata, string counts, image dimensions/stride, byte lengths, and SHA-256
checksums. Paths are relative to the bundle. Files not listed in the manifest
are not part of the bundle; use a clean build directory for distribution.

- `resources/<hex-FourCC>/<signed-id>.bin`: exact original resource payloads.
  Names are metadata only, never filesystem paths. MacRoman names and signed
  resource IDs are preserved; lengths and offsets are read as big-endian.
- `strings/*.json`: UTF-8 string arrays preserving order, newlines, and empty
  entries. Index conversion must remain at the API boundary.
- `images/<original-path>.rgba`: tightly packed, top-down RGBA8 pixels with
  straight alpha, native image dimensions, and no row padding. The future
  renderer must composite into opaque `U3Bitmap` surfaces and apply the game's
  per-tile scaling. These are raw pixels, not encoded PNG files.
- Modern PCM/MIDI/SoundFont assets, the MP3 level-up effect, PDFs, and notices
  retain their original bytes. The optional downloaded FluidR3 bank is excluded
  so the baseline does not vary by developer machine.

The current input produces 303 resources in 45 types, 14 string tables, 116
images (92 PNG, 16 GIF, 8 JPEG), and 54 copied files, about 83 MiB total.
All bundled raster images have one frame. Generated files are ignored by Git.
Different image decoder versions may produce different JPEG pixels; checksums
describe the actual build output. Repeated builds with the same dependencies
produce identical manifests.

The tests verify every output checksum, resource byte preservation, all string
tables, and reconstruction of the Standard tile sheet and font. Synthetic
resource-fork fixtures check truncation, out-of-range offsets and lengths,
signed IDs, MacRoman names, and attributes. Assertions remain enabled for the
bitmap test even in Release builds.

## Dependency decision for the next milestone

Use SDL2 for the initial Linux backend: the Ubuntu 24.04 package index provides
`libsdl2-dev` 2.30.0, while the inspected baseline has no `libsdl3-dev` candidate.
`libfluidsynth-dev` 2.3.4 is available for later MIDI playback. These packages
were checked for availability, not installed or linked in this milestone.
The asset conversion removes the need for a runtime PNG/JPEG/GIF decoder;
Pillow performs that work at build time. Raw pixels increase disk use but keep
the first renderer implementation small. Compression can follow later.

References: [SDL Linux installation](https://wiki.libsdl.org/SDL2/Installation),
[Ubuntu FluidSynth package](https://packages.ubuntu.com/noble/libfluidsynth-dev).

Validated locally on x86-64 Pop!_OS 24.04 (Ubuntu Noble base), GCC 13.3,
Python 3.12.3, Pillow 10.2.0: Release and sanitized Debug each pass all three
CTest suites; independently generated manifests match. No macOS regression
run or interactive Linux gameplay verification has been performed.
