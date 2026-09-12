//
//  U3LinuxSettings.h
//  Ultima3 (Linux port)
//
//  In-game settings overlay: the Linux answer to the macOS menu bar.
//
//  On macOS the game's Sound, Music, Speech, appearance and window-size
//  options live in the AppKit menu bar, which SDL has no equivalent of. Before
//  this, the Linux port could only be configured with command-line flags
//  before launch (U3CocoaInstallMenus is an honest no-op). This module puts the
//  same settings behind a key, in a modal overlay drawn into the game's own
//  bitmap -- the same technique U3LinuxVideo.c already uses for party
//  selection and character creation, so it inherits scaling, the CRT filter
//  and screenshots for free.
//
//  Layering: this sits *above* the adapter modules and composes them --
//  preferences from U3LinuxPlatform, display options from U3LinuxVideo, and
//  U3AudioApplyPreferences from U3LinuxAudio. Nothing below it depends on this
//  file; U3LinuxVideo.c reaches it only through the handler seam it registers.
//

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Registers the overlay with U3LinuxVideo so F1 opens it. Call once at
 * startup, after U3LinuxVideoConfigure(). Without this the overlay never
 * appears and F1 does nothing (but is still swallowed, never reaching the game
 * as a command). */
void U3LinuxSettingsInstall(void);

/* Runs the overlay modally: takes over the screen and keyboard, returning only
 * when the player closes it. Normally invoked through the handler above rather
 * than called directly.
 *
 * Every change is written to preferences immediately, so nothing is lost if
 * the game exits abnormally afterwards. Changes that can take effect live
 * (volumes, speech, the display and gameplay toggles) are applied on the spot;
 * the two that cannot -- appearance and tile size, which require the game to
 * rebuild its offscreen worlds -- are labelled "restart" in the overlay and
 * take effect next launch. */
void U3LinuxSettingsShow(void);

/* Number of rows the overlay presents, and the label/value text for one of
 * them (0 <= index < count) as it would be drawn right now.
 *
 * Exposed so a test can assert the overlay's contents and that each row
 * reflects the stored preference, without driving a modal loop through
 * synthesized keystrokes. `outValue` receives the value column. Returns false
 * if the index is out of range. */
int U3LinuxSettingsRowCount(void);
bool U3LinuxSettingsRowText(int index, char *outLabel, size_t labelSize,
                            char *outValue, size_t valueSize);

/* Applies the overlay's "change this row" action to row `index`, exactly as
 * Left/Right would, and returns whether anything changed. `delta` is -1 or +1.
 *
 * Also a test hook -- it is the same entry point the modal loop uses, so a
 * test can verify that adjusting a row really writes the preference and
 * reaches the subsystem, rather than only that the row renders. */
bool U3LinuxSettingsAdjustRow(int index, int delta);

/* Test hook: draw the overlay once into the game's bitmap, with row
 * `cursorRow` highlighted, and leave it there -- no modal loop, no
 * snapshot/restore.
 *
 * U3LinuxSettingsShow puts the game's screen back before it returns, so a
 * screenshot taken after it would capture everything except the thing under
 * test. This renders the same pixels without that. */
void U3LinuxSettingsDrawForTesting(int cursorRow);
