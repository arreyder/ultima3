//
//  U3LinuxAudio.c
//  Ultima3 Linux port
//
//  See U3LinuxAudio.h for the integration contract and the note on why
//  audio files are read directly from the assets root instead of through
//  U3LinuxAssets (which has no accessor for its manifest's plain "files"
//  category -- PCM effects, MIDI songs, and the SoundFont all live there).
//
//  Priorities, per the port plan: (1) link cleanly, (2) real PCM effect
//  playback, (3) MIDI only if it falls out cheaply. Speech stays an honest
//  no-op; the port plan defers it.
//

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1 /* strdup under -std=c11 */
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "U3LinuxAudio.h"
#include "U3LinuxSpeech.h"
#include "U3Platform.h"

#include <SDL2/SDL.h>
#include <fluidsynth.h>
#include <dlfcn.h>
#include <strings.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void GetPascalStringFromArrayByIndex(uint8_t *pstringPtr, const void *identifier, int index) __attribute__((weak));
extern bool U3PlatformGetBooleanPreference(U3PreferenceKey key) __attribute__((weak));

/* ---- The 14 contract functions' 5 companion globals -----------------
 *
 * Declared `extern short` / `extern Boolean` elsewhere (UltimaMain.c,
 * UltimaMacIF.c, UltimaSound.m, ...). `Boolean` is `typedef bool Boolean;`
 * on this target (Sources/Linux/U3LegacyTypes.h), so plain `bool` is
 * layout-identical; using it here avoids pulling the large Carbon-shim
 * header into an otherwise unrelated audio file. Zero-initialized here,
 * matching the macOS build: UltimaSound.m defines these with no
 * initializer, and CheckSystemRequirements() in UltimaMacIF.c additionally
 * sets both *Incapable flags to FALSE during startup on a capable machine.
 */
short gSongCurrent = 0;
short gSongNext = 0;
short gSongPlaying = 0;
bool gSoundIncapable = false;
bool gMusicIncapable = false;

/* ---------------------------------------------------------------------
 * Shared mix format. Fixed regardless of the underlying device/driver:
 * SDL_OpenAudioDevice is called with allowed_changes = 0, so SDL itself
 * resamples/reformats to this spec when the real device differs.
 * ------------------------------------------------------------------- */
#define U3_AUDIO_FREQ 44100
#define U3_AUDIO_CHANNELS 2
#define U3_AUDIO_FORMAT AUDIO_S16SYS
#define U3_AUDIO_BUFFER_SAMPLES 1024
/* Generous upper bound on frames requested per callback invocation; real
 * devices ask for U3_AUDIO_BUFFER_SAMPLES, but we size generously in case a
 * driver (e.g. "dummy") requests more. */
#define U3_AUDIO_MAX_CALLBACK_FRAMES 8192

/* ---- Preferences-derived settings, pushed by integration ------------- */
static U3LinuxAudioSettings gSettings = {
    .soundDisabled = false,
    .musicDisabled = false,
    .soundVolumePercent = 100,
    .musicVolumePercent = 100,
};

static char *gAssetsDirectory = NULL;
static bool gHardDisabled = false;

/* ---- SDL output device ------------------------------------------------ */
static SDL_AudioDeviceID gDevice = 0;
static bool gDeviceOpen = false;
static int gEffectVolumeMix = SDL_MIX_MAXVOLUME; /* 0..128, derived from soundVolumePercent */

/* ---- PCM effect voices (simple fixed-size mixer) ---------------------- */
typedef struct {
    const uint8_t *data; /* owned by the effect cache or the fade-tone buffer; never freed here */
    Uint32 length;        /* bytes */
    Uint32 position;      /* bytes already consumed */
    int volume;            /* 0..SDL_MIX_MAXVOLUME */
    bool active;
} U3AudioVoice;

#define U3_AUDIO_FADE_VOICE 0
#define U3_AUDIO_FIRST_EFFECT_VOICE 1
#define U3_AUDIO_VOICE_COUNT 8

static U3AudioVoice gVoices[U3_AUDIO_VOICE_COUNT];

/* ---- Decoded/converted PCM effect cache, lazy-loaded ------------------ */
typedef struct {
    int16_t *samples; /* interleaved stereo S16 at U3_AUDIO_FREQ, malloc'd */
    Uint32 length;      /* bytes */
    bool attempted;
    bool ok;
} U3AudioEffectCache;

static U3AudioEffectCache gEffectCache[U3SoundEffectUpwards + 1];

/* ---- Synthetic "Exodus" fade tone (see PlayLegacyFadeTone below) ------ */
#define U3_FADE_TONE_FRAMES (U3_AUDIO_FREQ * 3)
static int16_t *gFadeToneBuffer = NULL;
static bool gFadeTonePlaying = false;

/* ---- Music (FluidSynth rendered through our own SDL callback) --------- */
static fluid_settings_t *gFluidSettings = NULL;
static fluid_synth_t *gFluidSynth = NULL;
static fluid_player_t *gFluidPlayer = NULL;
static char *gCurrentSongPath = NULL;
static bool gMusicReady = false;
static float gMusicGain = 0.2f; /* FluidSynth's own default */

/* ======================================================================
 * Small helpers
 * ==================================================================== */

static char *U3AudioJoinAssetPath(const char *relative) {
    if (!gAssetsDirectory || !relative) return NULL;
    size_t length = strlen(gAssetsDirectory) + 1 + strlen(relative) + 1;
    char *path = malloc(length);
    if (path) snprintf(path, length, "%s/%s", gAssetsDirectory, relative);
    return path;
}

static int U3AudioVolumeMix(int16_t percent) {
    if (percent < 1) percent = 100;
    if (percent > 100) percent = 100;
    return (percent * SDL_MIX_MAXVOLUME) / 100;
}

/* ======================================================================
 * Audio callback: mixes music (FluidSynth) and active voices.
 * Runs on SDL's audio thread; all shared state it touches (gFluidSynth,
 * gFluidPlayer, gVoices) is only mutated elsewhere while this device's
 * lock is held, or before the pointer is published under that lock.
 * ==================================================================== */

static void U3AudioCallback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    SDL_memset(stream, 0, (size_t)len);

    if (gFluidSynth && gFluidPlayer) {
        static int16_t musicBuffer[U3_AUDIO_MAX_CALLBACK_FRAMES * U3_AUDIO_CHANNELS];
        int frames = len / (int)(U3_AUDIO_CHANNELS * sizeof(int16_t));
        if (frames > U3_AUDIO_MAX_CALLBACK_FRAMES) frames = U3_AUDIO_MAX_CALLBACK_FRAMES;
        if (frames > 0 &&
            fluid_synth_write_s16(gFluidSynth, frames, musicBuffer, 0, 2, musicBuffer, 1, 2) == FLUID_OK) {
            Uint32 bytes = (Uint32)frames * U3_AUDIO_CHANNELS * sizeof(int16_t);
            if (bytes > (Uint32)len) bytes = (Uint32)len;
            SDL_MixAudioFormat(stream, (const Uint8 *)musicBuffer, U3_AUDIO_FORMAT, bytes, SDL_MIX_MAXVOLUME);
        }
    }

    for (int i = 0; i < U3_AUDIO_VOICE_COUNT; ++i) {
        U3AudioVoice *voice = &gVoices[i];
        if (!voice->active || !voice->data) continue;
        Uint32 remaining = voice->length - voice->position;
        Uint32 chunk = remaining < (Uint32)len ? remaining : (Uint32)len;
        if (chunk > 0) {
            SDL_MixAudioFormat(stream, voice->data + voice->position, U3_AUDIO_FORMAT, chunk, voice->volume);
            voice->position += chunk;
        }
        if (voice->position >= voice->length) {
            voice->active = false;
            if (i == U3_AUDIO_FADE_VOICE) gFadeTonePlaying = false;
        }
    }
}

/* ======================================================================
 * Configuration entry points (U3LinuxAudio.h)
 * ==================================================================== */

void U3LinuxAudioConfigure(const char *assetsDirectory, bool disableAudio) {
    free(gAssetsDirectory);
    gAssetsDirectory = (assetsDirectory && !disableAudio) ? strdup(assetsDirectory) : NULL;
    gHardDisabled = disableAudio;
    if (disableAudio) {
        gSoundIncapable = true;
        gMusicIncapable = true;
    }
}

void U3LinuxAudioSetSettings(const U3LinuxAudioSettings *settings) {
    if (!settings) return;
    gSettings = *settings;
}

/* ======================================================================
 * Effects: device lifecycle
 * ==================================================================== */

void U3AudioOpenEffects(void) {
    if (gHardDisabled) {
        gSoundIncapable = true;
        return;
    }
    if (gDeviceOpen) return;

    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "U3Audio: SDL audio init failed: %s\n", SDL_GetError());
        gSoundIncapable = true;
        return;
    }

    SDL_AudioSpec desired;
    SDL_zero(desired);
    desired.freq = U3_AUDIO_FREQ;
    desired.format = U3_AUDIO_FORMAT;
    desired.channels = U3_AUDIO_CHANNELS;
    desired.samples = U3_AUDIO_BUFFER_SAMPLES;
    desired.callback = U3AudioCallback;

    SDL_AudioSpec obtained;
    gDevice = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0 /* no allowed changes: we get exactly `desired` */);
    if (gDevice == 0) {
        fprintf(stderr, "U3Audio: no usable audio device: %s\n", SDL_GetError());
        gSoundIncapable = true;
        return;
    }

    memset(gVoices, 0, sizeof(gVoices));
    gDeviceOpen = true;
    gSoundIncapable = false;
    gEffectVolumeMix = U3AudioVolumeMix(gSettings.soundVolumePercent);
    SDL_PauseAudioDevice(gDevice, 0);
}

void U3AudioCloseEffects(void) {
    if (gDeviceOpen) {
        SDL_CloseAudioDevice(gDevice);
        gDevice = 0;
        gDeviceOpen = false;
    }
    for (int i = 0; i < (int)(sizeof(gEffectCache) / sizeof(gEffectCache[0])); ++i) {
        free(gEffectCache[i].samples);
        gEffectCache[i].samples = NULL;
        gEffectCache[i].attempted = false;
        gEffectCache[i].ok = false;
        gEffectCache[i].length = 0;
    }
    free(gFadeToneBuffer);
    gFadeToneBuffer = NULL;
    gFadeTonePlaying = false;
    memset(gVoices, 0, sizeof(gVoices));
}

/* ======================================================================
 * PCM sound effects
 * ==================================================================== */

/* Mirrors U3LegacySoundNameForEffect() in Sources/U3AudioLegacy.m. All
 * effects live under Resources/SoundsPCM/<Name>.wav in the exported assets,
 * except LevelUp, which is an MP3 under Resources/Sounds/ -- see the
 * special case in U3AudioLoadEffect() below. */
static const char *U3AudioEffectFileName(U3SoundEffect effect) {
    switch (effect) {
        case U3SoundEffectAlarm: return "Alarm";
        case U3SoundEffectAttack: return "Attack";
        case U3SoundEffectBigDeath: return "BigDeath";
        case U3SoundEffectBump: return "Bump";
        case U3SoundEffectCombatStart: return "CombatStart";
        case U3SoundEffectCombatVictory: return "CombatVictory";
        case U3SoundEffectCreak: return "Creak";
        case U3SoundEffectDeathFemale: return "DeathFemale";
        case U3SoundEffectDeathMale: return "DeathMale";
        case U3SoundEffectDownwards: return "Downwards";
        case U3SoundEffectError1: return "Error1";
        case U3SoundEffectError2: return "Error2";
        case U3SoundEffectFailedSpell: return "FailedSpell";
        case U3SoundEffectForceField: return "ForceField";
        case U3SoundEffectHeal: return "Heal";
        case U3SoundEffectHit: return "Hit";
        case U3SoundEffectHorseWalk: return "HorseWalk";
        case U3SoundEffectImmolate: return "Immolate";
        case U3SoundEffectInvocation: return "Invocation";
        case U3SoundEffectLBLevelRise: return "LBLevelRise";
        case U3SoundEffectLevelUp: return "ExpLevelUp"; /* MP3; not decoded, see below */
        case U3SoundEffectMiscSpell: return "MiscSpell";
        case U3SoundEffectMonsterSpell: return "MonsterSpell";
        case U3SoundEffectMoongate: return "Moongate";
        case U3SoundEffectMountHorse: return "MountHorse";
        case U3SoundEffectOuch: return "Ouch";
        case U3SoundEffectShoot: return "Shoot";
        case U3SoundEffectShrine: return "Shrine";
        case U3SoundEffectSink: return "Sink";
        case U3SoundEffectStep: return "Step";
        case U3SoundEffectSwish1: return "Swish1";
        case U3SoundEffectSwish2: return "Swish2";
        case U3SoundEffectSwish3: return "Swish3";
        case U3SoundEffectSwish4: return "Swish4";
        case U3SoundEffectTorchIgnite: return "TorchIgnite";
        case U3SoundEffectUpwards: return "Upwards";
    }
    return NULL;
}

static bool U3AudioLoadEffect(U3SoundEffect effect) {
    if (effect < 0 || effect > U3SoundEffectUpwards) return false;
    U3AudioEffectCache *cache = &gEffectCache[effect];
    if (cache->attempted) return cache->ok;
    cache->attempted = true;

    if (effect == U3SoundEffectLevelUp) {
        /* Exported as an MP3 (Resources/Sounds/ExpLevelUp.mp3); SDL2 alone
         * cannot decode MP3 and no MP3 decoder is staged for this build.
         * Honest gap, reported rather than faked -- see task report. */
        fprintf(stderr, "U3Audio: LevelUp effect is MP3-encoded; MP3 decoding is not implemented, skipping.\n");
        return false;
    }

    const char *name = U3AudioEffectFileName(effect);
    if (!name || !gAssetsDirectory) return false;

    char relative[96];
    snprintf(relative, sizeof relative, "Resources/SoundsPCM/%s.wav", name);
    char *path = U3AudioJoinAssetPath(relative);
    if (!path) return false;

    SDL_AudioSpec wavSpec;
    Uint8 *wavBuffer = NULL;
    Uint32 wavLength = 0;
    SDL_RWops *rw = SDL_RWFromFile(path, "rb");
    free(path);
    if (!rw) {
        fprintf(stderr, "U3Audio: effect asset missing: %s\n", name);
        return false;
    }
    if (!SDL_LoadWAV_RW(rw, 1, &wavSpec, &wavBuffer, &wavLength)) {
        fprintf(stderr, "U3Audio: failed to decode %s.wav: %s\n", name, SDL_GetError());
        return false;
    }

    SDL_AudioCVT cvt;
    int build = SDL_BuildAudioCVT(&cvt, wavSpec.format, wavSpec.channels, wavSpec.freq,
                                  U3_AUDIO_FORMAT, U3_AUDIO_CHANNELS, U3_AUDIO_FREQ);
    if (build < 0) {
        fprintf(stderr, "U3Audio: cannot convert %s.wav: %s\n", name, SDL_GetError());
        SDL_FreeWAV(wavBuffer);
        return false;
    }

    if (build == 0) {
        /* Already in the target format. */
        int16_t *copy = malloc(wavLength ? wavLength : 1);
        if (!copy) { SDL_FreeWAV(wavBuffer); return false; }
        memcpy(copy, wavBuffer, wavLength);
        cache->samples = copy;
        cache->length = wavLength;
    } else {
        cvt.len = (int)wavLength;
        cvt.buf = malloc((size_t)wavLength * (size_t)cvt.len_mult);
        if (!cvt.buf) { SDL_FreeWAV(wavBuffer); return false; }
        memcpy(cvt.buf, wavBuffer, wavLength);
        if (SDL_ConvertAudio(&cvt) < 0) {
            fprintf(stderr, "U3Audio: conversion failed for %s.wav: %s\n", name, SDL_GetError());
            free(cvt.buf);
            SDL_FreeWAV(wavBuffer);
            return false;
        }
        cache->samples = (int16_t *)cvt.buf;
        cache->length = (Uint32)cvt.len_cvt;
    }
    SDL_FreeWAV(wavBuffer);
    cache->ok = true;
    return true;
}

static int U3AudioFindFreeVoice(void) {
    for (int i = U3_AUDIO_FIRST_EFFECT_VOICE; i < U3_AUDIO_VOICE_COUNT; ++i) {
        if (!gVoices[i].active) return i;
    }
    return -1;
}

void U3AudioPlaySound(U3SoundEffect effect, bool forceAsync) {
    /* Our mixer never blocks the caller, so forceAsync (which on macOS only
     * controlled whether the cursor was hidden during a *synchronous*
     * QuickTime play call) has nothing to toggle here. */
    (void)forceAsync;
    if (gHardDisabled || gSoundIncapable || gSettings.soundDisabled || !gDeviceOpen) return;
    if (!U3AudioLoadEffect(effect)) return;

    U3AudioEffectCache *cache = &gEffectCache[effect];
    SDL_LockAudioDevice(gDevice);
    int slot = U3AudioFindFreeVoice();
    if (slot >= 0) {
        gVoices[slot].data = (const uint8_t *)cache->samples;
        gVoices[slot].length = cache->length;
        gVoices[slot].position = 0;
        gVoices[slot].volume = gEffectVolumeMix;
        gVoices[slot].active = true;
    }
    SDL_UnlockAudioDevice(gDevice);
    /* slot < 0 means every channel is busy; dropping the newest sound is an
     * acceptable fallback at this milestone (the original commented-out
     * PlaySound() in UltimaSound.m busy-waited for a free channel instead,
     * which we must not do on a mixer thread we don't control). */
}

/* ======================================================================
 * Legacy fade tone (Exodus dissolve). PrimeLegacySample is a genuine no-op
 * on macOS too (U3AudioLegacy.m: "(void)sampleData;") -- the tone itself is
 * entirely synthetic, not derived from the primed sample. PlayLegacyFadeTone
 * likewise ignores `pass` on macOS; it only gates "already playing, do
 * nothing" vs. "start the one clip". We reproduce the exact noise synthesis
 * from UltimaSound.m's FadeToneData() so the audible result matches.
 * ==================================================================== */

void U3AudioPrimeLegacySample(const uint8_t *sampleData) {
    (void)sampleData;
}

static void U3AudioBuildFadeTone(void) {
    if (gFadeToneBuffer) return;
    gFadeToneBuffer = malloc((size_t)U3_FADE_TONE_FRAMES * U3_AUDIO_CHANNELS * sizeof(int16_t));
    if (!gFadeToneBuffer) return;

    uint32_t lfsr = 0x1FFFF;
    uint32_t noiseClock = 0;
    int noiseBit = 0;
    for (int i = 0; i < U3_FADE_TONE_FRAMES; ++i) {
        if (++noiseClock >= 7) {
            noiseClock = 0;
            uint32_t feedback = (lfsr ^ (lfsr >> 3)) & 1u;
            noiseBit = (int)(lfsr & 1u);
            lfsr = (lfsr >> 1) | (feedback << 16);
        }
        double attack = (double)i / (U3_AUDIO_FREQ * 0.04);
        if (attack > 1.0) attack = 1.0;
        double release = (double)(U3_FADE_TONE_FRAMES - i) / (U3_AUDIO_FREQ * 0.08);
        if (release > 1.0) release = 1.0;
        double envelope = attack * release;
        double sample = (noiseBit ? 0.58 : -0.58) * envelope;
        if (sample > 1.0) sample = 1.0;
        if (sample < -1.0) sample = -1.0;
        int16_t scaled = (int16_t)(sample * 28000.0);
        gFadeToneBuffer[2 * i] = scaled;
        gFadeToneBuffer[2 * i + 1] = scaled;
    }
}

void U3AudioPlayLegacyFadeTone(int32_t pass) {
    (void)pass;
    if (gHardDisabled || gSoundIncapable || gSettings.soundDisabled || !gDeviceOpen) return;
    if (gFadeTonePlaying) return;

    U3AudioBuildFadeTone();
    if (!gFadeToneBuffer) return;

    SDL_LockAudioDevice(gDevice);
    gVoices[U3_AUDIO_FADE_VOICE].data = (const uint8_t *)gFadeToneBuffer;
    gVoices[U3_AUDIO_FADE_VOICE].length = (Uint32)U3_FADE_TONE_FRAMES * U3_AUDIO_CHANNELS * sizeof(int16_t);
    gVoices[U3_AUDIO_FADE_VOICE].position = 0;
    gVoices[U3_AUDIO_FADE_VOICE].volume = (int)(SDL_MIX_MAXVOLUME * 0.75f);
    gVoices[U3_AUDIO_FADE_VOICE].active = true;
    gFadeTonePlaying = true;
    SDL_UnlockAudioDevice(gDevice);
}

void U3AudioStopLegacyFadeTone(void) {
    gFadeTonePlaying = false;
    if (!gDeviceOpen) return;
    SDL_LockAudioDevice(gDevice);
    gVoices[U3_AUDIO_FADE_VOICE].active = false;
    SDL_UnlockAudioDevice(gDevice);
}

/* ======================================================================
 * Music: FluidSynth rendered through U3AudioCallback above, scheduled by
 * fluid_player_t. This sidesteps FluidSynth's own audio driver entirely
 * (no "audio.driver" setting, no second audio backend to fail in CI) --
 * we only ever pull already-scheduled samples via fluid_synth_write_s16,
 * which works identically whether the SDL device is real or "dummy".
 * ==================================================================== */

static bool U3AudioMusicIsPlaying(void) {
    return gFluidPlayer != NULL && fluid_player_get_status(gFluidPlayer) == FLUID_PLAYER_PLAYING;
}

static void U3AudioStopSong(void) {
    fluid_player_t *old = NULL;
    if (gDeviceOpen) SDL_LockAudioDevice(gDevice);
    old = gFluidPlayer;
    gFluidPlayer = NULL;
    if (gDeviceOpen) SDL_UnlockAudioDevice(gDevice);
    if (old) {
        fluid_player_stop(old);
        fluid_player_join(old);
        delete_fluid_player(old);
    }
    free(gCurrentSongPath);
    gCurrentSongPath = NULL;
}

void U3AudioSetUpMusic(void) {
    if (gMusicIncapable) return;
    if (gHardDisabled || !gAssetsDirectory) {
        gMusicIncapable = true;
        return;
    }
    /* Music is rendered through the same SDL device/callback as effects
     * (see U3AudioCallback above) -- if that device never opened, there is
     * no path to a speaker at all, so music is genuinely incapable too,
     * not just silently unheard. This requires U3AudioOpenEffects() to run
     * first, which is already the order the game uses (UltimaMain.c). */
    if (!gDeviceOpen) {
        fprintf(stderr, "U3Audio: no audio device open; music unavailable.\n");
        gMusicIncapable = true;
        return;
    }

    gFluidSettings = new_fluid_settings();
    if (!gFluidSettings) {
        fprintf(stderr, "U3Audio: FluidSynth settings could not be created; music unavailable.\n");
        gMusicIncapable = true;
        return;
    }
    fluid_settings_setnum(gFluidSettings, "synth.sample-rate", (double)U3_AUDIO_FREQ);

    gFluidSynth = new_fluid_synth(gFluidSettings);
    if (!gFluidSynth) {
        fprintf(stderr, "U3Audio: FluidSynth synth could not be created; music unavailable.\n");
        delete_fluid_settings(gFluidSettings);
        gFluidSettings = NULL;
        gMusicIncapable = true;
        return;
    }

    const char *override = getenv("U3_MIDI_SOUNDBANK");
    char *soundfontPath = (override && override[0]) ? strdup(override)
                                                      : U3AudioJoinAssetPath("Resources/MusicMIDI/GeneralUser-GS.sf2");
    bool loaded = soundfontPath && fluid_synth_sfload(gFluidSynth, soundfontPath, 1) != FLUID_FAILED;
    if (!loaded) {
        fprintf(stderr, "U3Audio: SoundFont unavailable (%s); music unavailable.\n",
                soundfontPath ? soundfontPath : "no path");
        free(soundfontPath);
        delete_fluid_synth(gFluidSynth);
        gFluidSynth = NULL;
        delete_fluid_settings(gFluidSettings);
        gFluidSettings = NULL;
        gMusicIncapable = true;
        return;
    }
    free(soundfontPath);

    gMusicGain = 0.2f * ((float)U3AudioVolumeMix(gSettings.musicVolumePercent) / (float)SDL_MIX_MAXVOLUME);
    fluid_synth_set_gain(gFluidSynth, gMusicGain);
    gMusicReady = true;
}

void U3AudioCloseMusic(void) {
    U3AudioStopSong();
    if (gFluidSynth) { delete_fluid_synth(gFluidSynth); gFluidSynth = NULL; }
    if (gFluidSettings) { delete_fluid_settings(gFluidSettings); gFluidSettings = NULL; }
    gMusicReady = false;
}

void U3AudioStopMusic(void) {
    /* Mirrors EndSong() in UltimaSound.m: a short fade rather than a hard
     * cut. We can't use U3PlatformTickCount/WaitTicks (that module belongs
     * to a different, concurrently-written task), so we use SDL_Delay for
     * a short, fixed-length ramp instead -- same audible intent, different
     * clock source. */
    if (!gFluidPlayer || !gFluidSynth) return;
    const int steps = 12;
    for (int i = 0; i < steps; ++i) {
        float gain = gMusicGain * (1.0f - (float)i / (float)steps);
        fluid_synth_set_gain(gFluidSynth, gain);
        SDL_Delay(4);
    }
    U3AudioStopSong();
    if (gFluidSynth) fluid_synth_set_gain(gFluidSynth, gMusicGain);
}

void U3AudioUpdateMusic(void) {
    if (gMusicIncapable || !gMusicReady || gSettings.musicDisabled) return;

    extern unsigned char Party[65]; /* defined in UltimaMain.c; Party[3] == 0x80 means "in combat", exactly as UltimaSound.m's MusicUpdate() checks. */
    bool inCombat = (Party[3] == 0x80);
    if (inCombat && (gSongCurrent != 5 || gSongNext != 5)) {
        gSongCurrent = 5;
        gSongNext = 5;
        if (gSongPlaying != 5) gSongPlaying = 0;
    }

    if (!U3AudioMusicIsPlaying()) {
        if (gSongNext == gSongCurrent) {
            if (gFluidPlayer) {
                fluid_player_seek(gFluidPlayer, 0);
                fluid_player_play(gFluidPlayer);
            }
        } else {
            U3AudioStopSong();
            gSongCurrent = gSongNext;
            gSongPlaying = 0;
        }
    }

    if (gSongCurrent == gSongPlaying && gSongPlaying != 0) return;
    if (gSongCurrent > 0x10) gSongCurrent = 0;
    if (gSongCurrent == 0) gSongCurrent = gSongNext;
    gSongPlaying = gSongCurrent;
    if (gSongCurrent == 0) {
        if (gFluidPlayer && U3AudioMusicIsPlaying()) U3AudioStopSong();
        return;
    }

    int songid = '0' + gSongPlaying;
    if (songid > '9') songid += 7;
    if (songid == '9' || songid == 'C' || songid == 'D' || songid == 'E' || songid == 'F') return;

    U3AudioStopSong();

    char relative[64];
    snprintf(relative, sizeof relative, "Resources/MusicMIDI/Song_%c.mid", (char)songid);
    char *path = U3AudioJoinAssetPath(relative);
    if (!path) {
        gSongPlaying = 0;
        fprintf(stderr, "U3Audio: music asset missing: Song_%c\n", (char)songid);
        return;
    }

    fluid_player_t *player = new_fluid_player(gFluidSynth);
    bool started = player && fluid_player_add(player, path) == FLUID_OK &&
                   fluid_player_play(player) == FLUID_OK;
    if (!started) {
        fprintf(stderr, "U3Audio: music unavailable: %s\n", path);
        if (player) delete_fluid_player(player);
        free(path);
        gSongPlaying = 0;
        return;
    }

    if (gDeviceOpen) SDL_LockAudioDevice(gDevice);
    gFluidPlayer = player;
    if (gDeviceOpen) SDL_UnlockAudioDevice(gDevice);
    gCurrentSongPath = path;
}

/* ======================================================================
 * Preferences
 * ==================================================================== */

void U3AudioApplyPreferences(void) {
    gEffectVolumeMix = U3AudioVolumeMix(gSettings.soundVolumePercent);

    if (gFluidSynth) {
        gMusicGain = 0.2f * ((float)U3AudioVolumeMix(gSettings.musicVolumePercent) / (float)SDL_MIX_MAXVOLUME);
        fluid_synth_set_gain(gFluidSynth, gMusicGain);
    }

    /* Mirrors ApplyVolumePreferences() in UltimaSound.m exactly, including
     * its gSongPlaying = 0xFF trick to force UpdateMusic() to (re)start. */
    bool isPlayingMusic = (gFluidPlayer != NULL && gSongPlaying != 0);
    bool shouldPlayMusic = gMusicReady && !gMusicIncapable && !gSettings.musicDisabled;
    if (isPlayingMusic != shouldPlayMusic) {
        if (shouldPlayMusic) {
            gSongPlaying = 0xFF;
            gSongCurrent = 0;
            U3AudioUpdateMusic();
        } else {
            U3AudioStopSong();
            gSongPlaying = 0;
        }
    }
}

/* ======================================================================
 * Speech synthesis: delegated to pluggable U3LinuxSpeech subsystem
 * ==================================================================== */

void U3AudioSetUpSpeech(void) {
    U3LinuxSpeechInit(gAssetsDirectory);
}

void U3AudioCloseSpeech(void) {
    U3LinuxSpeechShutdown();
}

void U3AudioSpeakText(const char *text, int16_t voiceID) {
    if (!text || !*text)
        return;

    char voiceName[64] = {0};
    if (GetPascalStringFromArrayByIndex && voiceID > 0) {
        uint8_t pstr[256] = {0};
        GetPascalStringFromArrayByIndex(pstr, "TilesVoices", voiceID);
        if (pstr[0] > 0 && pstr[0] < sizeof(voiceName)) {
            memcpy(voiceName, pstr + 1, pstr[0]);
            voiceName[pstr[0]] = '\0';
        }
    }

    U3LinuxSpeechSpeak(text, voiceID, voiceName[0] ? voiceName : NULL);
}

void U3AudioSpeakPascalString(uint8_t *pascalString, int16_t voiceID) {
    if (!pascalString || pascalString[0] == 0)
        return;

    char buffer[256];
    uint8_t len = pascalString[0];
    uint8_t start = 1;

    for (uint8_t i = 1; i <= len; ++i) {
        if (pascalString[i] == ':') {
            start = i + 1;
            while (start <= len && pascalString[start] == ' ')
                start++;
            break;
        }
    }

    uint8_t outLen = 0;
    for (uint8_t i = start; i <= len && outLen < sizeof(buffer) - 1; ++i) {
        buffer[outLen++] = (char)pascalString[i];
    }
    buffer[outLen] = '\0';

    if (outLen > 0)
        U3AudioSpeakText(buffer, voiceID);
}

void U3AudioSpeakMessages(int16_t messageID, int16_t additionalMessageID, int16_t voiceID) {
    if (!GetPascalStringFromArrayByIndex || messageID <= 0)
        return;

    uint8_t msg1[256] = {0};
    GetPascalStringFromArrayByIndex(msg1, "Messages", messageID - 1);
    if (msg1[0] == 0)
        return;

    char combined[512] = {0};
    size_t len1 = msg1[0];
    if (len1 >= sizeof(combined)) len1 = sizeof(combined) - 1;
    memcpy(combined, msg1 + 1, len1);
    combined[len1] = '\0';

    if (additionalMessageID > 0) {
        uint8_t msg2[256] = {0};
        GetPascalStringFromArrayByIndex(msg2, "Messages", additionalMessageID - 1);
        if (msg2[0] > 0) {
            size_t curr = strlen(combined);
            if (curr + 1 < sizeof(combined)) {
                combined[curr++] = ' ';
                size_t len2 = msg2[0];
                if (curr + len2 >= sizeof(combined))
                    len2 = sizeof(combined) - 1 - curr;
                memcpy(combined + curr, msg2 + 1, len2);
                combined[curr + len2] = '\0';
            }
        }
    }

    U3AudioSpeakText(combined, voiceID);
}

/* ======================================================================
 * Shutdown
 * ==================================================================== */

void U3LinuxAudioShutdown(void) {
    U3AudioCloseSpeech();
    U3AudioCloseMusic();
    U3AudioCloseEffects();
    free(gAssetsDirectory);
    gAssetsDirectory = NULL;
}

/* ======================================================================
 * Test/debug introspection -- see U3LinuxAudio.h.
 * ==================================================================== */

bool U3LinuxAudioDebugEffectFrameCount(U3SoundEffect effect, size_t *outFrames) {
    if (!U3AudioLoadEffect(effect)) return false;
    if (outFrames) *outFrames = gEffectCache[effect].length / (U3_AUDIO_CHANNELS * sizeof(int16_t));
    return true;
}

bool U3LinuxAudioDebugRenderMusic(int16_t *buffer, int frames) {
    if (!gFluidSynth || !gFluidPlayer || !buffer || frames <= 0) return false;
    return fluid_synth_write_s16(gFluidSynth, frames, buffer, 0, 2, buffer, 1, 2) == FLUID_OK;
}
