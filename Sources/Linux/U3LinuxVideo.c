/* SDL2-backed implementation of the CocoaBridge video/input surface.
 *
 * Scope: this file implements exactly the 43 U3Cocoa* entry points the
 * compiled game core leaves unresolved on Linux (see docs/linux-agent-handoff.md).
 * It does not reimplement rendering: the preserved software renderer draws
 * into a U3Bitmap exactly as it always has, and this file's only rendering
 * job is to upload that bitmap to an SDL texture and present it, scaled by
 * an integer factor with nearest-neighbor sampling.
 *
 * Two things the original CocoaBridge.m got "for free" from AppKit do not
 * exist here and are deliberately reimplemented by drawing into the bitmap
 * with our own primitives, per the task's explicit requirement that these
 * may not be stubs: character creation (U3CocoaCreateCharacter) and party
 * selection (U3CocoaChooseParty). See the "In-bitmap dialogs" section below.
 *
 * Headless/diagnostic behavior: U3CocoaIsHeadlessDiagnostic() preserves the
 * original env-var checks from CocoaBridge.m (other Linux-ported files such
 * as UltimaGraphics.c read those same vars directly, so this must keep
 * agreeing with them) and additionally treats SDL_VIDEODRIVER=dummy as
 * headless, per this task's instructions. Unlike the Mac original -- which
 * skips window/surface creation entirely when headless -- this file still
 * creates a real SDL window/renderer/texture even when headless, so the
 * dummy driver actually exercises the upload/present path under test rather
 * than bypassing it. Only the *input* path switches to the deterministic
 * diagnostic-queue behavior when headless; drawing and presentation always
 * go through real SDL calls.
 */

/* Requested with -std=c11 (strict ISO mode) by this task's verify command,
 * which hides glibc's strdup() behind feature-test macros. Defined here,
 * in this file only, rather than in the shared U3LegacyTypes.h. */
#define _DEFAULT_SOURCE
#include "U3LegacyTypes.h"
#include "CarbonShunts.h"
#include "CocoaBridge.h"
#include "U3LinuxVideo.h"
#include "U3LinuxPlatform.h"

#include <SDL2/SDL.h>

/* ------------------------------------------------------------------------
 * Small bitmap-font glyph cell. Sources/Linux/U3LinuxAssets.c already
 * verifies (see tests/linux-assets-test.c) that "Resources/Graphics/
 * Standard-Font.gif" decodes to a 1536x16 strip of 96 16x16 cells. 127-32+1
 * == 96, so cell index (ch - 32) covers printable ASCII exactly. Ink in
 * that asset is the *light* pixels (background is dark) -- verified by
 * inspection, not assumed; see the report for how this was checked.
 * ------------------------------------------------------------------------ */
#define U3_GLYPH_CELL 16
#define U3_GLYPH_FIRST 32
#define U3_GLYPH_COUNT 96
#define U3_DIAG_KEY_CAPACITY 256
#define U3_PENDING_CAPACITY 64

typedef struct {
    Boolean isMouse;
    char key;
    short mouseX, mouseY;
} U3PendingInput;

/* ---- Configuration (U3LinuxVideoConfigure) ---- */
static U3LinuxAssets *gAssets = NULL;
static char *gWindowTitle = NULL;
static int gScale = 2;

/* ---- Main software surface and draw state ---- */
static U3Bitmap gMainBitmap = {0};
static U3Bitmap *gSelectedBitmap = NULL;
static int gSelectedOriginX = 0, gSelectedOriginY = 0;
static uint8_t gForeground[3] = {255, 255, 255};
static uint8_t gBackground[3] = {0, 0, 0};
static short gPenH = 0, gPenV = 0;
static short gTextFont = 0, gTextSize = 16, gTextFace = 0;
static U3Bitmap gFontBitmap = {0};
static bool gFontLoadAttempted = false;

/* ---- SDL window/renderer/texture ---- */
static bool gSDLVideoReady = false;
static SDL_Window *gWindow = NULL;
static SDL_Renderer *gRenderer = NULL;
static SDL_Texture *gTexture = NULL;
static int gTextureWidth = 0, gTextureHeight = 0;
static char gSurfaceToken;
static bool gSurfaceCreated = false;
static bool gFullscreen = false;
static bool gHeadlessDiagnostic = false;

/* Presentation options, seeded from preferences the first time a surface is
 * created and thereafter toggled live by the hotkeys in HandleSDLEvent. Kept
 * as plain bools rather than read from preferences on every present, which
 * happens once per game frame. */
static bool gSmoothScaling = false;
static bool gScanlines = false;
static int gFontScalePercent = 100;
static bool gPresentationOptionsLoaded = false;

/* Settings overlay seam. The overlay lives above this file (it also drives
 * audio, which this layer knows nothing about), so it registers a handler
 * instead of being called directly -- the same arrangement U3LinuxPlatform.h
 * uses for its input source, and for the same reason: it keeps the dependency
 * one-way.
 *
 * The request is recorded when the key arrives but run later, from
 * U3CocoaPollKeyMouse. Running it where the key is handled would re-enter the
 * event pump: HandleSDLEvent is itself called from inside U3CocoaPollKeyMouse,
 * and the overlay polls for its own input. */
static U3LinuxVideoOverlayHandler gOverlayHandler = NULL;
static bool gOverlayRequested = false;
static bool gOverlayRunning = false;

/* ---- Input ---- */
static U3PendingInput gPending[U3_PENDING_CAPACITY];
static int gPendingHead = 0, gPendingCount = 0;
static Point gMousePoint = {0, 0};
static char gDiagKey = 0;
static unsigned char gDiagKeys[U3_DIAG_KEY_CAPACITY];
static unsigned int gDiagIndex = 0, gDiagCount = 0;
static bool gDiagMousePending = false;

/* ==========================================================================
 * Configuration / lifecycle
 * ========================================================================== */

void U3LinuxVideoConfigure(U3LinuxAssets *assets, const char *windowTitle, int scale) {
    gAssets = assets;
    if (windowTitle) {
        char *copy = strdup(windowTitle);
        if (copy) {
            free(gWindowTitle);
            gWindowTitle = copy;
        }
    }
    if (scale >= 1)
        gScale = scale;
}

void U3LinuxVideoShutdown(void) {
    if (gTexture) { SDL_DestroyTexture(gTexture); gTexture = NULL; }
    if (gRenderer) { SDL_DestroyRenderer(gRenderer); gRenderer = NULL; }
    if (gWindow) { SDL_DestroyWindow(gWindow); gWindow = NULL; }
    if (gSDLVideoReady) { SDL_QuitSubSystem(SDL_INIT_VIDEO); gSDLVideoReady = false; }
    gTextureWidth = gTextureHeight = 0;
    gSurfaceCreated = false;
    U3BitmapDispose(&gMainBitmap);
    U3BitmapDispose(&gFontBitmap);
    gFontLoadAttempted = false;
    gSelectedBitmap = NULL;
    free(gWindowTitle);
    gWindowTitle = NULL;
}

/* ==========================================================================
 * Headless / diagnostic detection
 * ========================================================================== */

Boolean U3CocoaIsHeadlessDiagnostic(void) {
    if (getenv("U3_VERIFY_NATIVE_INPUT"))
        return false;
    if (getenv("U3_BOOT_CHECK") || getenv("U3_WORLD_RENDER_CHECK") ||
        getenv("U3_WORLD_INPUT_CHECK") || getenv("U3_WORLD_MOUSE_CHECK") ||
        getenv("U3_AUDIO_SELF_TEST") || getenv("U3_PARTY_FLOW_CHECK") ||
        getenv("U3_MAIN_MENU_INPUT_CHECK"))
        return true;
    /* Extension beyond CocoaBridge.m, per this task's instructions: running
     * under SDL's dummy video driver means there is no real display, so
     * treat that the same way for input purposes even when none of the
     * original U3_* diagnostic variables are set. */
    const char *driver = getenv("SDL_VIDEODRIVER");
    return driver && strcmp(driver, "dummy") == 0;
}

/* ==========================================================================
 * Bitmap-local drawing primitives
 * ========================================================================== */

static void ResolveTarget(U3Bitmap **target, int *offsetX, int *offsetY) {
    if (gSelectedBitmap) {
        *target = gSelectedBitmap;
        *offsetX = gSelectedOriginX;
        *offsetY = gSelectedOriginY;
    } else {
        *target = &gMainBitmap;
        *offsetX = 0;
        *offsetY = 0;
    }
}

static inline void PutPixel(U3Bitmap *bitmap, int x, int y, const uint8_t rgb[3]) {
    if (!bitmap->pixels || x < 0 || y < 0 || x >= bitmap->width || y >= bitmap->height)
        return;
    uint8_t *pixel = bitmap->pixels + (size_t)y * bitmap->stride + (size_t)x * 4;
    pixel[0] = rgb[0];
    pixel[1] = rgb[1];
    pixel[2] = rgb[2];
}

static void FillRectRaw(U3Bitmap *bitmap, int left, int top, int right, int bottom,
                        const uint8_t rgb[3]) {
    if (!bitmap->pixels)
        return;
    int l = left < right ? left : right, r = left < right ? right : left;
    int t = top < bottom ? top : bottom, b = top < bottom ? bottom : top;
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > bitmap->width) r = bitmap->width;
    if (b > bitmap->height) b = bitmap->height;
    for (int y = t; y < b; ++y) {
        uint8_t *row = bitmap->pixels + (size_t)y * bitmap->stride + (size_t)l * 4;
        for (int x = l; x < r; ++x, row += 4) {
            row[0] = rgb[0];
            row[1] = rgb[1];
            row[2] = rgb[2];
        }
    }
}

static void StrokeRectRaw(U3Bitmap *bitmap, int left, int top, int right, int bottom,
                          const uint8_t rgb[3]) {
    FillRectRaw(bitmap, left, top, right, top + 1, rgb);
    FillRectRaw(bitmap, left, bottom - 1, right, bottom, rgb);
    FillRectRaw(bitmap, left, top, left + 1, bottom, rgb);
    FillRectRaw(bitmap, right - 1, top, right, bottom, rgb);
}

void U3CocoaPaintRect(short left, short top, short right, short bottom) {
    U3Bitmap *target;
    int offX, offY;
    ResolveTarget(&target, &offX, &offY);
    FillRectRaw(target, left - offX, top - offY, right - offX, bottom - offY, gForeground);
    if (!gSelectedBitmap)
        U3CocoaInvalidateMainSurface();
}

void U3CocoaEraseRect(short left, short top, short right, short bottom) {
    U3Bitmap *target;
    int offX, offY;
    ResolveTarget(&target, &offX, &offY);
    FillRectRaw(target, left - offX, top - offY, right - offX, bottom - offY, gBackground);
    if (!gSelectedBitmap)
        U3CocoaInvalidateMainSurface();
}

void U3CocoaFrameRect(short left, short top, short right, short bottom) {
    U3Bitmap *target;
    int offX, offY;
    ResolveTarget(&target, &offX, &offY);
    StrokeRectRaw(target, left - offX, top - offY, right - offX, bottom - offY, gForeground);
    if (!gSelectedBitmap)
        U3CocoaInvalidateMainSurface();
}

void U3CocoaSelectBitmap(U3Bitmap *bitmap, short originX, short originY) {
    gSelectedBitmap = bitmap;
    gSelectedOriginX = originX;
    gSelectedOriginY = originY;
}

/* U3CocoaDrawBitmap always targets the main surface regardless of the
 * currently selected bitmap -- CocoaBridge.m does the same (it calls
 * U3BitmapCopy(&sU3MainBitmap, ...) directly, bypassing sU3SelectedBitmap). */
void U3CocoaDrawBitmap(const U3Bitmap *bitmap, U3BitmapRect source,
                       short x, short y, short width, short height) {
    U3BitmapRect destination = {x, y, width, height};
    if (U3BitmapCopy(&gMainBitmap, destination, bitmap, source))
        U3CocoaInvalidateMainSurface();
}

/* ==========================================================================
 * Color / text state
 * ========================================================================== */

static void QuickDrawColorToRGB(long color, uint8_t out[3]) {
    switch (color) {
        case blackColor: out[0] = out[1] = out[2] = 0; break;
        case whiteColor: out[0] = out[1] = out[2] = 255; break;
        case redColor: out[0] = 255; out[1] = 0; out[2] = 0; break;
        case greenColor: out[0] = 0; out[1] = 255; out[2] = 0; break;
        case blueColor: out[0] = 0; out[1] = 0; out[2] = 255; break;
        case cyanColor: out[0] = 0; out[1] = 255; out[2] = 255; break;
        case magentaColor: out[0] = 255; out[1] = 0; out[2] = 255; break;
        case yellowColor: out[0] = 255; out[1] = 255; out[2] = 0; break;
        default: out[0] = out[1] = out[2] = 255; break;
    }
}

void U3CocoaSetForegroundQuickDrawColor(long color) {
    QuickDrawColorToRGB(color, gForeground);
}

void U3CocoaSetBackgroundQuickDrawColor(long color) {
    QuickDrawColorToRGB(color, gBackground);
}

void U3CocoaSetForegroundRGB(UInt16 red, UInt16 green, UInt16 blue) {
    gForeground[0] = (uint8_t)(red >> 8);
    gForeground[1] = (uint8_t)(green >> 8);
    gForeground[2] = (uint8_t)(blue >> 8);
}

void U3CocoaSetBackgroundRGB(UInt16 red, UInt16 green, UInt16 blue) {
    gBackground[0] = (uint8_t)(red >> 8);
    gBackground[1] = (uint8_t)(green >> 8);
    gBackground[2] = (uint8_t)(blue >> 8);
}

void U3CocoaGetBackground(uint8_t color[3]) {
    if (!color) return;
    color[0] = gBackground[0];
    color[1] = gBackground[1];
    color[2] = gBackground[2];
}

void U3CocoaSetTextFont(short font) { gTextFont = font; }
void U3CocoaSetTextSize(short size) { gTextSize = size > 0 ? size : 16; }
void U3CocoaSetTextFace(short face) { gTextFace = face; }

void U3CocoaMoveTo(short h, short v) {
    gPenH = h;
    gPenV = v;
}

void U3CocoaGetPen(Point *point) {
    if (point) {
        point->h = gPenH;
        point->v = gPenV;
    }
}

/* Lazily loads the bitmap font through the U3LinuxAssets context configured
 * via U3LinuxVideoConfigure(). If no context was configured, or loading
 * fails, or the asset's dimensions don't match the 96-glyph/16px-cell
 * layout this code assumes, text drawing honestly draws nothing rather than
 * guessing at a fallback glyph shape. */
static void EnsureFontLoaded(void) {
    if (gFontLoadAttempted)
        return;
    gFontLoadAttempted = true;
    if (!gAssets)
        return;
    U3Bitmap loaded = {0};
    if (!U3LinuxAssetImage(gAssets, "Resources/Graphics/Standard-Font.gif", &loaded,
                           U3_GLYPH_CELL * U3_GLYPH_COUNT, U3_GLYPH_CELL, U3_GLYPH_COUNT, 1)) {
        return;
    }
    gFontBitmap = loaded;
}

/* Font scale limits.
 *
 * The maximum is 100, i.e. "no larger than the game asked for", and that is a
 * real constraint rather than caution. Sources/UltimaText.c lays text out on a
 * grid of one tile per line (NewPrint draws at ty * blkSiz into an offscreen
 * exactly blkSiz tall) while SetNewFont asks for a font size of exactly
 * blkSiz -- so at 100% a glyph cell already fills its own line completely.
 * Going above it was tried and rendered as collided, unreadable lines: each
 * line erases its blkSiz-tall box and then draws glyphs that spill outside it,
 * over the line drawn before. Fixing that means rescaling the layout's line
 * pitch, which lives in the shared cross-platform text engine.
 *
 * Larger text is instead the tile size's job (blkSiz 16 -> 32, the game's own
 * "double size" mode), which scales art, text and line pitch together. See
 * --double-size in Sources/Linux/main.c.
 */
#define U3_FONT_SCALE_MIN 50
#define U3_FONT_SCALE_MAX 100

static int ClampFontScale(int percent) {
    if (percent < U3_FONT_SCALE_MIN) return U3_FONT_SCALE_MIN;
    if (percent > U3_FONT_SCALE_MAX) return U3_FONT_SCALE_MAX;
    return percent;
}

/* The font size to actually render at: what the game asked for via
 * TextSize(), adjusted by the player's font scale.
 *
 * Applied here rather than in U3CocoaSetTextSize so the game's own value is
 * preserved and rescaled per draw -- and because every text path (draw,
 * TextWidth, the dialogs drawn in this file) funnels through GetFontMetrics,
 * so one adjustment here keeps measurement and drawing in agreement. That
 * matters: the game centers and right-aligns text with U3CocoaTextWidth, and
 * wraps message lines by measured width, so those all follow the scale
 * automatically.
 */
static int ScaledTextSize(void) {
    int size = gTextSize > 0 ? gTextSize : 16;
    if (gFontScalePercent == 100)
        return size; /* exact 1:1 path, no rounding at all */
    int scaled = (size * gFontScalePercent + 50) / 100;
    /* Below ~6px the glyph strip is unreadable; above 64 nothing in the
     * game's layout could hold it. */
    if (scaled < 6) scaled = 6;
    if (scaled > 64) scaled = 64;
    return scaled;
}

static void GetFontMetrics(int *outCellW, int *outCellH, int *outAscent) {
    int size = ScaledTextSize();
    if (size == 16) {
        *outCellW = 16;
        *outCellH = 16;
        *outAscent = 13;
    } else if (size < 16) {
        int h = size >= 8 ? size : 8;
        *outCellH = h;
        if (h <= 9) {
            *outCellW = 6;
        } else if (h <= 11) {
            *outCellW = 7;
        } else if (h <= 13) {
            *outCellW = 8;
        } else {
            *outCellW = 10;
        }
        *outAscent = (int)((13.0f * h) / 16.0f + 0.5f);
    } else {
        *outCellH = size;
        *outCellW = size;
        *outAscent = (int)((13.0f * size) / 16.0f + 0.5f);
    }
}

static void DrawGlyphCell(U3Bitmap *target, int x, int y, unsigned char ch,
                          int cellW, int cellH, int ascent, Boolean bold) {
    if (!gFontBitmap.pixels)
        return;
    if (ch < U3_GLYPH_FIRST || ch >= U3_GLYPH_FIRST + U3_GLYPH_COUNT)
        return;
    int cellX = (ch - U3_GLYPH_FIRST) * U3_GLYPH_CELL;
    int topY = y - ascent;
    for (int dy = 0; dy < cellH; ++dy) {
        int ty = topY + dy;
        if (ty < 0 || ty >= target->height)
            continue;
        int sy = (cellH == U3_GLYPH_CELL) ? dy : (int)((dy * U3_GLYPH_CELL + cellH / 2) / cellH);
        if (sy < 0) sy = 0;
        if (sy >= U3_GLYPH_CELL) sy = U3_GLYPH_CELL - 1;
        const uint8_t *srcRow = gFontBitmap.pixels + (size_t)sy * gFontBitmap.stride + (size_t)cellX * 4;
        for (int dx = 0; dx < cellW; ++dx) {
            int sx = (cellW == U3_GLYPH_CELL) ? dx : (int)((dx * U3_GLYPH_CELL + cellW / 2) / cellW);
            if (sx < 0) sx = 0;
            if (sx >= U3_GLYPH_CELL) sx = U3_GLYPH_CELL - 1;
            if (srcRow[sx * 4] <= 128)
                continue; /* background pixel in the glyph strip: leave destination alone */
            PutPixel(target, x + dx, ty, gForeground);
            if (bold)
                PutPixel(target, x + dx + 1, ty, gForeground);
        }
    }
}

void U3CocoaDrawPascalString(ConstStr255Param text) {
    if (!text || text[0] == 0)
        return;
    EnsureFontLoaded();
    U3Bitmap *target;
    int offX, offY;
    ResolveTarget(&target, &offX, &offY);
    int cellW, cellH, ascent;
    GetFontMetrics(&cellW, &cellH, &ascent);
    int x = gPenH - offX, y = gPenV - offY;
    for (int i = 1; i <= text[0]; ++i) {
        DrawGlyphCell(target, x, y, text[i], cellW, cellH, ascent, (Boolean)(gTextFace & bold));
        x += cellW;
    }
    gPenH = (short)(gPenH + U3CocoaTextWidth(text));
    if (!gSelectedBitmap)
        U3CocoaInvalidateMainSurface();
}

short U3CocoaTextWidth(ConstStr255Param text) {
    if (!text)
        return 0;
    int cellW, cellH, ascent;
    GetFontMetrics(&cellW, &cellH, &ascent);
    long width = (long)text[0] * cellW;
    return (short)(width > 32767 ? 32767 : width);
}

void U3CocoaDrawBytes(const void *textBuf, short firstByte, short byteCount) {
    if (!textBuf || byteCount <= 0)
        return;
    const unsigned char *bytes = (const unsigned char *)textBuf + (firstByte > 0 ? firstByte : 0);
    Str255 pstring;
    short length = byteCount < 255 ? byteCount : 255;
    pstring[0] = (unsigned char)length;
    memcpy(pstring + 1, bytes, (size_t)length);
    U3CocoaDrawPascalString(pstring);
}

/* ==========================================================================
 * Main bitmap lifecycle
 * ========================================================================== */

U3Bitmap *U3CocoaMainBitmap(void) {
    return gMainBitmap.pixels ? &gMainBitmap : NULL;
}

Boolean U3CocoaResizeMainBitmap(short width, short height) {
    if (width <= 0 || height <= 0)
        return false;
    if (gMainBitmap.width == width && gMainBitmap.height == height)
        return true;
    U3Bitmap replacement = {0};
    if (!U3BitmapAllocate(&replacement, width, height))
        return false;
    if (gMainBitmap.pixels) {
        U3BitmapRect whole = {0, 0, gMainBitmap.width, gMainBitmap.height};
        U3BitmapCopy(&replacement, whole, &gMainBitmap, whole);
    }
    U3BitmapDispose(&gMainBitmap);
    gMainBitmap = replacement;
    return true;
}

Boolean U3CocoaWriteMainBitmap(const char *path) {
    U3Bitmap *bitmap = U3CocoaMainBitmap();
    if (!path || !bitmap)
        return false;
    /* No PNG encoder is available in this environment without adding a new
     * dependency (no libpng/stb in the staged sysroot). SDL2 itself can
     * write BMP losslessly, so that's what this writes -- real pixel data,
     * honestly, just not in the container format the filename might
     * suggest. See the task report for this divergence. */
    SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(bitmap->pixels, bitmap->width, bitmap->height,
        32, (int)bitmap->stride, 0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0u);
    if (!surface)
        return false;
    Boolean ok = SDL_SaveBMP(surface, path) == 0;
    SDL_FreeSurface(surface);
    return ok;
}

void U3CocoaTextCheckpoint(const char *name) {
    const char *prefix = getenv("U3_COMMAND_TEXT_CHECK");
    if (!prefix)
        return;
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s-%s.bmp", prefix, name);
    if (length < 0 || (size_t)length >= sizeof(path) || !U3CocoaWriteMainBitmap(path))
        exit(EXIT_FAILURE);
    fprintf(stderr, "Command text: %s\n", name);
}

void U3CocoaInvalidateMainSurface(void) {
    /* U3CocoaPresentMainSurface always re-uploads the whole bitmap, so there
     * is no separate dirty-rect bookkeeping to do here. */
}

/* ==========================================================================
 * Image loading
 * ========================================================================== */

/* CFURLRef is just `const char *` on Linux (U3LegacyTypes.h). The Carbon/CF
 * bucket of symbols (ResourcesDirectoryURL, GraphicsDirectoryURL,
 * CFURLCreateCopyAppendingPathComponent -- not owned by this file) is not
 * implemented yet, so the exact string this function receives at runtime is
 * not yet pinned down. U3LinuxAssets indexes images by a manifest-relative
 * "source" key such as "Resources/Graphics/Standard-Tiles.png" or
 * "Images/Exodus.png" (verified against build/linux-game/assets/manifest.json).
 * This resolves whatever string arrives down to that form: used as-is if it
 * already matches, otherwise by taking the suffix starting at the last
 * occurrence of a known top-level asset directory. See the task report for
 * why this is a heuristic rather than a known-exact contract. */
static const char *ResolveAssetSource(const char *url) {
    if (!url || !*url)
        return NULL;
    static const char *const prefixes[] = {"Resources/Graphics/", "Images/", "Cursors/"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        const char *match = strstr(url, prefixes[i]);
        if (match)
            return match;
    }
    const char *res = strstr(url, "Resources/");
    if (res) {
        static char mapped[512];
        snprintf(mapped, sizeof(mapped), "Images/%s", res + 10);
        return mapped;
    }
    const char *slash = strrchr(url, '/');
    const char *filename = slash ? slash + 1 : url;
    if (filename[0]) {
        static char mapped[512];
        snprintf(mapped, sizeof(mapped), "Images/%s", filename);
        return mapped;
    }
    return url;
}

Boolean U3CocoaLoadImage(U3Bitmap *output, CFURLRef url, int width, int height,
                        int columns, int rows) {
    if (!output || !url || width <= 0 || height <= 0 || columns <= 0 || rows <= 0)
        return false;
    if (!gAssets)
        return false;
    const char *source = ResolveAssetSource(url);
    if (!source)
        return false;
    return U3LinuxAssetImage(gAssets, source, output, width, height, columns, rows);
}

/* ==========================================================================
 * SDL window / renderer / texture lifecycle
 * ========================================================================== */

static bool EnsureSDLVideo(void) {
    if (gSDLVideoReady)
        return true;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
        return false;
    gSDLVideoReady = true;
    return true;
}

/* Centered, aspect-preserving placement of the game bitmap inside a window of
 * winW x winH, with the leftover margin left as letterbox/pillarbox bars.
 *
 * Integer scaling is the default because it keeps the original pixel grid
 * exact, but it cannot honor a window smaller than the bitmap: the factor
 * would floor to zero. Rather than clamp to 1 and let the image overflow the
 * window -- which silently cropped the game when the player dragged the window
 * below 640x384 -- fall back to a fractional fit whenever no integer factor
 * fits, and whenever the player has asked for smooth scaling outright. Either
 * way the whole screen stays visible and correctly proportioned at every size
 * the compositor hands us, which is what makes live resizing usable.
 */
static void ComputePresentation(int winW, int winH, int bmpW, int bmpH, SDL_Rect *out) {
    if (bmpW <= 0 || bmpH <= 0 || winW <= 0 || winH <= 0) {
        out->x = out->y = 0;
        out->w = winW > 0 ? winW : 1;
        out->h = winH > 0 ? winH : 1;
        return;
    }
    int byWidth = winW / bmpW, byHeight = winH / bmpH;
    int factor = byWidth < byHeight ? byWidth : byHeight;
    int w, h;
    if (!gSmoothScaling && factor >= 1) {
        w = bmpW * factor;
        h = bmpH * factor;
    } else if ((long)winW * bmpH <= (long)winH * bmpW) {
        /* Width is the tighter axis: fill it and letterbox above and below. */
        w = winW;
        h = (int)(((long)winW * bmpH) / bmpW);
    } else {
        h = winH;
        w = (int)(((long)winH * bmpW) / bmpH);
    }
    out->x = (winW - w) / 2;
    out->y = (winH - h) / 2;
    out->w = w > 0 ? w : 1;
    out->h = h > 0 ? h : 1;
}

/* The destination rect the bitmap is currently presented into. */
static void CurrentPresentation(SDL_Rect *out) {
    int winW = gMainBitmap.width, winH = gMainBitmap.height;
    if (gWindow)
        SDL_GetWindowSize(gWindow, &winW, &winH);
    ComputePresentation(winW, winH, gMainBitmap.width, gMainBitmap.height, out);
}

/* Nearest-neighbor keeps whole-pixel scaling exact; linear only earns its
 * blur once the factor is fractional and the grid cannot be preserved anyway. */
static void ApplyTextureScaleMode(void) {
    if (gTexture)
        SDL_SetTextureScaleMode(gTexture, gSmoothScaling ? SDL_ScaleModeLinear
                                                         : SDL_ScaleModeNearest);
}

static bool EnsureTexture(int width, int height) {
    if (gTexture && gTextureWidth == width && gTextureHeight == height)
        return true;
    if (gTexture) {
        SDL_DestroyTexture(gTexture);
        gTexture = NULL;
    }
    gTexture = SDL_CreateTexture(gRenderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                 width, height);
    if (!gTexture)
        return false;
    ApplyTextureScaleMode();
    gTextureWidth = width;
    gTextureHeight = height;
    return true;
}

/* Seed the live presentation options from stored preferences. Done once, on
 * first surface creation, because the preferences file is not readable until
 * U3LinuxPlatform has resolved its directory -- and skipped entirely once a
 * setter has run, so a CLI flag is never clobbered by the stored value it was
 * meant to override. */
static void LoadPresentationOptions(void) {
    if (gPresentationOptionsLoaded)
        return;
    gPresentationOptionsLoaded = true;
    gSmoothScaling = U3PlatformGetBooleanPreference(U3PreferenceLinuxSmoothScaling);
    gScanlines = U3PlatformGetBooleanPreference(U3PreferenceLinuxScanlines);
    gFontScalePercent = ClampFontScale(U3PlatformGetIntegerPreference(U3PreferenceLinuxFontScale));
}

void U3LinuxVideoSetFontScalePercent(int percent) {
    gPresentationOptionsLoaded = true;
    gFontScalePercent = ClampFontScale(percent);
    U3PlatformSetIntegerPreference(U3PreferenceLinuxFontScale, gFontScalePercent);
    /* Text is drawn into the game's bitmap, not composited at present time, so
     * a new scale only shows up where the game redraws. Ask it to repaint
     * everything rather than leaving a half-rescaled screen behind. */
    if (gSurfaceCreated) {
        U3CocoaInvalidateMainSurface();
        U3CocoaPresentMainSurface();
    }
}

int U3LinuxVideoFontScalePercent(void) {
    LoadPresentationOptions();
    return gFontScalePercent;
}

void U3LinuxVideoSetSmoothScaling(bool smooth) {
    gPresentationOptionsLoaded = true;
    gSmoothScaling = smooth;
    U3PlatformSetBooleanPreference(U3PreferenceLinuxSmoothScaling, smooth);
    ApplyTextureScaleMode();
    if (gSurfaceCreated)
        U3CocoaPresentMainSurface();
}

bool U3LinuxVideoSmoothScaling(void) {
    LoadPresentationOptions();
    return gSmoothScaling;
}

void U3LinuxVideoSetScanlines(bool enabled) {
    gPresentationOptionsLoaded = true;
    gScanlines = enabled;
    U3PlatformSetBooleanPreference(U3PreferenceLinuxScanlines, enabled);
    if (gSurfaceCreated)
        U3CocoaPresentMainSurface();
}

bool U3LinuxVideoScanlines(void) {
    LoadPresentationOptions();
    return gScanlines;
}

void U3LinuxVideoTextCellSize(int *outWidth, int *outHeight, int *outAscent) {
    int cellW = 0, cellH = 0, ascent = 0;
    GetFontMetrics(&cellW, &cellH, &ascent);
    if (outWidth) *outWidth = cellW;
    if (outHeight) *outHeight = cellH;
    if (outAscent) *outAscent = ascent;
}

void U3LinuxVideoPresentationRectForTesting(int windowWidth, int windowHeight,
                                            int bitmapWidth, int bitmapHeight,
                                            int *outX, int *outY, int *outW, int *outH) {
    SDL_Rect rect;
    ComputePresentation(windowWidth, windowHeight, bitmapWidth, bitmapHeight, &rect);
    if (outX) *outX = rect.x;
    if (outY) *outY = rect.y;
    if (outW) *outW = rect.w;
    if (outH) *outH = rect.h;
}

/* Give the window the game's own icon so GNOME Shell, KDE's task manager and
 * Wayland docks show Exodus rather than a generic placeholder. The icon comes
 * from the asset bundle (Images/AppIcon.png, exported as straight RGBA8) and
 * must keep its alpha: compositing it onto black first, as U3LinuxAssetImage
 * does for game art, would put a black square behind the dragon.
 *
 * Purely cosmetic, so every failure path here is silent -- a missing icon must
 * not stop the game from opening a window. */
static void ApplyWindowIcon(void) {
    if (!gWindow || !gAssets)
        return;
    uint8_t *pixels = NULL;
    int width = 0, height = 0;
    if (!U3LinuxAssetRGBA(gAssets, "Images/AppIcon.png", &pixels, &width, &height))
        return;
    SDL_Surface *icon = SDL_CreateRGBSurfaceWithFormatFrom(pixels, width, height, 32,
                                                           width * 4, SDL_PIXELFORMAT_RGBA32);
    if (icon) {
        SDL_SetWindowIcon(gWindow, icon);
        SDL_FreeSurface(icon);
    }
    /* SDL_SetWindowIcon copies the pixels into the window manager's own
     * representation, so the source buffer is ours to release immediately. */
    free(pixels);
}

void *U3CocoaCreateMainSurface(short xposn, short yposn, short width, short height) {
    if (!U3CocoaResizeMainBitmap(width, height))
        return NULL;
    gHeadlessDiagnostic = U3CocoaIsHeadlessDiagnostic();
    LoadPresentationOptions();
    if (!EnsureSDLVideo())
        return NULL;
    if (!gWindow) {
        int winW = width * gScale;
        int winH = height * gScale;
        SDL_Rect usable;
        if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
            int maxW = (usable.w * 9) / 10;
            int maxH = (usable.h * 9) / 10;
            while ((winW > maxW || winH > maxH) && gScale > 1) {
                gScale--;
                winW = width * gScale;
                winH = height * gScale;
            }
            if (winW > maxW) winW = maxW;
            if (winH > maxH) winH = maxH;
        }
        int posX = xposn > 0 ? xposn : (int)SDL_WINDOWPOS_CENTERED;
        int posY = yposn > 0 ? yposn : (int)SDL_WINDOWPOS_CENTERED;
        Uint32 flags = SDL_WINDOW_RESIZABLE;
        if (gHeadlessDiagnostic)
            flags |= SDL_WINDOW_HIDDEN;
        gWindow = SDL_CreateWindow(gWindowTitle ? gWindowTitle : "Ultima III",
                                   posX, posY, winW, winH, flags);
        if (!gWindow)
            return NULL;
        ApplyWindowIcon();
        if (!gHeadlessDiagnostic)
            SDL_RaiseWindow(gWindow);
        gRenderer = SDL_CreateRenderer(gWindow, -1, SDL_RENDERER_PRESENTVSYNC);
        if (!gRenderer)
            gRenderer = SDL_CreateRenderer(gWindow, -1, 0);
        if (!gRenderer) {
            SDL_DestroyWindow(gWindow);
            gWindow = NULL;
            return NULL;
        }
        SDL_StartTextInput();
    } else {
        int winW = width * gScale;
        int winH = height * gScale;
        SDL_Rect usable;
        if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
            int maxW = (usable.w * 9) / 10;
            int maxH = (usable.h * 9) / 10;
            if (winW > maxW) winW = maxW;
            if (winH > maxH) winH = maxH;
        }
        SDL_SetWindowSize(gWindow, winW, winH);
    }
    if (!EnsureTexture(width, height))
        return NULL;
    gSurfaceCreated = true;
    U3CocoaPumpEvents();
    return &gSurfaceToken;
}

Boolean U3CocoaResizeMainSurface(short width, short height) {
    if (!U3CocoaResizeMainBitmap(width, height))
        return false;
    if (gWindow) {
        if (!gFullscreen) {
            int winW = width * gScale;
            int winH = height * gScale;
            SDL_Rect usable;
            if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
                int maxW = (usable.w * 9) / 10;
                int maxH = (usable.h * 9) / 10;
                if (winW > maxW) winW = maxW;
                if (winH > maxH) winH = maxH;
            }
            SDL_SetWindowSize(gWindow, winW, winH);
        }
        if (!EnsureTexture(width, height))
            return false;
    }
    U3CocoaInvalidateMainSurface();
    return true;
}

void U3CocoaSetMainSurfaceFullScreen(Boolean fullScreen) {
    gFullscreen = fullScreen;
    if (!gWindow)
        return;
    SDL_SetWindowFullscreen(gWindow, fullScreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

Boolean U3CocoaHasMainSurface(void) {
    return gSurfaceCreated;
}

/* Mirrors CocoaBridge.m's `return true`. The one caller (SetUpDisplayDialog
 * in UltimaNew.c) uses this to mean "the host already manages display mode
 * itself, skip the legacy Carbon full-screen-prompt dialog" -- true for the
 * SDL-backed host too, even though the dialogs this file *does* draw
 * (character creation, party selection) are bitmap-drawn, not OS-native. */
Boolean U3CocoaUsesNativeUI(void) {
    return true;
}

/* U3CocoaInstallMenus / U3CocoaUpdateMenuState: CocoaBridge.m builds a real
 * NSMenu (Sound/Music/Speech/etc. toggles, Quit, Save Game) here. SDL has no
 * menu-bar concept on Linux, so there is nothing truthful to install; these
 * are intentional no-ops rather than a faked menu. See the task report. */
void U3CocoaInstallMenus(void) {}
void U3CocoaUpdateMenuState(void) {}

/* ==========================================================================
 * Input
 * ========================================================================== */

static void PushPendingBack(Boolean isMouse, char key, short x, short y) {
    if (gPendingCount >= U3_PENDING_CAPACITY)
        return;
    int slot = (gPendingHead + gPendingCount) % U3_PENDING_CAPACITY;
    gPending[slot].isMouse = isMouse;
    gPending[slot].key = key;
    gPending[slot].mouseX = x;
    gPending[slot].mouseY = y;
    ++gPendingCount;
}

static void PushPendingFront(Boolean isMouse, char key, short x, short y) {
    if (gPendingCount >= U3_PENDING_CAPACITY)
        return;
    gPendingHead = (gPendingHead - 1 + U3_PENDING_CAPACITY) % U3_PENDING_CAPACITY;
    gPending[gPendingHead].isMouse = isMouse;
    gPending[gPendingHead].key = key;
    gPending[gPendingHead].mouseX = x;
    gPending[gPendingHead].mouseY = y;
    ++gPendingCount;
}

static Boolean PopPending(U3PendingInput *out) {
    if (gPendingCount == 0)
        return false;
    *out = gPending[gPendingHead];
    gPendingHead = (gPendingHead + 1) % U3_PENDING_CAPACITY;
    --gPendingCount;
    return true;
}

/* Virtual-key control codes, shared between real event handling and
 * U3CocoaKeyboardSelfTest. Printable characters arrive via SDL_TEXTINPUT
 * instead (FilterTextByte), not through this table -- SDL already resolves
 * shift/layout for us, unlike the raw HID keycodes CocoaBridge.m had to
 * decode by hand for AppKit. */
static char MapControlKey(SDL_Keycode sym) {
    switch (sym) {
        case SDLK_RETURN: case SDLK_KP_ENTER: return 13;
        case SDLK_BACKSPACE: return 8;
        case SDLK_ESCAPE: return 27;
        case SDLK_LEFT: return 28;
        case SDLK_RIGHT: return 29;
        case SDLK_UP: return 30;
        case SDLK_DOWN: return 31;
        case SDLK_TAB: return 9;
        default: return 0;
    }
}

static char FilterTextByte(unsigned char c) {
    return (c >= 32 && c < 127) ? (char)c : 0;
}

/* Inverse of the presentation mapping, so a click lands on the tile the player
 * actually pointed at under any scale factor, integer or not. */
static void WindowToBitmapPoint(int wx, int wy, short *outX, short *outY) {
    SDL_Rect dest;
    CurrentPresentation(&dest);
    int bx = (int)(((long)(wx - dest.x) * gMainBitmap.width) / dest.w);
    int by = (int)(((long)(wy - dest.y) * gMainBitmap.height) / dest.h);
    if (bx < 0) bx = 0;
    if (by < 0) by = 0;
    if (gMainBitmap.width > 0 && bx >= gMainBitmap.width) bx = gMainBitmap.width - 1;
    if (gMainBitmap.height > 0 && by >= gMainBitmap.height) by = gMainBitmap.height - 1;
    *outX = (short)bx;
    *outY = (short)by;
}

/* Window-manager-level display keys, handled here and deliberately not
 * forwarded to the game: F11 / Alt+Enter is the modern PC convention for
 * fullscreen, and neither it nor the presentation toggles corresponds to any
 * Ultima III command, so swallowing them cannot shadow a game key.
 *
 * Each one persists its new state, so a display choice made in-game survives a
 * restart the same way the CLI flags do.
 *
 * Returns true if the event was consumed as a display key.
 */
static bool HandleDisplayKey(const SDL_KeyboardEvent *key) {
    SDL_Keycode sym = key->keysym.sym;
    bool altEnter = (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) &&
                    (key->keysym.mod & KMOD_ALT) != 0;
    if (sym == SDLK_F11 || altEnter) {
        U3CocoaSetMainSurfaceFullScreen(!gFullscreen);
        U3PlatformSetBooleanPreference(U3PreferenceFullScreen, gFullscreen);
        return true;
    }
    if (sym == SDLK_F10) {
        U3LinuxVideoSetScanlines(!gScanlines);
        return true;
    }
    if (sym == SDLK_F8) {
        U3LinuxVideoSetSmoothScaling(!gSmoothScaling);
        return true;
    }
    /* Font size in 25-point steps, the granularity at which a 16px glyph cell
     * actually changes size (16 -> 20 -> 24). Smaller increments would round
     * back to the same cell and look like a dead key. */
    if (sym == SDLK_F6) {
        U3LinuxVideoSetFontScalePercent(gFontScalePercent - 25);
        return true;
    }
    if (sym == SDLK_F7) {
        U3LinuxVideoSetFontScalePercent(gFontScalePercent + 25);
        return true;
    }
    /* F1 only records the request; see gOverlayRequested. Swallowed either way,
     * so the game never receives it as a command. */
    if (sym == SDLK_F1) {
        if (gOverlayHandler && !gOverlayRunning)
            gOverlayRequested = true;
        return true;
    }
    return false;
}

static void HandleSDLEvent(const SDL_Event *event) {
    switch (event->type) {
        case SDL_QUIT:
            U3LinuxVideoShutdown();
            exit(0);
        case SDL_WINDOWEVENT:
            /* The game only presents when it redraws, which during a drag-resize
             * can be many seconds away (it blocks waiting for input). Re-present
             * on the spot so the letterbox bars and scale follow the pointer
             * instead of leaving a stretched or torn window behind. SIZE_CHANGED
             * covers both user resizes and fullscreen transitions; EXPOSED covers
             * the compositor asking for a repaint after an occlusion. */
            if (event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                event->window.event == SDL_WINDOWEVENT_RESIZED ||
                event->window.event == SDL_WINDOWEVENT_EXPOSED)
                U3CocoaPresentMainSurface();
            break;
        case SDL_KEYDOWN: {
            if (HandleDisplayKey(&event->key))
                break;
            char key = MapControlKey(event->key.keysym.sym);
            if (key)
                PushPendingBack(false, key, 0, 0);
            break;
        }
        case SDL_TEXTINPUT: {
            char key = FilterTextByte((unsigned char)event->text.text[0]);
            if (key)
                PushPendingBack(false, key, 0, 0);
            break;
        }
        case SDL_MOUSEMOTION: {
            short bx, by;
            WindowToBitmapPoint(event->motion.x, event->motion.y, &bx, &by);
            gMousePoint.h = bx;
            gMousePoint.v = by;
            break;
        }
        case SDL_MOUSEBUTTONDOWN: {
            short bx, by;
            WindowToBitmapPoint(event->button.x, event->button.y, &bx, &by);
            PushPendingBack(true, 0, bx, by);
            break;
        }
        default:
            break;
    }
}

void U3CocoaPumpEvents(void) {
    if (!gWindow)
        return;
    SDL_Event event;
    while (SDL_PollEvent(&event))
        HandleSDLEvent(&event);
}

/* Optional CRT-style scanline overlay.
 *
 * Drawn as one darkened band per *game* pixel row rather than as a fixed
 * striped texture, so the line spacing tracks the real destination height and
 * stays even at any scale factor -- a pre-baked texture sampled at 3x would
 * give a 1.5-on/1.5-off pattern that beats against the pixel grid.
 *
 * Skipped below 2x, where there is no room for a dark row between the bright
 * ones and the filter would only halve the overall brightness.
 */
static void DrawScanlines(const SDL_Rect *dest) {
    int rows = gMainBitmap.height;
    if (rows <= 0 || dest->h < rows * 2)
        return;
    int thickness = (dest->h / rows) / 2;
    if (thickness < 1)
        thickness = 1;
    SDL_SetRenderDrawBlendMode(gRenderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 96);
    for (int row = 0; row < rows; ++row) {
        int bottom = dest->y + (int)(((long)(row + 1) * dest->h) / rows);
        SDL_Rect line = {dest->x, bottom - thickness, dest->w, thickness};
        SDL_RenderFillRect(gRenderer, &line);
    }
    SDL_SetRenderDrawBlendMode(gRenderer, SDL_BLENDMODE_NONE);
}

/* Draw one frame into the renderer's back buffer without presenting it, so the
 * readback hook below can inspect exactly what the player would see. */
static bool RenderFrame(void) {
    if (!gRenderer || !gTexture || !gMainBitmap.pixels)
        return false;
    SDL_UpdateTexture(gTexture, NULL, gMainBitmap.pixels, (int)gMainBitmap.stride);
    SDL_Rect destination;
    CurrentPresentation(&destination);
    /* Clear every frame: the letterbox bars move when the window is resized,
     * so stale content outside the destination rect would otherwise persist. */
    SDL_SetRenderDrawColor(gRenderer, 0, 0, 0, 255);
    SDL_RenderClear(gRenderer);
    SDL_RenderCopy(gRenderer, gTexture, NULL, &destination);
    if (gScanlines)
        DrawScanlines(&destination);
    return true;
}

void U3CocoaPresentMainSurface(void) {
    if (!RenderFrame())
        return;
    SDL_RenderPresent(gRenderer);
}

bool U3LinuxVideoReadFrameForTesting(uint8_t *rgba, size_t capacity,
                                     int *outWidth, int *outHeight) {
    if (!gRenderer || !gTexture || !gMainBitmap.pixels)
        return false;
    SDL_Rect area;
    CurrentPresentation(&area);
    if (outWidth) *outWidth = area.w;
    if (outHeight) *outHeight = area.h;
    if (!rgba)
        return true; /* size query only */
    if (capacity < (size_t)area.w * (size_t)area.h * 4 || !RenderFrame())
        return false;
    return SDL_RenderReadPixels(gRenderer, &area, SDL_PIXELFORMAT_RGBA32,
                                rgba, area.w * 4) == 0;
}

void U3CocoaFlushInput(void) {
    U3CocoaPumpEvents();
    gPendingHead = gPendingCount = 0;
    gDiagKey = 0;
    gDiagMousePending = false;
    gDiagIndex = gDiagCount = 0;
}

void U3CocoaQueueDiagnosticKey(char key) {
    if (gHeadlessDiagnostic) {
        gDiagMousePending = false;
        gDiagKey = key;
        return;
    }
    PushPendingFront(false, key, 0, 0);
}

void U3CocoaQueueDiagnosticKeys(const char *keys) {
    size_t count = keys ? strlen(keys) : 0;
    if (!gHeadlessDiagnostic) {
        for (size_t i = 0; i < count; ++i)
            PushPendingBack(false, keys[i], 0, 0);
        return;
    }
    if (count > sizeof(gDiagKeys))
        count = sizeof(gDiagKeys);
    memcpy(gDiagKeys, keys ? keys : "", count);
    gDiagIndex = 0;
    gDiagCount = (unsigned int)count;
}

void U3CocoaQueueDiagnosticMouse(short x, short y) {
    gMousePoint.h = x;
    gMousePoint.v = y;
    if (gHeadlessDiagnostic) {
        gDiagKey = 0;
        gDiagMousePending = true;
        return;
    }
    PushPendingFront(true, 0, x, y);
}

void U3LinuxVideoSetOverlayHandler(U3LinuxVideoOverlayHandler handler) {
    gOverlayHandler = handler;
}

bool U3LinuxVideoOverlayIsOpen(void) {
    return gOverlayRunning;
}

void U3LinuxVideoRequestOverlay(void) {
    if (gOverlayHandler && !gOverlayRunning)
        gOverlayRequested = true;
}

/* Run a pending overlay request, if this is a safe moment for it.
 *
 * Called from U3CocoaPollKeyMouse, which is where the game has finished a turn
 * and is waiting for the player -- the one point at which taking over the
 * screen and the keyboard cannot interrupt a half-drawn frame or a command
 * mid-prompt. The guard keeps the overlay's own input polling from re-entering
 * it. */
static void RunPendingOverlay(void) {
    if (!gOverlayRequested || gOverlayRunning || !gOverlayHandler)
        return;
    gOverlayRequested = false;
    gOverlayRunning = true;
    gOverlayHandler();
    gOverlayRunning = false;
}

Boolean U3CocoaPollKeyMouse(Boolean includeMouse, long timeoutTicks, char *outKey,
                            Boolean *outMouse) {
    if (outMouse)
        *outMouse = false;
    if (outKey)
        *outKey = 0;
    RunPendingOverlay();
    if (gHeadlessDiagnostic) {
        if (gDiagMousePending) {
            gDiagMousePending = false;
            if (outMouse)
                *outMouse = true;
            return includeMouse;
        }
        char key = gDiagKey ? gDiagKey : (gDiagIndex < gDiagCount ? (char)gDiagKeys[gDiagIndex++] : 0);
        gDiagKey = 0;
        if (outKey)
            *outKey = key;
        return key != 0;
    }
    if (!gWindow)
        return false;
    if (gPendingCount == 0) {
        SDL_Event event;
        Boolean haveEvent;
        if (timeoutTicks > 0) {
            int timeoutMs = (int)((double)timeoutTicks * 1000.0 / 60.0);
            haveEvent = SDL_WaitEventTimeout(&event, timeoutMs) ? true : false;
        } else {
            haveEvent = SDL_PollEvent(&event) ? true : false;
        }
        if (haveEvent)
            HandleSDLEvent(&event);
        while (SDL_PollEvent(&event))
            HandleSDLEvent(&event);
    }
    U3PendingInput input;
    if (!PopPending(&input))
        return false;
    if (input.isMouse) {
        gMousePoint.h = input.mouseX;
        gMousePoint.v = input.mouseY;
        if (outMouse)
            *outMouse = true;
        return includeMouse;
    }
    if (outKey)
        *outKey = input.key;
    return true;
}

void U3CocoaGetMousePoint(Point *point) {
    if (point)
        *point = gMousePoint;
}

Boolean U3CocoaKeyboardSelfTest(void) {
    struct { SDL_Keycode sym; char expected; } controlCases[] = {
        {SDLK_RETURN, 13}, {SDLK_KP_ENTER, 13}, {SDLK_BACKSPACE, 8}, {SDLK_ESCAPE, 27},
        {SDLK_LEFT, 28}, {SDLK_RIGHT, 29}, {SDLK_UP, 30}, {SDLK_DOWN, 31}, {SDLK_TAB, 9},
        {SDLK_F1, 0}, {SDLK_a, 0} /* letters are not in the control table, see FilterTextByte */
    };
    for (size_t i = 0; i < sizeof(controlCases) / sizeof(controlCases[0]); ++i)
        if (MapControlKey(controlCases[i].sym) != controlCases[i].expected)
            return false;
    if (FilterTextByte('A') != 'A' || FilterTextByte('0') != '0')
        return false;
    if (FilterTextByte(1) != 0 || FilterTextByte(127) != 0 || FilterTextByte(27) != 0)
        return false;
    return true;
}

/* ==========================================================================
 * In-bitmap dialogs: character creation and party selection
 *
 * Neither has a native-UI equivalent under SDL, and the task explicitly
 * disallows stubbing them out. Both run a small synchronous modal loop that
 * draws its own UI into the main bitmap with the primitives above and reads
 * keys back through U3CocoaPollKeyMouse -- the same function the rest of
 * the game uses, in both its real-SDL and headless-diagnostic forms, so
 * these dialogs are fully drivable by U3CocoaQueueDiagnosticKeys() under
 * test (see tests/linux-video-test.c) and by the real keyboard in play.
 * ========================================================================== */

static bool SnapshotMainBitmap(U3Bitmap *snapshot) {
    if (!gMainBitmap.pixels)
        return false;
    if (!U3BitmapAllocate(snapshot, gMainBitmap.width, gMainBitmap.height))
        return false;
    U3BitmapRect whole = {0, 0, gMainBitmap.width, gMainBitmap.height};
    return U3BitmapCopy(snapshot, whole, &gMainBitmap, whole);
}

static void RestoreMainBitmap(U3Bitmap *snapshot) {
    if (snapshot->pixels && gMainBitmap.pixels) {
        U3BitmapRect whole = {0, 0, gMainBitmap.width, gMainBitmap.height};
        U3BitmapCopy(&gMainBitmap, whole, snapshot, whole);
    }
    U3BitmapDispose(snapshot);
    U3CocoaInvalidateMainSurface();
    U3CocoaPresentMainSurface();
}

static void DialogText(short x, short y, const char *ascii) {
    Str255 pstring;
    size_t length = strlen(ascii);
    if (length > 255) length = 255;
    pstring[0] = (unsigned char)length;
    memcpy(pstring + 1, ascii, length);
    U3CocoaMoveTo(x, y);
    U3CocoaDrawPascalString(pstring);
}

/* Blocks for exactly one logical keypress, using the same input path the
 * rest of the game uses. In headless-diagnostic mode, running out of
 * scripted keys is treated as an implicit Escape/cancel rather than an
 * infinite spin, since the Mac original's diagnostic branch never blocks
 * either. Mouse events reaching a keyboard-only dialog are swallowed. */
static char ModalReadKey(void) {
    for (;;) {
        char key = 0;
        Boolean isMouse = false;
        Boolean got = U3CocoaPollKeyMouse(true, gHeadlessDiagnostic ? 0 : 6, &key, &isMouse);
        if (gHeadlessDiagnostic) {
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

Boolean U3CocoaChooseParty(const unsigned char names[20][16], const Boolean available[20],
                           short selection[4]) {
    if (!names || !available || !selection)
        return false;
    Boolean any = false;
    for (int i = 0; i < 20; ++i)
        any = any || available[i];
    U3Bitmap snapshot = {0};
    Boolean haveSnapshot = SnapshotMainBitmap(&snapshot);
    U3Bitmap *savedSelected = gSelectedBitmap;
    int savedOriginX = gSelectedOriginX, savedOriginY = gSelectedOriginY;
    uint8_t savedFg[3], savedBg[3];
    memcpy(savedFg, gForeground, 3);
    memcpy(savedBg, gBackground, 3);
    U3CocoaSelectBitmap(NULL, 0, 0);

    for (int i = 0; i < 4; ++i)
        selection[i] = 0;
    Boolean result = false;

    if (!any) {
        U3CocoaSetBackgroundQuickDrawColor(blackColor);
        U3CocoaEraseRect(0, 0, (short)gMainBitmap.width, (short)gMainBitmap.height);
        U3CocoaSetForegroundQuickDrawColor(whiteColor);
        DialogText(8, 8, "No available characters.");
        DialogText(8, 8 + U3_GLYPH_CELL, "Create one first. Press any key.");
        U3CocoaInvalidateMainSurface();
        U3CocoaPresentMainSurface();
        ModalReadKey();
        goto cleanup;
    }

    {
        int cursor = 0;
        int candidateCount = 0;
        int candidateIndex[20];
        for (int i = 0; i < 20; ++i)
            if (available[i] && names[i][0] > 0)
                candidateIndex[candidateCount++] = i;
        if (candidateCount == 0)
            goto cleanup;

        for (;;) {
            U3CocoaSetBackgroundQuickDrawColor(blackColor);
            U3CocoaEraseRect(0, 0, (short)gMainBitmap.width, (short)gMainBitmap.height);
            U3CocoaSetForegroundQuickDrawColor(whiteColor);
            DialogText(8, 8, "Form a Party - Up/Down, Enter to toggle, F to form, Esc to cancel");
            for (int row = 0; row < candidateCount; ++row) {
                int who = candidateIndex[row];
                char line[64];
                int slotNumber = 0;
                for (int s = 0; s < 4; ++s)
                    if (selection[s] == who + 1)
                        slotNumber = s + 1;
                unsigned char nameLength = names[who][0] > 15 ? 15 : names[who][0];
                char nameBuffer[16];
                memcpy(nameBuffer, names[who] + 1, nameLength);
                nameBuffer[nameLength] = 0;
                snprintf(line, sizeof(line), "%s%2d. %s", slotNumber ? "*" : " ", who + 1, nameBuffer);
                short y = (short)(8 + (row + 2) * U3_GLYPH_CELL);
                if (row == cursor) {
                    U3CocoaSetForegroundQuickDrawColor(whiteColor);
                    U3CocoaPaintRect(8, y, (short)(8 + 20 * U3_GLYPH_CELL), (short)(y + U3_GLYPH_CELL));
                    U3CocoaSetForegroundQuickDrawColor(blackColor);
                } else {
                    U3CocoaSetForegroundQuickDrawColor(whiteColor);
                }
                DialogText(8, y, line);
            }
            U3CocoaSetForegroundQuickDrawColor(whiteColor);
            U3CocoaInvalidateMainSurface();
            U3CocoaPresentMainSurface();

            char key = ModalReadKey();
            if (key == 27) { result = false; goto cleanup; }
            if (key == 30) cursor = (cursor - 1 + candidateCount) % candidateCount;
            else if (key == 31) cursor = (cursor + 1) % candidateCount;
            else if (key == 13) {
                int who = candidateIndex[cursor] + 1;
                int existing = -1;
                for (int s = 0; s < 4; ++s)
                    if (selection[s] == who) existing = s;
                if (existing >= 0) {
                    selection[existing] = 0;
                } else {
                    for (int s = 0; s < 4; ++s)
                        if (selection[s] == 0) { selection[s] = (short)who; break; }
                }
            } else if (key == 'f' || key == 'F') {
                Boolean haveAny = false;
                for (int s = 0; s < 4; ++s)
                    haveAny = haveAny || selection[s] != 0;
                if (haveAny) { result = true; goto cleanup; }
            }
        }
    }

cleanup:
    memcpy(gForeground, savedFg, 3);
    memcpy(gBackground, savedBg, 3);
    gSelectedBitmap = savedSelected;
    gSelectedOriginX = savedOriginX;
    gSelectedOriginY = savedOriginY;
    if (haveSnapshot)
        RestoreMainBitmap(&snapshot);
    if (!result)
        for (int i = 0; i < 4; ++i)
            selection[i] = 0;
    return result;
}

static const char kRaceCodes[] = "HEDBF";
static const char *const kRaceNames[] = {"Human", "Elf", "Dwarf", "Bobbit", "Fuzzy"};
static const char kClassCodes[] = "FCWTPBLIDAR";
static const char *const kClassNames[] = {"Fighter", "Cleric", "Wizard", "Thief", "Paladin",
    "Barbarian", "Lark", "Illusionist", "Druid", "Alchemist", "Ranger"};
static const char kSexCodes[] = "FMO";
static const char *const kSexNames[] = {"Female", "Male", "Other"};

static int CodeIndex(const char *codes, char value) {
    const char *at = strchr(codes, value);
    return at ? (int)(at - codes) : 0;
}

Boolean U3CocoaCreateCharacter(const Boolean available[20], short *outSlot, U3CharacterDraft *draft) {
    if (!available || !outSlot || !draft)
        return false;
    int slot = -1;
    for (int i = 0; i < 20; ++i)
        if (available[i]) { slot = i + 1; break; }
    if (slot < 0)
        return false;

    U3Bitmap snapshot = {0};
    Boolean haveSnapshot = SnapshotMainBitmap(&snapshot);
    U3Bitmap *savedSelected = gSelectedBitmap;
    int savedOriginX = gSelectedOriginX, savedOriginY = gSelectedOriginY;
    uint8_t savedFg[3], savedBg[3];
    memcpy(savedFg, gForeground, 3);
    memcpy(savedBg, gBackground, 3);
    U3CocoaSelectBitmap(NULL, 0, 0);

    char name[13];
    size_t nameLength = 0;
    memset(name, 0, sizeof(name));
    int raceIndex = CodeIndex(kRaceCodes, draft->race ? draft->race : 'H');
    int classIndex = CodeIndex(kClassCodes, draft->characterClass ? draft->characterClass : 'F');
    int sexIndex = CodeIndex(kSexCodes, draft->sex ? draft->sex : 'M');
    uint8_t attributes[4];
    for (int i = 0; i < 4; ++i)
        attributes[i] = draft->attributes[i] ? draft->attributes[i] : 10;

    enum { kFieldName, kFieldRace, kFieldClass, kFieldSex, kFieldStr, kFieldDex, kFieldInt,
           kFieldWis, kFieldCount };
    int field = kFieldName;
    Boolean result = false;

    for (;;) {
        int total = attributes[0] + attributes[1] + attributes[2] + attributes[3];
        U3CocoaSetBackgroundQuickDrawColor(blackColor);
        U3CocoaEraseRect(0, 0, (short)gMainBitmap.width, (short)gMainBitmap.height);
        U3CocoaSetForegroundQuickDrawColor(whiteColor);
        char line[80];
        snprintf(line, sizeof(line), "Create Character - slot %d", slot);
        DialogText(8, 8, line);
        DialogText(8, 8 + U3_GLYPH_CELL, "Tab/Up/Down: field  Left/Right: change  Enter: create  Esc: cancel");

        const char *fieldLabel[kFieldCount] = {"Name", "Race", "Class", "Sex",
            "Strength", "Dexterity", "Intelligence", "Wisdom"};
        for (int f = 0; f < kFieldCount; ++f) {
            short y = (short)(8 + (f + 3) * U3_GLYPH_CELL);
            U3CocoaSetForegroundQuickDrawColor(f == field ? yellowColor : whiteColor);
            char valueText[40];
            if (f == kFieldName) snprintf(valueText, sizeof(valueText), "%s: %s", fieldLabel[f], name);
            else if (f == kFieldRace) snprintf(valueText, sizeof(valueText), "%s: %s", fieldLabel[f], kRaceNames[raceIndex]);
            else if (f == kFieldClass) snprintf(valueText, sizeof(valueText), "%s: %s", fieldLabel[f], kClassNames[classIndex]);
            else if (f == kFieldSex) snprintf(valueText, sizeof(valueText), "%s: %s", fieldLabel[f], kSexNames[sexIndex]);
            else snprintf(valueText, sizeof(valueText), "%s: %d", fieldLabel[f], attributes[f - kFieldStr]);
            DialogText(8, y, valueText);
        }
        U3CocoaSetForegroundQuickDrawColor(whiteColor);
        snprintf(line, sizeof(line), "Points remaining: %d", 50 - total);
        DialogText(8, (short)(8 + (kFieldCount + 4) * U3_GLYPH_CELL), line);
        U3CocoaInvalidateMainSurface();
        U3CocoaPresentMainSurface();

        char key = ModalReadKey();
        if (key == 27) { result = false; break; }
        if (key == 9 || key == 31) field = (field + 1) % kFieldCount;
        else if (key == 30) field = (field - 1 + kFieldCount) % kFieldCount;
        else if (key == 13) {
            if (nameLength == 0)
                continue;
            result = true;
            break;
        } else if (field == kFieldName) {
            if (key == 8) { if (nameLength > 0) name[--nameLength] = 0; }
            else if (key >= 32 && key < 127 && nameLength < 12) { name[nameLength++] = key; name[nameLength] = 0; }
        } else if (field == kFieldRace && (key == 28 || key == 29)) {
            int count = (int)(sizeof(kRaceCodes) - 1);
            raceIndex = (raceIndex + (key == 29 ? 1 : count - 1)) % count;
        } else if (field == kFieldClass && (key == 28 || key == 29)) {
            int count = (int)(sizeof(kClassCodes) - 1);
            classIndex = (classIndex + (key == 29 ? 1 : count - 1)) % count;
        } else if (field == kFieldSex && (key == 28 || key == 29)) {
            int count = (int)(sizeof(kSexCodes) - 1);
            sexIndex = (sexIndex + (key == 29 ? 1 : count - 1)) % count;
        } else if (field >= kFieldStr && (key == 28 || key == 29)) {
            int i = field - kFieldStr;
            int delta = key == 29 ? 1 : -1;
            int next = attributes[i] + delta;
            int nextTotal = total - attributes[i] + next;
            if (next >= 5 && next <= 25 && nextTotal <= 50)
                attributes[i] = (uint8_t)next;
        }
    }

    if (result) {
        memset(draft, 0, sizeof(*draft));
        memcpy(draft->name, name, nameLength);
        memcpy(draft->attributes, attributes, 4);
        draft->race = kRaceCodes[raceIndex];
        draft->characterClass = kClassCodes[classIndex];
        draft->sex = kSexCodes[sexIndex];
        /* Belt-and-suspenders: the field-entry rules above already enforce
         * every constraint U3ValidateCharacterDraft checks, but call the
         * authoritative validator (declared in U3Types.h, defined in
         * UltimaMain.c) as the final gate rather than trusting that this
         * UI's bookkeeping can never drift from it. */
        result = U3ValidateCharacterDraft(draft);
        if (result)
            *outSlot = (short)slot;
    }

    memcpy(gForeground, savedFg, 3);
    memcpy(gBackground, savedBg, 3);
    gSelectedBitmap = savedSelected;
    gSelectedOriginX = savedOriginX;
    gSelectedOriginY = savedOriginY;
    if (haveSnapshot)
        RestoreMainBitmap(&snapshot);
    return result;
}
