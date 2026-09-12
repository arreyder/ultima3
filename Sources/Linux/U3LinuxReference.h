//
//  U3LinuxReference.h
//  Ultima3 (Linux port)
//
//  In-game command and spell reference card (F2). See U3LinuxReference.c for
//  why it exists and where its contents come from.
//

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "U3LinuxAssets.h"

/* Registers the card with U3LinuxVideo so F2 opens it, and supplies the asset
 * context its spell names are read from. Call once at startup. Passing NULL
 * assets is legal: the command page still works and spell rows read "--"
 * rather than inventing names. */
void U3LinuxReferenceInstall(U3LinuxAssets *assets);

/* Runs the card modally until the player closes it. Normally reached through
 * the handler above. */
void U3LinuxReferenceShow(void);

/* Page structure, exposed so a test can assert the contents -- that every
 * command letter is listed, and that spell names come from the game's own
 * table -- without driving a modal loop through synthesized keystrokes. */
int U3LinuxReferencePageCount(void);
const char *U3LinuxReferencePageTitle(int page);

/* One entry of a page: `outKey` receives the letter to press and `outText` what
 * it does. Returns false once `index` is past the end of the page, which is
 * also how the page length is discovered. */
bool U3LinuxReferenceEntry(int page, int index, char *outKey, size_t keySize,
                           char *outText, size_t textSize);

/* Test hook: draw one page into the game bitmap and leave it there, so it can
 * be captured. Show() restores the screen before returning. */
void U3LinuxReferenceDrawForTesting(int page);
