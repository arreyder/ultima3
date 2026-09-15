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
