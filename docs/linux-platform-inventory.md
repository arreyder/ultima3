# Remaining platform and data work

Inventory for the first Linux milestone; this is a source review, not evidence
that the full game compiles. All legacy translation units inherit Carbon types
through `Ultima3_Prefix.pch`, including files without direct framework imports.

## Translation units

| Files | Responsibility and required Linux work |
| --- | --- |
| `U3Bitmap.c` | Portable raster operations; already built and tested with CMake. |
| `UltimaAutocombat.c` | Primarily rules; remove inherited Mac scalar/string declarations and validate platform calls. |
| `UltimaSpellCombat.c` | Rules plus renderer/platform adapters; still includes Cocoa bridge and legacy declarations. |
| `UltimaNewMap.c` | Map rules and resource lifetime; needs portable legacy types and remaining shim dependencies. |
| `UltimaMisc.c` | Resource-backed rules, saves, handles, Pascal strings, and some bridge calls. Replace handle storage without changing map layout. |
| `UltimaMain.c` | Startup/game loop and diagnostic hooks call Cocoa directly; extract host startup, surface checks, and diagnostic event injection. |
| `UltimaNew.c` | Character creation combines game validation with dialogs/controls and drawing. Supply portable UI while retaining draft validation. |
| `UltimaMacIF.c` | Input, direction selection, menus, dialogs, and graphics glue; extract event processing and essential UI. |
| `UltimaGraphics.c` | Tile/font/image loading and draw logic; replace URLs/CF strings and legacy drawing surfaces, retain atlas layout and animation. |
| `UltimaDngn.c` | Dungeon rules and drawing; replace QuickDraw-style primitives and bridge dependencies. |
| `UltimaText.c` | Pascal strings, glyph drawing, text layout and input; retain bitmap-font path, replace CF text conversion and drawing shims. |
| `CarbonShunts.c` | Mixed reusable software drawing and host-specific window/control/resource helpers. Split, rather than compile unchanged. |
| `CocoaBridge.m` | Cocoa event loop, windows, image loading, native text, diagnostic captures, audio helpers. Implement SDL equivalents for used entry points. |
| `U3PlatformLegacy.m` | Input/timing/preferences adapter still delegates to Mac helpers. Add a Linux implementation. |
| `U3RendererLegacy.m` | Renderer facade calls existing drawing functions. Reuse once those functions have portable dependencies. |
| `U3IOLegacy.m` | Foundation save dictionary plus Resource Manager lookup; implement bundle lookup and portable persistence. |
| `U3AudioLegacy.m`, `UltimaSound.m`, `LWSoundPlayer.m` | Audio facade and Apple playback; initially disabled, later SDL mixing/FluidSynth. |
| `main.m`, `UltimaAppleEvents.c` | Cocoa startup and Mac menu/application events; replace Linux startup and host actions. |
| `PrefsDialog.m`, `LWIntegerTransformer.m` | Cocoa preferences/bindings; replace with a small portable preferences UI. |
| `UltimaIncludes.m` | Shared legacy definitions; audit with the include header when compiling the game core. |

Types needing removal or bounded compatibility definitions include `Handle`,
`Ptr`, `Size`, `Boolean`, `OSErr`, `Point`, `Rect`, Pascal string types,
`GWorldPtr`, `CGrafPtr`, window/dialog/control/menu handles, and `CFStringRef` /
`CFURLRef`. Host objects should become opaque adapter-owned handles, not fake
Apple objects. A successful bitmap build does not settle these dependencies.

## Resource interpretation

The generated manifest lists all type/ID/length/name combinations. Extraction
preserves even currently unused Mac UI resources so nothing is silently lost.

| Data | Bundled form and runtime path |
| --- | --- |
| World | `MAPS` 420 is the original Sosaria template: 4,101 bytes (dimension 64, 4,096 tiles, four extra bytes). `OpenRstr` copies it to mutable 419; `ResetSosaria` restores from 420. 419 is absent from the bundle. |
| Towns/other maps | `MAPS` 400–411 and 421 are 4,097 bytes: dimension then tiles. |
| Dungeons | `MAPS` 412–418 are 2,048-byte dungeon arrays, with no overworld size header. |
| Monsters/talk | `MONS` and `TLKS` payloads are 256 bytes each; preserve indexing and original bytes. |
| Party/roster | `PRTY` 500 (64 bytes) and `ROST` 500 (1,280 bytes) are templates copied into mutable 400. |
| Misc/demo/combat | `MISC`, `DEMO`, and `CONS` remain opaque byte arrays until their existing game readers are connected. |
| Text | Fourteen plist arrays export to JSON. Six `STR#` resources are also preserved; a bounded count/Pascal-string reader is needed if their legacy call paths remain. |
| Raster graphics | All PNG/GIF/JPEG files decode to RGBA; Standard tiles are 768×1024 (12×16 atlas), font 1536×16 (96 glyphs). Keep per-cell scaling to avoid tile bleeding. |
| Legacy pictures/fonts/UI | Five `PICT` resources plus `NFNT`, dialogs, controls, menus, icons, and color tables export unchanged. These are not decoded by the exporter; replace active UI with portable widgets and modern file assets, or implement conversion if an indispensable legacy picture is found. |
| PDFs | Three command/spell/reference PDFs are copied unchanged. Initially open externally or defer their in-game viewer; the old bridge rasterizes them using CoreGraphics. |
| Audio | PCM WAV and MIDI/SF2 paths avoid QuickTime; one level-up effect is MP3. MIDI synthesis and MP3 decoding still need runtime integration. |

## Save compatibility

Current saves are binary plists named `Roster-v1.plist`, with a version field
and resource dictionary keyed by hex FourCC and ID. The structured game-state
payload already uses explicit big-endian serialization, magic `0x55335356`
(`U3SV`), version 1, reserved field, and payload length. Reuse/extract these
codecs rather than serializing native structs or inventing another game-state
layout. Linux still needs an atomic container writer, validation, and save
directory policy. Python's `plistlib` can support a future offline importer;
no Mac-save importer is implemented by this milestone.

## Next acceptance gate

Resource extraction is proven and no longer the leading unknown. The next
spike should compile the game-facing types and SDL surface/event adapter,
then display the actual startup/world using the existing draw logic. Do not
substitute a standalone map viewer for the game loop. Residual Carbon/Cocoa
coupling remains the main scheduling risk; the several-week initial estimate
still applies until the first responsive game window exists.
