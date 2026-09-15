#include "Linux/U3LegacyTypes.h"
#include "CarbonShunts.h"
#include <assert.h>

int main(void) {
    assert(!NewHandle(-1));
    assert(MemError() == paramErr);
    Handle map = NewHandleClear(4096);
    assert(map && MemError() == noErr && GetHandleSize(map) == 4096);
    for (int i = 0; i < 4096; ++i) assert((*map)[i] == 0);
    (*map)[4095] = 42;
    assert((*map)[4095] == 42);
    DisposeHandle(map);
    Handle empty = NewHandle(0);
    assert(empty && GetHandleSize(empty) == 0);
    DisposeHandle(empty);
    DisposeHandle(NULL);
    assert(GetHandleSize(NULL) == 0);

    /* Clang must emit an actual length byte for original Pascal literals. */
    unsigned char text[] = "\p-123";
    assert(text[0] == 4 && text[1] == '-');
    long number;
    StringToNum(text, &number);
    assert(number == -123);
    StringToNum((unsigned char *)"\pno", &number);
    assert(number == 0);
    assert(EqualString((unsigned char *)"\pLord", (unsigned char *)"\plord", false, false));
    assert(!EqualString((unsigned char *)"\pLord", (unsigned char *)"\plord", true, false));

    Rect a = {0, 0, 10, 10}, b = {5, 5, 15, 15};
    assert(PtInRect((Point){0, 0}, &a));
    assert(!PtInRect((Point){10, 9}, &a));
    assert(SectRect(&a, &b, &a)); /* Aliasing is used by legacy draw code. */
    assert(EqualRect(&a, &(Rect){5, 5, 10, 10}));
    assert(!SectRect(&a, &(Rect){10, 10, 20, 20}, &a));
    assert(EqualRect(&a, &(Rect){0}));
    puts("legacy memory, Pascal string, and geometry tests passed");
    return 0;
}
