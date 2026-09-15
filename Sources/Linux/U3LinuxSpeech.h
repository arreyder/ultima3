//
//  U3LinuxSpeech.h
//  Ultima III: Exodus — Linux Native Port
//
//  Pluggable speech synthesis subsystem supporting multiple engines:
//    - Piper: Local neural offline text-to-speech with natural human voices
//    - SpeechD: System Speech Dispatcher daemon (libspeechd)
//    - Custom Command: User-defined shell command or external script
//

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum U3SpeechEngine {
    U3SpeechEngineAuto = 0,
    U3SpeechEnginePiper,
    U3SpeechEngineSpeechD,
    U3SpeechEngineCommand,
    U3SpeechEngineNone
} U3SpeechEngine;

void U3LinuxSpeechSetEngine(U3SpeechEngine engine);
void U3LinuxSpeechSetCustomCommand(const char *commandTemplate);
void U3LinuxSpeechInit(const char *assetsDir);
void U3LinuxSpeechSpeak(const char *text, int16_t voiceID, const char *voiceName);
void U3LinuxSpeechCancel(void);
void U3LinuxSpeechShutdown(void);
const char *U3LinuxSpeechGetActiveEngineName(void);
