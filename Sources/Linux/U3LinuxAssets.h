#pragma once
#include "U3Bitmap.h"

typedef struct U3LinuxAssets U3LinuxAssets;

/* An asset context owns the index; returned bytes and strings belong to callers. */
U3LinuxAssets *U3LinuxAssetsOpen(const char *directory);
void U3LinuxAssetsClose(U3LinuxAssets *assets);
bool U3LinuxAssetResource(U3LinuxAssets *assets, uint32_t type, int16_t id,
                         uint8_t **bytes, size_t *size);
char *U3LinuxAssetString(U3LinuxAssets *assets, const char *table, size_t index);
/* Convert straight-alpha RGBA into an opaque black-backed U3Bitmap.
 * Scaling is cell-by-cell, matching the original tiled image loader. */
bool U3LinuxAssetImage(U3LinuxAssets *assets, const char *source, U3Bitmap *bitmap,
                      int width, int height, int columns, int rows);

/* Fetch an exported image as straight (un-premultiplied, un-composited) RGBA8
 * pixels, preserving the alpha channel that U3LinuxAssetImage flattens onto
 * black. Needed by callers that hand pixels to the host toolkit rather than to
 * U3Bitmap -- the window icon is the only one today, and a silhouette composited
 * onto black would show as a black box in the taskbar.
 *
 * On success *pixels is a malloc'd width*height*4 buffer the caller frees.
 * Returns false and leaves the outputs untouched if the image is missing or not
 * stored as tightly-packed RGBA8. */
bool U3LinuxAssetRGBA(U3LinuxAssets *assets, const char *source,
                      uint8_t **pixels, int *width, int *height);
