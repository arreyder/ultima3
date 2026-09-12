#pragma once
#include "U3IO.h"
#include "U3LinuxAssets.h"

/* Linux implementation of the U3IO.h boundary: mutable save state lives in
 * a single container file (see U3IOLinux.c for its exact format) under a
 * save directory; read-only bundled game data comes from `assets` (see
 * U3LinuxAssets.h) via the exported asset manifest. A resource present in
 * the save container shadows the bundled one of the same kind/id.
 *
 * Configure the process-wide adapter before calling U3IOOpenSaveContainer().
 * `saveDirectory` must be non-NULL/non-empty; pass the directory explicitly
 * for integration and tests (U3_SAVE_DIRECTORY is also honored, and an
 * XDG-based default is used, if U3IOOpenSaveContainer() is called without
 * ever calling this first). `assets` may be NULL if bundled resource
 * fallback is not needed (e.g. tests that only exercise the mutable side). */
bool U3IOLinuxConfigure(const char *saveDirectory, U3LinuxAssets *assets);

/* Resets all adapter state (as if U3IOLinuxConfigure() had never been
 * called) and clears U3IOLastError(). Intended for test isolation between
 * scenarios run in the same process. */
void U3IOLinuxReset(void);
