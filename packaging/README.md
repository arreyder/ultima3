# Linux packaging

Everything needed to install or redistribute the native Linux port, covering
section 6 of [the modernization roadmap](../docs/linux-modernization-roadmap.md).

| Path | Purpose |
|---|---|
| `ultima3.desktop` | Freedesktop desktop entry |
| `icons/hicolor/<size>/apps/ultima3.png` | App icons at 32, 64, 128 and 256 px |
| `appimage/AppRun`, `appimage/build-appimage.sh` | Portable single-file build |
| `flatpak/io.lairware.Ultima3.yml` | Flatpak manifest |
| `flatpak/generate-pillow-module.sh` | Generates the one module the manifest expects |

The icons and the window icon (`Images/AppIcon.png`, loaded at runtime with
`SDL_SetWindowIcon`) are all resampled from the 512 px Exodus dragon in
`Resources/Application.icns`, so the Linux build is branded identically to the
macOS app.

## Verification status

Be aware of the difference before relying on any of these:

- **`cmake --install` — verified.** Installed to a scratch prefix, then the
  installed binary was run from an unrelated working directory and found its own
  assets through `$XDG_DATA_DIRS`, `$XDG_DATA_HOME`, and with nothing set.
- **AppImage AppDir — verified up to `appimagetool`.** `build-appimage.sh` was
  run end to end; the staged AppDir launches the game through its `AppRun` from
  an unrelated directory with no environment set. `appimagetool` itself was not
  available, so the single-file `.AppImage` has not been produced or run.
- **Flatpak — not built.** The manifest is written from the build's real
  dependency list, and its FluidSynth pin and CMake option names were checked
  against FluidSynth 2.6.0's own `CMakeLists.txt`, but `flatpak-builder` was not
  available. Treat the first build as bring-up work, not a regression test.

## Install from source

```sh
cmake -S . -B build/linux-game \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
    -DU3_BUILD_GAME_CORE=ON -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build/linux-game --parallel
sudo cmake --install build/linux-game --prefix /usr/local
```

That lays out:

```
/usr/local/bin/ultima3
/usr/local/share/ultima3/assets/          exported asset bundle
/usr/local/share/applications/            desktop entry
/usr/local/share/icons/hicolor/           32/64/128/256 px icons
/usr/local/share/doc/ultima3/LICENSE
```

`ultima3` then runs from any working directory: `FindAssetsDirectory()` in
`Sources/Linux/main.c` probes `$U3_ASSETS_DIRECTORY`, `--assets`, paths relative
to the current directory, `$XDG_DATA_HOME/ultima3/assets`,
`~/.local/share/ultima3/assets`, every `$XDG_DATA_DIRS` entry, and finally
`/usr/local/share` and `/usr/share`.

Refresh the desktop database and icon cache after a system-wide install if your
desktop does not pick the entry up on its own:

```sh
sudo update-desktop-database /usr/local/share/applications
sudo gtk-update-icon-cache /usr/local/share/icons/hicolor
```

## AppImage

```sh
sh packaging/appimage/build-appimage.sh
```

Builds Release, stages `build/appimage/Ultima3.AppDir`, bundles the game's own
library stack, and — if `appimagetool` is on `$PATH` — writes
`build/Ultima3-x86_64.AppImage`. Without `appimagetool` it stops after staging
and prints the one command left to run; the AppDir is directly runnable either
way via `build/appimage/Ultima3.AppDir/AppRun`.

The bundling step deliberately excludes glibc, `libstdc++`, the graphics and
input drivers, and the X11/Wayland client libraries, all of which must come from
the host. `AppRun` sets `$U3_ASSETS_DIRECTORY` to the bundled copy because the
AppDir mounts at a different path on every run, which no fixed search path can
anticipate.

## Flatpak

```sh
# Once: generate the Pillow module (build-time dependency of the asset export).
sh packaging/flatpak/generate-pillow-module.sh

flatpak install -y flathub org.freedesktop.Sdk//24.08 org.freedesktop.Platform//24.08
flatpak-builder --user --install --force-clean \
    build/flatpak packaging/flatpak/io.lairware.Ultima3.yml
flatpak run io.lairware.Ultima3
```

Pillow is not committed as a module because a Flatpak source must pin a URL and
sha256 for every wheel in the dependency tree; generating those at packaging
time is how Flathub handles it, and it avoids hand-written pins that rot without
anyone noticing.

SDL2 and JSON-C come from the freedesktop runtime. FluidSynth does not, so the
manifest builds it with every one of its own audio and MIDI backends disabled —
the game pulls frames from the synth inside its own SDL audio callback and never
opens a FluidSynth driver. `libsndfile` support stays on, since that is what
decodes the Vorbis-compressed `.sf3` banks in the port's SoundFont probe.
