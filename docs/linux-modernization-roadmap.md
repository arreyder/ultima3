# Ultima III: Modern Linux Port Modernization & Enhancement Roadmap

This document outlines architectural improvements, quality-of-life enhancements, and modern Linux ecosystem integrations for the native Linux SDL2 port of **Ultima III: Exodus**.

## Status

| Section | State |
|---|---|
| 1. In-game settings overlay | Not started — the preference storage it needs is in place |
| 2. Windowing & display | **Done**, plus text-size controls added on request |
| 3. Audio & SoundFont discoverability | **Done** |
| 4. Input & modern controls (gamepad) | Not started |
| 5. Quality of life | Not started |
| 6. Linux packaging & distribution | **Done**, except an untested Flatpak manifest |

---

## 1. In-Game Settings & Options Overlay

### Problem
On macOS, the game uses the native AppKit menu bar to toggle Audio, Music, Speech, adjust volume, switch graphics modes (Classic vs. Modern), and configure window scales. In the Linux SDL2 environment, there is no desktop menu bar, requiring players to configure these options via CLI flags (`--scale`, `--modern`, `--speech`, etc.) before launching.

### Objectives
- Provide an in-game settings modal/overlay accessible via **`Esc`**, **`Tab`**, or **`F1`** (or a top-corner HUD button).
- **Options Available in Overlay**:
  - **Display**: Toggle Modern Mode (status bar + portraits) vs. Classic 640×384 mode, Window scale (1x, 2x, 3x, Integer Fill), Fullscreen toggle.
  - **Audio**: Sound FX volume (0–100%), Music volume (0–100%), Mute toggles.
  - **Speech**: Speech engine selection (`piper`, `speechd`, `cmd`, `off`), Voice selection, Speech volume/rate.
  - **Gameplay / QoL**: Turn pacing delay (0–1000ms), Auto-heal on/off, Speed limiter.
- Persist settings to `~/.config/ultima3/preferences.conf` on change.

---

## 2. Windowing & Display Modernization

### Objectives
- **Dynamic Fullscreen Toggle (`F11` / `Alt+Enter`)**:
  - Standardize modern PC convention to toggle between windowed and `SDL_WINDOW_FULLSCREEN_DESKTOP` seamlessly without restarting.
- **Window Resizing & Aspect-Ratio Letterboxing**:
  - Handle `SDL_WINDOWEVENT_RESIZED` and `SDL_WINDOWEVENT_SIZE_CHANGED` events in `Sources/Linux/U3LinuxVideo.c`.
  - Recalculate aspect-ratio preservation and centering so user window dragging adjusts presentation smoothly.
- **Window Icon**:
  - Set the application window icon using `SDL_SetWindowIcon` with the classic dragon or ankh bitmap so GNOME Dash, KDE Task Manager, and Wayland docks display proper game branding.
- **Optional CRT / Scanline Shader**:
  - Implement an optional retro CRT scanline / shadow-mask filter using SDL2 render targets or simple fragment shaders for players who desire an authentic 1980s composite/CRT look.

### Outcome — done

All of the above landed in `Sources/Linux/U3LinuxVideo.c`:

- `F11` / `Alt`+`Enter` toggle `SDL_WINDOW_FULLSCREEN_DESKTOP` live, handled in
  `HandleDisplayKey()` and swallowed before the game sees the key.
- `SDL_WINDOWEVENT_SIZE_CHANGED` / `RESIZED` / `EXPOSED` re-present immediately,
  so a drag-resize follows the pointer instead of waiting for the game's next
  redraw (it blocks on input between turns).
- `ComputePresentation()` was rewritten. It kept whole-pixel scaling as the
  default but **fixed a real bug**: the factor was clamped to 1, so a window
  smaller than 640×384 cropped the game. It now falls back to a fractional
  aspect-preserving fit whenever no integer factor fits, or on request (`F8` /
  `--smooth-scale`, persisted). Mouse mapping goes through the inverse of the
  same rect, so clicks stay accurate under fractional scaling.
- `SDL_SetWindowIcon` with the 256 px Exodus dragon resampled from
  `Resources/Application.icns` into `Images/AppIcon.png`. Needed a new
  alpha-preserving `U3LinuxAssetRGBA()`, since `U3LinuxAssetImage()` flattens
  alpha onto black and would have shown the icon as a black square.
- Scanline filter (`F10` / `--crt`), drawn as one darkened band per game pixel
  row so spacing stays even at any factor, and inert below 2x where it would
  only dim the picture. Verified by reading rendered pixels back
  (`U3LinuxVideoReadFrameForTesting`), not by eye. A shadow mask was not
  implemented; scanlines carry the effect.

Three Linux-only preference keys (`LinuxSmoothScaling`, `LinuxScanlines`,
`LinuxFontScale`) extend `U3PreferenceKey`'s numbering from `U3LinuxPlatform.h`
rather than the shared enum in `Sources/U3Platform.h`, which
`U3PlatformLegacy.m` switches over exhaustively — adding members there would
have broken the macOS build for options only the SDL host can honor.

### Text size (added after the fact)

Not in the original objectives; added on request. Two controls, because
measurement showed one knob cannot cover both directions:

- `--font-scale <50..100>` / `F6` / `F7` scales every font size the game
  requests, applied in `GetFontMetrics()` — the single point every text path
  funnels through, so drawing and `U3CocoaTextWidth` measurement stay in
  agreement and the game's own centring, right-alignment and word wrap follow
  automatically.
- `--double-size` / `--original-size` expose the game's existing tile size
  (`blkSiz` 16 ↔ 32), which was reachable only through the macOS menu bar
  before. This is the control for *larger* text.

The 100% ceiling on `--font-scale` is measured, not cautious.
`Sources/UltimaText.c` lays out one line per tile (`NewPrint` draws at
`ty * blkSiz` into an offscreen exactly `blkSiz` tall) while `SetNewFont` asks
for a font size of exactly `blkSiz`, so at 100% a glyph cell already fills its
line. Rendering the conversation screen at 125% and 150% produced collided,
unreadable text: each line erases its one-tile box, then draws glyphs that
spill over the line drawn before it. Raising the ceiling means rescaling the
line pitch in the shared text engine, which is a larger change than this was.

Worth recording for whoever touches text next: the two appearances rasterize
text completely differently. Classic blits each character from a pre-rendered
font sheet, one tile per character (`UPrint`), and never reaches this port's
glyph rasterizer at all — the first round of font-scale screenshots came back
byte-identical for that reason. Only the modern appearance goes through
`NewPrint` → `U3CocoaDrawPascalString` → `GetFontMetrics`, so `--font-scale`
affects modern only.

---

## 3. Audio & SoundFont Discoverability

### Objectives
- **Automatic System SoundFont Probing**:
  - Currently, FluidSynth MIDI expects `Resources/MusicMIDI/GeneralUser-GS.sf2` in assets or `$U3_MIDI_SOUNDBANK`.
  - Add automatic fallback scanning in `Sources/Linux/U3LinuxAudio.c` across standard Linux distribution locations:
    - `/usr/share/sounds/sf2/default-GM.sf2` (Ubuntu / Debian `fluid-soundfont-gm`)
    - `/usr/share/sounds/sf2/FluidR3_GM.sf2`
    - `/usr/share/soundfonts/default.sf2` (Arch / Fedora)
    - `/usr/share/sounds/sf3/default-GM.sf3`
  - Allow zero-configuration MIDI music playback on any Linux distribution without requiring manual downloads.
- **Dynamic Volume Control**:
  - Expose runtime master/music/SFX volume scaling to the audio mixer without restarting the audio device.

### Outcome — done

- `U3AudioLoadSoundFont()` in `Sources/Linux/U3LinuxAudio.c` probes, in order:
  `$U3_MIDI_SOUNDBANK`, the bundled `GeneralUser-GS.sf2`, then the distribution
  locations above plus `TimGM6mb.sf2` and `/usr/share/soundfonts/FluidR3_GM.sf2`.
  Candidates are tried by actually loading them, so a corrupt bank is skipped
  rather than counted as success. An explicitly named bank stays authoritative:
  if it will not load the game says so instead of quietly substituting another,
  which would hide the player's typo. Verified both ways on a machine with
  `/usr/share/sounds/sf2/default-GM.sf2` installed.
- Dynamic volume turned out to be **half-built and disconnected**. The retuning
  itself already existed in `U3AudioApplyPreferences()` — effect volume and
  FluidSynth gain are both adjusted in place, no device reopen — but nothing
  ever called `U3LinuxAudioSetSettings()`, so the stored `SoundVolume`,
  `MusicVolume`, `SoundDisabled` and `MusicDisabled` preferences were read by
  nobody and the compiled-in defaults (unmuted, 100%) always won.
  `U3AudioApplyPreferences()` now refreshes from stored preferences first,
  matching `ApplyVolumePreferences()` in `UltimaSound.m`, which reads them
  itself on macOS. That fixes every existing call site — startup and the
  preferences dialog — instead of only startup.

---

## 4. Input & Modern Controls (Steam Deck & Gamepad)

### Objectives
- **Gamepad / Steam Deck Support (`SDL_GameController`)**:
  - Native controller input for Linux handhelds (Steam Deck, ROG Ally) and standard gamepads (Xbox, DualShock/DualSense):
    - **D-Pad / Left Stick**: Directional movement (North, South, East, West).
    - **A Button**: Confirm / Talk / Search / Pass turn.
    - **B Button**: Cancel / Escape / Disband.
    - **X Button**: Attack (followed by directional prompt).
    - **Y Button**: Cast Spell / Magic.
    - **LB / RB**: Cycle active character / quick status inspect.
    - **Start / Menu**: Open in-game options overlay.
- **Input Ergonomics & Numpad Support**:
  - Verify and guarantee numpad diagonal movement (`7, 9, 1, 3`) in combat and dungeons.
  - Contextual movement option (allowing Arrow keys or WASD to navigate without interfering with combat command letters).
- **Mouse Pathfinding & Map Interaction**:
  - Expand mouse support beyond main menus to allow clicking on adjacent tiles to move and clicking enemy targets in combat.

---

## 5. Quality of Life (QoL) for Gameplay

### Objectives
- **Multiple Save Slots & Quick Save / Quick Load (`F5` / `F9`)**:
  - Support multiple named party slots (`Slot 1` through `Slot 5`) instead of a single `Roster-v1.u3s`.
  - Implement Quick Save (`F5`) and Quick Load (`F9`) snapshots to protect players from accidental party wipes.
- **In-Game Spell & Command Reference Card**:
  - Classic Ultima III requires typing 2-letter arcane abbreviations for Cleric/Wizard spells (e.g. `RP` = Repel, `MM` = Magic Missile) and individual command letters.
  - Implement a quick reference overlay triggered by **`?`** or **`F2`** showing available commands, spell codes, and equipment stats.
- **Optional Dungeon Compass & Auto-Mapping**:
  - Add an optional directional compass HUD element in dungeons (showing current facing direction: N, E, S, W).
  - Optional minimap toggle for explored dungeon floors to reduce reliance on external graph paper.

---

## 6. Linux Packaging & Distribution

### Objectives
- **Standard XDG Search Paths**:
  - Update `FindAssetsDirectory` in `Sources/Linux/main.c` to search:
    1. CLI `--assets <dir>`
    2. `$U3_ASSETS_DIRECTORY`
    3. Relative local directories (`assets`, `../assets`)
    4. `$XDG_DATA_HOME/ultima3/assets` or `~/.local/share/ultima3/assets`
    5. System paths: `/usr/local/share/ultima3/assets`, `/usr/share/ultima3/assets`
  - Allows launching `ultima3` from any working directory.
- **Desktop Entry & Icons**:
  - Add `ultima3.desktop` adhering to Freedesktop standards.
  - Provide desktop icons in standard sizes (32x32, 64x64, 128x128, 256x256) in `hicolor` icon directories.
- **CMake Install Target**:
  - Add `install(TARGETS ultima3 ...)` and asset installation rules to `CMakeLists.txt` for standard `cmake --install build/linux-game --prefix /usr/local`.
- **AppImage & Flatpak Packaging**:
  - Create an AppImage recipe and Flatpak manifest to provide portable, self-contained single-file releases bundling assets, Piper voices, and default soundbanks.

### Outcome — done, with one caveat

See [packaging/README.md](../packaging/README.md) for the full layout and the
per-artifact verification status.

- `FindAssetsDirectory()` in `Sources/Linux/main.c` searches the full XDG chain
  described above. Verified by installing to a scratch prefix and launching the
  installed binary from an unrelated working directory — via `$XDG_DATA_DIRS`,
  via `$XDG_DATA_HOME`, and with nothing set (which fails with a clear message).
- `packaging/ultima3.desktop`, validated with `desktop-file-validate`, plus
  hicolor icons at 32/64/128/256 px.
- CMake `install()` rules for the binary, asset bundle, desktop entry, icons and
  licence. Verified end to end.
- `packaging/appimage/build-appimage.sh` stages a standard AppDir and bundles the
  game's library stack while deliberately excluding what must come from the host
  (glibc, `libstdc++`, graphics/input drivers, X11/Wayland). Verified up to
  `appimagetool`, which was not available: the staged AppDir launches the game
  through its `AppRun` from an unrelated directory with no environment set, but
  no single-file `.AppImage` has been produced or run.
- `packaging/flatpak/io.lairware.Ultima3.yml`. **Caveat: not built.**
  `flatpak-builder` was unavailable, so this is the one deliverable here that is
  unverified. Its FluidSynth pin (git tag + resolved commit) and CMake option
  names were checked against FluidSynth 2.6.0's own `CMakeLists.txt` rather than
  assumed — `enable-sdl2`, an obvious-looking guess, no longer exists. Pillow,
  which `scripts/export-portable-assets.py` needs at build time and the
  freedesktop SDK lacks, is generated as a separate module by
  `packaging/flatpak/generate-pillow-module.sh` instead of being pinned by hand.

## Not yet started

Sections 1, 4 and 5. Section 1's persistence layer is already in place
(`~/.config/ultima3/preferences.conf` via `U3LinuxPlatform.c`), and the display
toggles it would expose now exist as real API
(`U3LinuxVideoSetSmoothScaling`, `U3LinuxVideoSetScanlines`,
`U3CocoaSetMainSurfaceFullScreen`) rather than only as hotkeys, so an overlay
can drive them directly.
