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
    U3LinuxAssetsClose(assets);
    puts("Linux runtime map, text, tile, and font loading passed");
    return 0;
}
