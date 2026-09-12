//
//  U3LinuxAudio.h
//  Ultima3 Linux port
//
//  Linux implementation of the U3Audio.h boundary: SDL2 for PCM sound
//  effects plus a synthetic fade tone, FluidSynth (fed through the same
//  SDL audio callback) for MIDI music, and honest no-ops for speech.
//
//  Integration contract
//  ---------------------
//  U3Audio.h's functions take no configuration arguments (they are a fixed
//  boundary shared with the original Mac sources), so this header adds two
//  small entry points the platform/integration layer is expected to call:
//
//    1. U3LinuxAudioConfigure() once at startup, before any U3Audio* call,
//       to point this module at the exported game assets and/or force audio
//       off entirely (e.g. for a headless run or a --no-audio flag).
//    2. U3LinuxAudioSetSettings() whenever sound/music toggles or volumes
//       change (the preferences layer owns *reading* those values; it hands
//       them to us). Call U3AudioApplyPreferences() afterwards -- exactly as
//       PrefsDialog.m does on macOS -- to make a change take effect.
//
//  Asset loading note (see also the Linux audio task report): U3LinuxAssets.h
//  only exposes type/ID resource-fork lookups, a strings-table reader, and a
//  fixed-size image decoder. The PCM effects, MIDI songs, and the SoundFont
//  are exported under the manifest's plain "files" list (e.g.
//  "Resources/SoundsPCM/Alarm.wav"), which U3LinuxAssets has no accessor
//  for. Rather than reach into its private (opaque) struct or duplicate its
//  manifest parsing, this module reads those well-known, internally
//  generated relative paths directly from the same assets root directory
//  that U3LinuxAssetsOpen() uses. No path is ever derived from untrusted
//  input. A real fix is a small U3LinuxAssetFile()-style addition to
//  U3LinuxAssets.h; flagged to integration rather than made here.
//

#pragma once

#include "U3Audio.h"

/* Sound/music toggles and volumes, as read from preferences. The preferences
 * task (U3LinuxPlatform.*) owns deciding these values; this module only
 * consumes them. Percentages follow the original convention: < 1 means "use
 * the default of 100". */
typedef struct U3LinuxAudioSettings {
    bool soundDisabled;
    bool musicDisabled;
    int16_t soundVolumePercent;
    int16_t musicVolumePercent;
} U3LinuxAudioSettings;

/* Must be called once before U3AudioOpenEffects()/U3AudioSetUpMusic() (and
 * safe to call again later, e.g. to repoint at a different assets build).
 *
 * assetsDirectory: root that also backs U3LinuxAssetsOpen(), e.g.
 *   "build/linux-game/assets". May be NULL if disableAudio is true.
 * disableAudio: force every audio entry point into its honest "incapable"
 *   path (gSoundIncapable / gMusicIncapable both end up true) regardless of
 *   what devices or soundfonts are actually present. Use this for headless
 *   runs/tests rather than simply not calling the Open/SetUp functions, so
 *   the rest of the game still sees a truthful, consistent state. */
void U3LinuxAudioConfigure(const char *assetsDirectory, bool disableAudio);

/* Replaces the last-pushed settings used by U3AudioApplyPreferences(). Does
 * not itself start/stop anything -- call U3AudioApplyPreferences() after,
 * the same way the existing Mac preferences dialog does. */
void U3LinuxAudioSetSettings(const U3LinuxAudioSettings *settings);

/* Full teardown of SDL audio device and FluidSynth state. Safe to call even
 * if U3LinuxAudioConfigure() was never called or nothing was opened. */
void U3LinuxAudioShutdown(void);

/* ---- Test/debug introspection -----------------------------------------
 * Not part of the game's runtime contract; used only by
 * tests/linux-audio-test.c to verify real decoding/synthesis happened
 * (rather than merely that nothing crashed). */

/* Loads (if needed) and reports the decoded, device-format frame count for
 * one effect. Returns false if the asset is missing/undecodable (e.g. the
 * LevelUp MP3, which is a known, reported gap -- see U3LinuxAudio.c). */
bool U3LinuxAudioDebugEffectFrameCount(U3SoundEffect effect, size_t *outFrames);

/* Renders `frames` stereo S16 frames directly from the live music synth (if
 * U3AudioSetUpMusic() succeeded and a song is playing) into `buffer`
 * (caller-owned, >= frames * 2 int16_t). Returns false if music isn't set
 * up. Lets a test confirm FluidSynth is actually producing non-silent audio
 * without needing a real speaker. */
bool U3LinuxAudioDebugRenderMusic(int16_t *buffer, int frames);
