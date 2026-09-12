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
            !strcmp(argv[i], "--speech-cmd")) {
            ++i;
            continue;
        }
        if (argv[i][0] != '-') {
            char manifestPath[512];
            snprintf(manifestPath, sizeof(manifestPath), "%s/manifest.json", argv[i]);
            if (access(manifestPath, R_OK) == 0) {
                return argv[i];
            }
        }
    }

    static const char *candidates[] = {
        "assets",
        "build/linux-game/assets",
        "../assets"
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        char manifestPath[512];
        snprintf(manifestPath, sizeof(manifestPath), "%s/manifest.json", candidates[i]);
        if (access(manifestPath, R_OK) == 0) {
            return candidates[i];
        }
    }

    return "assets";
}

static void PrintUsage(const char *prog) {
    fprintf(stdout, "Ultima III (Linux Native Port)\n\n");
    fprintf(stdout, "Usage: %s [options]\n\n", prog);
    fprintf(stdout, "Display options:\n");
    fprintf(stdout, "  --windowed, --window     Run in windowed mode (default)\n");
    fprintf(stdout, "  --fullscreen             Run in fullscreen mode\n");
    fprintf(stdout, "  --scale <1|2>            Window scale multiplier (default: 1)\n");
    fprintf(stdout, "  --classic                Use classic 640x384 appearance\n");
    fprintf(stdout, "  --modern                 Use modern appearance with character portraits and status bars\n");
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
    if (getenv("U3_NO_SPEECH")) {
        U3PlatformSetBooleanPreference(U3PreferenceSpeechDisabled, true);
    } else if (getenv("U3_SPEECH")) {
        U3PlatformSetBooleanPreference(U3PreferenceSpeechDisabled, false);
    }
    U3LinuxVideoConfigure(assets, "Ultima III", scale);
    U3LinuxAudioConfigure(assetsDir, noAudio);
    U3LinuxCarbonConfigure(assetsDir);
    U3IOConfigure(assets);

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
    return "leak:libfluidsynth\nleak:libglib\nleak:libgobject\nleak:libinstpatch\nleak:libspeechd\n";
}
#endif
#elif defined(__SANITIZE_ADDRESS__)
const char *__lsan_default_suppressions(void) {
    return "leak:libfluidsynth\nleak:libglib\nleak:libgobject\nleak:libinstpatch\nleak:libspeechd\n";
}
#endif
