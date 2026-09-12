// Exercises Sources/Linux/U3LinuxCarbon.c standalone (no full game link):
// Pascal/C string round trips, CF create/retain/release ownership, the
// string-table path against the real exported assets, and screen bounds.
//
// Usage: linux-carbon-test <assets-directory>
#define _DEFAULT_SOURCE
#include "Linux/U3LegacyTypes.h"
#include "Linux/U3LinuxCarbon.h"
#include "CocoaBridge.h"
#include "PrefsDialog.h"
#include "UltimaAppleEvents.h"
#include "UltimaSound.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    size_t baseline = U3LinuxCarbonLiveObjectCount();

    // --- Pascal <-> C string round trips ---------------------------------

    unsigned char ascii[] = "\pHello, Ultima";
    CFStringRef helloRef = CFStringCreateWithPascalString(kCFAllocatorDefault, ascii, kCFStringEncodingMacRoman);
    assert(helloRef);
    assert(CFStringGetLength(helloRef) == ascii[0]);
    assert(!memcmp(helloRef, ascii + 1, ascii[0]));
    ConstStringPtr roundTrip = CFStringGetPascalStringPtr(helloRef, kCFStringEncodingMacRoman);
    assert(roundTrip && roundTrip[0] == ascii[0] && !memcmp(roundTrip + 1, ascii + 1, ascii[0]));
    CFRelease(helloRef);

    // A literal (never registered) string must honestly report NULL rather
    // than fabricate a pointer.
    assert(CFStringGetPascalStringPtr(CFSTR("literal"), kCFStringEncodingMacRoman) == NULL);

    // Exactly 255 bytes: the Pascal length byte's maximum, must not truncate.
    unsigned char full255[256];
    full255[0] = 255;
    for (int i = 0; i < 255; ++i)
        full255[1 + i] = (unsigned char)(i & 0xff); // includes high-bit (>=128) bytes.
    CFStringRef full255Ref = CFStringCreateWithPascalString(kCFAllocatorDefault, full255, kCFStringEncodingMacRoman);
    assert(full255Ref && CFStringGetLength(full255Ref) == 255);
    for (int i = 0; i < 255; ++i)
        assert(((const unsigned char *)full255Ref)[i] == (unsigned char)(i & 0xff));
    ConstStringPtr full255Ptr = CFStringGetPascalStringPtr(full255Ref, kCFStringEncodingMacRoman);
    assert(full255Ptr && full255Ptr[0] == 255 && !memcmp(full255Ptr + 1, full255 + 1, 255));
    CFRelease(full255Ref);

    // High-bit bytes specifically (embedded values >= 0x80), not just length.
    unsigned char highBit[] = "\p\x80\x81\xfe\xff";
    CFStringRef highBitRef = CFStringCreateWithPascalString(kCFAllocatorDefault, highBit, kCFStringEncodingMacRoman);
    assert(highBitRef && CFStringGetLength(highBitRef) == 4);
    assert((unsigned char)((const char *)highBitRef)[0] == 0x80 && (unsigned char)((const char *)highBitRef)[3] == 0xff);
    CFRelease(highBitRef);

    // A string longer than 255 bytes (built by repeated concatenation) must
    // truncate loudly (stderr warning), not silently, when asked for a
    // Pascal pointer -- it cannot be represented in a Str255 at all.
    CFStringRef longString = CFRetain(CFSTR(""));
    for (int i = 0; i < 40; ++i) {
        CFStringRef grown = CopyCatStrings(longString, CFSTR("0123456789"));
        CFRelease(longString);
        longString = grown;
    }
    assert(CFStringGetLength(longString) == 400);
    ConstStringPtr truncated = CFStringGetPascalStringPtr(longString, kCFStringEncodingMacRoman);
    assert(truncated && truncated[0] == 255);
    CFRelease(longString);

    // --- CFRetain/CFRelease ownership -------------------------------------

    CFStringRef literal = CFSTR("Standard");
    CFStringRef retained = CFRetain(literal); // must duplicate, not alias, the literal.
    assert(retained && retained != literal);
    assert(CFStringCompare(retained, literal, 0) == kCFCompareEqualTo);
    CFRelease(retained);                 // must not crash/free the literal.
    CFRelease(literal);                  // releasing an unmanaged literal is a safe no-op.
    assert(CFStringCompare(literal, CFSTR("Standard"), 0) == kCFCompareEqualTo); // literal still intact.

    CFStringRef cat = CopyCatStrings(CFSTR("Standard"), CFSTR("-Tiles"));
    assert(cat && !strcmp((const char *)cat, "Standard-Tiles"));
    assert(CFStringHasPrefix(cat, CFSTR("Standard-Tiles")));
    assert(!CFStringHasPrefix(cat, CFSTR("Standard-Tiles.")));
    CFRange found = CFStringFind(cat, CFSTR("-Tiles"), 0);
    assert(found.location == 8 && found.length == 6);
    CFRange notFound = CFStringFind(cat, CFSTR("nope"), 0);
    assert(notFound.location == kCFNotFound);
    CFStringRef sub = CFStringCreateWithSubstring(NULL, cat, CFRangeMake(0, found.location));
    assert(sub && !strcmp((const char *)sub, "Standard"));
    CFRelease(sub);
    CFRelease(cat);

    // Double CFRetain/CFRelease balance: two retains need two releases
    // before the object disappears from the live registry.
    CFStringRef owned = CFStringCreateWithPascalString(kCFAllocatorDefault, (ConstStr255Param) "\pOwned", kCFStringEncodingMacRoman);
    size_t withOne = U3LinuxCarbonLiveObjectCount();
    CFRetain(owned);
    assert(U3LinuxCarbonLiveObjectCount() == withOne); // same object, refcount 2, not a new entry.
    CFRelease(owned);
    assert(U3LinuxCarbonLiveObjectCount() == withOne); // still alive (refcount 1).
    CFRelease(owned);
    assert(U3LinuxCarbonLiveObjectCount() == baseline); // now freed.

    // --- CFURL path joining ------------------------------------------------

    U3LinuxCarbonConfigure(argv[1]);
    CFURLRef graphicsDir = GraphicsDirectoryURL();
    assert(graphicsDir && strstr((const char *)graphicsDir, "Resources/Graphics"));
    CFURLRef joined = CFURLCreateCopyAppendingPathComponent(NULL, graphicsDir, CFSTR("Standard-Tiles.png"), false);
    assert(joined && strstr((const char *)joined, "Resources/Graphics/Standard-Tiles.png"));
    CFRelease(joined);

    // --- CopyGraphicsDirectoryItems against the real export ---------------

    CFArrayRef graphicsItems = (CFArrayRef)CopyGraphicsDirectoryItems();
    assert(graphicsItems);
    CFIndex itemCount = CFArrayGetCount(graphicsItems);
    bool sawStandardTiles = false;
    for (CFIndex i = 0; i < itemCount; ++i) {
        CFStringRef item = (CFStringRef)CFArrayGetValueAtIndex(graphicsItems, i);
        assert(item);
        if (!strcmp((const char *)item, "Standard-Tiles.png"))
            sawStandardTiles = true;
    }
    assert(sawStandardTiles); // the real export's manifest has a "Standard" tileset.
    CFRelease(graphicsItems);
    assert(CFArrayGetCount(NULL) == 0);       // honest on a NULL array.
    assert(CFArrayGetValueAtIndex(NULL, 0) == NULL);

    // --- String tables against the real exported assets --------------------

    CFArrayRef messages = StringsArray(CFSTR("Messages"));
    assert(messages);
    assert(CFArrayGetCount(messages) == 264); // matches manifest.json's recorded count.
    CFArrayRef messagesAgain = StringsArray(CFSTR("Messages")); // cache hit, same pointer.
    assert(messagesAgain == messages);

    unsigned char buf[256];
    GetPascalStringFromArrayByIndex(buf, CFSTR("Messages"), 6);
    assert(buf[0] == 19 && !memcmp(buf + 1, "TIME IS BENEATH YOU", 19));

    GetPascalStringFromArrayByIndex(buf, CFSTR("Messages"), 172);
    assert(buf[0] == 5 && !memcmp(buf + 1, "None\n", 5));

    // An empty table slot (confirmed present at TilesVoices[1] in the
    // real export) must come back as an honest empty string, not a crash
    // and not silently treated as "end of table".
    GetPascalStringFromArrayByIndex(buf, CFSTR("TilesVoices"), 1);
    assert(buf[0] == 0);

    // Out of range: honest empty string plus a loud (stderr) warning, never
    // a fabricated value.
    GetPascalStringFromArrayByIndex(buf, CFSTR("Messages"), 9999);
    assert(buf[0] == 0);

    // Unknown table: honest empty, not a crash.
    GetPascalStringFromArrayByIndex(buf, CFSTR("NoSuchTable"), 0);
    assert(buf[0] == 0);

    // --- Version / system info --------------------------------------------

    CFStringRef version = (CFStringRef)CopyAppVersionString();
    assert(version && CFStringGetLength(version) > 0);
    CFRelease(version);

    unsigned major = 0, minor = 0, bugFix = 0;
    Boolean gotVersion = GetSystemVersion(&major, &minor, &bugFix);
    assert(gotVersion); // real `uname()` on this machine must succeed.
    (void)bugFix;

    // --- Screen bounds -------------------------------------------------------

    Rect screen = {0, 0, 0, 0};
    U3LinuxScreenBounds(&screen);
    assert(screen.right > screen.left && screen.bottom > screen.top);

    // --- Honest "nothing happened" behaviour for host-UI stubs --------------

    assert(IsMenuBarVisible());
    HideMenuBar();
    assert(!IsMenuBarVisible());
    ShowMenuBar();
    assert(IsMenuBarVisible());
    assert(CountMenuItems(NULL) == 0);
    MenuItemIndex appendedIndex = 77;
    AppendMenuItemTextWithCFString(NULL, CFSTR("x"), 0, 0, &appendedIndex);
    assert(appendedIndex == 0);
    assert(MenuSelect((Point){0, 0}) == 0);
    WindowRef hitWindow = (WindowRef)0x1;
    assert(FindWindow((Point){0, 0}, &hitWindow) == 0 && hitWindow == NULL);
    short type = -1;
    Handle handle = (Handle)0x1;
    Rect bounds = {1, 1, 1, 1};
    GetDialogItem(NULL, 1, &type, &handle, &bounds);
    assert(type == 0 && handle == NULL && bounds.top == 0 && bounds.left == 0 && bounds.right == 0 &&
           bounds.bottom == 0);
    ControlRef control = (ControlRef)0x1;
    GetDialogItemAsControl(NULL, 1, &control);
    assert(control == NULL);
    assert(GetCTable(0) == NULL);
    assert(ResError() == noErr);
    assert(!Button());
    assert(!SetCursorNamed(CFSTR("CursorUp"), 1.0f));
    assert(InitializeAEHandlers());
    assert(AEProcessAppleEvent(NULL) == noErr);
    InitCursor();
    HideCursor();
    ShowCursor();
    SetMusicPortAndDevice(NULL, NULL);
    SetRefMenuIcons(NULL);
    GameOptionsDialog();
    LWOpenURL(CFSTR("")); // empty string: must not attempt to launch anything.

    // --- Timing: real clock, bounded, non-fabricated event ------------------

    EventRecord event;
    memset(&event, 0xAA, sizeof(event));
    Boolean hadEvent = WaitNextEvent(everyEvent, &event, 0, NULL);
    assert(!hadEvent && event.what == nullEvent);
    unsigned long finalTicks = 0;
    Delay(1, &finalTicks); // ~1/60s: keeps the test fast while exercising the real sleep path.
    assert(finalTicks > 0);

    assert(U3LinuxCarbonLiveObjectCount() == baseline);

    puts("Linux Carbon shim string, ownership, asset, and stub behaviour passed");
    return 0;
}
