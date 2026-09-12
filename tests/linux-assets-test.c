#include "Linux/U3LinuxAssets.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    U3LinuxAssets *assets = U3LinuxAssetsOpen(argv[1]);
    assert(assets);
    uint8_t *map = NULL;
    size_t size;
    assert(U3LinuxAssetResource(assets, 0x4d415053, 420, &map, &size));
    assert(size == 4101 && map[0] == 64);
    free(map);
    assert(!U3LinuxAssetResource(assets, 0x4d415053, 419, &map, &size));
    assert(map == NULL && size == 0);
    char *message = U3LinuxAssetString(assets, "Messages", 6);
    assert(message && !strcmp(message, "TIME IS BENEATH YOU"));
    free(message);
    assert(!U3LinuxAssetString(assets, "Messages", SIZE_MAX));
    assert(!U3LinuxAssetString(assets, "missing", 0));
    U3Bitmap tiles = {0}, font = {0};
    assert(U3LinuxAssetImage(assets, "Resources/Graphics/Standard-Tiles.png", &tiles, 384, 512, 12, 16));
    assert(tiles.width == 384 && tiles.height == 512 && tiles.stride == 1536);
    assert(U3LinuxAssetImage(assets, "Resources/Graphics/Standard-Font.gif", &font, 1536, 16, 96, 1));
    for (int i = 3; i < 1536 * 16 * 4; i += 4) assert(font.pixels[i] == 255);
    uint8_t *old = tiles.pixels;
    assert(!U3LinuxAssetImage(assets, "missing", &tiles, 384, 512, 12, 16));
    assert(tiles.pixels == old);
    assert(!U3LinuxAssetImage(assets, "Resources/Graphics/Standard-Tiles.png", &tiles, 385, 512, 12, 16));
    U3BitmapDispose(&tiles); U3BitmapDispose(&font);

    /* U3LinuxAssetRGBA must hand back the alpha channel intact, which is the
     * whole reason it exists alongside U3LinuxAssetImage: the window icon is a
     * silhouette on transparency, and flattening it onto black would show as a
     * black square in the taskbar. */
    uint8_t *iconPixels = NULL;
    int iconWidth = 0, iconHeight = 0;
    assert(U3LinuxAssetRGBA(assets, "Images/AppIcon.png", &iconPixels, &iconWidth, &iconHeight));
    assert(iconWidth == 256 && iconHeight == 256);
    bool sawTransparent = false, sawOpaque = false;
    for (int i = 3; i < iconWidth * iconHeight * 4; i += 4) {
        if (iconPixels[i] == 0) sawTransparent = true;
        if (iconPixels[i] == 255) sawOpaque = true;
    }
    assert(sawTransparent && "the icon's transparent margin should survive the load");
    assert(sawOpaque && "the icon's artwork should survive the load");
    free(iconPixels);

    /* Absent images and bad arguments fail without writing the outputs. */
    uint8_t *missingPixels = (uint8_t *)&missingPixels;
    int missingWidth = -7, missingHeight = -7;
    assert(!U3LinuxAssetRGBA(assets, "no/such/asset.png", &missingPixels, &missingWidth, &missingHeight));
    assert(missingWidth == -7 && missingHeight == -7);
    assert(!U3LinuxAssetRGBA(assets, "Images/AppIcon.png", NULL, &missingWidth, &missingHeight));

    U3LinuxAssetsClose(assets);
    puts("Linux runtime map, text, tile, font, and icon loading passed");
    return 0;
}
