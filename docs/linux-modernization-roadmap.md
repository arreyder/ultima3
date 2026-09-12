# Ultima III: Modern Linux Port Modernization & Enhancement Roadmap

This document outlines architectural improvements, quality-of-life enhancements, and modern Linux ecosystem integrations for the native Linux SDL2 port of **Ultima III: Exodus**.

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
