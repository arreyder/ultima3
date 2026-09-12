// Must precede every #include (even our own header, which drags in <string.h>
// transitively via U3LegacyTypes.h): strdup, nanosleep and clock_gettime are
// POSIX extensions hidden by -std=c11's strict feature-test macros otherwise.
#define _DEFAULT_SOURCE

// Linux definitions for the 56 Carbon/CoreFoundation/menu/dialog/cursor/event
// symbols the preserved game sources call but the macOS hosts (CarbonShunts.c,
// CocoaBridge.m, PrefsDialog.m, UltimaAppleEvents.c, UltimaSound.m) no longer
// provide on this platform.
//
// OWNERSHIP RULE for CFStringRef/CFURLRef (both `const char *`) and CFArrayRef
// (`void *`) here:
//
//   Every object returned by a Create/Copy-prefixed function (and every
//   duplicate CFRetain makes of a pointer it does not already own) is heap
//   allocated and registered, with a refcount, in a process-wide linked list
//   (sLiveObjects). CFRelease looks a pointer up in that list: if found, it
//   decrements the refcount and frees the object at zero; if NOT found, the
//   pointer is a borrowed literal (a CFSTR(...) constant, or the result of a
//   non-owning accessor such as CFArrayGetValueAtIndex) and CFRelease is a
//   safe no-op, exactly as the real call sites in UltimaGraphics.c,
//   UltimaMacIF.c, UltimaText.c and UltimaMain.c expect: they CFRelease
//   every CopyXxx()/CFStringCreateXxx() result and every CFRetain() result,
//   and they never release a CFSTR literal or an array element. CFRetain on
//   a pointer we do not already own (observed exactly once in the real
//   sources: `CFRetain(defaultTilesRef)` where defaultTilesRef is a CFSTR
//   literal) duplicates it into a new owned, independently releasable
//   object, since a string literal has no refcount of its own to bump.
//
//   StringsArray()'s cached tables are a second, deliberate exception: like
//   the macOS original's process-lifetime NSMutableDictionary cache, they
//   are allocated with the same array representation but are never linked
//   into sLiveObjects, so CFRelease on them is always a no-op (never
//   observed in the real sources, but safe either way) and they live for
//   the life of the process/configuration, matching how every call site
//   treats them as borrowed.
//
// CFStringRef/CFURLRef must remain literal `const char *` values (not a
// wrapped struct) because CFURLRef values cross into code this task does not
// own (e.g. U3CocoaLoadImage, called from U3LegacyDrawImageURL in
// CarbonShunts.c) that the handoff notes treats CFURLRef as a plain path
// string on Linux. CFArrayRef has no such external contract (only
// CFArrayGetCount/CFArrayGetValueAtIndex/CFRelease touch it, all defined
// here), so it is a private struct pointer.

#include "U3LinuxCarbon.h"

#include "U3LegacyTypes.h"
#include "CocoaBridge.h"
#include "PrefsDialog.h"
#include "UltimaAppleEvents.h"
#include "UltimaSound.h"
#include "U3LinuxAssets.h"

#include <json-c/json.h>
#include <SDL2/SDL.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((weak)) void U3CocoaPumpEvents(void) {}
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define U3LINUX_CARBON_DEFAULT_ASSETS_DIR "build/linux-game/assets"

// ---------------------------------------------------------------------------
// CF object registry
// ---------------------------------------------------------------------------

typedef enum { U3CF_STRING, U3CF_ARRAY } U3CFKind;

typedef struct U3CFEntry {
    struct U3CFEntry *next;
    U3CFKind kind;
    int refcount;
    /* STRING */
    char *bytes;
    size_t length;
    unsigned char *pascalCache; /* lazily built Str255, freed with the entry */
    /* ARRAY */
    CFIndex count;
    CFStringRef *items; /* each item owned (strdup'd) by the array */
} U3CFEntry;

static U3CFEntry *sLiveObjects;

static void LinkEntry(U3CFEntry *entry) {
    entry->next = sLiveObjects;
    sLiveObjects = entry;
}

static void UnlinkEntry(U3CFEntry *entry) {
    for (U3CFEntry **link = &sLiveObjects; *link; link = &(*link)->next) {
        if (*link == entry) {
            *link = entry->next;
            return;
        }
    }
}

static U3CFEntry *FindByPointer(const void *ptr) {
    if (!ptr)
        return NULL;
    for (U3CFEntry *entry = sLiveObjects; entry; entry = entry->next) {
        if (entry->kind == U3CF_STRING && entry->bytes == ptr)
            return entry;
        if (entry->kind == U3CF_ARRAY && (const void *)entry == ptr)
            return entry;
    }
    return NULL;
}

static size_t StringLength(CFStringRef text) {
    if (!text)
        return 0;
    U3CFEntry *entry = FindByPointer(text);
    if (entry && entry->kind == U3CF_STRING)
        return entry->length;
    return strlen((const char *)text);
}

static U3CFEntry *NewStringEntry(size_t length) {
    char *buf = malloc(length + 1);
    if (!buf)
        return NULL;
    buf[length] = '\0';
    U3CFEntry *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        free(buf);
        return NULL;
    }
    entry->kind = U3CF_STRING;
    entry->refcount = 1;
    entry->bytes = buf;
    entry->length = length;
    LinkEntry(entry);
    return entry;
}

static CFStringRef MakeOwnedString(const void *bytes, size_t length) {
    U3CFEntry *entry = NewStringEntry(length);
    if (!entry)
        return NULL;
    if (length)
        memcpy(entry->bytes, bytes, length);
    return entry->bytes;
}

static void FreeArrayEntry(U3CFEntry *entry) {
    for (CFIndex i = 0; i < entry->count; ++i)
        free((void *)entry->items[i]);
    free(entry->items);
    free(entry);
}

// Builds a growable list of owned (strdup'd) strings; used for both
// CopyGraphicsDirectoryItems (registered, refcounted) and StringsArray
// (cached, never released).
typedef struct {
    CFStringRef *items;
    CFIndex count;
    CFIndex capacity;
} U3CFItemList;

static void ItemListPush(U3CFItemList *list, char *item) {
    if (!item)
        return;
    if (list->count == list->capacity) {
        CFIndex grownCapacity = list->capacity ? list->capacity * 2 : 16;
        CFStringRef *grown = realloc(list->items, (size_t)grownCapacity * sizeof(*grown));
        if (!grown) {
            free(item);
            return;
        }
        list->items = grown;
        list->capacity = grownCapacity;
    }
    list->items[list->count++] = item;
}

static CFArrayRef MakeArray(U3CFItemList *list, bool registerForRefcounting) {
    U3CFEntry *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        for (CFIndex i = 0; i < list->count; ++i)
            free((void *)list->items[i]);
        free(list->items);
        return NULL;
    }
    entry->kind = U3CF_ARRAY;
    entry->refcount = 1;
    entry->items = list->items;
    entry->count = list->count;
    if (registerForRefcounting)
        LinkEntry(entry);
    return (CFArrayRef)entry;
}

// ---------------------------------------------------------------------------
// CFRetain / CFRelease
// ---------------------------------------------------------------------------

const void *CFRetain(const void *value) {
    if (!value)
        return value;
    U3CFEntry *entry = FindByPointer(value);
    if (entry) {
        entry->refcount++;
        return value;
    }
    // Unregistered pointer: the only real call site (CFRetain on a CFSTR
    // literal in UltimaGraphics.c) retains a borrowed C string. Duplicate it
    // into a new owned object so the eventual CFRelease is safe.
    return MakeOwnedString(value, strlen((const char *)value));
}

void CFRelease(const void *value) {
    if (!value)
        return;
    U3CFEntry *entry = FindByPointer(value);
    if (!entry)
        return; // borrowed/literal: never owned here, never freed here.
    if (--entry->refcount > 0)
        return;
    UnlinkEntry(entry);
    if (entry->kind == U3CF_STRING) {
        free(entry->bytes);
        free(entry->pascalCache);
        free(entry);
    } else {
        FreeArrayEntry(entry);
    }
}

size_t U3LinuxCarbonLiveObjectCount(void) {
    size_t n = 0;
    for (U3CFEntry *entry = sLiveObjects; entry; entry = entry->next)
        ++n;
    return n;
}

// ---------------------------------------------------------------------------
// CFString* / CFArray* / CFRange
// ---------------------------------------------------------------------------

CFRange CFRangeMake(CFIndex location, CFIndex length) {
    CFRange range;
    range.location = location;
    range.length = length;
    return range;
}

CFIndex CFArrayGetCount(CFArrayRef array) {
    const U3CFEntry *entry = array;
    return entry ? entry->count : 0;
}

const void *CFArrayGetValueAtIndex(CFArrayRef array, CFIndex index) {
    const U3CFEntry *entry = array;
    if (!entry || index < 0 || index >= entry->count)
        return NULL;
    return entry->items[index];
}

CFStringRef CFStringCreateWithPascalString(void *allocator, ConstStr255Param text, CFStringEncoding encoding) {
    (void)allocator;
    (void)encoding; // Single-byte legacy text is copied verbatim; Linux has
                     // no MacRoman codec and no call site needs recoding.
    if (!text)
        return NULL;
    size_t length = text[0]; // Pascal length byte: 0..255, cannot overflow.
    return MakeOwnedString(text + 1, length);
}

CFStringRef CFStringCreateWithSubstring(void *allocator, CFStringRef text, CFRange range) {
    (void)allocator;
    if (!text || range.location < 0 || range.length < 0)
        return NULL;
    size_t total = StringLength(text);
    if ((size_t)range.location > total || (size_t)range.location + (size_t)range.length > total)
        return NULL;
    return MakeOwnedString((const char *)text + range.location, (size_t)range.length);
}

CFStringRef CopyCatStrings(CFStringRef str1, CFStringRef str2) {
    size_t len1 = StringLength(str1), len2 = StringLength(str2);
    U3CFEntry *entry = NewStringEntry(len1 + len2);
    if (!entry)
        return NULL;
    if (len1)
        memcpy(entry->bytes, str1, len1);
    if (len2)
        memcpy(entry->bytes + len1, str2, len2);
    return entry->bytes;
}

CFRange CFStringFind(CFStringRef text, CFStringRef needle, unsigned options) {
    (void)options;
    CFRange notFound = CFRangeMake(kCFNotFound, 0);
    if (!text || !needle)
        return notFound;
    size_t textLength = StringLength(text), needleLength = StringLength(needle);
    if (needleLength == 0 || needleLength > textLength)
        return notFound;
    const char *t = (const char *)text, *n = (const char *)needle;
    for (size_t i = 0; i + needleLength <= textLength; ++i) {
        if (memcmp(t + i, n, needleLength) == 0)
            return CFRangeMake((CFIndex)i, (CFIndex)needleLength);
    }
    return notFound;
}

CFIndex CFStringGetLength(CFStringRef text) {
    return (CFIndex)StringLength(text);
}

CFStringEncoding CFStringGetSystemEncoding(void) {
    return kCFStringEncodingUTF8;
}

Boolean CFStringHasPrefix(CFStringRef text, CFStringRef prefix) {
    if (!text || !prefix)
        return false;
    size_t textLength = StringLength(text), prefixLength = StringLength(prefix);
    if (prefixLength > textLength)
        return false;
    return memcmp(text, prefix, prefixLength) == 0;
}

int CFStringCompare(CFStringRef a, CFStringRef b, unsigned options) {
    (void)options;
    size_t la = StringLength(a), lb = StringLength(b);
    size_t n = la < lb ? la : lb;
    int c = n ? memcmp(a, b, n) : 0;
    if (c == 0)
        c = (la > lb) - (la < lb);
    return (c > 0) - (c < 0);
}

ConstStringPtr CFStringGetPascalStringPtr(CFStringRef text, CFStringEncoding encoding) {
    (void)encoding;
    if (!text)
        return NULL;
    U3CFEntry *entry = FindByPointer(text);
    if (!entry || entry->kind != U3CF_STRING)
        return NULL; // Honest CF behaviour: no pointer can be produced
                      // without copying for a string we do not own.
    size_t length = entry->length;
    if (length > 255) {
        fprintf(stderr,
                "U3LinuxCarbon: CFStringGetPascalStringPtr truncating a %zu-byte string to 255 bytes\n",
                length);
        length = 255;
    }
    if (!entry->pascalCache) {
        entry->pascalCache = malloc(256);
        if (!entry->pascalCache)
            return NULL;
    }
    entry->pascalCache[0] = (unsigned char)length;
    if (length)
        memcpy(entry->pascalCache + 1, entry->bytes, length);
    return entry->pascalCache;
}

// ---------------------------------------------------------------------------
// Asset-backed directories and string tables
// ---------------------------------------------------------------------------

static char *sAssetsBaseDir;
static char *sGraphicsDirCache;   // "<base>/Resources/Graphics"; borrowed, never released.
static char *sResourcesDirCache;  // "<base>/Resources"; borrowed, never released.
static U3LinuxAssets *sAssets;

typedef struct U3TableCache {
    struct U3TableCache *next;
    char *name;
    CFArrayRef array;
} U3TableCache;

static U3TableCache *sTableCaches;

static void FreeTableCaches(void) {
    while (sTableCaches) {
        U3TableCache *next = sTableCaches->next;
        free(sTableCaches->name);
        if (sTableCaches->array)
            FreeArrayEntry((U3CFEntry *)sTableCaches->array); // never registered; free directly.
        free(sTableCaches);
        sTableCaches = next;
    }
}

static void EnsureBaseDir(void) {
    if (!sAssetsBaseDir)
        sAssetsBaseDir = strdup(U3LINUX_CARBON_DEFAULT_ASSETS_DIR);
}

static U3LinuxAssets *EnsureAssets(void) {
    EnsureBaseDir();
    if (!sAssets)
        sAssets = U3LinuxAssetsOpen(sAssetsBaseDir);
    return sAssets;
}

void U3LinuxCarbonConfigure(const char *assetsDirectory) {
    free(sAssetsBaseDir);
    sAssetsBaseDir = strdup(assetsDirectory && *assetsDirectory ? assetsDirectory
                                                                 : U3LINUX_CARBON_DEFAULT_ASSETS_DIR);
    free(sGraphicsDirCache);
    sGraphicsDirCache = NULL;
    free(sResourcesDirCache);
    sResourcesDirCache = NULL;
    if (sAssets) {
        U3LinuxAssetsClose(sAssets);
        sAssets = NULL;
    }
    FreeTableCaches();
}

CFURLRef GraphicsDirectoryURL(void) {
    EnsureBaseDir();
    if (!sGraphicsDirCache) {
        size_t n = strlen(sAssetsBaseDir) + strlen("/Resources/Graphics") + 1;
        sGraphicsDirCache = malloc(n);
        if (sGraphicsDirCache)
            snprintf(sGraphicsDirCache, n, "%s/Resources/Graphics", sAssetsBaseDir);
    }
    return sGraphicsDirCache;
}

CFURLRef ResourcesDirectoryURL(void) {
    EnsureBaseDir();
    if (!sResourcesDirCache) {
        size_t n = strlen(sAssetsBaseDir) + strlen("/Resources") + 1;
        sResourcesDirCache = malloc(n);
        if (sResourcesDirCache)
            snprintf(sResourcesDirCache, n, "%s/Resources", sAssetsBaseDir);
    }
    return sResourcesDirCache;
}

CFURLRef CFURLCreateCopyAppendingPathComponent(void *allocator, CFURLRef base, CFStringRef component,
                                               Boolean directory) {
    (void)allocator;
    if (!base || !component)
        return NULL;
    size_t baseLength = strlen((const char *)base);
    size_t componentLength = StringLength(component);
    bool needSlash = baseLength > 0 && ((const char *)base)[baseLength - 1] != '/';
    size_t trailingSlash = directory ? 1 : 0;
    U3CFEntry *entry = NewStringEntry(baseLength + (needSlash ? 1 : 0) + componentLength + trailingSlash);
    if (!entry)
        return NULL;
    size_t pos = 0;
    memcpy(entry->bytes + pos, base, baseLength);
    pos += baseLength;
    if (needSlash)
        entry->bytes[pos++] = '/';
    memcpy(entry->bytes + pos, component, componentLength);
    pos += componentLength;
    if (directory)
        entry->bytes[pos++] = '/';
    return entry->bytes;
}

// Recursively lists real files under `root`/`relative`, matching the
// recursive-subpaths behaviour of the macOS original's
// `-[NSFileManager subpathsAtPath:]`. Depth-limited defensively against
// symlink cycles.
static void WalkDirectory(const char *root, const char *relative, U3CFItemList *list, int depthRemaining) {
    if (depthRemaining <= 0)
        return;
    char path[PATH_MAX];
    if (relative && *relative)
        snprintf(path, sizeof(path), "%s/%s", root, relative);
    else
        snprintf(path, sizeof(path), "%s", root);
    DIR *dir = opendir(path);
    if (!dir)
        return;
    struct dirent *dirEntry;
    while ((dirEntry = readdir(dir)) != NULL) {
        if (!strcmp(dirEntry->d_name, ".") || !strcmp(dirEntry->d_name, ".."))
            continue;
        char childRelative[PATH_MAX];
        if (relative && *relative)
            snprintf(childRelative, sizeof(childRelative), "%s/%s", relative, dirEntry->d_name);
        else
            snprintf(childRelative, sizeof(childRelative), "%s", dirEntry->d_name);
        char childAbsolute[PATH_MAX];
        snprintf(childAbsolute, sizeof(childAbsolute), "%s/%s", root, childRelative);
        struct stat info;
        if (stat(childAbsolute, &info) != 0)
            continue;
        if (S_ISDIR(info.st_mode)) {
            WalkDirectory(root, childRelative, list, depthRemaining - 1);
        } else if (S_ISREG(info.st_mode)) {
            char *owned = strdup(childRelative);
            if (owned)
                ItemListPush(list, owned);
        }
    }
    closedir(dir);
}

// Lists every manifest "images" record whose "source" starts with `prefix`,
// stripped of that prefix. The exported asset bundle stores decoded graphics
// under images/... (as raw RGBA, keyed by the original "source" path), not
// as real files under Resources/Graphics/, so a literal directory walk of
// the latter finds nothing against the real export in build/linux-game/assets
// even though GetGraphics()'s filename-prefix matching needs real entries.
static void CollectManifestImageNames(const char *assetsDirectory, const char *prefix, U3CFItemList *list) {
    char manifestPath[PATH_MAX];
    snprintf(manifestPath, sizeof(manifestPath), "%s/manifest.json", assetsDirectory);
    struct json_object *manifest = json_object_from_file(manifestPath);
    if (!manifest)
        return;
    struct json_object *images = NULL;
    if (json_object_object_get_ex(manifest, "images", &images) && json_object_is_type(images, json_type_array)) {
        size_t prefixLength = strlen(prefix);
        size_t n = json_object_array_length(images);
        for (size_t i = 0; i < n; ++i) {
            struct json_object *record = json_object_array_get_idx(images, i);
            struct json_object *source = NULL;
            if (record && json_object_object_get_ex(record, "source", &source) &&
                json_object_is_type(source, json_type_string)) {
                const char *src = json_object_get_string(source);
                if (!strncmp(src, prefix, prefixLength)) {
                    char *owned = strdup(src + prefixLength);
                    if (owned)
                        ItemListPush(list, owned);
                }
            }
        }
    }
    json_object_put(manifest);
}

CFArrayRef CopyGraphicsDirectoryItems(void) {
    EnsureBaseDir();
    U3CFItemList list = {0};
    CollectManifestImageNames(sAssetsBaseDir, "Resources/Graphics/", &list);
    // Also walk a literal directory, in case a future export layout (or a
    // hand-populated assets directory) puts real files there; harmless and
    // empty against the current export.
    const char *dir = (const char *)GraphicsDirectoryURL();
    if (dir)
        WalkDirectory(dir, "", &list, 8);
    return MakeArray(&list, true);
}

// Reads the real element count for a string table directly from
// manifest.json's "strings" record (each carries a "count" field already
// computed by the export pipeline). U3LinuxAssetString cannot be used alone
// to discover this: some tables contain JSON `null` placeholders for unused
// message slots, and U3LinuxAssetString returns NULL both for "index past
// end of table" and for "this slot is null" -- the two are ambiguous from
// that API alone, so probing until the first NULL would silently truncate
// any table with a null before its real end (confirmed present in the real
// exported assets, e.g. Messages.json[172]).
static CFIndex TableLength(const char *assetsDirectory, const char *table) {
    if (!assetsDirectory || !table)
        return 0;
    char manifestPath[PATH_MAX];
    snprintf(manifestPath, sizeof(manifestPath), "%s/manifest.json", assetsDirectory);
    struct json_object *manifest = json_object_from_file(manifestPath);
    if (!manifest)
        return 0;
    CFIndex result = 0;
    struct json_object *strings = NULL;
    if (json_object_object_get_ex(manifest, "strings", &strings) && json_object_is_type(strings, json_type_array)) {
        size_t n = json_object_array_length(strings);
        for (size_t i = 0; i < n; ++i) {
            struct json_object *record = json_object_array_get_idx(strings, i);
            struct json_object *name = NULL, *count = NULL;
            if (record && json_object_object_get_ex(record, "name", &name) &&
                json_object_is_type(name, json_type_string) && !strcmp(json_object_get_string(name), table) &&
                json_object_object_get_ex(record, "count", &count) && json_object_is_type(count, json_type_int)) {
                result = (CFIndex)json_object_get_int64(count);
                break;
            }
        }
    }
    json_object_put(manifest);
    return result;
}

static CFArrayRef FindTableCache(const char *name) {
    for (U3TableCache *cache = sTableCaches; cache; cache = cache->next) {
        if (!strcmp(cache->name, name))
            return cache->array;
    }
    return NULL;
}

CFArrayRef StringsArray(CFStringRef identifier) {
    if (!identifier)
        return NULL;
    const char *name = (const char *)identifier;
    CFArrayRef cached = FindTableCache(name);
    if (cached)
        return cached;

    EnsureBaseDir();
    U3LinuxAssets *assets = EnsureAssets();
    CFIndex total = TableLength(sAssetsBaseDir, name);

    U3CFItemList list = {0};
    for (CFIndex index = 0; index < total; ++index) {
        char *value = assets ? U3LinuxAssetString(assets, name, (size_t)index) : NULL;
        // A null table slot (or a missing asset context) becomes an empty
        // string, matching the `Str255 "\p"` empty-string convention this
        // codebase already uses for unused message slots.
        ItemListPush(&list, value ? value : strdup(""));
    }

    CFArrayRef array = MakeArray(&list, false); // cached: never released, like the macOS original.
    if (array) {
        U3TableCache *cache = calloc(1, sizeof(*cache));
        if (cache) {
            cache->name = strdup(name);
            cache->array = array;
            cache->next = sTableCaches;
            sTableCaches = cache;
        }
    }
    return array;
}

void GetPascalStringFromArrayByIndex(StringPtr pstringPtr, CFStringRef identifier, int index) {
    if (!pstringPtr)
        return;
    pstringPtr[0] = 0;
    if (!identifier || index < 0)
        return;
    CFArrayRef array = StringsArray(identifier);
    CFIndex count = CFArrayGetCount(array);
    if (index >= count) {
        fprintf(stderr, "U3LinuxCarbon: string index %d out of range (table '%s' has %ld entries)\n", index,
                (const char *)identifier, (long)count);
        return;
    }
    CFStringRef value = (CFStringRef)CFArrayGetValueAtIndex(array, index);
    if (!value)
        return;
    size_t length = StringLength(value);
    if (length > 255) {
        fprintf(stderr, "U3LinuxCarbon: truncating a %zu-byte table string to 255 bytes\n", length);
        length = 255;
    }
    pstringPtr[0] = (unsigned char)length;
    if (length)
        memcpy(pstringPtr + 1, value, length);
}

CFStringRef CopyAppVersionString(void) {
    static const char kVersion[] = "Ultima III (Linux port, development build)";
    return MakeOwnedString(kVersion, sizeof(kVersion) - 1);
}

Boolean GetSystemVersion(unsigned *majorVersion, unsigned *minorVersion, unsigned *bugFixVersion) {
    struct utsname info;
    unsigned major = 0, minor = 0, bugFix = 0;
    bool ok = false;
    if (uname(&info) == 0 && sscanf(info.release, "%u.%u.%u", &major, &minor, &bugFix) >= 2)
        ok = true;
    if (majorVersion)
        *majorVersion = ok ? major : 0;
    if (minorVersion)
        *minorVersion = ok ? minor : 0;
    if (bugFixVersion)
        *bugFixVersion = ok ? bugFix : 0;
    return ok;
}

// ---------------------------------------------------------------------------
// Screen bounds (SDL2)
// ---------------------------------------------------------------------------

void U3LinuxScreenBounds(Rect *rect) {
    if (!rect)
        return;
    short width = 1280, height = 800; // Reasonable fallback when no display is available.
    if (SDL_WasInit(SDL_INIT_VIDEO) || SDL_InitSubSystem(SDL_INIT_VIDEO) == 0) {
        SDL_DisplayMode mode;
        if (SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.w > 0 && mode.h > 0) {
            width = (short)mode.w;
            height = (short)mode.h;
        }
    }
    rect->left = 0;
    rect->top = 0;
    rect->right = width;
    rect->bottom = height;
}

// ---------------------------------------------------------------------------
// Timing (real clock, not busy loops)
// ---------------------------------------------------------------------------

static uint32_t MonotonicTicks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t millis = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    return (uint32_t)(millis * 60ULL / 1000ULL);
}

static void SleepTicks(uint32_t ticks) {
    if (ticks == 0)
        return;
    uint64_t nanos = (uint64_t)ticks * 1000000000ULL / 60ULL;
    struct timespec req;
    req.tv_sec = (time_t)(nanos / 1000000000ULL);
    req.tv_nsec = (long)(nanos % 1000000000ULL);
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
        // retry with the remaining time nanosleep wrote back into req.
    }
}

void Delay(uint32_t ticks, unsigned long *finalTicks) {
    SleepTicks(ticks);
    if (finalTicks)
        *finalTicks = MonotonicTicks();
}

Boolean WaitNextEvent(uint32_t mask, EventRecord *event, uint32_t ticks, RgnHandle region) {
    (void)mask;
    (void)region;
    U3CocoaPumpEvents();
    SleepTicks(ticks);
    U3CocoaPumpEvents();
    if (event) {
        event->what = nullEvent;
        event->message = 0;
        event->when = MonotonicTicks();
        event->where.h = 0;
        event->where.v = 0;
        event->modifiers = 0;
    }
    return false; // Honest: we never fabricate an event; none is pending.
}

OSErr AEProcessAppleEvent(const EventRecord *event) {
    (void)event;
    return noErr; // No AppleEvent manager exists; WaitNextEvent never
                   // produces kHighLevelEvent, so this is never reached in
                   // practice, but it must link and behave honestly.
}

Boolean InitializeAEHandlers(void) {
    return true; // No AppleEvent handlers exist to install; nothing failed.
}

void ExitToShell(void) {
    exit(0);
}

// ---------------------------------------------------------------------------
// Cursor
// ---------------------------------------------------------------------------

void InitCursor(void) {
}

void HideCursor(void) {
}

void ShowCursor(void) {
}

Boolean Button(void) {
    return false; // Honest: no real mouse-button state is tracked here;
                  // real input comes from U3PlatformGetKeyMouse elsewhere.
}

Boolean SetCursorNamed(CFStringRef cursorName, float scale) {
    (void)cursorName;
    (void)scale;
    return false; // Visual cursor assignment is owned by the (not yet
                   // written) SDL video layer; this function's one call site
                   // discards the result, so an honest "did nothing" is safe.
}

// ---------------------------------------------------------------------------
// Menus, dialogs, controls, windows (all already stubbed to nil/no-op in
// CarbonShunts.c for GetNewMBar/GetMenuHandle/GetNewDialog/NewCWindow, so
// these are never reached with a live menu/dialog/window in practice; they
// stay internally consistent and honest regardless).
// ---------------------------------------------------------------------------

static bool sMenuBarVisible = true;

Boolean IsMenuBarVisible(void) {
    return sMenuBarVisible;
}

void HideMenuBar(void) {
    sMenuBarVisible = false;
}

void ShowMenuBar(void) {
    sMenuBarVisible = true;
}

void CheckMenuItem(MenuRef menu, short item, Boolean checked) {
    (void)menu;
    (void)item;
    (void)checked;
}

short CountMenuItems(MenuRef menu) {
    (void)menu;
    return 0;
}

void AppendMenuItemTextWithCFString(MenuRef menu, CFStringRef text, uint32_t attributes, uint32_t command,
                                    MenuItemIndex *index) {
    (void)menu;
    (void)text;
    (void)attributes;
    (void)command;
    if (index)
        *index = 0;
}

long MenuSelect(Point point) {
    (void)point;
    return 0; // Honest "nothing chosen": no native menu exists to select from.
}

void HiliteMenu(short menu) {
    (void)menu;
}

void HiliteControl(ControlRef control, short part) {
    (void)control;
    (void)part;
}

void SetControlValue(ControlRef control, short value) {
    (void)control;
    (void)value;
}

WindowRef GetDialogWindow(DialogRef dialog) {
    return (WindowRef)dialog; // A DialogRef is-a WindowRef; always nil today
                               // since GetNewDialog always returns nil.
}

void GetDialogItem(DialogRef dialog, short item, short *type, Handle *handle, Rect *bounds) {
    (void)dialog;
    (void)item;
    if (type)
        *type = 0;
    if (handle)
        *handle = NULL;
    if (bounds) {
        bounds->top = 0;
        bounds->left = 0;
        bounds->bottom = 0;
        bounds->right = 0;
    }
}

void GetDialogItemAsControl(DialogRef dialog, short item, ControlRef *control) {
    (void)dialog;
    (void)item;
    if (control)
        *control = NULL;
}

short FindWindow(Point point, WindowRef *window) {
    (void)point;
    if (window)
        *window = NULL;
    return 0; // inDesk: honestly, no window exists to hit-test against.
}

void DragWindow(WindowRef window, Point point, const Rect *bounds) {
    (void)window;
    (void)point;
    (void)bounds;
}

void TransitionWindow(WindowRef window, int effect, int action, const Rect *bounds) {
    (void)window;
    (void)effect;
    (void)action;
    (void)bounds;
}

void ParamText(ConstStr255Param a, ConstStr255Param b, ConstStr255Param c, ConstStr255Param d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d; // StandardAlert/Alert already ignore their text parameters in
             // CarbonShunts.c, so there is nothing for substituted text to
             // affect; stored state would never be read.
}

CTabHandle GetCTable(short id) {
    (void)id;
    return NULL; // No Mac-style indexed color tables exist; NewGWorld in
                 // CarbonShunts.c already ignores its cTable parameter.
}

OSErr ResError(void) {
    return noErr; // No resource manager exists to have failed.
}

void SetRefMenuIcons(MenuRef theMenu) {
    (void)theMenu; // gRefMenu is always nil (GetMenuHandle always returns
                    // nil), so MenuBarInit's real-menu branch that calls this
                    // is unreachable; nothing to do.
}

void SetMusicPortAndDevice(CGrafPtr thePort, GDHandle theDevice) {
    (void)thePort;
    (void)theDevice; // Matches the macOS original in UltimaSound.m, whose
                       // body is empty too.
}

void GameOptionsDialog(void) {
    // No native options dialog exists on Linux; honest no-op.
}

void LWOpenURL(CFStringRef urlString) {
    if (!urlString || !*(const char *)urlString)
        return;
    // Double-fork so the browser process is reparented to init and we never
    // leave a zombie, without touching the program's global SIGCHLD
    // disposition (which other code may rely on).
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            execlp("xdg-open", "xdg-open", (const char *)urlString, (char *)NULL);
            _exit(127); // xdg-open unavailable; best effort only, matching
                        // the macOS original's fire-and-forget NSWorkspace call.
        }
        _exit(0);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0); // Reaps the short-lived middle process only.
    }
}
