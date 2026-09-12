// tests/linux-audio-test.c
//
// Standalone test for Sources/Linux/U3LinuxAudio.{c,h}. Does not link the
// rest of the game core (too many unrelated dependencies for a focused
// audio test); instead it provides the one external symbol U3AudioUpdateMusic
// reads (`Party`, normally defined in UltimaMain.c) itself.
//
// Run three ways to cover both truthful-incapable branches end to end:
//
//   SDL_AUDIODRIVER=dummy ./linux-audio-test
//     -> expects a real (if silent) SDL device: gSoundIncapable/gMusicIncapable
//        false, PCM effects decode, and FluidSynth renders non-silent audio
//        for a real song.
//
//   SDL_AUDIODRIVER=does-not-exist ./linux-audio-test
//     -> expects SDL_OpenAudioDevice() to fail: gSoundIncapable AND
//        gMusicIncapable (music rides the same device) truthfully true, and
//        no call anywhere crashes.
//
//   ./linux-audio-test --hard-disabled
//     -> U3LinuxAudioConfigure(..., true): both incapable flags true
//        immediately, independent of any driver.

#include "U3LinuxAudio.h"

#include <SDL2/SDL.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Normally defined in UltimaMain.c; U3AudioUpdateMusic() reads Party[3] to
 * detect combat. Not linked here, so we provide it ourselves. */
unsigned char Party[65];

/* Declared `extern short`/`extern Boolean` by every consumer in the real
 * game; we do the same here rather than adding a shared header for it. */
extern short gSongCurrent, gSongNext, gSongPlaying;
extern bool gSoundIncapable, gMusicIncapable;

static int gFailures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++gFailures;                                                   \
        } else {                                                           \
            fprintf(stderr, "ok: %s\n", msg);                              \
        }                                                                  \
    } while (0)

static void ResetGlobals(void) {
    gSongCurrent = gSongNext = gSongPlaying = 0;
    gSoundIncapable = gMusicIncapable = false;
}

/* Every U3Audio* entry point must be safe to call before configuration
 * (no assets dir, no device) -- this is what the real process looks like
 * if U3LinuxAudioConfigure() is somehow skipped, and it must degrade, not
 * crash. */
static void TestUnconfiguredIsSafe(void) {
    ResetGlobals();
    /* OpenEffects() does not require U3LinuxAudioConfigure() -- it only
     * needs an SDL device, which may well open (e.g. the dummy driver) even
     * with no assets directory set. What must be truthful with no assets
     * directory is that nothing can actually be *loaded*. */
    U3AudioOpenEffects();
    U3AudioSetUpMusic();
    CHECK(gMusicIncapable, "unconfigured: SetUpMusic reports music incapable (no assets dir)");
    size_t frames = 0;
    CHECK(!U3LinuxAudioDebugEffectFrameCount(U3SoundEffectStep, &frames),
          "unconfigured: effect load honestly fails with no assets dir");
    U3AudioPlaySound(U3SoundEffectStep, false);
    U3AudioPlayLegacyFadeTone(0);
    U3AudioStopLegacyFadeTone();
    U3AudioPrimeLegacySample(NULL);
    U3AudioUpdateMusic();
    U3AudioStopMusic();
    U3AudioApplyPreferences();
    U3AudioSetUpSpeech();
    U3AudioSpeakMessages(1, 0, 0);
    U3AudioSpeakPascalString((uint8_t *)"\x00", 0);
    U3AudioCloseMusic();
    U3AudioCloseEffects();
    CHECK(1, "unconfigured: full call sequence did not crash");
}

static void TestHardDisabled(void) {
    ResetGlobals();
    U3LinuxAudioConfigure("build/linux-game/assets", true);
    CHECK(gSoundIncapable, "hard-disabled: gSoundIncapable true immediately");
    CHECK(gMusicIncapable, "hard-disabled: gMusicIncapable true immediately");
    U3AudioOpenEffects();
    U3AudioSetUpMusic();
    CHECK(gSoundIncapable && gMusicIncapable, "hard-disabled: stays incapable after Open/SetUp");
    U3AudioPlaySound(U3SoundEffectStep, false);
    U3AudioUpdateMusic();
    CHECK(1, "hard-disabled: calls did not crash");
    U3LinuxAudioShutdown();
}

/* Runs against whatever SDL_AUDIODRIVER the process was launched with.
 * Verifies flags are set *truthfully*: capable when a (possibly dummy)
 * device opens, incapable when it can't -- never the reverse, and never a
 * fake "playing" in either case. */
static void TestRealConfiguration(const char *assetsDir) {
    ResetGlobals();
    if (!assetsDir)
        assetsDir = getenv("U3_ASSETS_DIRECTORY");
    if (!assetsDir || access(assetsDir, R_OK) != 0) {
        if (access("assets", R_OK) == 0)
            assetsDir = "assets";
        else
            assetsDir = "build/linux-game/assets";
    }
    U3LinuxAudioConfigure(assetsDir, false);
    U3AudioOpenEffects();

    const char *driver = SDL_GetCurrentAudioDriver();
    fprintf(stderr, "info: SDL audio driver = %s, gSoundIncapable = %d\n",
            driver ? driver : "(none)", (int)gSoundIncapable);

    const char *expect = getenv("U3_AUDIO_TEST_EXPECT_CAPABLE");
    if (expect && strcmp(expect, "1") == 0) {
        CHECK(!gSoundIncapable, "expected-capable run: device opened (gSoundIncapable == false)");
    } else if (expect && strcmp(expect, "0") == 0) {
        CHECK(gSoundIncapable, "expected-incapable run: device failed to open (gSoundIncapable == true)");
    }

    U3AudioSetUpMusic();
    fprintf(stderr, "info: gMusicIncapable = %d\n", (int)gMusicIncapable);
    if (!gSoundIncapable) {
        /* Device opened (even if it's the dummy driver): music rides the
         * same callback, so it must report capable too, and the soundfont
         * that ships in the repo must actually load. */
        CHECK(!gMusicIncapable, "device open: music also capable (real SoundFont present)");
    } else {
        CHECK(gMusicIncapable, "device not open: music honestly incapable too");
    }

    if (!gSoundIncapable) {
        /* ---- Real PCM decode check: exact asset, exact pipeline ---- */
        size_t frames = 0;
        bool decoded = U3LinuxAudioDebugEffectFrameCount(U3SoundEffectStep, &frames);
        CHECK(decoded, "Step.wav decodes through the real effect pipeline");
        CHECK(frames > 0, "Step.wav decodes to a non-zero frame count");
        fprintf(stderr, "info: Step.wav decoded to %zu device-format frames (44100Hz stereo)\n", frames);

        /* Step.wav is 22050Hz mono in the shipped asset; resampled to
         * 44100Hz stereo the frame (per-channel sample) count should double,
         * not change channel count's effect on "frames". Sanity-bound
         * rather than pin an exact value, since the resampler's filter
         * tail can shift the count by a handful of samples. */
        CHECK(frames > 500 && frames < 20000, "Step.wav frame count is in a sane range");

        size_t alarmFrames = 0;
        CHECK(U3LinuxAudioDebugEffectFrameCount(U3SoundEffectAlarm, &alarmFrames),
              "a second effect (Alarm.wav) also decodes");
        CHECK(alarmFrames > 0, "Alarm.wav decodes to a non-zero frame count");

        /* LevelUp is the known, honestly-reported MP3 gap: it must fail
         * cleanly, not fake success. */
        size_t levelUpFrames = 0;
        bool levelUpDecoded = U3LinuxAudioDebugEffectFrameCount(U3SoundEffectLevelUp, &levelUpFrames);
        CHECK(!levelUpDecoded, "LevelUp (MP3) honestly reports it cannot decode, rather than faking it");

        /* Exercise actual playback paths for crash-safety; with the dummy
         * driver these genuinely mix into a (discarded) buffer. */
        U3AudioPlaySound(U3SoundEffectStep, false);
        U3AudioPlaySound(U3SoundEffectHit, true);
        SDL_Delay(20);
        CHECK(1, "playing two real effects concurrently did not crash");

        U3AudioPlayLegacyFadeTone(0);
        U3AudioPlayLegacyFadeTone(12345); /* must be ignored: already playing */
        SDL_Delay(20);
        U3AudioStopLegacyFadeTone();
        CHECK(1, "legacy fade tone start/restart/stop did not crash");
    }

    if (!gMusicIncapable) {
        /* ---- Real MIDI synthesis check ---- */
        gSongNext = 1;
        U3AudioUpdateMusic();
        CHECK(gSongCurrent == 1 && gSongPlaying == 1, "UpdateMusic adopts the requested song (1)");

        /* Give fluid_player's scheduling thread time to inject the first
         * note-on events before we pull samples. */
        SDL_Delay(150);

        int16_t buffer[4096 * 2];
        bool rendered = U3LinuxAudioDebugRenderMusic(buffer, 4096);
        CHECK(rendered, "FluidSynth renders a buffer for the playing song");
        bool nonSilent = false;
        for (size_t i = 0; i < sizeof(buffer) / sizeof(buffer[0]); ++i) {
            if (buffer[i] != 0) { nonSilent = true; break; }
        }
        CHECK(nonSilent, "rendered MIDI buffer is audibly non-silent (real synthesis, not a fake 'playing')");

        /* Combat override: Party[3] == 0x80 must force song 5 regardless
         * of what was requested, matching UltimaSound.m's MusicUpdate(). */
        Party[3] = 0x80;
        U3AudioUpdateMusic();
        CHECK(gSongCurrent == 5 && gSongNext == 5, "combat flag forces song 5");
        Party[3] = 0;

        U3AudioStopMusic();
        CHECK(1, "StopMusic (fade + stop) did not crash");
    }

    U3LinuxAudioShutdown();
}

int main(int argc, char **argv) {
    const char *assetsDir = NULL;
    if (argc > 1) {
        if (strcmp(argv[1], "--hard-disabled") == 0) {
            TestHardDisabled();
            return gFailures > 0 ? 1 : 0;
        } else {
            assetsDir = argv[1];
        }
    }
    TestUnconfiguredIsSafe();
    TestRealConfiguration(assetsDir);

    if (gFailures > 0) {
        fprintf(stderr, "\n%d check(s) FAILED\n", gFailures);
        return 1;
    }
    fprintf(stderr, "\nall checks passed\n");
    return 0;
}
