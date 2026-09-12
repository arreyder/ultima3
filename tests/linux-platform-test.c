// Standalone test for Sources/Linux/U3LinuxPlatform.c. Builds and links
// against *only* U3LinuxPlatform.c (no game core, no SDL/video layer) --
// that is the point: this file must prove the platform layer works on its
// own, decoupled from the concurrent video task.
//
// Uses U3_SAVE_DIRECTORY (a temp directory created below) so this never
// touches a real ~/.config.

#define _DEFAULT_SOURCE 1

#include "Linux/U3LinuxPlatform.h"
#include "U3Platform.h"
#include "U3Types.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double NowSeconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void TestTickMonotonicity(void) {
    uint32_t last = U3PlatformTickCount();
    for (int i = 0; i < 1000; ++i) {
        uint32_t now = U3PlatformTickCount();
        assert(now >= last);
        last = now;
    }
}

static void TestWaitTicksAccuracy(void) {
    /* 30 ticks @ 60Hz ~= 0.5s; 6 ticks ~= 0.1s. Generous bounds: this is
     * checking for gross scale errors (e.g. sleeping ms worth of ns, or
     * ticks worth of seconds), not microsecond precision under ASan. */
    double t0 = NowSeconds();
    U3PlatformWaitTicks(30);
    double elapsed = NowSeconds() - t0;
    assert(elapsed >= 0.35 && elapsed <= 2.0);

    t0 = NowSeconds();
    U3PlatformWaitTicks(6);
    elapsed = NowSeconds() - t0;
    assert(elapsed >= 0.05 && elapsed <= 1.0);

    /* Non-positive ticks must return immediately, not block. */
    t0 = NowSeconds();
    U3PlatformWaitTicks(0);
    U3PlatformWaitTicks(-5);
    elapsed = NowSeconds() - t0;
    assert(elapsed < 0.2);
}

static void TestRandomRawVaries(void) {
    bool sawNegative = false, sawNonNegative = false;
    int16_t first = U3PlatformRandomRaw();
    bool sawDifferent = false;
    for (int i = 0; i < 2000; ++i) {
        int16_t r = U3PlatformRandomRaw();
        if (r < 0)
            sawNegative = true;
        else
            sawNonNegative = true;
        if (r != first)
            sawDifferent = true;
    }
    assert(sawNegative && sawNonNegative);
    assert(sawDifferent);
}

static void TestRandomRangeAndUniformity(void) {
    /* low == high must always return exactly low, with no division by
     * zero or other edge-case misbehavior. */
    for (int i = 0; i < 200; ++i) {
        assert(U3PlatformRandom(42, 42) == 42);
        assert(U3PlatformRandom(0, 0) == 0);
        assert(U3PlatformRandom(65535, 65535) == 65535);
    }

    /* Small inclusive range: every draw must land in [low, high], every
     * value in that range must be reachable, and the distribution must
     * not be wildly skewed (catches modulo-bias-style bugs). */
    enum { kLow = 5, kHigh = 14, kBins = kHigh - kLow + 1, kSamples = 200000 };
    long counts[kBins] = {0};
    for (int i = 0; i < kSamples; ++i) {
        uint16_t v = U3PlatformRandom(kLow, kHigh);
        assert(v >= kLow && v <= kHigh);
        counts[v - kLow]++;
    }
    long expected = kSamples / kBins;
    for (int b = 0; b < kBins; ++b) {
        assert(counts[b] > 0);
        /* Loose tolerance (+/-40%) -- this is a sanity check against
         * gross bias, not a statistical uniformity proof. */
        assert(counts[b] > expected * 6 / 10 && counts[b] < expected * 14 / 10);
    }
}

static char *TempDir(void) {
    static char template[] = "/tmp/u3-platform-test-XXXXXX";
    char *dir = strdup(template);
    assert(dir);
    assert(mkdtemp(dir) != NULL);
    return dir;
}

static void TestPreferenceRoundTrip(const char *prefDir) {
    (void)prefDir;
    U3LinuxPlatformResetPreferencesCacheForTesting();

    /* Boolean round trip. */
    assert(!U3PlatformHasPreference(U3PreferenceSoundDisabled));
    assert(U3PlatformGetBooleanPreference(U3PreferenceSoundDisabled) == false);
    U3PlatformSetBooleanPreference(U3PreferenceSoundDisabled, true);
    assert(U3PlatformHasPreference(U3PreferenceSoundDisabled));
    assert(U3PlatformGetBooleanPreference(U3PreferenceSoundDisabled) == true);
    U3PlatformSetBooleanPreference(U3PreferenceSoundDisabled, false);
    assert(U3PlatformGetBooleanPreference(U3PreferenceSoundDisabled) == false);
    assert(U3PlatformHasPreference(U3PreferenceSoundDisabled)); /* explicit false, still "has" */

    /* Integer round trip, plus absent-vs-default. */
    assert(!U3PlatformHasPreference(U3PreferenceHealThreshold));
    assert(U3PlatformGetIntegerPreference(U3PreferenceHealThreshold) == 750); /* sane default */
    U3PlatformSetIntegerPreference(U3PreferenceHealThreshold, 500);
    assert(U3PlatformHasPreference(U3PreferenceHealThreshold));
    assert(U3PlatformGetIntegerPreference(U3PreferenceHealThreshold) == 500);
    /* Setting it back to the default value must still count as present:
     * absent and default-but-explicit are different states. */
    U3PlatformSetIntegerPreference(U3PreferenceHealThreshold, 750);
    assert(U3PlatformHasPreference(U3PreferenceHealThreshold));
    assert(U3PlatformGetIntegerPreference(U3PreferenceHealThreshold) == 750);

    U3PlatformRemovePreference(U3PreferenceHealThreshold);
    assert(!U3PlatformHasPreference(U3PreferenceHealThreshold));
    assert(U3PlatformGetIntegerPreference(U3PreferenceHealThreshold) == 750);

    /* String round trip via the Linux-only test setter (U3Platform.h has
     * no public string setter -- see U3LinuxPlatform.h for why). */
    assert(!U3PlatformHasPreference(U3PreferenceGameFont));
    uint8_t pascalBuf[256];
    assert(!U3PlatformCopyPascalStringPreference(U3PreferenceGameFont, pascalBuf, sizeof(pascalBuf)));
    char utf8Buf[256];
    assert(!U3PlatformCopyUTF8StringPreference(U3PreferenceGameFont, utf8Buf, sizeof(utf8Buf)));

    assert(U3LinuxPlatformSetStringPreferenceForTesting(U3PreferenceGameFont, "Chicago"));
    assert(U3PlatformHasPreference(U3PreferenceGameFont));
    assert(U3PlatformCopyPascalStringPreference(U3PreferenceGameFont, pascalBuf, sizeof(pascalBuf)));
    assert(pascalBuf[0] == 7 && memcmp(pascalBuf + 1, "Chicago", 7) == 0);
    assert(U3PlatformCopyUTF8StringPreference(U3PreferenceGameFont, utf8Buf, sizeof(utf8Buf)));
    assert(strcmp(utf8Buf, "Chicago") == 0);

    /* Too-small output buffers must fail honestly, not truncate silently. */
    uint8_t tinyPascal[2];
    assert(!U3PlatformCopyPascalStringPreference(U3PreferenceGameFont, tinyPascal, sizeof(tinyPascal)));
    char tinyUtf8[3];
    assert(!U3PlatformCopyUTF8StringPreference(U3PreferenceGameFont, tinyUtf8, sizeof(tinyUtf8)));

    /* A key of the wrong type must not accept the wrong-typed setter. */
    U3PlatformSetBooleanPreference(U3PreferenceGameFont, true); /* no-op: GameFont is a string key */
    assert(U3PlatformCopyUTF8StringPreference(U3PreferenceGameFont, utf8Buf, sizeof(utf8Buf)));
    assert(strcmp(utf8Buf, "Chicago") == 0);
}

static void TestAtomicRewriteSurvival(const char *prefDir) {
    U3LinuxPlatformResetPreferencesCacheForTesting();
    U3PlatformSetIntegerPreference(U3PreferenceSoundVolume, 42);
    U3PlatformSetBooleanPreference(U3PreferenceAutoSave, true);
    U3PlatformSynchronizePreferences();

    char path[4096];
    snprintf(path, sizeof(path), "%s/preferences.conf", prefDir);

    /* Sanity: the file really exists and round-trips normally first. */
    U3LinuxPlatformResetPreferencesCacheForTesting();
    assert(U3PlatformGetIntegerPreference(U3PreferenceSoundVolume) == 42);
    assert(U3PlatformGetBooleanPreference(U3PreferenceAutoSave) == true);

    /* Simulate a crash mid-write: truncate the file so the integrity
     * footer ("U3PREFSEND") is missing. A real crash can't corrupt the
     * file this way because writes go to a temp file and are only made
     * visible by an atomic rename -- this directly tests the loader's
     * half of that guarantee: a file without its footer must never be
     * trusted, not even partially. */
    FILE *f = fopen(path, "r+");
    assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f);
    assert(size > 10);
    assert(ftruncate(fileno(f), size - 5) == 0); /* chop off the footer line */
    fclose(f);

    U3LinuxPlatformResetPreferencesCacheForTesting();
    assert(!U3PlatformHasPreference(U3PreferenceSoundVolume));
    assert(!U3PlatformHasPreference(U3PreferenceAutoSave));
    assert(U3PlatformGetIntegerPreference(U3PreferenceSoundVolume) == 100); /* back to sane default */
    assert(U3PlatformGetBooleanPreference(U3PreferenceAutoSave) == false);

    /* Writing again afterwards must still work and produce a valid file. */
    U3PlatformSetIntegerPreference(U3PreferenceSoundVolume, 77);
    U3LinuxPlatformResetPreferencesCacheForTesting();
    assert(U3PlatformGetIntegerPreference(U3PreferenceSoundVolume) == 77);
}

static void TestDefaultNoInputSource(void) {
    /* With no video/SDL layer wired in, every input entry point must
     * report "nothing available" honestly and must not hang or spin. */
    double t0 = NowSeconds();
    assert(U3PlatformGetKeyMouse(2) == false); /* mode 2: no-wait poll */
    assert(NowSeconds() - t0 < 0.05);

    t0 = NowSeconds();
    assert(U3PlatformGetKeyMouse(1) == false); /* mode 1: ~5-tick wait */
    double elapsed = NowSeconds() - t0;
    assert(elapsed >= 0.03 && elapsed < 0.5);

    U3InputEvent event;
    memset(&event, 0xAA, sizeof(event)); /* poison, so "left untouched" would be visible */
    assert(U3PlatformPollInput(&event, 0) == false);
    assert(event.command == U3CommandNone);
    assert(event.direction == U3DirectionNone);
    assert(event.rawKey == 0);
    assert(event.isMouse == false);

    /* Must not crash; these are genuinely no-ops with the default source. */
    U3PlatformFlushInputEvents();
    U3PlatformFlushAllEvents();
    U3PlatformObscureCursor();

    /* CursorKey/GetDirection forward to game-core functions via weak
     * externs that are absent in this standalone test binary: must report
     * "no key" honestly rather than linking-fail or fabricating a result. */
    assert(U3PlatformCursorKey(false) == 0);
    U3PlatformGetDirection(0); /* void; must simply not crash */

    assert(U3LinuxPlatformGetInputSource() != NULL);
}

static void TestQuitFlag(void) {
    assert(U3PlatformShouldQuit() == false);
    U3PlatformRequestQuit();
    assert(U3PlatformShouldQuit() == true);
}

/* A custom input source, to prove the abstraction itself is wired
 * correctly (the video task will plug in something like this, backed by
 * SDL instead of a canned script). */
static int sScriptIndex = 0;
static bool ScriptedPoll(void *userData, uint32_t waitMs, uint8_t *outKey, bool *outIsMouse) {
    (void)userData;
    (void)waitMs;
    if (sScriptIndex == 0) {
        sScriptIndex = 1;
        *outKey = 'Q';
        *outIsMouse = false;
        return true;
    }
    return false;
}

static void TestCustomInputSource(void) {
    sScriptIndex = 0;
    U3LinuxInputSource scripted = {0};
    scripted.pollKeyMouse = ScriptedPoll;
    U3LinuxPlatformSetInputSource(&scripted);

    assert(U3PlatformGetKeyMouse(2) == true);
    assert(U3PlatformGetKeyMouse(2) == false); /* script only has one event */

    /* Restore the default so later tests (and any other code in this
     * process) see the honest no-input behavior again. */
    U3LinuxPlatformSetInputSource(NULL);
    assert(U3PlatformGetKeyMouse(2) == false);
}

int main(void) {
    char *tempDir = TempDir();
    char envBuf[4096];
    snprintf(envBuf, sizeof(envBuf), "%s", tempDir);
    setenv("U3_SAVE_DIRECTORY", envBuf, 1);

    const char *resolved = U3LinuxPlatformPreferencesDirectory();
    assert(strcmp(resolved, tempDir) == 0);

    TestTickMonotonicity();
    TestWaitTicksAccuracy();
    TestRandomRawVaries();
    TestRandomRangeAndUniformity();
    TestPreferenceRoundTrip(tempDir);
    TestAtomicRewriteSurvival(tempDir);
    TestDefaultNoInputSource();
    TestCustomInputSource();
    TestQuitFlag();

    free(tempDir);
    fprintf(stderr, "linux-platform-test: all checks passed\n");
    return 0;
}
