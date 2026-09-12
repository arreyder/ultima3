//
//  U3LinuxOverlay.c
//  Ultima3 (Linux port)
//
//  See U3LinuxOverlay.h.
//

#define _DEFAULT_SOURCE

#include "U3LinuxOverlay.h"
#include "U3LinuxVideo.h"
#include "U3LegacyTypes.h"
#include "CarbonShunts.h"
#include "CocoaBridge.h"

#include <string.h>

/* The text size the game had before an overlay took over, so U3LinuxOverlayEnd
 * can put it back. Overlays are strictly modal and never nest, so one slot is
 * enough. */
static short sSavedTextSize = 16;

bool U3LinuxOverlayBegin(U3Bitmap *snapshot, U3LinuxOverlayMetrics *metrics) {
    U3Bitmap *bitmap = U3CocoaMainBitmap();
    if (!bitmap || !bitmap->pixels || !snapshot || !metrics)
        return false;

    /* Save the screen: the overlay paints over the live bitmap and the game
     * does not necessarily redraw everything on its next turn, so anything not
     * put back stays corrupted on screen. */
    memset(snapshot, 0, sizeof(*snapshot));
    if (U3BitmapAllocate(snapshot, bitmap->width, bitmap->height)) {
        U3BitmapRect whole = {0, 0, bitmap->width, bitmap->height};
        if (!U3BitmapCopy(snapshot, whole, bitmap, whole))
            U3BitmapDispose(snapshot);
    }

    U3CocoaSelectBitmap(NULL, 0, 0);

    int cellW = 16, cellH = 16, ascent = 13;
    U3LinuxVideoTextCellSize(&cellW, &cellH, &ascent);
    sSavedTextSize = (short)(cellH > 0 ? cellH : 16);

    /* Ask for a cell sized to the canvas, so an overlay stays proportionate in
     * the game's double-size mode instead of shrinking into a corner. */
    U3CocoaSetTextSize(bitmap->width >= 1280 ? 32 : 16);
    U3LinuxVideoTextCellSize(&cellW, &cellH, &ascent);

    metrics->cellWidth = cellW > 0 ? cellW : 1;
    metrics->cellHeight = cellH > 0 ? cellH : 1;
    metrics->ascent = ascent;
    /* Exactly one cell, which is also the pitch the game's own text engine
     * uses. The glyph box carries its own padding, so lines do not touch, and
     * added leading pushes a full-screen list past the bottom edge. */
    metrics->pitch = metrics->cellHeight;
    metrics->width = bitmap->width;
    metrics->height = bitmap->height;
    return true;
}

void U3LinuxOverlayEnd(U3Bitmap *snapshot) {
    U3CocoaSetTextSize(sSavedTextSize);
    U3Bitmap *bitmap = U3CocoaMainBitmap();
    if (snapshot && snapshot->pixels && bitmap && bitmap->pixels) {
        U3BitmapRect whole = {0, 0, bitmap->width, bitmap->height};
        U3BitmapCopy(bitmap, whole, snapshot, whole);
    }
    if (snapshot)
        U3BitmapDispose(snapshot);
    U3CocoaInvalidateMainSurface();
    U3CocoaPresentMainSurface();
}

void U3LinuxOverlayText(short x, short y, const char *ascii) {
    if (!ascii)
        return;
    Str255 pstring;
    size_t length = strlen(ascii);
    if (length > 255) length = 255;
    pstring[0] = (unsigned char)length;
    memcpy(pstring + 1, ascii, length);
    U3CocoaMoveTo(x, y);
    U3CocoaDrawPascalString(pstring);
}

char U3LinuxOverlayReadKey(void) {
    for (;;) {
        char key = 0;
        Boolean isMouse = false;
        Boolean headless = U3CocoaIsHeadlessDiagnostic();
        Boolean got = U3CocoaPollKeyMouse(true, headless ? 0 : 6, &key, &isMouse);
        if (headless) {
            if (!got)
                return 27;
            if (isMouse)
                continue;
            return key;
        }
        if (got && !isMouse)
            return key;
        U3CocoaPresentMainSurface();
    }
}
