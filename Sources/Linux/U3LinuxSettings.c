//
//  U3LinuxSettings.c
//  Ultima3 (Linux port)
//
//  See U3LinuxSettings.h for what this is and why it sits above the adapter
//  modules rather than inside U3LinuxVideo.c.
//
//  The overlay draws into the game's own bitmap through the public U3Cocoa*
//  primitives, exactly as U3LinuxVideo.c's party-selection and
//  character-creation dialogs do. That is deliberate: it means the overlay is
//  scaled, letterboxed, CRT-filtered and captured by screenshots on the same
//  path as everything else, with no separate presentation path to keep in
//  step.
//

#define _DEFAULT_SOURCE

#include "U3LinuxSettings.h"
#include "U3LinuxVideo.h"
#include "U3LinuxOverlay.h"
#include "U3LinuxPlatform.h"
#include "U3LinuxSpeech.h"
#include "U3LegacyTypes.h"
#include "CarbonShunts.h"
#include "CocoaBridge.h"
#include "U3Audio.h"
#include "U3Bitmap.h"

#include <stdio.h>
#include <string.h>

/* Glyph cell of the built-in bitmap font, which is what the row pitch below is
 * measured in. Matches U3_GLYPH_CELL in U3LinuxVideo.c; this file cannot see
 * that constant, and duplicating one number is better than exporting the
 * rasterizer's internals for it. */
#define U3_SETTINGS_CELL 16

/* Every row the overlay offers. Order is display, then audio, then gameplay;
 * the section header is drawn whenever it changes, so grouping lives here
 * rather than in a parallel list that could drift out of step. */
typedef enum {
    RowAppearance = 0,
    RowTileSize,
    RowFullScreen,
    RowScaling,
    RowScanlines,
    RowTextSize,
    RowSound,
    RowSoundVolume,
    RowMusic,
    RowMusicVolume,
    RowSpeech,
    RowSpeedLimiter,
    RowAutoHeal,
    RowDiagonals,
    RowAutoSave,
    RowCombat,
    RowCount
} SettingsRow;

typedef struct {
    const char *section; /* NULL continues the previous section */
    const char *label;
    /* True for the two settings the game can only pick up while rebuilding its
     * offscreen worlds at startup. Marked in the UI rather than applied live,
     * because tearing those down from inside an input poll is not safe. */
    bool restartRequired;
} RowInfo;

static const RowInfo kRows[RowCount] = {
    [RowAppearance]   = {"DISPLAY",  "Appearance",    true},
    [RowTileSize]     = {NULL,       "Tile size",     true},
    [RowFullScreen]   = {NULL,       "Full screen",   false},
    [RowScaling]      = {NULL,       "Scaling",       false},
    [RowScanlines]    = {NULL,       "CRT scanlines", false},
    [RowTextSize]     = {NULL,       "Text size",     false},
    [RowSound]        = {"AUDIO",    "Sound effects", false},
    [RowSoundVolume]  = {NULL,       "Sound volume",  false},
    [RowMusic]        = {NULL,       "Music",         false},
    [RowMusicVolume]  = {NULL,       "Music volume",  false},
    [RowSpeech]       = {NULL,       "Speech",        false},
    [RowSpeedLimiter] = {"GAMEPLAY", "Speed limiter", false},
    [RowAutoHeal]     = {NULL,       "Auto-heal",     false},
    [RowDiagonals]    = {NULL,       "Diagonal moves",false},
    [RowAutoSave]     = {NULL,       "Auto-save",     false},
    [RowCombat]       = {NULL,       "Combat",        false},
};

/* ==========================================================================
 * Reading and writing one row
 * ========================================================================== */

/* Several stored preferences are phrased as negatives (NoAutoHeal,
 * NoDiagonals, SoundDisabled), because an absent preference reads as false and
 * the original defaults are "on". The overlay shows the positive sense, so
 * these two helpers keep the inversion in one place instead of scattering
 * "!"s through the value and adjust paths. */
static bool EnabledPreference(U3PreferenceKey key, bool storedMeansDisabled) {
    bool stored = U3PlatformGetBooleanPreference(key);
    return storedMeansDisabled ? !stored : stored;
}

static void SetEnabledPreference(U3PreferenceKey key, bool storedMeansDisabled, bool enabled) {
    U3PlatformSetBooleanPreference(key, storedMeansDisabled ? !enabled : enabled);
}

static int ClampVolume(int value) {
    if (value < 0) return 0;
    if (value > 100) return 100;
    return value;
}

/* Stored volumes follow the original convention where anything below 1 means
 * "unset, use 100". Resolve that here so the overlay never displays 0 for a
 * volume the player never touched. */
static int VolumePercent(U3PreferenceKey key) {
    int value = (int)U3PlatformGetIntegerPreference(key);
    return ClampVolume(value < 1 ? 100 : value);
}

int U3LinuxSettingsRowCount(void) {
    return RowCount;
}

bool U3LinuxSettingsRowText(int index, char *outLabel, size_t labelSize,
                            char *outValue, size_t valueSize) {
    if (index < 0 || index >= RowCount || !outLabel || !outValue ||
        labelSize == 0 || valueSize == 0) {
        return false;
    }
    snprintf(outLabel, labelSize, "%s", kRows[index].label);

    switch ((SettingsRow)index) {
        case RowAppearance:
            snprintf(outValue, valueSize, "%s",
                     U3PlatformGetBooleanPreference(U3PreferenceClassicAppearance)
                         ? "Classic" : "Modern");
            break;
        case RowTileSize:
            snprintf(outValue, valueSize, "%s",
                     U3PlatformGetBooleanPreference(U3PreferenceOriginalSize)
                         ? "Original (640x384)" : "Double (1280x768)");
            break;
        case RowFullScreen:
            snprintf(outValue, valueSize, "%s",
                     U3PlatformGetBooleanPreference(U3PreferenceFullScreen) ? "On" : "Off");
            break;
        case RowScaling:
            snprintf(outValue, valueSize, "%s",
                     U3LinuxVideoSmoothScaling() ? "Smooth" : "Whole pixels");
            break;
        case RowScanlines:
            snprintf(outValue, valueSize, "%s", U3LinuxVideoScanlines() ? "On" : "Off");
            break;
        case RowTextSize:
            snprintf(outValue, valueSize, "%d%%", U3LinuxVideoFontScalePercent());
            break;
        case RowSound:
            snprintf(outValue, valueSize, "%s",
                     EnabledPreference(U3PreferenceSoundDisabled, true) ? "On" : "Off");
            break;
        case RowSoundVolume:
            snprintf(outValue, valueSize, "%d%%", VolumePercent(U3PreferenceSoundVolume));
            break;
        case RowMusic:
            snprintf(outValue, valueSize, "%s",
                     EnabledPreference(U3PreferenceMusicDisabled, true) ? "On" : "Off");
            break;
        case RowMusicVolume:
            snprintf(outValue, valueSize, "%d%%", VolumePercent(U3PreferenceMusicVolume));
            break;
        case RowSpeech:
            snprintf(outValue, valueSize, "%s",
                     EnabledPreference(U3PreferenceSpeechDisabled, true) ? "On" : "Off");
            break;
        case RowSpeedLimiter:
            /* UnconstrainedSpeed set means the limiter is off. */
            snprintf(outValue, valueSize, "%s",
                     EnabledPreference(U3PreferenceUnconstrainedSpeed, true) ? "On" : "Off");
            break;
        case RowAutoHeal:
            snprintf(outValue, valueSize, "%s",
                     EnabledPreference(U3PreferenceNoAutoHeal, true) ? "On" : "Off");
            break;
        case RowDiagonals:
            snprintf(outValue, valueSize, "%s",
                     EnabledPreference(U3PreferenceNoDiagonals, true) ? "On" : "Off");
            break;
        case RowAutoSave:
            snprintf(outValue, valueSize, "%s",
                     U3PlatformGetBooleanPreference(U3PreferenceAutoSave) ? "On" : "Off");
            break;
        case RowCombat:
            snprintf(outValue, valueSize, "%s",
                     U3PlatformGetBooleanPreference(U3PreferenceManualCombat)
                         ? "Manual" : "Automatic");
            break;
        case RowCount:
            return false;
    }
    return true;
}

/* Push the audio preferences this overlay just changed into the live mixer.
 * U3AudioApplyPreferences re-reads them itself, so writing the preference and
 * calling this is the whole story -- no separate settings push. */
static void ApplyAudio(void) {
    U3AudioApplyPreferences();
}

bool U3LinuxSettingsAdjustRow(int index, int delta) {
    if (index < 0 || index >= RowCount || delta == 0)
        return false;
    int step = delta > 0 ? 1 : -1;

    switch ((SettingsRow)index) {
        case RowAppearance:
            U3PlatformSetBooleanPreference(
                U3PreferenceClassicAppearance,
                !U3PlatformGetBooleanPreference(U3PreferenceClassicAppearance));
            return true;
        case RowTileSize:
            U3PlatformSetBooleanPreference(
                U3PreferenceOriginalSize,
                !U3PlatformGetBooleanPreference(U3PreferenceOriginalSize));
            return true;
        case RowFullScreen: {
            bool next = !U3PlatformGetBooleanPreference(U3PreferenceFullScreen);
            U3CocoaSetMainSurfaceFullScreen(next);
            U3PlatformSetBooleanPreference(U3PreferenceFullScreen, next);
            return true;
        }
        case RowScaling:
            U3LinuxVideoSetSmoothScaling(!U3LinuxVideoSmoothScaling());
            return true;
        case RowScanlines:
            U3LinuxVideoSetScanlines(!U3LinuxVideoScanlines());
            return true;
        case RowTextSize: {
            /* 25-point steps: the granularity at which a 16px glyph cell
             * actually changes size. The setter clamps to its own range. */
            int current = U3LinuxVideoFontScalePercent();
            U3LinuxVideoSetFontScalePercent(current + step * 25);
            return U3LinuxVideoFontScalePercent() != current;
        }
        case RowSound:
            SetEnabledPreference(U3PreferenceSoundDisabled, true,
                                 !EnabledPreference(U3PreferenceSoundDisabled, true));
            ApplyAudio();
            return true;
        case RowSoundVolume: {
            int current = VolumePercent(U3PreferenceSoundVolume);
            int next = ClampVolume(current + step * 10);
            U3PlatformSetIntegerPreference(U3PreferenceSoundVolume, next);
            ApplyAudio();
            return next != current;
        }
        case RowMusic:
            SetEnabledPreference(U3PreferenceMusicDisabled, true,
                                 !EnabledPreference(U3PreferenceMusicDisabled, true));
            ApplyAudio();
            return true;
        case RowMusicVolume: {
            int current = VolumePercent(U3PreferenceMusicVolume);
            int next = ClampVolume(current + step * 10);
            U3PlatformSetIntegerPreference(U3PreferenceMusicVolume, next);
            ApplyAudio();
            return next != current;
        }
        case RowSpeech:
            SetEnabledPreference(U3PreferenceSpeechDisabled, true,
                                 !EnabledPreference(U3PreferenceSpeechDisabled, true));
            U3LinuxSpeechRefreshFromPreferences();
            return true;
        case RowSpeedLimiter:
            SetEnabledPreference(U3PreferenceUnconstrainedSpeed, true,
                                 !EnabledPreference(U3PreferenceUnconstrainedSpeed, true));
            return true;
        case RowAutoHeal:
            SetEnabledPreference(U3PreferenceNoAutoHeal, true,
                                 !EnabledPreference(U3PreferenceNoAutoHeal, true));
            return true;
        case RowDiagonals:
            SetEnabledPreference(U3PreferenceNoDiagonals, true,
                                 !EnabledPreference(U3PreferenceNoDiagonals, true));
            return true;
        case RowAutoSave:
            U3PlatformSetBooleanPreference(
                U3PreferenceAutoSave, !U3PlatformGetBooleanPreference(U3PreferenceAutoSave));
            return true;
        case RowCombat:
            U3PlatformSetBooleanPreference(
                U3PreferenceManualCombat,
                !U3PlatformGetBooleanPreference(U3PreferenceManualCombat));
            return true;
        case RowCount:
            break;
    }
    return false;
}

/* ==========================================================================
 * Modal presentation
 *
 * The screen save/restore, the text helper and the modal key loop all live in
 * U3LinuxOverlay.{c,h}, shared with the reference card.
 * ========================================================================== */

static void DrawOverlay(int cursor, const U3LinuxOverlayMetrics *m) {
    const short width = (short)m->width, height = (short)m->height;
    const int cellW = m->cellWidth, ascent = m->ascent, cellH = m->cellHeight;
    const int pitch = m->pitch;

    U3CocoaSetBackgroundQuickDrawColor(blackColor);
    U3CocoaEraseRect(0, 0, width, height);
    U3CocoaSetForegroundQuickDrawColor(whiteColor);
    U3CocoaFrameRect(2, 2, (short)(width - 2), (short)(height - 2));

    const short left = (short)(cellW / 2 + 4);
    const short indent = (short)(left + cellW);
    /* Widest label is "Diagonal moves"; one cell of gap after it. */
    const short valueColumn = (short)(indent + 16 * cellW);
    short y = (short)(4 + ascent);

    U3LinuxOverlayText(left, y, "SETTINGS");
    y = (short)(y + pitch);
    /* Deliberately terse: the long form did not fit 40 columns. */
    U3LinuxOverlayText(left, y, "Up/Down  Left/Right change  Esc close");
    y = (short)(y + pitch);

    const char *section = NULL;
    for (int row = 0; row < RowCount; ++row) {
        if (kRows[row].section && kRows[row].section != section) {
            section = kRows[row].section;
            U3CocoaSetForegroundQuickDrawColor(whiteColor);
            U3LinuxOverlayText(left, y, section);
            y = (short)(y + pitch);
        }

        char label[64], value[64];
        if (!U3LinuxSettingsRowText(row, label, sizeof(label), value, sizeof(value)))
            continue;

        /* Selected row is black on a white bar, the same highlight the
         * party-selection dialog uses. The bar covers the glyph box, which
         * starts one ascent above the baseline. */
        if (row == cursor) {
            U3CocoaSetForegroundQuickDrawColor(whiteColor);
            U3CocoaPaintRect(left, (short)(y - ascent), (short)(width - left),
                             (short)(y - ascent + cellH));
            U3CocoaSetForegroundQuickDrawColor(blackColor);
        } else {
            U3CocoaSetForegroundQuickDrawColor(whiteColor);
        }
        /* A leading asterisk, explained in the footer, rather than a
         * "(restart)" column: the long form ran off the right edge next to the
         * widest value ("Original (640x384)"). */
        char marked[72];
        snprintf(marked, sizeof(marked), "%s%s", kRows[row].restartRequired ? "*" : " ", label);
        U3LinuxOverlayText(indent, y, marked);
        U3LinuxOverlayText(valueColumn, y, value);
        y = (short)(y + pitch);
    }

    U3CocoaSetForegroundQuickDrawColor(whiteColor);
    U3LinuxOverlayText(left, y, "* takes effect on restart");

    U3CocoaInvalidateMainSurface();
    U3CocoaPresentMainSurface();
}

void U3LinuxSettingsShow(void) {
    U3Bitmap snapshot;
    U3LinuxOverlayMetrics metrics;
    if (!U3LinuxOverlayBegin(&snapshot, &metrics))
        return;

    int cursor = 0;
    for (;;) {
        DrawOverlay(cursor, &metrics);
        char key = U3LinuxOverlayReadKey();
        if (key == 27 || key == 'q' || key == 'Q')
            break;
        switch (key) {
            case 30: /* Up */
                cursor = (cursor + RowCount - 1) % RowCount;
                break;
            case 31: /* Down */
                cursor = (cursor + 1) % RowCount;
                break;
            case 28: /* Left */
                U3LinuxSettingsAdjustRow(cursor, -1);
                break;
            case 29: /* Right */
            case 13: /* Enter toggles, which is what Right does for a boolean */
                U3LinuxSettingsAdjustRow(cursor, +1);
                break;
            default:
                break;
        }
    }

    U3LinuxOverlayEnd(&snapshot);
}

void U3LinuxSettingsDrawForTesting(int cursorRow) {
    U3Bitmap snapshot;
    U3LinuxOverlayMetrics metrics;
    if (!U3LinuxOverlayBegin(&snapshot, &metrics))
        return;
    /* Leave the overlay on screen for the caller to capture; Show() restores
     * the game's pixels before returning. */
    U3BitmapDispose(&snapshot);
    DrawOverlay(cursorRow, &metrics);
}

void U3LinuxSettingsInstall(void) {
    U3LinuxVideoSetOverlayHandler(U3LinuxVideoOverlaySettings, U3LinuxSettingsShow);
}
