#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include "Linux/U3LegacyTypes.h"
#include "Linux/U3LinuxAssets.h"
#include "Linux/U3LinuxPlatform.h"
#include "Linux/U3LinuxVideo.h"
#include "Linux/U3LinuxSettings.h"
#include "Linux/U3LinuxAudio.h"
#include "Linux/U3IOLinux.h"
#include "Linux/U3LinuxCarbon.h"
#include "UltimaMain.h"
#include "U3Platform.h"
#include "U3Audio.h"
#include "CocoaBridge.h"

extern Boolean U3LegacyBitmapSelfTest(void);
extern Boolean U3PartySelectionSelfTest(void);
extern Boolean U3CharacterCreationSelfTest(void);

__attribute__((weak)) bool U3IOSelfTest(void) {
    return true;
}

__attribute__((weak)) Boolean U3CocoaImageSelfTest(void) {
    return true;
}

__attribute__((weak)) bool U3AudioMusicSelfTest(void) {
    return true;
}

void U3CocoaRunApplication(void) {
    // On Linux SDL2, events are pumped in the main loop; no-op or pump here if called
}

static bool VideoPollKeyMouse(void *userData, uint32_t waitMs, uint8_t *outKey, bool *outIsMouse) {
    (void)userData;
    char key = 0;
    Boolean mouse = false;
    long timeoutTicks = (waitMs * 60 + 999) / 1000;
    if (U3CocoaPollKeyMouse(true, timeoutTicks, &key, &mouse)) {
        if (outKey) *outKey = (uint8_t)key;
        if (outIsMouse) *outIsMouse = (bool)mouse;
        return true;
    }
    return false;
}

static bool VideoGetMousePoint(void *userData, int16_t *outX, int16_t *outY) {
    (void)userData;
    Point pt = {0, 0};
    U3CocoaGetMousePoint(&pt);
    if (outX) *outX = pt.h;
    if (outY) *outY = pt.v;
    return true;
}

static void VideoFlushInput(void *userData) {
    (void)userData;
    U3CocoaFlushInput();
}

static void InstallVideoInputSource(void) {
    static const U3LinuxInputSource src = {
        .pollKeyMouse = VideoPollKeyMouse,
        .getMousePoint = VideoGetMousePoint,
        .flush = VideoFlushInput,
        .obscureCursor = NULL,
        .userData = NULL
    };
    U3LinuxPlatformSetInputSource(&src);
}

static void U3LinuxPlatformConfigure(U3LinuxAssets *assets) {
    (void)assets;
    InstallVideoInputSource();
}

static void U3IOConfigure(U3LinuxAssets *assets) {
    const char *saveDir = getenv("U3_SAVE_DIRECTORY");
    char *allocatedSaveDir = NULL;
    if (!saveDir || !*saveDir) {
        const char *xdgDataHome = getenv("XDG_DATA_HOME");
        if (xdgDataHome && *xdgDataHome) {
            size_t len = strlen(xdgDataHome) + strlen("/ultima3") + 1;
            allocatedSaveDir = malloc(len);
            if (allocatedSaveDir) {
                snprintf(allocatedSaveDir, len, "%s/ultima3", xdgDataHome);
                saveDir = allocatedSaveDir;
            }
        } else {
            const char *home = getenv("HOME");
            if (home && *home) {
                size_t len = strlen(home) + strlen("/.local/share/ultima3") + 1;
                allocatedSaveDir = malloc(len);
                if (allocatedSaveDir) {
                    snprintf(allocatedSaveDir, len, "%s/.local/share/ultima3", home);
                    saveDir = allocatedSaveDir;
                }
            }
        }
    }
    if (saveDir) {
        U3IOLinuxConfigure(saveDir, assets);
    }
    free(allocatedSaveDir);
}

/* A directory counts as an asset bundle only if it holds a readable
 * manifest.json, which is what U3LinuxAssetsOpen requires. */
static bool HasAssetManifest(const char *directory) {
    if (!directory || !*directory) {
        return false;
    }
    char manifestPath[512];
    int written = snprintf(manifestPath, sizeof(manifestPath), "%s/manifest.json", directory);
    if (written < 0 || (size_t)written >= sizeof(manifestPath)) {
        return false;
    }
    return access(manifestPath, R_OK) == 0;
}

/* Append `suffix` to `base` into the caller's buffer, returning it only if the
 * result is a real asset bundle. Lets the XDG probe below stay a flat list. */
static const char *AssetCandidate(char *buffer, size_t size,
                                  const char *base, const char *suffix) {
    if (!base || !*base) {
        return NULL;
    }
    int written = snprintf(buffer, size, "%s%s", base, suffix);
    if (written < 0 || (size_t)written >= size) {
        return NULL;
    }
    return HasAssetManifest(buffer) ? buffer : NULL;
}

/* Locate the exported asset bundle, in descending order of explicitness:
 *
 *   1. $U3_ASSETS_DIRECTORY
 *   2. --assets <dir>
 *   3. a bare path argument that looks like a bundle
 *   4. directories relative to the current one (a build tree)
 *   5. $XDG_DATA_HOME/ultima3/assets, then ~/.local/share/ultima3/assets
 *   6. $XDG_DATA_DIRS entries, then /usr/local/share and /usr/share
 *
 * The last two groups are what let an installed `ultima3` be launched from any
 * working directory, or from a desktop entry that sets none at all -- before
 * them, only a launch from the build tree could find its own data.
 */
static const char *FindAssetsDirectory(int argc, char **argv) {
    const char *env = getenv("U3_ASSETS_DIRECTORY");
    if (env && env[0] != '\0') {
        return env;
    }

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--assets") && i + 1 < argc) {
            return argv[i + 1];
        }
        if (!strncmp(argv[i], "--assets=", 9)) {
            return argv[i] + 9;
        }
    }

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--script") || !strcmp(argv[i], "--scale") ||
            !strcmp(argv[i], "--screenshot") || !strcmp(argv[i], "--assets") ||
            !strcmp(argv[i], "--step-delay") || !strcmp(argv[i], "--speech-engine") ||
            !strcmp(argv[i], "--speech-cmd") || !strcmp(argv[i], "--font-scale")) {
            ++i;
            continue;
        }
        if (argv[i][0] != '-' && HasAssetManifest(argv[i])) {
            return argv[i];
        }
    }

    static const char *relative[] = {
        "assets",
        "build/linux-game/assets",
        "../assets"
    };
    for (size_t i = 0; i < sizeof(relative) / sizeof(relative[0]); ++i) {
        if (HasAssetManifest(relative[i])) {
            return relative[i];
        }
    }

    /* Static so the chosen path outlives this function, matching the string
     * literals and argv pointers the other branches return. */
    static char resolved[512];
    const char *found = NULL;

    const char *xdgDataHome = getenv("XDG_DATA_HOME");
    if (xdgDataHome && *xdgDataHome) {
        found = AssetCandidate(resolved, sizeof(resolved), xdgDataHome, "/ultima3/assets");
    } else {
        found = AssetCandidate(resolved, sizeof(resolved), getenv("HOME"),
                               "/.local/share/ultima3/assets");
    }
    if (found) {
        return found;
    }

    /* $XDG_DATA_DIRS is a colon-separated search path; walk it in order before
     * falling back to the defaults the spec prescribes when it is unset. */
    const char *dataDirs = getenv("XDG_DATA_DIRS");
    if (dataDirs && *dataDirs) {
        for (const char *entry = dataDirs; *entry;) {
            const char *separator = strchr(entry, ':');
            size_t length = separator ? (size_t)(separator - entry) : strlen(entry);
            if (length > 0 && length < sizeof(resolved) / 2) {
                char base[256];
                memcpy(base, entry, length);
                base[length] = '\0';
                found = AssetCandidate(resolved, sizeof(resolved), base, "/ultima3/assets");
                if (found) {
                    return found;
                }
            }
            entry += length;
            if (*entry) {
                ++entry;
            }
        }
    }

    static const char *systemPaths[] = {
        "/usr/local/share/ultima3/assets",
        "/usr/share/ultima3/assets"
    };
    for (size_t i = 0; i < sizeof(systemPaths) / sizeof(systemPaths[0]); ++i) {
        if (HasAssetManifest(systemPaths[i])) {
            return systemPaths[i];
        }
    }

    /* Nothing found. Return the conventional relative path so the caller's
     * error message names something a player can act on. */
    return "assets";
}

static void PrintUsage(const char *prog) {
    fprintf(stdout, "Ultima III (Linux Native Port)\n\n");
    fprintf(stdout, "Usage: %s [options]\n\n", prog);
    fprintf(stdout, "Display options:\n");
    fprintf(stdout, "  --windowed, --window     Run in windowed mode (default)\n");
    fprintf(stdout, "  --fullscreen             Run in fullscreen mode\n");
    fprintf(stdout, "  --scale <n>              Initial window scale multiplier (default: 1)\n");
    fprintf(stdout, "  --classic                Use classic 640x384 appearance\n");
    fprintf(stdout, "  --modern                 Use modern appearance with character portraits and status bars\n");
    fprintf(stdout, "  --integer-scale          Scale by whole pixels, keeping the pixel grid crisp (default)\n");
    fprintf(stdout, "  --smooth-scale           Fit the window exactly, preserving aspect ratio\n");
    fprintf(stdout, "  --double-size            32px tiles: larger art, text and line spacing (1280x768)\n");
    fprintf(stdout, "  --original-size          16px tiles: the original 640x384 layout\n");
    fprintf(stdout, "  --font-scale <percent>   Shrink text relative to the tile size, 50-100 (default: 100)\n");
    fprintf(stdout, "                           For larger text use --double-size; see docs/controls.md\n");
    fprintf(stdout, "  --crt                    Enable the CRT scanline filter\n");
    fprintf(stdout, "  --no-crt                 Disable the CRT scanline filter\n");
    fprintf(stdout, "\nIn-game keys (settings are saved to the preferences file):\n");
    fprintf(stdout, "  F1                       Open the settings overlay\n");
    fprintf(stdout, "  F11, Alt+Enter           Toggle fullscreen\n");
    fprintf(stdout, "  F10                      Toggle the CRT scanline filter\n");
    fprintf(stdout, "  F8                       Toggle smooth vs. whole-pixel scaling\n");
    fprintf(stdout, "  F6, F7                   Shrink / restore text size (50-100%%)\n");
    fprintf(stdout, "\nAudio options:\n");
    fprintf(stdout, "  --no-audio               Disable audio playback\n");
    fprintf(stdout, "  --speech                 Enable text-to-speech synthesis\n");
    fprintf(stdout, "  --no-speech              Disable text-to-speech synthesis\n");
    fprintf(stdout, "  --speech-engine <engine> Speech engine: auto, piper, speechd, cmd, none\n");
    fprintf(stdout, "  --speech-cmd <template>  Custom TTS command template (e.g. \"spd-say %%t\")\n");
    fprintf(stdout, "\nAsset options:\n");
    fprintf(stdout, "  --assets <dir>           Path to assets directory\n");
    fprintf(stdout, "\nAutomated test options:\n");
    fprintf(stdout, "  --script <keys>          Execute a keystroke script to control character/test\n");
    fprintf(stdout, "  --screenshot <file>      Capture final game screen to BMP/PNG image\n");
    fprintf(stdout, "\nGeneral options:\n");
    fprintf(stdout, "  -h, --help               Show this help message and exit\n");
}

int main(int argc, char *argv[]) {
    int scale = 1;
    bool forceWindowed = true;
    bool forceFullscreen = false;
    bool classic = false;
    bool modern = false;
    bool noAudio = false;
    /* -1 leaves the stored preference alone; 0/1 override it. */
    int smoothScaling = -1;
    int scanlines = -1;
    /* 0 leaves the stored font scale alone; otherwise a percentage. */
    int fontScale = 0;
    /* -1 leaves the stored tile size alone; 0/1 select double/original. */
    int originalSize = -1;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            PrintUsage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--windowed") || !strcmp(argv[i], "--window")) {
            forceWindowed = true;
            forceFullscreen = false;
        } else if (!strcmp(argv[i], "--fullscreen")) {
            forceFullscreen = true;
            forceWindowed = false;
        } else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            scale = atoi(argv[++i]);
            if (scale < 1) scale = 1;
        } else if (!strncmp(argv[i], "--scale=", 8)) {
            scale = atoi(argv[i] + 8);
            if (scale < 1) scale = 1;
        } else if (!strcmp(argv[i], "--classic")) {
            classic = true;
        } else if (!strcmp(argv[i], "--modern")) {
            modern = true;
        } else if (!strcmp(argv[i], "--smooth-scale")) {
            smoothScaling = 1;
        } else if (!strcmp(argv[i], "--integer-scale")) {
            smoothScaling = 0;
        } else if (!strcmp(argv[i], "--double-size")) {
            originalSize = 0;
        } else if (!strcmp(argv[i], "--original-size")) {
            originalSize = 1;
        } else if (!strcmp(argv[i], "--font-scale") && i + 1 < argc) {
            fontScale = atoi(argv[++i]);
        } else if (!strncmp(argv[i], "--font-scale=", 13)) {
            fontScale = atoi(argv[i] + 13);
        } else if (!strcmp(argv[i], "--crt")) {
            scanlines = 1;
        } else if (!strcmp(argv[i], "--no-crt")) {
            scanlines = 0;
        } else if (!strcmp(argv[i], "--no-audio")) {
            noAudio = true;
        } else if (!strcmp(argv[i], "--speech")) {
            setenv("U3_SPEECH", "1", 1);
            unsetenv("U3_NO_SPEECH");
        } else if (!strcmp(argv[i], "--no-speech")) {
            setenv("U3_NO_SPEECH", "1", 1);
            unsetenv("U3_SPEECH");
        } else if (!strcmp(argv[i], "--speech-engine") && i + 1 < argc) {
            setenv("U3_SPEECH_ENGINE", argv[++i], 1);
        } else if (!strncmp(argv[i], "--speech-engine=", 16)) {
            setenv("U3_SPEECH_ENGINE", argv[i] + 16, 1);
        } else if (!strcmp(argv[i], "--speech-cmd") && i + 1 < argc) {
            setenv("U3_SPEECH_COMMAND", argv[++i], 1);
        } else if (!strncmp(argv[i], "--speech-cmd=", 13)) {
            setenv("U3_SPEECH_COMMAND", argv[i] + 13, 1);
        } else if (!strcmp(argv[i], "--script") && i + 1 < argc) {
            setenv("U3_SCRIPT", argv[++i], 1);
        } else if (!strncmp(argv[i], "--script=", 9)) {
            setenv("U3_SCRIPT", argv[i] + 9, 1);
        } else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) {
            setenv("U3_SCREENSHOT", argv[++i], 1);
        } else if (!strncmp(argv[i], "--screenshot=", 13)) {
            setenv("U3_SCREENSHOT", argv[i] + 13, 1);
        } else if (!strcmp(argv[i], "--step-delay") && i + 1 < argc) {
            setenv("U3_STEP_DELAY_MS", argv[++i], 1);
        } else if (!strncmp(argv[i], "--step-delay=", 13)) {
            setenv("U3_STEP_DELAY_MS", argv[i] + 13, 1);
        } else if (!strcmp(argv[i], "--keep-alive")) {
            setenv("U3_KEEP_ALIVE", "1", 1);
        }
    }

    if (getenv("U3_SCRIPT") && !getenv("U3_SAVE_DIRECTORY")) {
        setenv("U3_SAVE_DIRECTORY", "/tmp/u3-script-save", 0);
    }

    if (getenv("U3_BOOT_CHECK") || getenv("U3_WORLD_RENDER_CHECK") ||
        getenv("U3_WORLD_INPUT_CHECK") || getenv("U3_WORLD_MOUSE_CHECK") ||
        getenv("U3_SCRIPT")) {
        if (!getenv("U3_SAVE_DIRECTORY")) {
            fprintf(stderr, "Check requires U3_SAVE_DIRECTORY for isolated storage.\n");
            return 2;
        }
        alarm(90);
    }

    if (getenv("U3_NO_AUDIO")) {
        noAudio = true;
    }

    const char *assetsDir = FindAssetsDirectory(argc, argv);
    U3LinuxAssets *assets = U3LinuxAssetsOpen(assetsDir);
    if (!assets) {
        fprintf(stderr, "Error: could not open assets directory '%s'\n", assetsDir);
        fprintf(stderr, "Please specify an assets directory using U3_ASSETS_DIRECTORY or --assets <dir>.\n");
        exit(1);
    }

    U3LinuxPlatformConfigure(assets);
    if (forceWindowed) {
        U3PlatformSetBooleanPreference(U3PreferenceFullScreen, false);
    } else if (forceFullscreen) {
        U3PlatformSetBooleanPreference(U3PreferenceFullScreen, true);
    }
    if (classic) {
        U3PlatformSetBooleanPreference(U3PreferenceClassicAppearance, true);
        U3PlatformSetBooleanPreference(U3PreferenceOriginalSize, true);
    } else if (modern) {
        U3PlatformSetBooleanPreference(U3PreferenceClassicAppearance, false);
    }
    /* After the classic/modern block, so an explicit size flag overrides the
     * original size --classic implies rather than being silently dropped.
     *
     * Setting the preference at all also matters beyond its value: with the key
     * absent, SetUpDisplayDialog() in UltimaMacIF.c picks a default from the
     * screen dimensions instead, which is why an unattended or headless run can
     * otherwise land on a different tile size than a desktop one. */
    if (originalSize >= 0) {
        U3PlatformSetBooleanPreference(U3PreferenceOriginalSize, originalSize != 0);
    }
    if (getenv("U3_NO_SPEECH")) {
        U3PlatformSetBooleanPreference(U3PreferenceSpeechDisabled, true);
    } else if (getenv("U3_SPEECH")) {
        U3PlatformSetBooleanPreference(U3PreferenceSpeechDisabled, false);
    }
    U3LinuxVideoConfigure(assets, "Ultima III", scale);
    if (smoothScaling >= 0) {
        U3LinuxVideoSetSmoothScaling(smoothScaling != 0);
    }
    if (scanlines >= 0) {
        U3LinuxVideoSetScanlines(scanlines != 0);
    }
    if (fontScale > 0) {
        U3LinuxVideoSetFontScalePercent(fontScale);
    }
    U3LinuxAudioConfigure(assetsDir, noAudio);
    U3LinuxSettingsInstall();
    U3LinuxCarbonConfigure(assetsDir);
    U3IOConfigure(assets);

    /* Renders the settings overlay on its own, without booting the game, so the
     * thing the player sees can be captured and reviewed. The overlay normally
     * restores the screen before returning, which would leave a screenshot
     * showing everything except the overlay. */
    const char *settingsShot = getenv("U3_SETTINGS_CHECK");
    if (settingsShot) {
        /* Size the canvas the way the game would, from the tile-size
         * preference, so the check exercises the overlay at whichever scale is
         * actually configured rather than only the small one. */
        bool small = U3PlatformGetBooleanPreference(U3PreferenceOriginalSize);
        bool ok = U3CocoaCreateMainSurface(0, 0, small ? 640 : 1280,
                                           small ? 384 : 768) != NULL;
        if (ok) {
            const char *rowText = getenv("U3_SETTINGS_ROW");
            U3LinuxSettingsDrawForTesting(rowText ? atoi(rowText) : 0);
            ok = U3CocoaWriteMainBitmap(settingsShot);
        }
        fprintf(stderr, "Settings overlay render: %s\n", ok ? "written" : "FAILED");
        U3LinuxVideoShutdown();
        U3LinuxAudioShutdown();
        U3LinuxAssetsClose(assets);
        return ok ? 0 : 1;
    }

    if (getenv("U3_IO_SELF_TEST")) {
        bool passed = U3IOSelfTest();
        fprintf(stderr, "Save container test: %s\n", passed ? "passed" : "FAILED");
        U3LinuxVideoShutdown();
        U3LinuxAudioShutdown();
        U3LinuxAssetsClose(assets);
        return passed ? 0 : 1;
    }

    if (getenv("U3_AUDIO_SELF_TEST")) {
        bool passed = U3AudioMusicSelfTest();
        fprintf(stderr, "Music self-test: %s\n", passed ? "passed" : "FAILED");
        U3LinuxVideoShutdown();
        U3LinuxAudioShutdown();
        U3LinuxAssetsClose(assets);
        return passed ? 0 : 1;
    }

    if (getenv("U3_RENDER_SELF_TEST")) {
        bool passed = U3LegacyBitmapSelfTest() &&
                      U3PartySelectionSelfTest() &&
                      U3CharacterCreationSelfTest() &&
                      U3CocoaImageSelfTest();
        fprintf(stderr, "Render self-test: %s\n", passed ? "passed" : "FAILED");
        U3LinuxVideoShutdown();
        U3LinuxAudioShutdown();
        U3LinuxAssetsClose(assets);
        return passed ? 0 : 1;
    }

    int result = Ultima3_main();

    if (!getenv("U3_SKIP_APP_RUN") && !getenv("U3_STARTUP_RENDER_CHECK") &&
        !getenv("U3_BOOT_CHECK") && !getenv("U3_WORLD_RENDER_CHECK") &&
        !getenv("U3_WORLD_INPUT_CHECK") && !getenv("U3_WORLD_MOUSE_CHECK") &&
        !getenv("U3_SCRIPT")) {
        U3CocoaRunApplication();
    }

    U3LinuxVideoShutdown();
    U3LinuxAudioShutdown();
    U3LinuxAssetsClose(assets);

    return result;
}

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
const char *__lsan_default_suppressions(void) {
    return "leak:libfluidsynth\nleak:libglib\nleak:libgobject\nleak:libinstpatch\nleak:libspeechd\nleak:libdrm\nleak:libSDL2\n";
}
#endif
#elif defined(__SANITIZE_ADDRESS__)
const char *__lsan_default_suppressions(void) {
    return "leak:libfluidsynth\nleak:libglib\nleak:libgobject\nleak:libinstpatch\nleak:libspeechd\nleak:libdrm\nleak:libSDL2\n";
}
#endif

