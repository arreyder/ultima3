//
//  U3LinuxSpeech.c
//  Ultima III: Exodus — Linux Native Port
//

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "U3LinuxSpeech.h"
#include "U3Platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

extern bool U3PlatformGetBooleanPreference(U3PreferenceKey key) __attribute__((weak));

/* --------------------------------------------------------------------------
 * State & Driver Selection
 * ------------------------------------------------------------------------ */

static U3SpeechEngine sRequestedEngine = U3SpeechEngineAuto;
static U3SpeechEngine sActiveEngine = U3SpeechEngineNone;
static char sCustomCommand[512] = {0};
static char sPiperBinary[512] = {0};
static char sPiperMaleModel[512] = {0};
static char sPiperFemaleModel[512] = {0};
static bool sInitialized = false;

/* --------------------------------------------------------------------------
 * Speech Dispatcher (libspeechd) Types & State
 * ------------------------------------------------------------------------ */

typedef void SPDConnection;
typedef enum {
    SPD_MODE_SINGLE = 0,
    SPD_MODE_THREADED = 1
} SPDConnectionMode;
typedef enum {
    SPD_IMPORTANT = 1,
    SPD_MESSAGE = 2,
    SPD_TEXT = 3,
    SPD_NOTIFICATION = 4,
    SPD_PROGRESS = 5
} SPDPriority;
typedef enum {
    SPD_MALE1 = 1,
    SPD_MALE2 = 2,
    SPD_MALE3 = 3,
    SPD_FEMALE1 = 4,
    SPD_FEMALE2 = 5,
    SPD_FEMALE3 = 6,
    SPD_CHILD_MALE = 7,
    SPD_CHILD_FEMALE = 8
} SPDVoiceType;

typedef SPDConnection* (*spd_open_fn)(const char*, const char*, const char*, int);
typedef int (*spd_say_fn)(SPDConnection*, int, const char*);
typedef void (*spd_close_fn)(SPDConnection*);
typedef int (*spd_set_voice_type_fn)(SPDConnection*, int);
typedef int (*spd_set_voice_pitch_fn)(SPDConnection*, int);
typedef int (*spd_set_voice_rate_fn)(SPDConnection*, int);
typedef int (*spd_cancel_fn)(SPDConnection*);

static void *sSpeechdLib = NULL;
static SPDConnection *sSpeechdConn = NULL;
static spd_open_fn sSpdOpen = NULL;
static spd_say_fn sSpdSay = NULL;
static spd_close_fn sSpdClose = NULL;
static spd_set_voice_type_fn sSpdSetVoiceType = NULL;
static spd_set_voice_pitch_fn sSpdSetVoicePitch = NULL;
static spd_set_voice_rate_fn sSpdSetVoiceRate = NULL;
static spd_cancel_fn sSpdCancel = NULL;

/* --------------------------------------------------------------------------
 * Background Async Speech Worker (for Piper & Custom Command)
 * ------------------------------------------------------------------------ */

static pthread_t sWorkerThread;
static pthread_mutex_t sWorkerMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sWorkerCond = PTHREAD_COND_INITIALIZER;
static bool sWorkerRunning = false;
static bool sWorkerStop = false;
static bool sHasPending = false;
static char sPendingText[512] = {0};
static char sPendingVoice[64] = {0};
static pid_t sActiveChildPid = 0;

static void KillActiveChild(void) {
    if (sActiveChildPid > 0) {
        kill(-sActiveChildPid, SIGTERM);
        kill(sActiveChildPid, SIGTERM);
        int status = 0;
        waitpid(sActiveChildPid, &status, WNOHANG);
        sActiveChildPid = 0;
    }
}

static void *SpeechWorkerLoop(void *arg) {
    (void)arg;
    while (1) {
        char text[512] = {0};
        char voice[64] = {0};

        pthread_mutex_lock(&sWorkerMutex);
        while (!sHasPending && !sWorkerStop) {
            pthread_cond_wait(&sWorkerCond, &sWorkerMutex);
        }
        if (sWorkerStop && !sHasPending) {
            pthread_mutex_unlock(&sWorkerMutex);
            break;
        }
        strncpy(text, sPendingText, sizeof(text) - 1);
        strncpy(voice, sPendingVoice, sizeof(voice) - 1);
        sHasPending = false;
        pthread_mutex_unlock(&sWorkerMutex);

        if (!text[0])
            continue;

        if (sActiveEngine == U3SpeechEnginePiper) {
            bool isFemale = false;
            if (voice[0]) {
                if (strcasestr(voice, "Agnes") || strcasestr(voice, "Victoria") ||
                    strcasestr(voice, "Bubbles") || strcasestr(voice, "Hysterical")) {
                    isFemale = true;
                }
            }

            const char *model = (isFemale && sPiperFemaleModel[0]) ? sPiperFemaleModel : sPiperMaleModel;
            if (!model[0] && sPiperFemaleModel[0]) model = sPiperFemaleModel;

            int textPipe[2];
            if (pipe(textPipe) != 0)
                continue;

            pid_t pid = fork();
            if (pid == 0) {
                // Child process in new process group
                setpgid(0, 0);
                close(textPipe[1]);
                dup2(textPipe[0], STDIN_FILENO);
                close(textPipe[0]);

                // Choose audio player
                const char *player = (access("/usr/bin/paplay", X_OK) == 0) ? "paplay" : "aplay";
                char pipeline[1024];
                if (!strcmp(player, "paplay")) {
                    snprintf(pipeline, sizeof(pipeline),
                             "\"%s\" --model \"%s\" --output-raw 2>/dev/null | paplay --raw --rate=22050 --channels=1 --format=s16le",
                             sPiperBinary, model);
                } else {
                    snprintf(pipeline, sizeof(pipeline),
                             "\"%s\" --model \"%s\" --output-raw 2>/dev/null | aplay -q -r 22050 -f S16_LE -t raw -c 1",
                             sPiperBinary, model);
                }
                execl("/bin/sh", "sh", "-c", pipeline, NULL);
                _exit(127);
            } else if (pid > 0) {
                close(textPipe[0]);
                write(textPipe[1], text, strlen(text));
                close(textPipe[1]);

                pthread_mutex_lock(&sWorkerMutex);
                sActiveChildPid = pid;
                pthread_mutex_unlock(&sWorkerMutex);

                int status = 0;
                waitpid(pid, &status, 0);

                pthread_mutex_lock(&sWorkerMutex);
                if (sActiveChildPid == pid)
                    sActiveChildPid = 0;
                pthread_mutex_unlock(&sWorkerMutex);
            } else {
                close(textPipe[0]);
                close(textPipe[1]);
            }
        } else if (sActiveEngine == U3SpeechEngineCommand) {
            int textPipe[2];
            if (pipe(textPipe) != 0)
                continue;

            pid_t pid = fork();
            if (pid == 0) {
                setpgid(0, 0);
                close(textPipe[1]);
                dup2(textPipe[0], STDIN_FILENO);
                close(textPipe[0]);

                if (strstr(sCustomCommand, "%t")) {
                    char expanded[1024] = {0};
                    const char *p = sCustomCommand;
                    char *out = expanded;
                    size_t rem = sizeof(expanded) - 1;
                    while (*p && rem > 0) {
                        if (*p == '%' && *(p + 1) == 't') {
                            size_t tlen = strlen(text);
                            if (tlen > rem) tlen = rem;
                            memcpy(out, text, tlen);
                            out += tlen; rem -= tlen; p += 2;
                        } else if (*p == '%' && *(p + 1) == 'v') {
                            size_t vlen = strlen(voice);
                            if (vlen > rem) vlen = rem;
                            memcpy(out, voice, vlen);
                            out += vlen; rem -= vlen; p += 2;
                        } else {
                            *out++ = *p++; rem--;
                        }
                    }
                    *out = '\0';
                    execl("/bin/sh", "sh", "-c", expanded, NULL);
                } else {
                    execl("/bin/sh", "sh", "-c", sCustomCommand, NULL);
                }
                _exit(127);
            } else if (pid > 0) {
                close(textPipe[0]);
                write(textPipe[1], text, strlen(text));
                close(textPipe[1]);

                pthread_mutex_lock(&sWorkerMutex);
                sActiveChildPid = pid;
                pthread_mutex_unlock(&sWorkerMutex);

                int status = 0;
                waitpid(pid, &status, 0);

                pthread_mutex_lock(&sWorkerMutex);
                if (sActiveChildPid == pid)
                    sActiveChildPid = 0;
                pthread_mutex_unlock(&sWorkerMutex);
            } else {
                close(textPipe[0]);
                close(textPipe[1]);
            }
        }
    }
    return NULL;
}

static void StartWorkerThread(void) {
    if (!sWorkerRunning) {
        sWorkerStop = false;
        sWorkerRunning = true;
        pthread_create(&sWorkerThread, NULL, SpeechWorkerLoop, NULL);
    }
}

static void StopWorkerThread(void) {
    if (sWorkerRunning) {
        pthread_mutex_lock(&sWorkerMutex);
        sWorkerStop = true;
        KillActiveChild();
        pthread_cond_signal(&sWorkerCond);
        pthread_mutex_unlock(&sWorkerMutex);

        pthread_join(sWorkerThread, NULL);
        sWorkerRunning = false;
    }
}

/* --------------------------------------------------------------------------
 * Probe Helpers
 * ------------------------------------------------------------------------ */

static bool FindFile(const char **candidates, size_t count, char *outPath, size_t outSize) {
    for (size_t i = 0; i < count; ++i) {
        if (!candidates[i]) continue;
        if (access(candidates[i], R_OK) == 0) {
            strncpy(outPath, candidates[i], outSize - 1);
            outPath[outSize - 1] = '\0';
            return true;
        }
    }
    return false;
}

static bool ProbePiper(const char *assetsDir) {
    char candidateMale[512], candidateFemale[512], candidateBin[512];
    snprintf(candidateMale, sizeof(candidateMale), "%s/../tools/piper/en_GB-alan-medium.onnx", assetsDir ? assetsDir : ".");
    snprintf(candidateFemale, sizeof(candidateFemale), "%s/../tools/piper/en_GB-alba-medium.onnx", assetsDir ? assetsDir : ".");
    snprintf(candidateBin, sizeof(candidateBin), "%s/../tools/piper/piper", assetsDir ? assetsDir : ".");

    const char *binCandidates[] = {
        getenv("U3_PIPER_PATH"),
        candidateBin,
        "tools/piper/piper",
        "/home/arreyder/repos/ultima3/tools/piper/piper",
        "/usr/local/bin/piper",
        "/usr/bin/piper"
    };
    if (!FindFile(binCandidates, sizeof(binCandidates) / sizeof(binCandidates[0]), sPiperBinary, sizeof(sPiperBinary))) {
        return false;
    }
    if (access(sPiperBinary, X_OK) != 0) {
        return false;
    }

    const char *maleCandidates[] = {
        getenv("U3_PIPER_MODEL"),
        candidateMale,
        "tools/piper/en_GB-alan-medium.onnx",
        "/home/arreyder/repos/ultima3/tools/piper/en_GB-alan-medium.onnx"
    };
    FindFile(maleCandidates, sizeof(maleCandidates) / sizeof(maleCandidates[0]), sPiperMaleModel, sizeof(sPiperMaleModel));

    const char *femaleCandidates[] = {
        candidateFemale,
        "tools/piper/en_GB-alba-medium.onnx",
        "/home/arreyder/repos/ultima3/tools/piper/en_GB-alba-medium.onnx"
    };
    FindFile(femaleCandidates, sizeof(femaleCandidates) / sizeof(femaleCandidates[0]), sPiperFemaleModel, sizeof(sPiperFemaleModel));

    return (sPiperMaleModel[0] != '\0' || sPiperFemaleModel[0] != '\0');
}

static bool InitSpeechD(void) {
    if (sSpeechdConn)
        return true;

    sSpeechdLib = dlopen("libspeechd.so.2", RTLD_LAZY);
    if (!sSpeechdLib)
        sSpeechdLib = dlopen("libspeechd.so", RTLD_LAZY);
    if (!sSpeechdLib)
        return false;

    sSpdOpen = (spd_open_fn)dlsym(sSpeechdLib, "spd_open");
    sSpdSay = (spd_say_fn)dlsym(sSpeechdLib, "spd_say");
    sSpdClose = (spd_close_fn)dlsym(sSpeechdLib, "spd_close");
    sSpdSetVoiceType = (spd_set_voice_type_fn)dlsym(sSpeechdLib, "spd_set_voice_type");
    sSpdSetVoicePitch = (spd_set_voice_pitch_fn)dlsym(sSpeechdLib, "spd_set_voice_pitch");
    sSpdSetVoiceRate = (spd_set_voice_rate_fn)dlsym(sSpeechdLib, "spd_set_voice_rate");
    sSpdCancel = (spd_cancel_fn)dlsym(sSpeechdLib, "spd_cancel");

    if (!sSpdOpen || !sSpdSay || !sSpdClose) {
        dlclose(sSpeechdLib);
        sSpeechdLib = NULL;
        return false;
    }

    sSpeechdConn = sSpdOpen("ultima3", "speech", NULL, SPD_MODE_THREADED);
    if (!sSpeechdConn) {
        dlclose(sSpeechdLib);
        sSpeechdLib = NULL;
        return false;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * Public API Implementation
 * ------------------------------------------------------------------------ */

void U3LinuxSpeechSetEngine(U3SpeechEngine engine) {
    sRequestedEngine = engine;
}

void U3LinuxSpeechSetCustomCommand(const char *commandTemplate) {
    if (commandTemplate && *commandTemplate) {
        strncpy(sCustomCommand, commandTemplate, sizeof(sCustomCommand) - 1);
        sRequestedEngine = U3SpeechEngineCommand;
    }
}

void U3LinuxSpeechInit(const char *assetsDir) {
    if (sInitialized)
        return;
    sInitialized = true;

    if (U3PlatformGetBooleanPreference && U3PlatformGetBooleanPreference(U3PreferenceSpeechDisabled)) {
        sActiveEngine = U3SpeechEngineNone;
        return;
    }
    if (getenv("U3_NO_SPEECH") || getenv("U3_NO_AUDIO")) {
        sActiveEngine = U3SpeechEngineNone;
        return;
    }
    const char *audioDriver = getenv("SDL_AUDIODRIVER");
    if (audioDriver && !strcmp(audioDriver, "dummy")) {
        sActiveEngine = U3SpeechEngineNone;
        return;
    }

    const char *envCmd = getenv("U3_SPEECH_COMMAND");
    if (envCmd && *envCmd) {
        strncpy(sCustomCommand, envCmd, sizeof(sCustomCommand) - 1);
        sRequestedEngine = U3SpeechEngineCommand;
    }

    const char *envEngine = getenv("U3_SPEECH_ENGINE");
    if (envEngine) {
        if (!strcasecmp(envEngine, "piper") || !strcasecmp(envEngine, "neural")) {
            sRequestedEngine = U3SpeechEnginePiper;
        } else if (!strcasecmp(envEngine, "speechd") || !strcasecmp(envEngine, "system")) {
            sRequestedEngine = U3SpeechEngineSpeechD;
        } else if (!strcasecmp(envEngine, "command") || !strcasecmp(envEngine, "cmd")) {
            sRequestedEngine = U3SpeechEngineCommand;
        } else if (!strcasecmp(envEngine, "none") || !strcasecmp(envEngine, "off")) {
            sRequestedEngine = U3SpeechEngineNone;
        }
    }

    // Engine resolution
    if (sRequestedEngine == U3SpeechEngineCommand && sCustomCommand[0]) {
        sActiveEngine = U3SpeechEngineCommand;
        StartWorkerThread();
        fprintf(stderr, "U3Speech: initialized custom command engine: %s\n", sCustomCommand);
        return;
    }

    if (sRequestedEngine == U3SpeechEnginePiper || sRequestedEngine == U3SpeechEngineAuto) {
        if (ProbePiper(assetsDir)) {
            sActiveEngine = U3SpeechEnginePiper;
            StartWorkerThread();
            fprintf(stderr, "U3Speech: initialized Piper Neural TTS (%s%s%s)\n",
                    sPiperBinary,
                    sPiperMaleModel[0] ? " male: Alan" : "",
                    sPiperFemaleModel[0] ? " female: Alba" : "");
            return;
        }
        if (sRequestedEngine == U3SpeechEnginePiper) {
            fprintf(stderr, "U3Speech: requested Piper engine not found in tools/piper or PATH.\n");
        }
    }

    if (sRequestedEngine == U3SpeechEngineSpeechD || sRequestedEngine == U3SpeechEngineAuto) {
        if (InitSpeechD()) {
            sActiveEngine = U3SpeechEngineSpeechD;
            fprintf(stderr, "U3Speech: initialized Speech Dispatcher engine (libspeechd)\n");
            return;
        }
    }

    sActiveEngine = U3SpeechEngineNone;
    fprintf(stderr, "U3Speech: no speech synthesis engine available; speech disabled.\n");
}

void U3LinuxSpeechCancel(void) {
    if (sActiveEngine == U3SpeechEnginePiper || sActiveEngine == U3SpeechEngineCommand) {
        pthread_mutex_lock(&sWorkerMutex);
        sHasPending = false;
        KillActiveChild();
        pthread_mutex_unlock(&sWorkerMutex);
    } else if (sActiveEngine == U3SpeechEngineSpeechD) {
        if (sSpeechdConn && sSpdCancel)
            sSpdCancel(sSpeechdConn);
    }
}

void U3LinuxSpeechSpeak(const char *text, int16_t voiceID, const char *voiceName) {
    if (!text || !*text || sActiveEngine == U3SpeechEngineNone)
        return;

    if (sActiveEngine == U3SpeechEnginePiper || sActiveEngine == U3SpeechEngineCommand) {
        pthread_mutex_lock(&sWorkerMutex);
        KillActiveChild();
        strncpy(sPendingText, text, sizeof(sPendingText) - 1);
        sPendingText[sizeof(sPendingText) - 1] = '\0';
        if (voiceName) {
            strncpy(sPendingVoice, voiceName, sizeof(sPendingVoice) - 1);
            sPendingVoice[sizeof(sPendingVoice) - 1] = '\0';
        } else {
            sPendingVoice[0] = '\0';
        }
        sHasPending = true;
        pthread_cond_signal(&sWorkerCond);
        pthread_mutex_unlock(&sWorkerMutex);
    } else if (sActiveEngine == U3SpeechEngineSpeechD) {
        if (!sSpeechdConn)
            return;

        int vtype = SPD_MALE2;
        int pitch = 0;
        int rate = 0;

        if (voiceName && voiceName[0]) {
            if (strcasestr(voiceName, "Agnes")) {
                vtype = SPD_FEMALE1; pitch = 30;
            } else if (strcasestr(voiceName, "Victoria")) {
                vtype = SPD_FEMALE2; pitch = 15;
            } else if (strcasestr(voiceName, "Bubbles")) {
                vtype = SPD_CHILD_FEMALE; pitch = 50; rate = 30;
            } else if (strcasestr(voiceName, "Hysterical")) {
                vtype = SPD_FEMALE3; pitch = 40; rate = 40;
            } else if (strcasestr(voiceName, "Deranged")) {
                vtype = SPD_MALE3; pitch = -30; rate = 20;
            } else if (strcasestr(voiceName, "Whisper")) {
                vtype = SPD_MALE1; pitch = -10; rate = -25;
            } else if (strcasestr(voiceName, "Zarvox")) {
                vtype = SPD_MALE1; pitch = 40; rate = -15;
            } else if (strcasestr(voiceName, "Bruce")) {
                vtype = SPD_MALE1; pitch = -25;
            } else if (strcasestr(voiceName, "Ralph")) {
                vtype = SPD_MALE2; pitch = -10;
            }
        } else if (voiceID == 19) {
            vtype = SPD_MALE1; pitch = -20;
        }

        if (sSpdSetVoiceType) sSpdSetVoiceType(sSpeechdConn, vtype);
        if (sSpdSetVoicePitch) sSpdSetVoicePitch(sSpeechdConn, pitch);
        if (sSpdSetVoiceRate) sSpdSetVoiceRate(sSpeechdConn, rate);
        if (sSpdCancel) sSpdCancel(sSpeechdConn);
        if (sSpdSay) sSpdSay(sSpeechdConn, SPD_TEXT, text);
    }
}

void U3LinuxSpeechShutdown(void) {
    if (sActiveEngine == U3SpeechEnginePiper || sActiveEngine == U3SpeechEngineCommand) {
        StopWorkerThread();
    } else if (sActiveEngine == U3SpeechEngineSpeechD) {
        if (sSpeechdConn) {
            if (sSpdClose) sSpdClose(sSpeechdConn);
            sSpeechdConn = NULL;
        }
        if (sSpeechdLib) {
            dlclose(sSpeechdLib);
            sSpeechdLib = NULL;
        }
    }
    sActiveEngine = U3SpeechEngineNone;
    sInitialized = false;
}

const char *U3LinuxSpeechGetActiveEngineName(void) {
    switch (sActiveEngine) {
        case U3SpeechEnginePiper: return "Piper (Neural)";
        case U3SpeechEngineSpeechD: return "Speech Dispatcher";
        case U3SpeechEngineCommand: return "Custom Command";
        case U3SpeechEngineNone: return "Disabled";
        default: return "Unknown";
    }
}
