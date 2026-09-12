/* SDL2-backed implementation of the CocoaBridge video/input surface.
 *
 * This header is deliberately small: the 43 U3Cocoa* entry points the game
 * core actually calls are declared once, in Sources/CocoaBridge.h, and this
 * file implements them. The only things owned here are the pieces CocoaBridge.h
 * has no way to express: where image assets come from, and cosmetic window
 * setup, both of which the original Mac build got from global Cocoa/bundle
 * state that has no Linux equivalent.
 */
#pragma once

#include "U3LinuxAssets.h"

/* Must be called before U3CocoaCreateMainSurface(). Neither argument is
 * retained beyond the call except `assets`, which the caller continues to
 * own (U3LinuxVideo never opens or closes an asset context itself). Passing
 * assets == NULL is legal: U3CocoaLoadImage will then honestly fail instead
 * of crashing, and on-screen text/character-creation/party UI still work
 * because they draw with the built-in bitmap font, not `assets`.
 *
 * windowTitle is copied; NULL keeps whatever title is already configured
 * (or the built-in default on first call). scale is the integer upscale
 * factor applied when presenting the software bitmap to the window; values
 * less than 1 are clamped to 1.
 */
void U3LinuxVideoConfigure(U3LinuxAssets *assets, const char *windowTitle, int scale);

/* Tears down any SDL window/renderer/texture created by U3CocoaCreateMainSurface
 * and frees the main and selected-bitmap state. Safe to call multiple times
 * and safe to call even if a surface was never created. Does not call
 * SDL_Quit() for subsystems this file did not initialize itself beyond
 * SDL_INIT_VIDEO, and does not affect the U3LinuxAssets context passed to
 * U3LinuxVideoConfigure (still owned by the caller).
 */
void U3LinuxVideoShutdown(void);

/* ---------------------------------------------------------------------
 * Presentation options
 *
 * How the software bitmap is scaled into the window. Both default to the
 * stored preference (U3PreferenceLinuxSmoothScaling / ...Scanlines, see
 * U3LinuxPlatform.h) the first time a surface is created; calling a setter
 * overrides that, takes effect on the next present, and persists the new
 * value so a choice made at the command line or in-game survives a restart.
 *
 * Safe to call before a window exists -- the state is applied when one is
 * created -- which is what lets main.c honor its CLI flags.
 * ------------------------------------------------------------------- */

/* false: scale by whole pixels with nearest-neighbor sampling, keeping the
 * original pixel grid exact. true: fit the window exactly, preserving aspect
 * ratio, with linear sampling for the fractional factors that implies.
 *
 * Note that whole-pixel scaling cannot honor a window smaller than the game
 * bitmap; below that size the presentation falls back to an aspect-preserving
 * fit either way, so the whole screen stays visible. */
void U3LinuxVideoSetSmoothScaling(bool smooth);
bool U3LinuxVideoSmoothScaling(void);

/* Overlay a CRT-style scanline filter. Has no visible effect below a 2x
 * factor, where there is no room for a dark row between the bright ones. */
void U3LinuxVideoSetScanlines(bool enabled);
bool U3LinuxVideoScanlines(void);

/* Test hook: the centered, aspect-preserving destination rectangle, in window
 * pixels, that a bitmapWidth x bitmapHeight bitmap is presented into for a
 * window of windowWidth x windowHeight under the current scaling mode.
 *
 * Exposed so the letterboxing, whole-pixel, and below-1x downscale behavior
 * can be pinned without a real resizable window, which no headless SDL driver
 * provides. */
void U3LinuxVideoPresentationRectForTesting(int windowWidth, int windowHeight,
                                            int bitmapWidth, int bitmapHeight,
                                            int *outX, int *outY, int *outW, int *outH);

/* Test hook: draw one frame exactly as U3CocoaPresentMainSurface would -- the
 * scaled bitmap plus the scanline overlay if enabled -- and read back the
 * rectangle the bitmap was drawn into, rather than presenting it.
 *
 * Lets the presentation be checked pixel by pixel under a headless driver,
 * which is the only way to verify the overlay actually darkens the rows it
 * should: the game's own screenshots capture the source bitmap, before any of
 * this scaling and filtering happens.
 *
 * The read-back region excludes the letterbox bars, and its reported size
 * gives the scale factor actually in play -- which the caller cannot assume,
 * since the window is clamped to the display's usable bounds. Call with
 * rgba == NULL to query that size, then again with a buffer of at least
 * width*height*4 bytes, written as RGBA8 rows of width*4 bytes.
 *
 * Returns false if no surface exists, the buffer is too small, or the renderer
 * cannot read back. */
bool U3LinuxVideoReadFrameForTesting(uint8_t *rgba, size_t capacity,
                                     int *outWidth, int *outHeight);

/* Percentage applied to every font size the game requests. Clamped to 50..100
 * and persisted like the options above.
 *
 * 100 is the maximum by necessity, not caution: the shared text engine
 * (Sources/UltimaText.c) lays lines out one tile apart while asking for a font
 * size of exactly one tile, so at 100% a glyph cell already fills its line and
 * anything larger renders as collided, unreadable text. For *larger* text the
 * control is the tile size -- the game's own double-size mode, which scales
 * art, text and line pitch together.
 *
 * Reducing the scale is well behaved: the game measures with U3CocoaTextWidth
 * to centre, right-align and wrap, so those all follow automatically and more
 * words fit per line.
 *
 * Only the modern appearance is affected. Classic appearance blits text from a
 * pre-rendered font sheet one tile per character (the UPrint path in
 * UltimaText.c) and never reaches this file's glyph rasterizer.
 *
 * Takes effect where the game next redraws; the setter asks for a full
 * repaint so a change does not leave half the screen at the old size. */
void U3LinuxVideoSetFontScalePercent(int percent);
int U3LinuxVideoFontScalePercent(void);

/* ---------------------------------------------------------------------
 * Settings overlay seam
 *
 * The in-game settings overlay is not implemented here: it also reads and
 * applies audio preferences, and this layer deliberately knows nothing about
 * audio. It registers itself instead, the same way U3LinuxPlatform.h takes an
 * input source, so the dependency stays one-way (settings -> video, never
 * back).
 *
 * F1 requests the overlay. The handler is not called from the key handler
 * though: it runs from inside U3CocoaPollKeyMouse, at the point where the game
 * is idle waiting for the player. That is both the only safe moment to take
 * over the screen -- no half-drawn frame, no command mid-prompt -- and the only
 * way to avoid re-entering the SDL event pump, since the key handler is itself
 * called from within the poll and the overlay polls for its own input.
 * ------------------------------------------------------------------- */
typedef void (*U3LinuxVideoOverlayHandler)(void);

/* The overlays this layer knows how to open, and the key that asks for each.
 * They share one deferral mechanism rather than having one apiece, so the
 * "run it at a safe point, never re-entrantly" rule is written once. */
typedef enum U3LinuxVideoOverlayKind {
    U3LinuxVideoOverlaySettings = 0, /* F1 */
    U3LinuxVideoOverlayReference,    /* F2 */
    U3LinuxVideoOverlayKindCount
} U3LinuxVideoOverlayKind;

/* Installs a handler, or NULL to disable that overlay's key. Call once at
 * startup. With no handler the key is still swallowed rather than reaching the
 * game as a command. */
void U3LinuxVideoSetOverlayHandler(U3LinuxVideoOverlayKind kind,
                                   U3LinuxVideoOverlayHandler handler);

/* Requests an overlay as if its key had been pressed; it opens at the next safe
 * point. Lets other input sources (a gamepad button, a menu item) open one
 * without duplicating the deferral rules above. */
void U3LinuxVideoRequestOverlay(U3LinuxVideoOverlayKind kind);

/* True while any overlay handler is running. An overlay draws into the game's
 * own bitmap, so anything that would otherwise repaint the screen underneath
 * it can use this to hold off. */
bool U3LinuxVideoOverlayIsOpen(void);

/* Convenience for the reference card, which registers itself the same way. */
void U3LinuxVideoSetReferenceHandler(U3LinuxVideoOverlayHandler handler);

/* Cell width, cell height and baseline ascent, in pixels, that the built-in
 * bitmap font currently rasterizes at -- i.e. after both the size the game last
 * asked for via TextSize() and the player's font scale.
 *
 * Dialogs drawn into the game bitmap need this to lay themselves out. Without
 * it they can only assume a 16px cell, which is wrong whenever the font scale
 * is not 100% or the caller inherited a different TextSize, and an assumed cell
 * that is too small silently overflows the screen edge. */
void U3LinuxVideoTextCellSize(int *outWidth, int *outHeight, int *outAscent);
