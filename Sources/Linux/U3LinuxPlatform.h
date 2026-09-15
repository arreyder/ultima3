//
//  U3LinuxPlatform.h
//  Ultima3 (Linux port)
//
//  Linux implementation of the host platform boundary declared in
//  Sources/U3Platform.h: timing, random numbers, preferences, and
//  keyboard/mouse input polling.
//
//  SCOPE NOTE: this file owns only U3Platform.h's symbols. It does not
//  own, and must never call into, the SDL window/event pump
//  (Sources/Linux/U3LinuxVideo.{c,h}), which is a separate, concurrently
//  written task and may not exist yet. The input-source abstraction below
//  is the seam between the two: U3LinuxPlatform.c polls whatever source is
//  currently installed, and the video task wires a real SDL-backed source
//  in once it has a window. Until then (and for every standalone test of
//  this file) a built-in default source is active that honestly reports
//  "no input available" rather than ever synthesizing a keypress.
//

#ifndef U3LinuxPlatform_h
#define U3LinuxPlatform_h

#include "U3Platform.h"
#include "U3Types.h"
#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------
 * Input source abstraction
 *
 * A small struct of callbacks rather than a registration function per
 * primitive, so the video task can hand over one value at startup (and
 * swap it again later, e.g. for a headless/scripted-input test double).
 * Any field may be NULL; U3LinuxPlatform.c checks before calling through.
 *
 * Contract for every callback: "nothing happened" is reported honestly
 * (false / untouched outputs), never as a synthesized event.
 * ------------------------------------------------------------------- */
typedef struct U3LinuxInputSource {
    /* Poll for one key or mouse-click event, waiting up to waitMs
     * milliseconds for one to arrive (0 = do not block at all).
     *
     * Return false if nothing arrived within waitMs; *outKey and
     * *outIsMouse must be left untouched in that case.
     *
     * Return true if an event arrived: set *outKey to the raw key code
     * (0 if the event was a mouse click with no associated character)
     * and *outIsMouse to whether the event came from the mouse.
     */
    bool (*pollKeyMouse)(void *userData, uint32_t waitMs, uint8_t *outKey, bool *outIsMouse);

    /* Report the current mouse position in the game's own pixel
     * coordinate space (the space U3Bitmap/mainPort drawing uses).
     * Return false if no pointer position is available right now. */
    bool (*getMousePoint)(void *userData, int16_t *outX, int16_t *outY);

    /* Discard any buffered key/mouse events
     * (U3PlatformFlushInputEvents / U3PlatformFlushAllEvents). */
    void (*flush)(void *userData);

    /* Hide the cursor until the next mouse movement
     * (U3PlatformObscureCursor). */
    void (*obscureCursor)(void *userData);

    /* Opaque value passed back to every callback above. May be NULL. */
    void *userData;
} U3LinuxInputSource;

/* Installs `source` as the active input source, copying its contents (the
 * pointer itself need not stay valid afterwards). Passing NULL restores
 * the built-in default described above.
 *
 * Not synchronized for concurrent use: call this once, before the game
 * loop starts, from a single thread (normally whatever sets up the SDL
 * window).
 */
void U3LinuxPlatformSetInputSource(const U3LinuxInputSource *source);

/* Returns the currently installed source. Never NULL. */
const U3LinuxInputSource *U3LinuxPlatformGetInputSource(void);

/* ---------------------------------------------------------------------
 * Preferences: test / integration helpers
 *
 * These are Linux-only additions, not part of the U3Platform.h contract.
 * ------------------------------------------------------------------- */

/* Directory preferences are stored under: $U3_SAVE_DIRECTORY if set (so
 * tests and isolated runs never touch a real home directory), else
 * $XDG_CONFIG_HOME/ultima3, else ~/.config/ultima3. The file itself is
 * "preferences.conf" inside that directory. Returned string is owned
 * internally and valid only until the next call to this function. */
const char *U3LinuxPlatformPreferencesDirectory(void);

/* Drops the in-memory preferences cache so the next Get/Has call re-reads
 * the file from disk. Only needed by tests that modify the file out from
 * under this process (e.g. to simulate a truncated/corrupt write). */
void U3LinuxPlatformResetPreferencesCacheForTesting(void);

/* U3Platform.h declares only getters (Copy*StringPreference) for string
 * preferences, no setter -- on the reference mac implementation string
 * prefs are written through a different path this boundary does not
 * expose. This is a Linux-only test hook that exercises the same storage
 * string preferences otherwise use, so the round trip through
 * U3PlatformCopyPascalStringPreference / U3PlatformCopyUTF8StringPreference
 * can be verified. Returns false if `key` is not a string-typed key.
 */
bool U3LinuxPlatformSetStringPreferenceForTesting(U3PreferenceKey key, const char *utf8Value);

#endif /* U3LinuxPlatform_h */
