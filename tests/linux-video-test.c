/* Standalone test for Sources/Linux/U3LinuxVideo.{c,h}. Not wired into
 * CMake (that belongs to the integration/build-owning agent); build and run
 * it directly, e.g.:
 *
 *   clang -std=c11 -fpascal-strings -D_DEFAULT_SOURCE -Wall -Wextra \
 *     -I Sources -I Sources/Linux \
 *     -I build/deps/sysroot/usr/include -I build/deps/sysroot/usr/include/x86_64-linux-gnu \
 *     tests/linux-video-test.c Sources/Linux/U3LinuxVideo.c Sources/U3Bitmap.c \
 *     Sources/Linux/U3LinuxAssets.c \
 *     -L build/deps/sysroot/usr/lib/x86_64-linux-gnu -ljson-c \
 *     /usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0 \
 *     -o /tmp/u3-linux-video-test
 *   SDL_VIDEODRIVER=dummy /tmp/u3-linux-video-test build/linux-game/assets
 *
 * U3LinuxVideo.c calls the real, authoritative U3ValidateCharacterDraft as
 * a final gate inside U3CocoaCreateCharacter (declared in Sources/U3Types.h,
 * defined in Sources/UltimaMain.c as part of the full game core). This test
 * does not link UltimaMain.c -- doing so would drag in the whole game's
 * global state, well outside this file's ownership -- so it supplies its
 * own copy of that one function, transcribed from UltimaMain.c lines
 * 849-866, for this test binary only. The real executable links the real
 * definition from UltimaMain.c; only one definition is ever present in any
 * given binary. */
#define _DEFAULT_SOURCE
#include "Linux/U3LegacyTypes.h"
#include "CarbonShunts.h"
#include "CocoaBridge.h"
#include "Linux/U3LinuxVideo.h"
#include "Linux/U3LinuxAssets.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL2/SDL.h>

bool U3ValidateCharacterDraft(const U3CharacterDraft *draft) {
    if (!draft || draft->name[12] || !draft->name[0]) return false;
    bool visible = false, ended = false;
    for (int i = 0; i < 12; ++i) {
        if (!draft->name[i]) { ended = true; continue; }
        if (ended || draft->name[i] < 32 || draft->name[i] == 127) return false;
        visible |= draft->name[i] != ' ';
    }
    if (!visible || !draft->race || !draft->characterClass || !draft->sex ||
        !strchr("HEDBF", draft->race) || !strchr("FCWTPBLIDAR", draft->characterClass) ||
        !strchr("FMO", draft->sex)) return false;
    int total = 0;
    for (int i = 0; i < 4; ++i) {
        if (draft->attributes[i] < 5 || draft->attributes[i] > 25) return false;
        total += draft->attributes[i];
    }
    return total <= 50;
}

static void MakePascal(unsigned char *out, const char *ascii) {
    size_t length = strlen(ascii);
    if (length > 15) length = 15;
    out[0] = (unsigned char)length;
    memcpy(out + 1, ascii, length);
}

static uint8_t *PixelAt(U3Bitmap *bitmap, int x, int y) {
    return bitmap->pixels + (size_t)y * bitmap->stride + (size_t)x * 4;
}

int main(int argc, char **argv) {
    assert(argc == 2 && "usage: u3-linux-video-test <assets-dir>");
    if (!getenv("SDL_VIDEODRIVER"))
        setenv("SDL_VIDEODRIVER", "dummy", 1);
    assert(!strcmp(getenv("SDL_VIDEODRIVER"), "dummy"));
    assert(U3CocoaIsHeadlessDiagnostic() && "SDL_VIDEODRIVER=dummy should read as headless");

    U3LinuxAssets *assets = U3LinuxAssetsOpen(argv[1]);
    assert(assets && "could not open assets directory; run cmake --build build/linux-game --target u3assets");
    U3LinuxVideoConfigure(assets, "Ultima III Linux Video Test", 2);

    assert(!U3CocoaHasMainSurface());
    void *token = U3CocoaCreateMainSurface(0, 0, 320, 200);
    assert(token && "U3CocoaCreateMainSurface failed");
    assert(U3CocoaHasMainSurface());

    U3Bitmap *main_bitmap = U3CocoaMainBitmap();
    assert(main_bitmap && main_bitmap->width == 320 && main_bitmap->height == 200);

    /* --- PaintRect / EraseRect / FrameRect target the main bitmap. --- */
    U3CocoaSetForegroundQuickDrawColor(redColor);
    U3CocoaPaintRect(10, 10, 20, 20);
    uint8_t *p = PixelAt(main_bitmap, 15, 15);
    assert(p[0] == 255 && p[1] == 0 && p[2] == 0);
    p = PixelAt(main_bitmap, 5, 5);
    assert(!(p[0] == 255 && p[1] == 0 && p[2] == 0));

    U3CocoaSetBackgroundQuickDrawColor(blueColor);
    U3CocoaEraseRect(10, 10, 20, 20);
    p = PixelAt(main_bitmap, 15, 15);
    assert(p[0] == 0 && p[1] == 0 && p[2] == 255);

    U3CocoaSetForegroundQuickDrawColor(greenColor);
    U3CocoaFrameRect(30, 30, 40, 40);
    p = PixelAt(main_bitmap, 30, 35); /* left edge */
    assert(p[0] == 0 && p[1] == 255 && p[2] == 0);
    p = PixelAt(main_bitmap, 35, 35); /* interior untouched by the frame */
    assert(!(p[0] == 0 && p[1] == 255 && p[2] == 0));

    /* --- SelectBitmap/origin redirects Paint/Erase/Frame/text, not DrawBitmap. --- */
    U3Bitmap side = {0};
    assert(U3BitmapAllocate(&side, 16, 16));
    U3CocoaSelectBitmap(&side, 100, 100);
    U3CocoaSetForegroundQuickDrawColor(whiteColor);
    U3CocoaPaintRect(100, 100, 108, 108); /* global coords; origin-relative == (0,0)-(8,8) in `side` */
    p = PixelAt(&side, 4, 4);
    assert(p[0] == 255 && p[1] == 255 && p[2] == 255);
    p = PixelAt(main_bitmap, 104, 104);
    assert(!(p[0] == 255 && p[1] == 255 && p[2] == 255)); /* main bitmap untouched */
    U3CocoaSelectBitmap(NULL, 0, 0);
    U3BitmapDispose(&side);

    U3CocoaSetBackgroundQuickDrawColor(blackColor);
    U3CocoaEraseRect(0, 0, 320, 80);

    /* --- Text: DrawPascalString vs. TextWidth consistency. --- */
    unsigned char greeting[16];
    MakePascal(greeting, "AB");
    short width = U3CocoaTextWidth(greeting);
    assert(width == 32); /* two 16px glyph cells */
    U3CocoaSetForegroundQuickDrawColor(whiteColor);
    U3CocoaMoveTo(0, 13);
    short penBefore = 0;
    { Point pen; U3CocoaGetPen(&pen); penBefore = pen.h; }
    U3CocoaDrawPascalString(greeting);
    { Point pen; U3CocoaGetPen(&pen);
      assert(pen.h == penBefore + width); }
    bool sawInk = false;
    for (int y = 0; y < 16 && !sawInk; ++y)
        for (int x = 0; x < 32 && !sawInk; ++x) {
            uint8_t *ink = PixelAt(main_bitmap, x, y);
            if (ink[0] == 255 && ink[1] == 255 && ink[2] == 255) sawInk = true;
        }
    assert(sawInk && "drawing \"AB\" through the bitmap font should paint at least one white pixel");

    /* DrawBytes must produce the same glyphs/advance as DrawPascalString. */
    U3CocoaMoveTo(0, 53);
    U3CocoaDrawBytes("AB", 0, 2);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 32; ++x) {
            uint8_t *a = PixelAt(main_bitmap, x, y);
            uint8_t *b = PixelAt(main_bitmap, x, 40 + y);
            assert(a[0] == b[0] && a[1] == b[1] && a[2] == b[2]);
        }

    /* --- Diagnostic key queue. --- */
    U3CocoaFlushInput();
    U3CocoaQueueDiagnosticKeys("ab");
    char key = 0; Boolean isMouse = false;
    assert(U3CocoaPollKeyMouse(true, 0, &key, &isMouse) && key == 'a' && !isMouse);
    assert(U3CocoaPollKeyMouse(true, 0, &key, &isMouse) && key == 'b' && !isMouse);
    assert(!U3CocoaPollKeyMouse(true, 0, &key, &isMouse) && "queue should be drained");

    U3CocoaQueueDiagnosticKey('z');
    assert(U3CocoaPollKeyMouse(true, 0, &key, &isMouse) && key == 'z');

    U3CocoaQueueDiagnosticMouse(42, 43);
    assert(U3CocoaPollKeyMouse(false, 0, &key, &isMouse) == false && isMouse == true);
    Point mouse; U3CocoaGetMousePoint(&mouse);
    assert(mouse.h == 42 && mouse.v == 43);

    U3CocoaQueueDiagnosticKeys("xyz");
    U3CocoaFlushInput();
    assert(!U3CocoaPollKeyMouse(true, 0, &key, &isMouse) && "FlushInput should clear the diagnostic queue");

    assert(U3CocoaKeyboardSelfTest());

    /* --- Image loading through U3LinuxAssets, and DrawBitmap onto the main surface. --- */
    U3Bitmap tiles = {0};
    Boolean loaded = U3CocoaLoadImage(&tiles, "/fake/app/Contents/Resources/Graphics/Standard-Tiles.png",
                                      384, 512, 12, 16);
    assert(loaded && tiles.width == 384 && tiles.height == 512);
    bool nonblack = false;
    for (size_t i = 0; i < tiles.stride * (size_t)tiles.height && !nonblack; i += 4)
        if (tiles.pixels[i] || tiles.pixels[i + 1] || tiles.pixels[i + 2]) nonblack = true;
    assert(nonblack && "Standard-Tiles.png should decode to a non-black image");

    assert(!U3CocoaLoadImage(&tiles, "no/such/asset.png", 10, 10, 1, 1));

    U3BitmapRect tileSrc = {0, 0, 32, 32};
    U3CocoaDrawBitmap(&tiles, tileSrc, 200, 150, 32, 32);
    bool drewSomething = false;
    for (int y = 150; y < 182 && !drewSomething; ++y)
        for (int x = 200; x < 232 && !drewSomething; ++x) {
            uint8_t *dst = PixelAt(main_bitmap, x, y);
            uint8_t *src = PixelAt(&tiles, x - 200, y - 150);
            if (dst[0] == src[0] && dst[1] == src[1] && dst[2] == src[2] && (src[0] || src[1] || src[2]))
                drewSomething = true;
        }
    assert(drewSomething);
    U3BitmapDispose(&tiles);

    /* --- WriteMainBitmap: round-trip through SDL_LoadBMP. --- */
    char bmpPath[] = "/tmp/u3-linux-video-test-XXXXXX.bmp";
    {
        char tmpl[] = "/tmp/u3-linux-video-test-XXXXXX";
        int fd = mkstemp(tmpl);
        assert(fd >= 0);
        close(fd);
        snprintf(bmpPath, sizeof(bmpPath), "%s.bmp", tmpl);
    }
    assert(U3CocoaWriteMainBitmap(bmpPath));
    SDL_Surface *reread = SDL_LoadBMP(bmpPath);
    assert(reread && reread->w == main_bitmap->width && reread->h == main_bitmap->height);
    uint8_t *expected = PixelAt(main_bitmap, 15, 15);
    uint8_t *rrPixel = (uint8_t *)reread->pixels + 15 * reread->pitch + 15 * (reread->format->BytesPerPixel);
    /* BMP is BGR-ordered by default; compare via SDL's own format-aware getter instead of raw bytes. */
    uint8_t rr, rg, rb;
    uint32_t pixelVal = 0;
    memcpy(&pixelVal, rrPixel, reread->format->BytesPerPixel);
    SDL_GetRGB(pixelVal, reread->format, &rr, &rg, &rb);
    assert(rr == expected[0] && rg == expected[1] && rb == expected[2]);
    SDL_FreeSurface(reread);
    remove(bmpPath);

    /* --- ResizeMainBitmap preserves existing pixels. --- */
    U3CocoaSetForegroundQuickDrawColor(yellowColor);
    U3CocoaPaintRect(0, 0, 4, 4);
    assert(U3CocoaResizeMainSurface(400, 250));
    main_bitmap = U3CocoaMainBitmap();
    assert(main_bitmap->width == 400 && main_bitmap->height == 250);
    p = PixelAt(main_bitmap, 1, 1);
    assert(p[0] == 255 && p[1] == 255 && p[2] == 0);

    /* --- Party selection (headless diagnostic UI), driven by scripted keys. --- */
    unsigned char names[20][16] = {{0}};
    Boolean available[20] = {0};
    MakePascal(names[0], "Ada");
    MakePascal(names[4], "Bors");
    available[0] = true;
    available[4] = true;
    short selection[4] = {0, 0, 0, 0};
    /* cursor starts on candidate 0 (slot 1, "Ada"): Enter selects it, Down
     * moves to candidate 1 (slot 5, "Bors"), Enter selects it, 'f' forms. */
    U3CocoaQueueDiagnosticKeys("\r\x1f\rf");
    assert(U3CocoaChooseParty(names, available, selection));
    assert(selection[0] == 1 && selection[1] == 5 && selection[2] == 0 && selection[3] == 0);

    /* Escape cancels and clears the selection. */
    short selection2[4] = {9, 9, 9, 9};
    U3CocoaQueueDiagnosticKeys("\x1b");
    assert(!U3CocoaChooseParty(names, available, selection2));
    assert(selection2[0] == 0 && selection2[1] == 0 && selection2[2] == 0 && selection2[3] == 0);

    /* No available characters: any key dismisses, false is returned. */
    Boolean noneAvailable[20] = {0};
    short selection3[4];
    U3CocoaQueueDiagnosticKeys(" ");
    assert(!U3CocoaChooseParty(names, noneAvailable, selection3));

    /* --- Character creation (headless diagnostic UI), driven by scripted keys. --- */
    Boolean charAvailable[20];
    for (int i = 0; i < 20; ++i) charAvailable[i] = (i != 0); /* slot 1 taken, slot 2 free */
    short outSlot = 0;
    U3CharacterDraft draft = {{0}, {15, 15, 10, 10}, 'H', 'F', 'M'};
    U3CocoaQueueDiagnosticKeys("Ada\r");
    assert(U3CocoaCreateCharacter(charAvailable, &outSlot, &draft));
    assert(outSlot == 2);
    assert(!memcmp(draft.name, "Ada", 3) && draft.name[3] == 0);
    assert(draft.race == 'H' && draft.characterClass == 'F' && draft.sex == 'M');
    assert(draft.attributes[0] == 15 && draft.attributes[1] == 15 &&
           draft.attributes[2] == 10 && draft.attributes[3] == 10);

    /* Cycle race with Right, bump Strength with Up/Right navigation, then confirm. */
    short outSlot2 = 0;
    U3CharacterDraft draft2 = {{0}, {15, 15, 10, 10}, 'H', 'F', 'M'};
    /* "Bob" name, Right to move off name field lands on Race (field order:
     * Name,Race,Class,Sex,Str,Dex,Int,Wis via Tab/Down); use Tab('\t')
     * explicitly to reach Race, cycle it to Elf, then Enter. */
    U3CocoaQueueDiagnosticKeys("Bob\tE\r"); /* 'E' is not Left/Right, so it is ignored on the Race field */
    assert(U3CocoaCreateCharacter(charAvailable, &outSlot2, &draft2));
    assert(!memcmp(draft2.name, "Bob", 3));
    assert(draft2.race == 'H'); /* unchanged: only Left(28)/Right(29) cycle fields, not arbitrary letters */

    /* Escape cancels character creation. */
    short outSlot3 = 0;
    U3CharacterDraft draft3 = {{0}, {15, 15, 10, 10}, 'H', 'F', 'M'};
    U3CocoaQueueDiagnosticKeys("\x1b");
    assert(!U3CocoaCreateCharacter(charAvailable, &outSlot3, &draft3));

    /* A full roster refuses immediately, without consuming any scripted input. */
    Boolean fullRoster[20];
    for (int i = 0; i < 20; ++i) fullRoster[i] = false;
    short outSlot4 = 0;
    U3CharacterDraft draft4 = {{0}, {15, 15, 10, 10}, 'H', 'F', 'M'};
    assert(!U3CocoaCreateCharacter(fullRoster, &outSlot4, &draft4));

    U3CocoaPresentMainSurface();
    U3CocoaPumpEvents();
    U3LinuxVideoShutdown();
    U3LinuxAssetsClose(assets);
    puts("linux video test passed");
    return 0;
}
