# Linux port: agent handoff and review plan

Rewritten 2026-09-11 after recovering the prior Codex session
(`~/.codex/sessions/2026/09/11/rollout-*-01a09292-*.jsonl`).

## What actually happened in the previous session

The prior session set a standing goal to complete `linux-port-plan.md`, reached a
compiling game core, then delegated three sub-tasks (`video`, `io`,
`asset_review`) and **died on a provider usage limit**, not a technical blocker.
Recovered evidence from the sub-agent rollouts:

- `video`: 2 tool calls, both reads (`CocoaBridge.h/.m`, `U3Bitmap.h`,
  `U3LegacyDrawing.h`). **Wrote nothing.**
- `io`: 4 tool calls; read `U3IO.h`/`U3IOLegacy.m`/`U3GameState.h`, then one
  `apply_patch` creating `Sources/Linux/U3IOLinux.{c,h}`. Never compiled or
  tested it. **That draft does not build.**
- `asset_review`: 2 tool calls, both reads. **Wrote nothing, reviewed nothing.**

Treat the previous version of this document as aspirational: the ownership list
in it described work that was assigned but never performed.

## Verified current state

Verified by inspection and rebuild on 2026-09-11, not taken from prior notes.

**Working:**

- Portable CMake build, asset export pipeline (Python + Pillow →
  `build/linux-game/assets/manifest.json`), `U3Bitmap` + tests.
- `Sources/Linux/U3LegacyMemory.c` — handles, Pascal numeric/string helpers,
  geometry. Tested.
- `Sources/Linux/U3LinuxAssets.c` — resource bytes, UTF-8 strings, scaled RGBA
  images from the manifest via JSON-C. Tested. Reviewed this session: handles
  path traversal, size limits, and malformed input sanely.
- `u3game-core` compiles all preserved game/renderer sources as Linux C objects
  via `Sources/Linux/U3LegacyTypes.h` + `U3LegacyDrawing.h`, which already
  *declare* the Carbon/QuickDraw/CoreFoundation surface (`CFStringRef` is just
  `const char *` here). `build/core-errors.log` is stale — those errors are fixed.
- All CTest suites pass under ASan/UBSan (leak detection off under tracing).

**Not working / absent:**

- No executable target and no entry point. Nothing runs.
- `Sources/Linux/U3IOLinux.{c,h}`: does not compile (missing `_DEFAULT_SOURCE`,
  so `strdup`/`mkstemp`/`O_DIRECTORY` are undeclared; `kind_code` used before
  its `static` declaration). Not wired into CMake. Written as dense one-liners.
- No SDL video/input, platform services, audio, or Carbon shim *definitions*.

## Remaining link surface: 147 symbols

Measured, not estimated — `nm` over `u3game-core` objects minus everything the
objects and existing Linux libraries define, minus libc and sanitizer symbols.
Regenerate with the script in "Reproduce" below.

| Bucket | Count | Owner file |
|---|---|---|
| `U3Cocoa*` — surface, drawing, text, input | 43 | `Sources/Linux/U3LinuxVideo.{c,h}` |
| Carbon/CF/menus/dialogs/cursor | 56 | `Sources/Linux/U3LinuxCarbon.{c,h}` |
| `U3Platform*` — timing, input, preferences | 18 | `Sources/Linux/U3LinuxPlatform.{c,h}` |
| `U3Audio*` + 5 song/sound globals | 19 | `Sources/Linux/U3LinuxAudio.{c,h}` |
| `U3IO*` — resources and saves | 11 | `Sources/Linux/U3IOLinux.{c,h}` (fix) |

The six high-level `U3IOLoadGame`/`SaveGame`/`LoadRoster`/`SaveRoster`/
`LoadWorld`/`SaveWorld` entry points in `U3IO.h` are **not** referenced by
`u3game-core`; they are not required to link and are out of scope for now.

## Work split

Five parallel implementation tasks, one owner per file set. Each is independently
verifiable by a standalone compile, so no task needs the build wiring.

1. **video** — `U3LinuxVideo.{c,h}`: the 43 `U3Cocoa*` symbols over SDL2.
   Preserve the software bitmap and original command codes; no re-rendering.
2. **carbon** — `U3LinuxCarbon.{c,h}`: the 56 CF/menu/dialog/cursor symbols.
   `CFStringRef` is `const char *`; implement real refcount-free semantics that
   match how the game actually uses them.
3. **platform** — `U3LinuxPlatform.{c,h}`: the 18 `U3Platform*` symbols, with
   preferences under XDG and `U3_SAVE_DIRECTORY` honored for tests.
4. **audio** — `U3LinuxAudio.{c,h}`: real SDL2 PCM effect playback; MIDI/music
   may report unavailable, but must not fake success.
5. **io** — fix `U3IOLinux.{c,h}` so it compiles, is readable, and round-trips
   saves durably, with tests.

Boundary rules for implementation tasks:

- Touch only your own files plus your own new test file. Do **not** edit
  `CMakeLists.txt`, any shared `Sources/*.c`, `U3LegacyTypes.h`,
  `U3LegacyDrawing.h`, or another task's files.
- If a shared header genuinely needs a change, report it instead of making it.
- Match the declared signatures exactly. Do not invent stub success paths; a
  function that cannot work yet must report failure honestly.
- Verify with the standalone compile command below before reporting.

Integration (primary agent, not delegated): `CMakeLists.txt`, the Linux entry
point, linking the executable, reviewing each diff against original behavior,
running tests, and pushing verified checkpoints.

## Reproduce

Standalone compile for any Linux adapter file (note the multiarch include dir —
the staged SDL2 `SDL_config.h` forwards to `SDL2/_real_SDL_config.h`, which
lives only under `x86_64-linux-gnu`):

```sh
clang -c -std=c11 -fpascal-strings -Wall -Wextra \
  -I Sources -I Sources/Linux \
  -I build/deps/sysroot/usr/include \
  -I build/deps/sysroot/usr/include/x86_64-linux-gnu \
  Sources/Linux/<file>.c -o /tmp/<file>.o
```

SDL2 links against `/usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0` (no `.so`
symlink; there is no `sdl2` pkg-config in the staged sysroot). Runtime SDL2,
JSON-C and FluidSynth are installed; dev headers are staged under
`build/deps/sysroot` because sudo needs a password. Do not retry sudo.

Full build and tests:

```sh
cmake -S . -B build/linux-game -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug \
  -DU3_BUILD_GAME_CORE=ON -DU3_SANITIZERS=ON
cmake --build build/linux-game --parallel 2
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/linux-game --output-on-failure
```

Regenerate the unresolved-symbol inventory:

```sh
nm -g build/linux-game/CMakeFiles/u3game-core.dir/Sources/*.o build/linux-game/*.a
# undefined (U) minus defined, minus libc and __asan/__ubsan
```

## Status

Milestone 1 (dependency and data path) is met. Milestone 2 (boot to a rendered,
responsive window) is not: no executable exists yet. No gameplay, save/load,
audio, or packaging has been verified.
