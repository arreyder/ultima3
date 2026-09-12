//
//  U3LinuxOverlay.h
//  Ultima3 (Linux port)
//
//  Shared plumbing for the modal overlays this port draws into the game's own
//  bitmap -- the settings overlay and the command/spell reference card.
//
//  Drawing into the game bitmap, rather than compositing a separate layer at
//  present time, is what lets an overlay inherit scaling, letterboxing, the CRT
//  filter and screenshots with no second presentation path to keep in step. It
//  is the same technique U3LinuxVideo.c uses for party selection and character
//  creation.
//
//  These entry points exist because there are now two such overlays and a third
//  is cheap to add; without them each one re-implements the same screen
//  save/restore, the same text helper, and the same modal key loop, and they
//  drift.
//

#pragma once

#include <stdbool.h>
#include "U3Bitmap.h"

/* Layout metrics for the current overlay: the rasterized cell the built-in
 * bitmap font is drawing at. Overlays must lay themselves out from these rather
 * than assume 16px, because the game leaves TextSize() wherever its last draw
 * put it and the player's font scale applies on top -- an assumed cell that is
 * too small runs off the screen edge. */
typedef struct U3LinuxOverlayMetrics {
    int cellWidth;
    int cellHeight;
    int ascent;   /* baseline offset from the top of the glyph box */
    int pitch;    /* recommended row pitch */
    int width;    /* game bitmap dimensions */
    int height;
} U3LinuxOverlayMetrics;

/* Takes over the screen: saves the current game bitmap into `snapshot`,
 * deselects any offscreen target, sizes the font to the canvas, and reports the
 * resulting metrics. Returns false if there is no bitmap to draw into, in which
 * case the caller must not proceed.
 *
 * Always pair with U3LinuxOverlayEnd, which restores both the pixels and the
 * text size the game had set. */
bool U3LinuxOverlayBegin(U3Bitmap *snapshot, U3LinuxOverlayMetrics *metrics);

/* Restores the screen and the game's text size, then presents. */
void U3LinuxOverlayEnd(U3Bitmap *snapshot);

/* Draws an ASCII string with its baseline at (x, y). */
void U3LinuxOverlayText(short x, short y, const char *ascii);

/* Blocks for one keypress on the same input path the rest of the game uses,
 * presenting between polls so the window stays responsive.
 *
 * Under a headless diagnostic run, exhausting the scripted keys reads as
 * Escape rather than spinning forever -- without which every scripted test that
 * happened to open an overlay would hang. */
char U3LinuxOverlayReadKey(void);
