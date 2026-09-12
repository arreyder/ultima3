//
//  U3LinuxPlatform.c
//  Ultima3 (Linux port)
//
//  See U3LinuxPlatform.h for the input-source abstraction this file relies
//  on instead of calling into SDL directly, and Sources/U3PlatformLegacy.m
//  for the macOS reference semantics this implementation tracks.
//

#define _DEFAULT_SOURCE 1

#include "U3LinuxPlatform.h"
#include "U3Platform.h"
#include "U3Types.h"
#include "Linux/U3LegacyTypes.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* =======================================================================
 * Timing -- 60Hz monotonic ticks, matching the classic Mac TickCount()
 * rate the preserved game was written against (see UltimaSound.m using
 * U3PlatformTickCount deltas directly as a frame/volume-fade clock).
 * ===================================================================== */

static struct timespec sTickBase;
static bool sTickBaseSet = false;

static void EnsureTickBase(void) {
    if (sTickBaseSet)
        return;
    clock_gettime(CLOCK_MONOTONIC, &sTickBase);
    sTickBaseSet = true;
}

uint32_t U3PlatformTickCount(void) {
    EnsureTickBase();
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t deltaNanos = (int64_t)(now.tv_sec - sTickBase.tv_sec) * 1000000000LL +
                          (int64_t)(now.tv_nsec - sTickBase.tv_nsec);
    if (deltaNanos < 0)
        deltaNanos = 0; /* CLOCK_MONOTONIC should never go backwards; guard anyway. */
    uint64_t ticks = (uint64_t)deltaNanos * 60ULL / 1000000000ULL;
    /* Wraps after roughly 828 days of continuous uptime, same as the
     * 32-bit Mac TickCount this replaces. */
    return (uint32_t)ticks;
}

static uint32_t TicksToMilliseconds(uint32_t ticks) {
    return (uint32_t)(((uint64_t)ticks * 1000ULL) / 60ULL);
}

static void SleepNanoseconds(uint64_t nanos) {
    if (nanos == 0)
        return;
    struct timespec target;
    clock_gettime(CLOCK_MONOTONIC, &target);
    target.tv_sec += (time_t)(nanos / 1000000000ULL);
    target.tv_nsec += (long)(nanos % 1000000000ULL);
    if (target.tv_nsec >= 1000000000L) {
        target.tv_nsec -= 1000000000L;
        target.tv_sec += 1;
    }
    int rc;
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, NULL);
    } while (rc == EINTR);
}

void U3PlatformWaitTicks(int32_t ticks) {
    if (ticks <= 0)
        return;
    uint64_t nanos = (uint64_t)ticks * 1000000000ULL / 60ULL;
    SleepNanoseconds(nanos);
}

/* =======================================================================
 * Random -- mirrors Sources/UltimaMain.c's RandNum() multiply-shift
 * formula exactly (see U3PlatformLegacy.m: U3PlatformRandom calls RandNum,
 * U3PlatformRandomRaw calls Random()). The original avoids modulo bias by
 * construction (high * range / 65536 instead of high % range); we keep
 * that formula verbatim and only replace the underlying 16-bit source
 * (classic-Mac Random()/arc4random() on macOS) with a seeded splitmix64,
 * which is itself bias-free since it is never reduced with modulo -- we
 * just take 16 uniformly-distributed bits out of a 64-bit state.
 * ===================================================================== */

static uint64_t sRandomState;
static bool sRandomSeeded = false;

static void SeedRandomIfNeeded(void) {
    if (sRandomSeeded)
        return;
    uint64_t seed = 0;
    if (getrandom(&seed, sizeof(seed), 0) != (ssize_t)sizeof(seed)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        seed = (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)getpid();
    }
    if (seed == 0)
        seed = 0x9E3779B97F4A7C15ULL; /* avoid the degenerate all-zero splitmix64 state */
    sRandomState = seed;
    sRandomSeeded = true;
}

static uint64_t NextSplitMix64(void) {
    SeedRandomIfNeeded();
    uint64_t z = (sRandomState += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

int16_t U3PlatformRandomRaw(void) {
    uint64_t r = NextSplitMix64();
    return (int16_t)(uint16_t)(r & 0xFFFFu);
}

uint16_t U3PlatformRandom(uint16_t low, uint16_t high) {
    /* Verbatim port of RandNum() in Sources/UltimaMain.c:
     *   qdRdm = Random();
     *   rangernd = (highrnd + 1) - lowrnd;
     *   return ((qdRdm * rangernd) / 65536) + lowrnd;
     * low > high behaves identically to the original
     * (undefined-but-deterministic) rather than being "fixed" into
     * different behavior. low == high yields rangernd == 1, so the
     * division collapses to exactly `low`.
     *
     * The intermediate is 64-bit because the original's `long` is 64-bit
     * on the 64-bit Mac build: a full-width range (low 0, high 65535)
     * makes raw * rangernd reach 65535 * 65536, which overflows int32_t.
     * No current caller is that wide, but narrowing it here would make
     * Linux trap where macOS computes a value. */
    uint16_t raw = (uint16_t)U3PlatformRandomRaw();
    int64_t rangernd = (int64_t)high + 1 - (int64_t)low;
    return (uint16_t)(((int64_t)raw * rangernd) / 65536) + low;
}

/* =======================================================================
 * Input source abstraction (see U3LinuxPlatform.h)
 * ===================================================================== */

static bool DefaultPollKeyMouse(void *userData, uint32_t waitMs, uint8_t *outKey, bool *outIsMouse) {
    (void)userData;
    (void)outKey;
    (void)outIsMouse;
    /* Honestly wait out the requested time (so callers that loop on this
     * don't busy-spin a CPU core), then honestly report nothing arrived. */
    if (waitMs > 0)
        SleepNanoseconds((uint64_t)waitMs * 1000000ULL);
    return false;
}

static bool DefaultGetMousePoint(void *userData, int16_t *outX, int16_t *outY) {
    (void)userData;
    (void)outX;
    (void)outY;
    return false;
}

static void DefaultFlush(void *userData) {
    (void)userData;
}

static void DefaultObscureCursor(void *userData) {
    (void)userData;
}

static const U3LinuxInputSource kDefaultInputSource = {
    .pollKeyMouse = DefaultPollKeyMouse,
    .getMousePoint = DefaultGetMousePoint,
    .flush = DefaultFlush,
    .obscureCursor = DefaultObscureCursor,
    .userData = NULL,
};

static U3LinuxInputSource sActiveInputSource;
static bool sActiveInputSourceInitialized = false;

const U3LinuxInputSource *U3LinuxPlatformGetInputSource(void) {
    if (!sActiveInputSourceInitialized) {
        sActiveInputSource = kDefaultInputSource;
        sActiveInputSourceInitialized = true;
    }
    return &sActiveInputSource;
}

void U3LinuxPlatformSetInputSource(const U3LinuxInputSource *source) {
    sActiveInputSource = source ? *source : kDefaultInputSource;
    sActiveInputSourceInitialized = true;
}

/* =======================================================================
 * Input entry points
 *
 * Sources/U3PlatformLegacy.m's U3PlatformGetKeyMouse replicates a fair
 * amount of game-specific logic inline (macro-key playback, main-menu
 * button hit testing via U3ButtonBounds/Player[], cursor-direction state
 * via CursorUpdate/gMouseState). That logic lives in already-ported,
 * already-compiling shared game sources (UltimaGraphics.c / UltimaMisc.c /
 * UltimaMain.c) -- it is not something a platform-services file should
 * reimplement, and this file must stay linkable on its own (no game core)
 * for its tests. So: the two entry points that are pure passthroughs on
 * mac (CursorKey, GetDirection) forward to those same shared functions via
 * *weak* extern references -- when the real game core is linked in, they
 * behave exactly as the mac port does; when it is not (this file's own
 * standalone compile/test), the weak symbols resolve to NULL and we
 * honestly report "nothing happened" rather than faking a result.
 *
 * The genuinely platform-specific entry points (GetKeyMouse, WaitKeyMouse,
 * Flush*, ObscureCursor, PollInput) go through the input-source
 * abstraction only, decoupled from the concurrent SDL/video task.
 *
 * gKeyPress/gMouseKey are shared-game globals that ~90 call sites across
 * the preserved sources read directly rather than going back through
 * U3Platform.h. They are mirrored here (again via weak externs) purely
 * for compatibility when linked against the real game core; this file's
 * own notion of "the last key/mouse event" does not depend on them.
 * ===================================================================== */

extern char CursorKey(bool usePenLoc) __attribute__((weak));
extern void GetDirection(short mode) __attribute__((weak));
extern char gKeyPress __attribute__((weak));
extern short gMouseKey __attribute__((weak));
extern bool gDone __attribute__((weak));
extern short gMouseState __attribute__((weak));
extern short blkSiz __attribute__((weak));
extern short gUpdateWhere __attribute__((weak));
extern short gCurMouseDir __attribute__((weak));
extern char Macro[] __attribute__((weak));
extern void DecMacro(void) __attribute__((weak));
extern char U3MainMenuButtonKey(Point mouse) __attribute__((weak));
extern void CursorUpdate(void) __attribute__((weak));
extern void U3CocoaPresentMainSurface(void) __attribute__((weak));

static void SyncLegacyKeyGlobals(char key, bool isMouse) {
    if (&gKeyPress)
        gKeyPress = key;
    if (&gMouseKey)
        gMouseKey = isMouse ? 1 : 0;
}

static bool sQuitRequested = false;

bool U3PlatformShouldQuit(void) {
    return sQuitRequested;
}

void U3PlatformRequestQuit(void) {
    sQuitRequested = true;
}

static bool GameOrHostRequestedQuit(void) {
    if (sQuitRequested)
        return true;
    return (&gDone) ? gDone : false;
}

static char sLastKeyChar = 0;
static bool sLastWasMouse = false;

bool U3PlatformGetKeyMouse(uint8_t mode) {
    SyncLegacyKeyGlobals(0, false);
    if (GameOrHostRequestedQuit())
        return false;

    if (&Macro && &DecMacro && Macro[0]) {
        sLastKeyChar = Macro[0];
        DecMacro();
        sLastWasMouse = true;
        SyncLegacyKeyGlobals(sLastKeyChar, sLastWasMouse);
        return true;
    }

    /* Matches U3PlatformLegacy.m: mode 2 polls immediately, anything else
     * waits up to 5 ticks (~83ms) for an event. */
    uint32_t waitMs = (mode == 2) ? 0 : TicksToMilliseconds(5);
    uint8_t key = 0;
    bool isMouse = false;
    const U3LinuxInputSource *src = U3LinuxPlatformGetInputSource();
    bool got = src->pollKeyMouse && src->pollKeyMouse(src->userData, waitMs, &key, &isMouse);
    if (!got)
        return false;

    sLastKeyChar = (char)key;
    sLastWasMouse = isMouse;

    if (isMouse && &gMouseState) {
        int16_t mx = 0, my = 0;
        Point mouse = {0, 0};
        if (src->getMousePoint && src->getMousePoint(src->userData, &mx, &my)) {
            mouse.h = mx;
            mouse.v = my;
        }
        short bs = (&blkSiz) ? blkSiz : 16;
        if (gMouseState == 4) {
            for (int i = 0; i < 4; ++i) {
                if (mouse.h >= bs * 24 && mouse.h < bs * 39 &&
                    mouse.v >= bs * (1 + i * 4) && mouse.v < bs * (4 + i * 4)) {
                    sLastKeyChar = (char)('1' + i);
                    sLastWasMouse = true;
                    SyncLegacyKeyGlobals(sLastKeyChar, sLastWasMouse);
                    return true;
                }
            }
            sLastKeyChar = 0;
            SyncLegacyKeyGlobals(0, false);
            return false;
        }
        if (&U3MainMenuButtonKey) {
            char menuKey = U3MainMenuButtonKey(mouse);
            if (menuKey) {
                sLastKeyChar = menuKey;
                sLastWasMouse = false;
                SyncLegacyKeyGlobals(sLastKeyChar, sLastWasMouse);
                return true;
            }
        }
        if (&gUpdateWhere && (gUpdateWhere == 5 || gUpdateWhere == 6)) {
            sLastKeyChar = 0;
            SyncLegacyKeyGlobals(0, false);
            return false;
        }
        if (&gCurMouseDir)
            gCurMouseDir = 0;
        if (&CursorUpdate)
            CursorUpdate();
        if (&gCurMouseDir && gCurMouseDir)
            sLastKeyChar = (char)gCurMouseDir;

        short uw = (&gUpdateWhere) ? gUpdateWhere : 0;
        bool keep = sLastKeyChar != 0 || (uw == 1 || uw == 2 || uw == 7);
        SyncLegacyKeyGlobals(sLastKeyChar, sLastWasMouse);
        return keep;
    }

    SyncLegacyKeyGlobals(sLastKeyChar, sLastWasMouse);
    return got;
}

int16_t U3PlatformWaitKeyMouse(void) {
    if (&U3CocoaPresentMainSurface)
        U3CocoaPresentMainSurface();
    while (!GameOrHostRequestedQuit() && !U3PlatformGetKeyMouse(1)) {
        /* U3PlatformGetKeyMouse(1) itself waits ~83ms per call via the
         * input source (or the default source's honest sleep), so this
         * is not a busy loop. */
    }
    if (GameOrHostRequestedQuit())
        return 0;
    return (int16_t)(unsigned char)sLastKeyChar;
}

char U3PlatformCursorKey(bool usePenLocation) {
    if (&CursorKey)
        return CursorKey(usePenLocation);
    /* Game core not linked (standalone compile/tests): honestly report no
     * key rather than synthesizing one. */
    return 0;
}

void U3PlatformGetDirection(int16_t mode) {
    if (&GetDirection)
        GetDirection((short)mode);
    /* else: no-op. The function is void, so there is nothing to report;
     * it simply does not move the party, which is the honest outcome of
     * "no direction input is available." */
}

void U3PlatformFlushInputEvents(void) {
    const U3LinuxInputSource *src = U3LinuxPlatformGetInputSource();
    if (src->flush)
        src->flush(src->userData);
}

void U3PlatformFlushAllEvents(void) {
    U3PlatformFlushInputEvents();
}

void U3PlatformObscureCursor(void) {
    const U3LinuxInputSource *src = U3LinuxPlatformGetInputSource();
    if (src->obscureCursor)
        src->obscureCursor(src->userData);
}

bool U3PlatformPollInput(U3InputEvent *event, uint32_t timeoutTicks) {
    if (event) {
        event->command = U3CommandNone;
        event->direction = U3DirectionNone;
        event->rawKey = 0;
        event->isMouse = false;
    }
    if (GameOrHostRequestedQuit())
        return false;
    uint32_t waitMs = TicksToMilliseconds(timeoutTicks);
    uint8_t key = 0;
    bool isMouse = false;
    const U3LinuxInputSource *src = U3LinuxPlatformGetInputSource();
    bool got = src->pollKeyMouse && src->pollKeyMouse(src->userData, waitMs, &key, &isMouse);
    if (got) {
        sLastKeyChar = (char)key;
        sLastWasMouse = isMouse;
        SyncLegacyKeyGlobals(sLastKeyChar, sLastWasMouse);
        if (event) {
            event->rawKey = key;
            event->isMouse = isMouse;
        }
    }
    return got;
}

/* =======================================================================
 * Preferences
 *
 * One flat file, "preferences.conf", under the resolved preferences
 * directory (see U3LinuxPlatformPreferencesDirectory). Format:
 *
 *   U3PREFS 1
 *   <Name>=<value>
 *   ...
 *   U3PREFSEND
 *
 * The header and footer lines are an integrity check, not decoration: a
 * file that does not end with "U3PREFSEND" (e.g. truncated by a crash
 * mid-write, or -- since writes are themselves atomic below -- truncated
 * by something else entirely) is rejected as a whole rather than having
 * its partial contents trusted. Absence of the file is not an error: it
 * just means no preferences have been stored yet.
 *
 * Every Set-family/Remove call persists immediately (not just on
 * U3PlatformSynchronizePreferences) via a temp-file + fsync + rename +
 * directory-fsync sequence, so a crash can lose at most the most recent
 * call, never corrupt the file on disk. Synchronize still exists as an
 * explicit flush point for API-shape parity with the mac CFPreferences
 * version, and because both Set calls return void, it doubles as the only
 * way this file can be told "make sure this is actually durable now."
 * ===================================================================== */

typedef enum { PrefTypeBool, PrefTypeInt, PrefTypeString } PrefType;

typedef struct {
    const char *name;
    PrefType type;
    int32_t intDefault;
} PrefMeta;

/* Defaults for integer keys come from Sources/PrefsDialog.m, which lazily
 * writes these exact values the first time the dialog notices the key is
 * unset (HealThreshold < 1 -> 750; Sound/MusicVolume < 1 -> 100). Window
 * positions default to 0, which Sources/UltimaMacIF.c's PlaceWindow()
 * already treats as "not yet placed, center the window" -- so 0 is a
 * correct "absent" sentinel, not a guess. Every boolean key's absent value
 * is false, matching the mac reference (CFPreferencesGetAppBooleanValue
 * returns false for an unset key, with no per-key override). String keys
 * report "absent" as a failed Copy call, never synthesized text -- every
 * call site that reads one already falls back to its own compiled-in
 * default when the Copy call returns false (see e.g. UltimaText.c:259).
 */
static const PrefMeta kPrefMeta[] = {
    [U3PreferenceSoundDisabled] = {"SoundDisabled", PrefTypeBool, 0},
    [U3PreferenceMusicDisabled] = {"MusicDisabled", PrefTypeBool, 0},
    [U3PreferenceSpeechDisabled] = {"SpeechDisabled", PrefTypeBool, 0},
    [U3PreferenceUnconstrainedSpeed] = {"UnconstrainedSpeed", PrefTypeBool, 0},
    [U3PreferenceOriginalSize] = {"OriginalSize", PrefTypeBool, 0},
    [U3PreferenceFullScreen] = {"FullScreen", PrefTypeBool, 0},
    [U3PreferenceClassicAppearance] = {"ClassicAppearance", PrefTypeBool, 0},
    [U3PreferenceIncludeWind] = {"IncludeWind", PrefTypeBool, 0},
    [U3PreferenceNoDiagonals] = {"NoDiagonals", PrefTypeBool, 0},
    [U3PreferenceAutoSave] = {"AutoSave", PrefTypeBool, 0},
    [U3PreferenceManualCombat] = {"ManualCombat", PrefTypeBool, 0},
    [U3PreferenceNoAutoHeal] = {"NoAutoHeal", PrefTypeBool, 0},
    [U3PreferenceAsyncSound] = {"AsyncSound", PrefTypeBool, 0},
    [U3PreferenceDontAskDisplayMode] = {"DontAskDisplayMode", PrefTypeBool, 0},
    [U3PreferenceNoEducateAboutFullScreen] = {"NoEducateAboutFullScreen", PrefTypeBool, 0},
    [U3PreferenceFullScreenResolutionChange] = {"FullScreenResolutionChange", PrefTypeBool, 0},
    [U3PreferenceHealThreshold] = {"HealThreshold", PrefTypeInt, 750},
    [U3PreferenceSoundVolume] = {"SoundVolume", PrefTypeInt, 100},
    [U3PreferenceMusicVolume] = {"MusicVolume", PrefTypeInt, 100},
    [U3PreferenceCurrentWindowX] = {"CurrentWindowX", PrefTypeInt, 0},
    [U3PreferenceCurrentWindowY] = {"CurrentWindowY", PrefTypeInt, 0},
    [U3PreferenceSaveWindowX] = {"SaveWindowX", PrefTypeInt, 0},
    [U3PreferenceSaveWindowY] = {"SaveWindowY", PrefTypeInt, 0},
    [U3PreferenceGameFont] = {"GameFont", PrefTypeString, 0},
    [U3PreferenceTileSet] = {"TileSet", PrefTypeString, 0},
};
#define kPrefCount (sizeof(kPrefMeta) / sizeof(kPrefMeta[0]))

typedef struct {
    bool present;
    bool boolValue;
    int32_t intValue;
    char *stringValue;
} PrefSlot;

static PrefSlot sSlots[kPrefCount];
static bool sLoaded = false;

static bool ValidKey(U3PreferenceKey key) {
    return (unsigned)key < kPrefCount;
}

static char *BuildPath(const char *dir, const char *file) {
    size_t n = strlen(dir) + 1 + strlen(file) + 1;
    char *out = malloc(n);
    if (out)
        snprintf(out, n, "%s/%s", dir, file);
    return out;
}

static char *ResolvePreferencesDirectory(void) {
    const char *override = getenv("U3_SAVE_DIRECTORY");
    if (override && *override)
        return strdup(override);
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char *base = NULL;
    if (xdg && *xdg) {
        base = strdup(xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home)
            home = "."; /* degraded but honest last resort */
        base = BuildPath(home, ".config");
    }
    if (!base)
        return NULL;
    char *full = BuildPath(base, "ultima3");
    free(base);
    return full;
}

static char *sPublicDirCache = NULL;

const char *U3LinuxPlatformPreferencesDirectory(void) {
    free(sPublicDirCache);
    sPublicDirCache = ResolvePreferencesDirectory();
    return sPublicDirCache ? sPublicDirCache : "";
}

static bool EnsureDirectoryExists(const char *path) {
    size_t len = strlen(path);
    if (len == 0 || len >= PATH_MAX)
        return false;
    char buffer[PATH_MAX];
    memcpy(buffer, path, len + 1);
    for (size_t i = 1; i < len; ++i) {
        if (buffer[i] == '/') {
            buffer[i] = '\0';
            if (mkdir(buffer, 0700) != 0 && errno != EEXIST)
                return false;
            buffer[i] = '/';
        }
    }
    if (mkdir(buffer, 0700) != 0 && errno != EEXIST)
        return false;
    return true;
}

static char *EncodeString(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len * 2 + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') {
            out[o++] = '\\';
            out[o++] = '\\';
        } else if (c == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (c == '\r') {
            out[o++] = '\\';
            out[o++] = 'r';
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return out;
}

static char *DecodeString(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] == '\\' && i + 1 < len) {
            char c = s[++i];
            if (c == 'n')
                out[o++] = '\n';
            else if (c == 'r')
                out[o++] = '\r';
            else
                out[o++] = c; /* handles \\ -> \, and is a safe fallback for any future escape */
        } else {
            out[o++] = s[i];
        }
    }
    out[o] = '\0';
    return out;
}

static void ClearSlots(void) {
    for (size_t i = 0; i < kPrefCount; ++i)
        free(sSlots[i].stringValue);
    memset(sSlots, 0, sizeof(sSlots));
}

static bool FindPrefIndexByName(const char *name, size_t nameLen, size_t *outIndex) {
    for (size_t i = 0; i < kPrefCount; ++i) {
        if (strlen(kPrefMeta[i].name) == nameLen && memcmp(kPrefMeta[i].name, name, nameLen) == 0) {
            *outIndex = i;
            return true;
        }
    }
    return false;
}

static void ApplyPreferenceLine(const char *line) {
    const char *eq = strchr(line, '=');
    if (!eq)
        return;
    size_t nameLen = (size_t)(eq - line);
    size_t idx;
    if (!FindPrefIndexByName(line, nameLen, &idx))
        return; /* unknown/future key: ignore, do not corrupt the rest of the load */
    const char *value = eq + 1;
    switch (kPrefMeta[idx].type) {
    case PrefTypeBool:
        sSlots[idx].present = true;
        sSlots[idx].boolValue = (strcmp(value, "1") == 0);
        break;
    case PrefTypeInt: {
        char *end = NULL;
        errno = 0;
        long v = strtol(value, &end, 10);
        if (end != value && *end == '\0' && errno == 0) {
            sSlots[idx].present = true;
            sSlots[idx].intValue = (int32_t)v;
        }
        break;
    }
    case PrefTypeString: {
        char *decoded = DecodeString(value);
        if (decoded) {
            free(sSlots[idx].stringValue);
            sSlots[idx].stringValue = decoded;
            sSlots[idx].present = true;
        }
        break;
    }
    }
}

static void LoadPreferencesFile(void) {
    ClearSlots();
    char *dir = ResolvePreferencesDirectory();
    if (!dir)
        return;
    char *path = BuildPath(dir, "preferences.conf");
    free(dir);
    if (!path)
        return;
    FILE *f = fopen(path, "r");
    free(path);
    if (!f)
        return; /* no file yet: honest "nothing stored", not an error */

    char *line = NULL;
    size_t cap = 0;
    bool sawHeader = false, sawFooter = false;
    char **lines = NULL;
    size_t lineCount = 0, lineCap = 0;

    ssize_t got;
    while ((got = getline(&line, &cap, f)) != -1) {
        size_t n = (size_t)got;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (!sawHeader) {
            if (strcmp(line, "U3PREFS 1") == 0)
                sawHeader = true;
            else
                break; /* not our file format at all */
            continue;
        }
        if (strcmp(line, "U3PREFSEND") == 0) {
            sawFooter = true;
            break;
        }
        if (lineCount == lineCap) {
            lineCap = lineCap ? lineCap * 2 : 16;
            char **grown = realloc(lines, lineCap * sizeof(char *));
            if (!grown)
                break;
            lines = grown;
        }
        lines[lineCount++] = strdup(line);
    }
    free(line);
    fclose(f);

    if (sawHeader && sawFooter) {
        for (size_t i = 0; i < lineCount; ++i)
            ApplyPreferenceLine(lines[i]);
    }
    /* else: truncated or otherwise malformed -- rejected as a whole. Slots
     * stay cleared, so every key reads back as absent/default rather than
     * risking a half-applied file being mistaken for valid data. */

    for (size_t i = 0; i < lineCount; ++i)
        free(lines[i]);
    free(lines);
}

static bool WritePreferencesFile(void) {
    char *dir = ResolvePreferencesDirectory();
    if (!dir)
        return false;
    if (!EnsureDirectoryExists(dir)) {
        free(dir);
        return false;
    }
    char *finalPath = BuildPath(dir, "preferences.conf");
    if (!finalPath) {
        free(dir);
        return false;
    }

    size_t tmpLen = strlen(finalPath) + 16;
    char *tmpPath = malloc(tmpLen);
    bool ok = false;
    if (tmpPath) {
        snprintf(tmpPath, tmpLen, "%s.tmp-XXXXXX", finalPath);
        int fd = mkstemp(tmpPath);
        if (fd >= 0) {
            FILE *f = fdopen(fd, "w");
            if (f) {
                ok = fprintf(f, "U3PREFS 1\n") >= 0;
                for (size_t i = 0; ok && i < kPrefCount; ++i) {
                    if (!sSlots[i].present)
                        continue;
                    switch (kPrefMeta[i].type) {
                    case PrefTypeBool:
                        ok = fprintf(f, "%s=%d\n", kPrefMeta[i].name, sSlots[i].boolValue ? 1 : 0) >= 0;
                        break;
                    case PrefTypeInt:
                        ok = fprintf(f, "%s=%d\n", kPrefMeta[i].name, sSlots[i].intValue) >= 0;
                        break;
                    case PrefTypeString: {
                        char *encoded = EncodeString(sSlots[i].stringValue ? sSlots[i].stringValue : "");
                        ok = encoded && fprintf(f, "%s=%s\n", kPrefMeta[i].name, encoded) >= 0;
                        free(encoded);
                        break;
                    }
                    }
                }
                if (ok)
                    ok = fprintf(f, "U3PREFSEND\n") >= 0;
                if (ok)
                    ok = fflush(f) == 0;
                if (ok)
                    ok = fsync(fd) == 0;
                if (fclose(f) != 0)
                    ok = false;
            } else {
                close(fd);
            }
            if (ok)
                ok = rename(tmpPath, finalPath) == 0;
            if (!ok)
                unlink(tmpPath);
            if (ok) {
                int dfd = open(dir, O_RDONLY | O_DIRECTORY);
                if (dfd >= 0) {
                    if (fsync(dfd) != 0)
                        ok = false;
                    close(dfd);
                } else {
                    ok = false;
                }
            }
        }
        free(tmpPath);
    }
    free(finalPath);
    free(dir);
    return ok;
}

static void EnsureLoaded(void) {
    if (sLoaded)
        return;
    sLoaded = true;
    LoadPreferencesFile();
}

void U3LinuxPlatformResetPreferencesCacheForTesting(void) {
    ClearSlots();
    sLoaded = false;
}

bool U3PlatformGetBooleanPreference(U3PreferenceKey key) {
    if (!ValidKey(key))
        return false;
    EnsureLoaded();
    return sSlots[key].present && sSlots[key].boolValue;
}

int32_t U3PlatformGetIntegerPreference(U3PreferenceKey key) {
    if (!ValidKey(key))
        return 0;
    EnsureLoaded();
    if (sSlots[key].present)
        return sSlots[key].intValue;
    return kPrefMeta[key].intDefault;
}

bool U3PlatformHasPreference(U3PreferenceKey key) {
    if (!ValidKey(key))
        return false;
    EnsureLoaded();
    return sSlots[key].present;
}

bool U3PlatformCopyPascalStringPreference(U3PreferenceKey key, uint8_t *outString, size_t outSize) {
    if (!ValidKey(key) || kPrefMeta[key].type != PrefTypeString || !outString || outSize == 0)
        return false;
    EnsureLoaded();
    if (!sSlots[key].present || !sSlots[key].stringValue)
        return false;
    size_t len = strlen(sSlots[key].stringValue);
    if (len > 255)
        len = 255;
    if (len + 1 > outSize)
        return false;
    outString[0] = (uint8_t)len;
    memcpy(outString + 1, sSlots[key].stringValue, len);
    return true;
}

bool U3PlatformCopyUTF8StringPreference(U3PreferenceKey key, char *outString, size_t outSize) {
    if (!ValidKey(key) || kPrefMeta[key].type != PrefTypeString || !outString || outSize == 0)
        return false;
    EnsureLoaded();
    if (!sSlots[key].present || !sSlots[key].stringValue)
        return false;
    size_t len = strlen(sSlots[key].stringValue);
    if (len + 1 > outSize)
        return false;
    memcpy(outString, sSlots[key].stringValue, len + 1);
    return true;
}

static void ReportPersistFailure(const char *what, U3PreferenceKey key) {
    fprintf(stderr, "U3LinuxPlatform: failed to %s preference '%s' to disk (kept in memory only): %s\n",
            what, ValidKey(key) ? kPrefMeta[key].name : "?", strerror(errno));
}

void U3PlatformSetBooleanPreference(U3PreferenceKey key, bool value) {
    if (!ValidKey(key) || kPrefMeta[key].type != PrefTypeBool)
        return;
    EnsureLoaded();
    sSlots[key].present = true;
    sSlots[key].boolValue = value;
    if (!WritePreferencesFile())
        ReportPersistFailure("persist", key);
}

void U3PlatformSetIntegerPreference(U3PreferenceKey key, int32_t value) {
    if (!ValidKey(key) || kPrefMeta[key].type != PrefTypeInt)
        return;
    EnsureLoaded();
    sSlots[key].present = true;
    sSlots[key].intValue = value;
    if (!WritePreferencesFile())
        ReportPersistFailure("persist", key);
}

void U3PlatformRemovePreference(U3PreferenceKey key) {
    if (!ValidKey(key))
        return;
    EnsureLoaded();
    if (kPrefMeta[key].type == PrefTypeString) {
        free(sSlots[key].stringValue);
        sSlots[key].stringValue = NULL;
    }
    sSlots[key].present = false;
    if (!WritePreferencesFile())
        ReportPersistFailure("persist removal of", key);
}

void U3PlatformSynchronizePreferences(void) {
    EnsureLoaded();
    if (!WritePreferencesFile())
        fprintf(stderr, "U3LinuxPlatform: failed to synchronize preferences to disk: %s\n", strerror(errno));
}

bool U3LinuxPlatformSetStringPreferenceForTesting(U3PreferenceKey key, const char *utf8Value) {
    if (!ValidKey(key) || kPrefMeta[key].type != PrefTypeString || !utf8Value)
        return false;
    EnsureLoaded();
    char *copy = strdup(utf8Value);
    if (!copy)
        return false;
    free(sSlots[key].stringValue);
    sSlots[key].stringValue = copy;
    sSlots[key].present = true;
    if (!WritePreferencesFile()) {
        ReportPersistFailure("persist", key);
        return false;
    }
    return true;
}
