// tests/linux-settings-test.c
//
// Tests Sources/Linux/U3LinuxSettings.{c,h}: the in-game settings overlay.
//
// Driving the modal loop through synthesized keystrokes would test the SDL
// input plumbing more than the settings themselves, so this exercises the same
// entry points the loop calls -- U3LinuxSettingsRowText and
// U3LinuxSettingsAdjustRow -- and checks the thing that actually matters: that
// changing a row writes the stored preference, and that the row then reports
// the new value back. A setting that renders but never persists, or persists
// but never redisplays, is the failure this is here to catch.
//
// Needs U3_SAVE_DIRECTORY so it never touches a real ~/.config/ultima3.

#include "U3LinuxSettings.h"
#include "U3LinuxPlatform.h"
#include "U3Platform.h"
#include "U3Types.h"
/* U3LegacyTypes.h defines Boolean and CarbonShunts.h the QuickDraw colour
 * constants that CocoaBridge.h and the drawing calls below rely on; both
 * must come first, as they do in tests/linux-video-test.c. */
#include "Linux/U3LegacyTypes.h"
#include "CarbonShunts.h"
#include "CocoaBridge.h"
#include "U3LinuxVideo.h"
#include "U3LinuxAssets.h"
#include "U3Bitmap.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Two symbols the video and audio layers reference that normally come from
 * UltimaMain.c. The whole game core is far more than this test needs, so they
 * are supplied here instead -- the same approach tests/linux-video-test.c and
 * tests/linux-audio-test.c take.
 *
 * Party is real state (U3AudioUpdateMusic reads Party[3] to detect combat);
 * U3ValidateCharacterDraft is only reachable through the character-creation
 * dialog, which the settings overlay never opens. Rather than return a
 * plausible answer this test has no business giving, it fails loudly if
 * anything ever calls it. */
unsigned char Party[65];

bool U3ValidateCharacterDraft(const U3CharacterDraft *draft) {
    (void)draft;
    fprintf(stderr, "FAIL: the settings test should never reach character validation\n");
    abort();
}

static int gFailures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++gFailures;                                                    \
        } else {                                                            \
            fprintf(stderr, "ok: %s\n", msg);                               \
        }                                                                   \
    } while (0)

/* Finds a row by its label, so the tests below do not hard-code row indices
 * that would silently shift if a setting were inserted above them. */
static int RowNamed(const char *label) {
    char rowLabel[64], rowValue[64];
    for (int i = 0; i < U3LinuxSettingsRowCount(); ++i) {
        if (U3LinuxSettingsRowText(i, rowLabel, sizeof(rowLabel), rowValue, sizeof(rowValue)) &&
            !strcmp(rowLabel, label)) {
            return i;
        }
    }
    return -1;
}

static void ValueOf(int row, char *out, size_t size) {
    char label[64];
    out[0] = '\0';
    U3LinuxSettingsRowText(row, label, sizeof(label), out, size);
}

int main(int argc, char **argv) {
    CHECK(getenv("U3_SAVE_DIRECTORY") != NULL,
          "runs against an isolated preferences directory");

    /* ---- Every row renders ---- */
    int rows = U3LinuxSettingsRowCount();
    CHECK(rows > 0, "the overlay offers at least one row");
    bool allRendered = true;
    for (int i = 0; i < rows; ++i) {
        char label[64], value[64];
        if (!U3LinuxSettingsRowText(i, label, sizeof(label), value, sizeof(value)) ||
            label[0] == '\0' || value[0] == '\0') {
            allRendered = false;
            fprintf(stderr, "  row %d did not render\n", i);
        }
    }
    CHECK(allRendered, "every row reports a non-empty label and value");

    char scratch[64];
    CHECK(!U3LinuxSettingsRowText(-1, scratch, sizeof(scratch), scratch, sizeof(scratch)) &&
              !U3LinuxSettingsRowText(rows, scratch, sizeof(scratch), scratch, sizeof(scratch)),
          "out-of-range rows are refused rather than read out of bounds");
    CHECK(!U3LinuxSettingsAdjustRow(-1, 1) && !U3LinuxSettingsAdjustRow(rows, 1),
          "out-of-range adjustments are refused");
    CHECK(!U3LinuxSettingsAdjustRow(0, 0), "a zero delta changes nothing");

    /* ---- A boolean row writes its preference and redisplays ---- */
    int autoSave = RowNamed("Auto-save");
    CHECK(autoSave >= 0, "the Auto-save row exists");
    if (autoSave >= 0) {
        U3PlatformSetBooleanPreference(U3PreferenceAutoSave, false);
        char before[64], after[64];
        ValueOf(autoSave, before, sizeof(before));
        CHECK(!strcmp(before, "Off"), "Auto-save reflects the stored preference");
        U3LinuxSettingsAdjustRow(autoSave, +1);
        CHECK(U3PlatformGetBooleanPreference(U3PreferenceAutoSave),
              "adjusting Auto-save writes the stored preference");
        ValueOf(autoSave, after, sizeof(after));
        CHECK(!strcmp(after, "On"), "the row redisplays the new value");
        U3LinuxSettingsAdjustRow(autoSave, -1);
        CHECK(!U3PlatformGetBooleanPreference(U3PreferenceAutoSave),
              "adjusting back restores the preference");
    }

    /* ---- Inverted preferences are shown in the positive sense ----
     *
     * Several stored keys are negatives (NoAutoHeal, SoundDisabled), because an
     * absent preference reads false and the original default is "on". Getting
     * that inversion backwards would show every such row wrong, so pin it. */
    int autoHeal = RowNamed("Auto-heal");
    CHECK(autoHeal >= 0, "the Auto-heal row exists");
    if (autoHeal >= 0) {
        U3PlatformSetBooleanPreference(U3PreferenceNoAutoHeal, false);
        char value[64];
        ValueOf(autoHeal, value, sizeof(value));
        CHECK(!strcmp(value, "On"), "NoAutoHeal unset displays as Auto-heal On");
        U3LinuxSettingsAdjustRow(autoHeal, +1);
        CHECK(U3PlatformGetBooleanPreference(U3PreferenceNoAutoHeal),
              "turning Auto-heal off sets the negative preference");
        ValueOf(autoHeal, value, sizeof(value));
        CHECK(!strcmp(value, "Off"), "and the row displays Off");
        U3PlatformSetBooleanPreference(U3PreferenceNoAutoHeal, false);
    }

    /* ---- Volumes step and clamp ---- */
    int soundVolume = RowNamed("Sound volume");
    CHECK(soundVolume >= 0, "the Sound volume row exists");
    if (soundVolume >= 0) {
        U3PlatformSetIntegerPreference(U3PreferenceSoundVolume, 50);
        U3LinuxSettingsAdjustRow(soundVolume, +1);
        CHECK(U3PlatformGetIntegerPreference(U3PreferenceSoundVolume) == 60,
              "a volume step is 10 points");
        char value[64];
        ValueOf(soundVolume, value, sizeof(value));
        CHECK(!strcmp(value, "60%"), "the volume row displays the stored percentage");

        U3PlatformSetIntegerPreference(U3PreferenceSoundVolume, 95);
        U3LinuxSettingsAdjustRow(soundVolume, +1);
        CHECK(U3PlatformGetIntegerPreference(U3PreferenceSoundVolume) == 100,
              "volume clamps at 100 rather than overshooting");
        CHECK(!U3LinuxSettingsAdjustRow(soundVolume, +1),
              "an adjustment that changes nothing reports so");

        U3PlatformSetIntegerPreference(U3PreferenceSoundVolume, 5);
        U3LinuxSettingsAdjustRow(soundVolume, -1);
        CHECK(U3PlatformGetIntegerPreference(U3PreferenceSoundVolume) == 0,
              "volume clamps at 0 rather than going negative");
        U3PlatformRemovePreference(U3PreferenceSoundVolume);
        ValueOf(soundVolume, value, sizeof(value));
        CHECK(!strcmp(value, "100%"),
              "an unset volume displays as 100%, not 0%, matching the stored convention");
    }

    /* ---- Text size follows the video layer's own clamp ---- */
    int textSize = RowNamed("Text size");
    CHECK(textSize >= 0, "the Text size row exists");
    if (textSize >= 0) {
        char value[64];
        for (int i = 0; i < 8; ++i)
            U3LinuxSettingsAdjustRow(textSize, -1);
        ValueOf(textSize, value, sizeof(value));
        CHECK(!strcmp(value, "50%"), "text size floors at 50%");
        for (int i = 0; i < 8; ++i)
            U3LinuxSettingsAdjustRow(textSize, +1);
        ValueOf(textSize, value, sizeof(value));
        CHECK(!strcmp(value, "100%"), "text size ceilings at 100%");
    }

    /* ---- The two restart-only rows still toggle their preference ----
     *
     * They are labelled "restart" in the UI because the game cannot rebuild its
     * offscreen worlds mid-turn, but the preference must still be written, or
     * the setting would silently do nothing at all. */
    int tileSize = RowNamed("Tile size");
    CHECK(tileSize >= 0, "the Tile size row exists");
    if (tileSize >= 0) {
        bool before = U3PlatformGetBooleanPreference(U3PreferenceOriginalSize);
        U3LinuxSettingsAdjustRow(tileSize, +1);
        CHECK(U3PlatformGetBooleanPreference(U3PreferenceOriginalSize) != before,
              "a restart-only row still writes its preference");
        U3LinuxSettingsAdjustRow(tileSize, +1);
        CHECK(U3PlatformGetBooleanPreference(U3PreferenceOriginalSize) == before,
              "and toggles back");
    }

    /* ---- Opening and closing the overlay leaves the screen untouched ----
     *
     * The overlay paints over the live game bitmap, and the game does not
     * necessarily redraw everything on its next turn, so anything it fails to
     * put back stays corrupted on screen. Checked against the real pixels
     * rather than by inspection.
     *
     * Under the dummy driver the modal loop reads an immediate Escape once the
     * scripted keys run out, so Show() draws once and returns. */
    if (argc > 1) {
        U3LinuxAssets *assets = U3LinuxAssetsOpen(argv[1]);
        CHECK(assets != NULL, "test assets opened");
        U3LinuxVideoConfigure(assets, "settings test", 1);
        bool haveSurface = U3CocoaCreateMainSurface(0, 0, 640, 384) != NULL;
        CHECK(haveSurface, "a main surface was created");
        if (haveSurface) {
            U3Bitmap *screen = U3CocoaMainBitmap();
            /* Something distinctive, so a restore that merely blanks the
             * screen cannot pass. */
            U3CocoaSetForegroundQuickDrawColor(whiteColor);
            U3CocoaPaintRect(20, 20, 300, 120);
            U3CocoaSetForegroundQuickDrawColor(redColor);
            U3CocoaPaintRect(40, 40, 100, 90);

            U3Bitmap before = {0};
            bool copied = U3BitmapAllocate(&before, screen->width, screen->height);
            if (copied) {
                U3BitmapRect whole = {0, 0, screen->width, screen->height};
                copied = U3BitmapCopy(&before, whole, screen, whole);
            }
            CHECK(copied, "captured the screen before opening the overlay");

            U3LinuxSettingsShow();

            bool identical = copied;
            if (copied) {
                for (int y = 0; y < screen->height && identical; ++y) {
                    if (memcmp(before.pixels + (size_t)y * before.stride,
                               screen->pixels + (size_t)y * screen->stride,
                               (size_t)screen->width * 4) != 0) {
                        identical = false;
                        fprintf(stderr, "  first differing row: %d\n", y);
                    }
                }
            }
            CHECK(identical, "closing the overlay restores the screen pixel for pixel");
            U3BitmapDispose(&before);
        }
        U3LinuxVideoShutdown();
        U3LinuxAssetsClose(assets);
    }

    if (gFailures > 0) {
        fprintf(stderr, "\n%d check(s) FAILED\n", gFailures);
        return 1;
    }
    fprintf(stderr, "\nall settings checks passed\n");
    return 0;
}
