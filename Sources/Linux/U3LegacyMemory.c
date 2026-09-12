#include "U3LegacyTypes.h"
#include <ctype.h>
#include <errno.h>

/* The first member is the pointer exposed as a legacy Handle. The allocation
 * remains stationary: the C game never needs classic Mac heap compaction. */
typedef struct {
    char *bytes;
    Size size;
} U3Handle;

static OSErr lastMemoryError;

Handle NewHandle(Size size) {
    lastMemoryError = noErr;
    if (size < 0) {
        lastMemoryError = paramErr;
        return NULL;
    }
    U3Handle *handle = calloc(1, sizeof(*handle));
    if (handle) {
        handle->bytes = malloc(size ? (size_t)size : 1);
        handle->size = size;
        if (!handle->bytes) {
            free(handle);
            handle = NULL;
        }
    }
    if (!handle) lastMemoryError = memFullErr;
    return handle ? &handle->bytes : NULL;
}

Handle NewHandleClear(Size size) {
    Handle handle = NewHandle(size);
    if (handle) memset(*handle, 0, (size_t)size);
    return handle;
}

void DisposeHandle(Handle handle) {
    if (!handle) return;
    free(*handle);
    free(handle);
}

Size GetHandleSize(Handle handle) {
    return handle ? ((const U3Handle *)handle)->size : 0;
}

OSErr MemError(void) { return lastMemoryError; }

void StringToNum(ConstStr255Param text, long *value) {
    if (!value) return;
    *value = 0;
    if (!text) return;
    char buffer[256];
    memcpy(buffer, text + 1, text[0]);
    buffer[text[0]] = 0;
    char *end;
    errno = 0;
    long parsed = strtol(buffer, &end, 10);
    if (end != buffer && errno != ERANGE) *value = parsed;
}

Boolean EqualString(ConstStr255Param a, ConstStr255Param b, Boolean sensitive, Boolean diacritic) {
    (void)diacritic; /* Game command matching uses ASCII, not locale collation. */
    if (!a || !b) return a == b;
    if (a[0] != b[0]) return false;
    for (unsigned i = 1; i <= a[0]; ++i) {
        unsigned char ac = a[i], bc = b[i];
        if (sensitive ? ac != bc : tolower(ac) != tolower(bc)) return false;
    }
    return true;
}

void SetRect(Rect *r, short left, short top, short right, short bottom) {
    *r = (Rect){top, left, bottom, right};
}
void OffsetRect(Rect *r, short dx, short dy) {
    r->left += dx; r->right += dx; r->top += dy; r->bottom += dy;
}
void InsetRect(Rect *r, short dx, short dy) {
    r->left += dx; r->right -= dx; r->top += dy; r->bottom -= dy;
}
Boolean PtInRect(Point p, const Rect *r) {
    return p.h >= r->left && p.h < r->right && p.v >= r->top && p.v < r->bottom;
}
Boolean EqualPt(Point a, Point b) { return a.h == b.h && a.v == b.v; }
Boolean EqualRect(const Rect *a, const Rect *b) {
    return a->top == b->top && a->left == b->left &&
           a->bottom == b->bottom && a->right == b->right;
}
Boolean SectRect(const Rect *a, const Rect *b, Rect *out) {
    Rect result = {a->top > b->top ? a->top : b->top,
                   a->left > b->left ? a->left : b->left,
                   a->bottom < b->bottom ? a->bottom : b->bottom,
                   a->right < b->right ? a->right : b->right};
    if (result.left >= result.right || result.top >= result.bottom) {
        *out = (Rect){0};
        return false;
    }
    *out = result;
    return true;
}
void UnionRect(const Rect *a, const Rect *b, Rect *out) {
    Rect result = {a->top < b->top ? a->top : b->top,
                   a->left < b->left ? a->left : b->left,
                   a->bottom > b->bottom ? a->bottom : b->bottom,
                   a->right > b->right ? a->right : b->right};
    *out = result;
}
