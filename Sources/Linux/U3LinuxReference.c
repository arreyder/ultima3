//
//  U3LinuxReference.c
//  Ultima3 (Linux port)
//
//  In-game command and spell reference card, opened with F2.
//
//  Ultima III expects the player to know 26 single-letter commands and to type
//  arcane spell names chosen by letter, all of which shipped on a printed
//  reference card in 1983. That card is what a modern player is missing, and
//  its absence is felt most in exactly the situations where guessing is
//  expensive -- which command lights a torch, which letter is the healing
//  spell.
//
//  The spell names are read from the game's own "Spells" string table rather
//  than copied into this file, so they cannot drift from what the game actually
//  casts and a localized table is picked up for free.
//

#define _DEFAULT_SOURCE

#include "U3LinuxReference.h"
#include "U3LinuxOverlay.h"
#include "U3LinuxVideo.h"
#include "U3LegacyTypes.h"
#include "CarbonShunts.h"
#include "CocoaBridge.h"
#include "U3Bitmap.h"

#include <stdio.h>
#include <string.h>

/* Transcribed from the LetterCommand switch in Sources/UltimaMain.c, which is
 * the authority on what each key actually does. Kept in the same order so the
 * two can be compared by eye; the test asserts every letter A-Z is present. */
static const char *const kCommands[26] = {
    "Attack",        /* A */
    "Board",         /* B */
    "Cast spell",    /* C */
    "Descend",       /* D */
    "Enter",         /* E */
    "Fire cannon",   /* F */
    "Get chest",     /* G */
    "Hand equipment",/* H */
    "Ignite torch",  /* I */
    "Join gold",     /* J */
    "Klimb",         /* K */
    "Look",          /* L */
    "Modify order",  /* M */
    "Negate time",   /* N */
    "Other command", /* O */
    "Peer at gem",   /* P */
    "Quit and save", /* Q */
    "Ready weapon",  /* R */
    "Steal",         /* S */
    "Transact",      /* T */
    "Unlock",        /* U */
    "Volume",        /* V */
    "Wear armour",   /* W */
    "eXit",          /* X */
    "Yell",          /* Y */
    "Ztats",         /* Z */
};

/* Where each caster's spells begin in the "Spells" table.
 *
 * WizardChoose and ClericChoose in Sources/UltimaSpellCombat.c are the
 * authority: a cleric's letter maps to `letter - ('A' - 16)` and a wizard's to
 * `letter - 'A'`, so the two casters share one 32-entry table, wizard first.
 * Wizards additionally accept Q, R and S for three bonus spells that are gated
 * on Party[16] and have no entry in that table, which is why they are described
 * rather than named. */
#define U3_WIZARD_SPELL_BASE 0
#define U3_CLERIC_SPELL_BASE 16
#define U3_SPELLS_PER_CASTER 16

typedef enum {
    PageCommands = 0,
    PageWizard,
    PageCleric,
    PageCount
} ReferencePage;

static const char *const kPageTitles[PageCount] = {
    "COMMANDS",
    "WIZARD SPELLS",
    "CLERIC SPELLS",
};

/* The asset context the spell names are read from; supplied at install time
 * because this module has no other route to the game's string tables. */
static U3LinuxAssets *sAssets = NULL;

int U3LinuxReferencePageCount(void) {
    return PageCount;
}

const char *U3LinuxReferencePageTitle(int page) {
    if (page < 0 || page >= PageCount)
        return NULL;
    return kPageTitles[page];
}

bool U3LinuxReferenceEntry(int page, int index, char *outKey, size_t keySize,
                           char *outText, size_t textSize) {
    if (!outKey || !outText || keySize == 0 || textSize == 0)
        return false;

    if (page == PageCommands) {
        if (index < 0 || index >= 26)
            return false;
        snprintf(outKey, keySize, "%c", 'A' + index);
        snprintf(outText, textSize, "%s", kCommands[index]);
        return true;
    }

    if (page != PageWizard && page != PageCleric)
        return false;

    /* Wizards have the three bonus spells past P; clerics stop at P. */
    int bonus = (page == PageWizard) ? 3 : 0;
    if (index < 0 || index >= U3_SPELLS_PER_CASTER + bonus)
        return false;
    snprintf(outKey, keySize, "%c", 'A' + index);

    if (index >= U3_SPELLS_PER_CASTER) {
        /* Q, R and S are unlocked by Party[16] and are not in the Spells
         * table, so say what they are rather than inventing a name. */
        snprintf(outText, textSize, "%s", "(bonus spell)");
        return true;
    }

    int base = (page == PageWizard) ? U3_WIZARD_SPELL_BASE : U3_CLERIC_SPELL_BASE;
    char *name = sAssets ? U3LinuxAssetString(sAssets, "Spells", (size_t)(base + index)) : NULL;
    if (!name || !name[0] || (name[0] == ' ' && !name[1])) {
        /* A blank entry is a real hole in the table (wizard P), not a load
         * failure; either way, say so instead of showing an empty row. */
        free(name);
        snprintf(outText, textSize, "%s", "--");
        return true;
    }
    snprintf(outText, textSize, "%s", name);
    free(name);
    return true;
}

static int EntryCount(int page) {
    int count = 0;
    char key[8], text[64];
    while (U3LinuxReferenceEntry(page, count, key, sizeof(key), text, sizeof(text)))
        ++count;
    return count;
}

static void DrawPage(int page, const U3LinuxOverlayMetrics *m) {
    U3CocoaSetBackgroundQuickDrawColor(blackColor);
    U3CocoaEraseRect(0, 0, (short)m->width, (short)m->height);
    U3CocoaSetForegroundQuickDrawColor(whiteColor);
    U3CocoaFrameRect(2, 2, (short)(m->width - 2), (short)(m->height - 2));

    const short left = (short)(m->cellWidth / 2 + 4);
    short y = (short)(4 + m->ascent);

    char heading[80];
    snprintf(heading, sizeof(heading), "%s  (%d/%d)", kPageTitles[page], page + 1, PageCount);
    U3LinuxOverlayText(left, y, heading);
    y = (short)(y + m->pitch);
    U3LinuxOverlayText(left, y, "Left/Right page   Esc close");
    y = (short)(y + m->pitch);

    int count = EntryCount(page);
    /* Two columns: 26 commands do not fit one screen at one row each, and the
     * entries are short enough that a second column costs nothing. The split
     * is by halves so the first column reads A..M top to bottom. */
    int rows = (count + 1) / 2;

    /* Unlike the settings overlay, which needs every pixel for 22 lines, these
     * pages are short enough to afford leading -- and at exactly one cell the
     * letter column reads as one continuous vertical strip. Use the roomier
     * pitch whenever it still fits, and fall back to tight if it would not. */
    int pitch = m->pitch + m->pitch / 4;
    if ((int)y + rows * pitch > m->height - m->pitch)
        pitch = m->pitch;
    int available = (m->height - (int)y - pitch) / pitch;
    if (available > 0 && rows > available)
        rows = available;

    const short columnWidth = (short)((m->width - 2 * left) / 2);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < 2; ++column) {
            int index = column * rows + row;
            char key[8], text[64];
            if (!U3LinuxReferenceEntry(page, index, key, sizeof(key), text, sizeof(text)))
                continue;
            char line[96];
            snprintf(line, sizeof(line), "%s  %s", key, text);
            /* Clip to the column rather than letting a long entry run past the
             * screen edge, which is what an over-long bonus-spell label did
             * before. One character of margin keeps adjacent columns apart. */
            int budget = columnWidth / m->cellWidth - 1;
            if (budget > 0 && (int)strlen(line) > budget)
                line[budget] = '\0';
            U3LinuxOverlayText((short)(left + column * columnWidth),
                               (short)(y + row * pitch), line);
        }
    }

    U3CocoaInvalidateMainSurface();
    U3CocoaPresentMainSurface();
}

void U3LinuxReferenceShow(void) {
    U3Bitmap snapshot;
    U3LinuxOverlayMetrics metrics;
    if (!U3LinuxOverlayBegin(&snapshot, &metrics))
        return;

    int page = 0;
    for (;;) {
        DrawPage(page, &metrics);
        char key = U3LinuxOverlayReadKey();
        if (key == 27 || key == 'q' || key == 'Q')
            break;
        switch (key) {
            case 28: /* Left */
                page = (page + PageCount - 1) % PageCount;
                break;
            case 29: /* Right */
            case 9:  /* Tab */
            case 13: /* Enter */
                page = (page + 1) % PageCount;
                break;
            default:
                break;
        }
    }

    U3LinuxOverlayEnd(&snapshot);
}

void U3LinuxReferenceDrawForTesting(int page) {
    U3Bitmap *bitmap = U3CocoaMainBitmap();
    if (!bitmap)
        return;
    U3Bitmap snapshot;
    U3LinuxOverlayMetrics metrics;
    if (!U3LinuxOverlayBegin(&snapshot, &metrics))
        return;
    /* Leave the page on screen for the caller to capture: Show() restores the
     * game's pixels before returning, so a screenshot taken after it would
     * show everything except the page under test. */
    U3BitmapDispose(&snapshot);
    DrawPage(page, &metrics);
}

void U3LinuxReferenceInstall(U3LinuxAssets *assets) {
    sAssets = assets;
    U3LinuxVideoSetReferenceHandler(U3LinuxReferenceShow);
}
