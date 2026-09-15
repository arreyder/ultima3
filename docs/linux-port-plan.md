# Native x86-64 Linux port

Prepared 2026-09-11 against `f13173e737a45cda4ae7807b87edceccb10ac03e`.
Personal fork: https://github.com/arreyder/ultima3
Upstream: https://github.com/hongjuny/ultima3

Milestone 1 implementation: see [build instructions and validation](linux-build.md)
and [platform/data inventory](linux-platform-inventory.md). The portable build
and asset extraction are working. SDL2 is selected for baseline package
availability, and Pillow converts images at build time. Runtime SDL/audio
linking and the first game window remain in the following milestones.

## Goal and scope

Build and run Ultima III natively on x86-64 Linux while preserving its game
rules, assets, and classic raster presentation. Start with Ubuntu 24.04 LTS
as the build baseline. Keep macOS support and make Linux additions separable
from the upstream implementation. A 32-bit x86 build is a later target, not
part of the initial acceptance criteria.

The first playable milestone includes character creation, world movement,
towns, combat, dungeons, and save/restart/load. Full sound, preferences,
alternate graphics, and desktop packaging follow. Speech and exact native
Mac dialog appearance can be deferred.

## Evidence and remaining constraints

- `U3Platform.h`, `U3Renderer.h`, `U3IO.h`, and `U3Audio.h` expose useful C
  boundaries; `U3Types.h` and `U3Bitmap` already use portable types.
- The existing bitmap test passed on x86-64 Linux with address and undefined
  behavior sanitizers. Leak detection was disabled because the review
  environment runs under tracing; normal CI should retain leak detection.
- `Ultima3_Prefix.pch` still imports Carbon globally. Game and presentation
  files still use legacy types and call `CocoaBridge` and `CarbonShunts`.
  Implementing the four adapters alone will not produce a Linux executable.
- `CocoaBridge.m` owns substantial window, input, image, text, and audio work.
  `CarbonShunts.c` mixes reusable bitmap operations with Apple dependencies.
- `U3IOLegacy.m` uses Foundation for saves and Resource Manager calls for
  bundled data. `MainResources.rsrc` and string plists need portable access.
- PCM effects, MIDI songs, and a GeneralUser GS SoundFont are already in the
  repository. A Linux build need not depend on QuickTime conversion or a
  build-time download of FluidR3.
- Command/text and gameplay scenario scripts currently launch a macOS app
  bundle; their useful coverage must be made available through a Linux binary.

No full Linux game build or interactive game test has been completed yet.

## Proposed implementation

Use CMake for the Linux executable and portable test targets, retaining the
Xcode project. Use SDL for the window, input, audio device, and presentation
of the existing software-rendered bitmap. Prefer SDL3, subject to confirming
dependency availability and a reproducible build on the baseline distro.
Pin any fetched dependency versions; support installed dependencies too.

Preserve the C game and software renderer. Extract reusable bitmap portions
of the legacy shims into portable code and replace remaining host calls at
explicit boundaries. Define only the compatibility types and helpers actually
needed; avoid recreating the whole Carbon API. Supply separate Linux adapter
files rather than scattering platform conditionals through the game rules.

Use an image decoder supporting the required PNG/JPEG/GIF assets; inventory
actual runtime usage before choosing the dependency. Use the bitmap font for
the initial classic UI. Route MIDI synthesis through FluidSynth and the
bundled SoundFont, with PCM mixing for effects. These are proposed dependency
choices to validate in the first implementation milestone.

Convert bundled resource containers and string plists into deterministic
portable assets at build time, preferably with Python standard-library tools.
Keep original data and resource IDs intact. If the resource inventory reveals
formats unsuitable for straightforward extraction, decide explicitly between
a small runtime reader and additional conversion before implementing loaders.
Use a versioned save container with fixed-width fields and explicit byte order;
store saves and preferences under XDG directories and retain
`U3_SAVE_DIRECTORY` for isolated testing. Importing existing Mac saves is
desirable but separate from Linux save/load correctness.

## Milestones and acceptance gates

### 1. Prove the dependency and data path

- Add a Linux CMake target for `U3Bitmap` and its existing tests.
- Inventory required Apple types/calls by translation unit, separating game
  code, renderer operations, and host services.
- Inventory `.rsrc` types/IDs, string tables, runtime images, and save formats.
- Demonstrate extraction/loading of a real world map, a string table, a tile
  sheet, and a font without Apple frameworks. Validate lengths and checksums.
- Confirm SDL, image decoding, and MIDI dependency acquisition on the baseline.

Gate: clean Linux configure/build/test plus a documented path for every data
format required to start the game. Re-estimate remaining work at this point.

### 2. Boot to a rendered, responsive game window

- Add a Linux entry point and platform timing, event, quit, and path services.
- Remove the global Carbon dependency from the Linux compilation path.
- Implement enough resource access and bitmap presentation to draw the title
  and world using real assets. Reuse draw operations rather than replacing
  game rendering logic.
- Implement keyboard commands, cursor coordinates, window resizing, and
  integer/nearest-neighbor scaling. Add minimal preferences with defaults.
- Allow audio to be disabled while boot and input are developed.

Gate: launch the Linux binary, display a real map, move a character with
correct timing, resize the window, and quit cleanly. No dummy resource data
or success-reporting save stubs may remain in the tested path.

### 3. Complete a playable game loop and persistence

- Finish roster and character creation, party selection, town interaction,
  combat, spells, dungeon drawing and controls, and essential dialogs.
- Implement versioned save/load with validation, atomic replacement, and
  useful errors for corrupt or unwritable saves.
- Move scenario checks behind a platform-neutral entry point and adapt shell
  scripts to accept a Linux executable.
- Exercise overworld, town, combat, dungeon, and save/restart/resume scenarios.

Gate: create a party, visit a town, fight, enter/leave a dungeon, save, restart,
and resume with matching party/world state. Run meaningful sanitizer checks
and record a manual interactive smoke test.

### 4. Restore audio and user-facing options

- Mix PCM effects and synthesize MIDI using the bundled SoundFont.
- Preserve sound/music toggles, volumes, looping, transitions, and timing.
- Complete fullscreen, mouse actions, alternate tiles/fonts, and preferences.
- Handle missing optional audio devices gracefully and document deferred speech
  or presentation differences.

Gate: sustained play with effects and music, correct song transitions, working
preferences after restart, and no regressions when audio is disabled.

### 5. Make releases reproducible

- Add Linux CI for configure/build, portable tests, and headless scenarios.
- Add install rules, resource discovery outside the source tree, a desktop
  entry, icon, dependency instructions, and a relocatable release archive.
- Check the archive on a clean Linux environment and manually verify display,
  input, audio, save paths, and fullscreen on X11 and Wayland where available.
- Preserve existing asset and third-party license notices; do not describe
  all bundled Ultima assets as MIT simply because the code is MIT.
- Run the existing macOS checks on an available Mac runner before claiming
  macOS compatibility has been preserved.

Gate: another user can build from documented steps or unpack the release and
play without Xcode, Apple frameworks, developer-local paths, or asset downloads.

## Validation strategy

Reuse the bitmap tests and existing gameplay scenarios. Add targeted tests
for resource decoding bounds/byte order and save round trips/corruption, where
errors could silently alter game state. Compare deterministic bitmap outputs
and game state where possible; keep platform-native text differences out of
strict pixel comparisons. Use dummy/headless SDL drivers in CI for logic and
render checks, plus real desktop testing for input, audio, and window behavior.
Headless success alone is not evidence of a fully playable port.

## Risks and sequencing

The largest uncertainty is residual coupling between game code, legacy drawing
types, and the Cocoa event loop. Resource extraction is the next early risk;
byte order and packed structures matter even though both current arm64 macOS
and x86-64 Linux are little-endian. Preserve on-disk conventions explicitly.
Save corruption, input reentrancy, and timing changes deserve regression
coverage. Avoid committing to a completion date before milestones 1 and 2.

This is plausibly several weeks of engineering and validation, rather than a
build-flag adjustment. That is an initial scope estimate, not a measured
schedule. Implement the milestones as small reviewable branches/commits,
starting with the portable build and resource spike. Keep `upstream` pointed
at hongjuny's fork so its modernization fixes remain easy to incorporate.
