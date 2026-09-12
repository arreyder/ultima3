// Linux definitions for the Carbon/CoreFoundation/menu/dialog/cursor surface
// declared (not defined) by U3LegacyTypes.h, U3LegacyDrawing.h, CocoaBridge.h,
// PrefsDialog.h, UltimaAppleEvents.h and UltimaSound.h. See U3LinuxCarbon.c for
// the ownership rule used for the CFStringRef/CFURLRef/CFArrayRef shims.
#pragma once

#include "U3LegacyTypes.h" // for Rect
#include <stddef.h>

// CarbonShunts.c's LWGetScreenRect forward-declares this itself (it is not
// declared in any shared header); declared here too so other callers,
// including tests, do not need to repeat that `extern`.
void U3LinuxScreenBounds(Rect *rect);

// Configures the real, on-disk directory backing GraphicsDirectoryURL(),
// ResourcesDirectoryURL(), CopyGraphicsDirectoryItems() and the asset context
// behind StringsArray()/GetPascalStringFromArrayByIndex(). Safe to call more
// than once (each call reopens the asset context and drops cached directory
// strings and string tables so they are rebuilt against the new directory);
// this is how tests point the module at a fixture directory. Pass NULL (or
// an empty string) to reset to the default, "build/linux-game/assets"
// relative to the current working directory.
//
// If this is never called, the default is used lazily on first access.
void U3LinuxCarbonConfigure(const char *assetsDirectory);

// Diagnostic hook for tests: the number of CFRetain/Create/Copy-owned objects
// (strings and arrays) that have not yet been balanced by a matching
// CFRelease. Not part of the legacy Carbon surface.
size_t U3LinuxCarbonLiveObjectCount(void);
