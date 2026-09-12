// tests/linux-reference-test.c
//
// Tests Sources/Linux/U3LinuxReference.{c,h}: the command and spell reference
// card.
//
// A reference card that lists the wrong letter is worse than no card at all --
// the player trusts it and loses a turn, or a torch. So this checks the
// contents against the game's own authorities rather than against a
// transcription: every command letter A-Z must be present exactly once, and
// the spell names must match what Sources/UltimaSpellCombat.c would actually
// cast for that letter, read out of the same "Spells" string table.

#include "U3LinuxReference.h"
#include "U3LinuxAssets.h"
#include "U3Types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pulled in transitively by the video layer, and normally defined in
 * UltimaMain.c. Only the character-creation dialog reaches it, which this test
 * never opens, so it fails loudly rather than returning an answer it has no
 * basis for. */
bool U3ValidateCharacterDraft(const U3CharacterDraft *draft) {
    (void)draft;
    fprintf(stderr, "FAIL: the reference test should never reach character validation\n");
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

/* Mirrors ClericChoose / WizardChoose in Sources/UltimaSpellCombat.c: both
 * casters index one 32-entry table, wizards from 0 and clerics from 16. */
#define WIZARD_BASE 0
#define CLERIC_BASE 16

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <assets-dir>\n", argv[0]);
        return 2;
    }
    U3LinuxAssets *assets = U3LinuxAssetsOpen(argv[1]);
    CHECK(assets != NULL, "assets opened");
    if (!assets)
        return 1;
    U3LinuxReferenceInstall(assets);

    CHECK(U3LinuxReferencePageCount() == 3, "three pages: commands, wizard, cleric");
    for (int page = 0; page < U3LinuxReferencePageCount(); ++page) {
        CHECK(U3LinuxReferencePageTitle(page) != NULL, "each page has a title");
    }
    CHECK(U3LinuxReferencePageTitle(-1) == NULL &&
              U3LinuxReferencePageTitle(U3LinuxReferencePageCount()) == NULL,
          "out-of-range pages have no title");

    char key[8], text[64];

    /* ---- Commands: every letter A-Z, once, in order ---- */
    bool seen[26] = {false};
    int commands = 0;
    bool ordered = true;
    while (U3LinuxReferenceEntry(0, commands, key, sizeof(key), text, sizeof(text))) {
        if (strlen(key) != 1 || key[0] < 'A' || key[0] > 'Z') {
            ordered = false;
        } else {
            if (key[0] != 'A' + commands)
                ordered = false;
            seen[key[0] - 'A'] = true;
        }
        if (text[0] == '\0')
            ordered = false;
        ++commands;
    }
    CHECK(commands == 26, "the command page lists 26 entries");
    CHECK(ordered, "commands run A-Z in order, each with a description");
    bool all = true;
    for (int i = 0; i < 26; ++i)
        all = all && seen[i];
    CHECK(all, "every letter A-Z is covered");

    /* The one the dungeon sends you looking for. */
    bool foundIgnite = false;
    for (int i = 0; i < 26; ++i) {
        if (U3LinuxReferenceEntry(0, i, key, sizeof(key), text, sizeof(text)) &&
            key[0] == 'I' && strstr(text, "gnite")) {
            foundIgnite = true;
        }
    }
    CHECK(foundIgnite, "I is listed as Ignite, which is what lights a dungeon");

    /* ---- Spells match the game's own table at the game's own offsets ---- */
    struct { int page; int base; const char *label; int count; } casters[] = {
        {1, WIZARD_BASE, "wizard", 16 + 3}, /* Q,R,S are bonus spells */
        {2, CLERIC_BASE, "cleric", 16},
    };
    for (size_t c = 0; c < sizeof(casters) / sizeof(casters[0]); ++c) {
        int entries = 0;
        bool matched = true;
        while (U3LinuxReferenceEntry(casters[c].page, entries, key, sizeof(key),
                                     text, sizeof(text))) {
            if (key[0] != 'A' + entries)
                matched = false;
            if (entries < 16) {
                char *expected = U3LinuxAssetString(assets, "Spells",
                                                    (size_t)(casters[c].base + entries));
                if (expected) {
                    /* A blank table slot is shown as "--" rather than an empty
                     * row; anything else must match the table exactly. */
                    bool blank = expected[0] == '\0' ||
                                 (expected[0] == ' ' && expected[1] == '\0');
                    if (blank ? strcmp(text, "--") != 0 : strcmp(text, expected) != 0) {
                        matched = false;
                        fprintf(stderr, "  %s %c: card says \"%s\", table says \"%s\"\n",
                                casters[c].label, key[0], text, expected);
                    }
                    free(expected);
                } else {
                    matched = false;
                }
            }
            ++entries;
        }
        char message[96];
        snprintf(message, sizeof(message), "%s page lists %d entries",
                 casters[c].label, casters[c].count);
        CHECK(entries == casters[c].count, message);
        snprintf(message, sizeof(message),
                 "%s spell names match the game's own Spells table", casters[c].label);
        CHECK(matched, message);
    }

    /* A caster's letters must not silently collide: wizard A and cleric A are
     * different spells, and showing the same name for both would be the most
     * likely way to get the 16-entry offset wrong. */
    char wizardA[64], clericA[64];
    U3LinuxReferenceEntry(1, 0, key, sizeof(key), wizardA, sizeof(wizardA));
    U3LinuxReferenceEntry(2, 0, key, sizeof(key), clericA, sizeof(clericA));
    CHECK(strcmp(wizardA, clericA) != 0,
          "wizard A and cleric A are different spells, so the table offset is applied");
    fprintf(stderr, "info: wizard A = %s, cleric A = %s\n", wizardA, clericA);

    CHECK(!U3LinuxReferenceEntry(0, -1, key, sizeof(key), text, sizeof(text)) &&
              !U3LinuxReferenceEntry(99, 0, key, sizeof(key), text, sizeof(text)),
          "out-of-range pages and indices are refused");

    U3LinuxAssetsClose(assets);
    if (gFailures > 0) {
        fprintf(stderr, "\n%d check(s) FAILED\n", gFailures);
        return 1;
    }
    fprintf(stderr, "\nall reference checks passed\n");
    return 0;
}
