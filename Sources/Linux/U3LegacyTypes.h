/* Compatibility vocabulary for the preserved C game and software renderer.
 * These are Linux-owned types, not binary-compatible Apple framework objects.
 * Do not use them to serialize game state. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#define pascal
#define nil NULL
#define TRUE true
#define FALSE false
#define DEBUGGING 1
#define TARGET_CPU_68K 0
#define TARGET_CPU_PPC 0
#define TARGET_RT_LITTLE_ENDIAN 1
#define CFSTR(s) (s)

typedef bool Boolean;
typedef int8_t SignedByte;
typedef uint8_t UInt8;
typedef uint16_t UInt16;
typedef int16_t SInt16;
typedef uint32_t UInt32;
typedef int32_t SInt32;
typedef int64_t SInt64;
typedef uint64_t UInt64;
typedef ptrdiff_t Size;
typedef int32_t OSErr, OSStatus;
typedef uint32_t OSType, ResType, MenuCommand;
typedef int16_t ScriptCode, MenuID, ThemeFontID, ThemeDrawState, AlertType;
typedef uint16_t MenuItemIndex;
typedef char *Ptr, **Handle;
typedef unsigned char Str255[256], Str63[64], Str32[33], Str31[32], Str15[16];
typedef unsigned char *StringPtr;
typedef const unsigned char *ConstStr255Param, *ConstStringPtr;
typedef Handle StringHandle, PicHandle, PolyHandle, CursHandle;
typedef const char *CFStringRef, *CFURLRef;
typedef const void *CFTypeRef;
typedef void *CFArrayRef, *CFMutableArrayRef, *CFMutableDictionaryRef, *CFNumberRef;
typedef const void *CFDictionaryRef;
typedef int boolean_t, CGDisplayErr;
typedef intptr_t CFIndex;
typedef uint32_t CFStringEncoding;
typedef struct { CFIndex location, length; } CFRange;
typedef struct { short v, h; } Point;
typedef struct { short top, left, bottom, right; } Rect;
typedef struct { UInt16 red, green, blue; } RGBColor;
typedef struct { Ptr baseAddr; short rowBytes; Rect bounds;
    short pixelSize, pixelType, cmpCount, cmpSize; int32_t hRes, vRes; } PixMap, BitMap;
typedef PixMap *PixMapPtr, **PixMapHandle;
typedef struct { PixMapHandle gdPMap; Rect gdRect; } GDevice, **GDHandle;
typedef void *CGrafPtr, *GrafPtr, *GWorldPtr, *WindowPtr, *WindowRef;
typedef void *DialogPtr, *DialogRef, *ControlRef, *ControlHandle;
typedef void *MenuRef, *MenuHandle, *MenuBarHandle, *MCTableHandle;
typedef short DialogItemType;
typedef struct { Boolean goAwayFlag; } *WindowPeek;
typedef Rect **RgnHandle;
typedef void *CTabHandle, *CGContextRef, *ComponentInstance;
typedef uint32_t GWorldFlags;
typedef struct { short vRefNum; int32_t parID; Str63 name; } FSSpec;
typedef struct { unsigned char opaque[80]; } FSRef;
typedef struct { short what; uintptr_t message; uint32_t when;
    Point where; uint16_t modifiers; } EventRecord;
typedef Boolean (*ModalFilterUPP)(DialogPtr, EventRecord *, short *);
typedef ModalFilterUPP UniversalProcPtr;
typedef struct { Boolean movable, helpButton; ModalFilterUPP filterProc;
    StringPtr defaultText, cancelText, otherText; short defaultButton, cancelButton, position;
} AlertStdAlertParamRec;

enum { noErr=0, paramErr=-50, memFullErr=-108, resNotFound=-192, fnfErr=-43, eofErr=-39 };
enum { nullEvent=0, mouseDown=1, mouseUp=2, keyDown=3, keyUp=4, autoKey=5, updateEvt=6,
    app4Evt=15,
    activateEvt=8, osEvt=15, kHighLevelEvent=23, everyEvent=0xffff,
    mDownMask=2, mUpMask=4, keyDownMask=8, keyUpMask=16, autoKeyMask=32,
    charCodeMask=0xff, keyCodeMask=0xff00, cmdKey=256, shiftKey=512,
    optionKey=2048, controlKey=4096, activeFlag=1 };
enum { srcCopy=0, patCopy=8, srcXor=2, notSrcCopy=4, transparent=36,
    normal=0, bold=1, italic=2, underline=4, systemFont=0, applFont=1,
    kAlertStopAlert=0, kAlertNoteAlert=1, kAlertCautionAlert=2,
    kAlertStdAlertOKButton=1, kWindowDefaultPosition=0,
    kHICommandPreferences=0x70726566, kCFStringEncodingMacRoman=0,
    kCFStringEncodingUTF8=0x08000100 };
enum { kThemeCurrentPortFont=0, kThemeStateActive=1, teFlushDefault=0,
    inMenuBar=1, inSysWindow=2, inDrag=4, inGoAway=6, kControlButtonPart=10,
    kCFCompareEqualTo=0, kCFNotFound=-1, kCFNumberIntType=9,
    kCGDirectMainDisplay=0, kCGErrorSuccess=0, gestaltAddressingModeAttr=0,
    kWindowHideTransitionAction=0, plainDBox=2,
    kWindowFullZoomDocumentProc=8, kWindowPlainDialogProc=2 };
#define kCFAllocatorDefault NULL
#define kCGDisplayWidth "width"
#define kCGDisplayHeight "height"

/* Declared individually so missing Linux implementations fail at link time. */
Handle NewHandle(Size size);
Handle NewHandleClear(Size size);
void DisposeHandle(Handle handle);
Size GetHandleSize(Handle handle);
OSErr MemError(void);
OSErr ResError(void);
void StringToNum(ConstStr255Param text, long *value);
Boolean EqualString(ConstStr255Param a, ConstStr255Param b, Boolean sensitive, Boolean diacritic);
void InitCursor(void);
void HideCursor(void);
void ShowCursor(void);
void ObscureCursor(void);
Boolean Button(void);
Boolean PtInRect(Point point, const Rect *rect);
Boolean EqualPt(Point a, Point b);
Boolean EqualRect(const Rect *a, const Rect *b);
Boolean SectRect(const Rect *a, const Rect *b, Rect *out);
void UnionRect(const Rect *a, const Rect *b, Rect *out);
uint32_t TickCount(void);
void Delay(uint32_t ticks, unsigned long *finalTicks);
Boolean WaitNextEvent(uint32_t mask, EventRecord *event, uint32_t ticks, RgnHandle region);
OSErr AEProcessAppleEvent(const EventRecord *event);
void ExitToShell(void);
#define NewModalFilterUPP(f) (f)
#define LoWord(v) ((int16_t)((uintptr_t)(v) & 0xffff))
#define HiWord(v) ((int16_t)(((uintptr_t)(v) >> 16) & 0xffff))
WindowRef GetDialogWindow(DialogRef dialog);
void HiliteControl(ControlRef control, short part);
void GetDialogItem(DialogRef dialog, short item, short *type, Handle *handle, Rect *bounds);
void GetDialogItemAsControl(DialogRef dialog, short item, ControlRef *control);
short FindWindow(Point point, WindowRef *window);
void DragWindow(WindowRef window, Point point, const Rect *bounds);
void CheckMenuItem(MenuRef menu, short item, Boolean checked);
void ParamText(ConstStr255Param a, ConstStr255Param b, ConstStr255Param c, ConstStr255Param d);
long MenuSelect(Point point);
void HiliteMenu(short menu);
void SystemClick(const EventRecord *event, WindowRef window);
void SetControlValue(ControlRef control, short value);
void SetWTitle(WindowRef window, ConstStr255Param title);
short CountMenuItems(MenuRef menu);
void AppendMenuItemTextWithCFString(MenuRef menu, CFStringRef text, uint32_t attributes,
                                  uint32_t command, MenuItemIndex *index);
Boolean IsMenuBarVisible(void);
void LMSetMBarHeight(short height);
void HideMenuBar(void);
void ShowMenuBar(void);
void TransitionWindow(WindowRef window, int effect, int action, const Rect *bounds);
void InitDialogs(void *resume);
void InitFonts(void);
void InitGraf(void *port);
void InitMenus(void);
void InitWindows(void);
void TEInit(void);
void MaxApplZone(void);
void GetDateTime(unsigned long *seconds);
extern struct U3LegacyQD { GrafPtr thePort; int32_t randSeed; } qd;
WindowRef GetNewCWindow(short id, void *storage, WindowRef behind);
CTabHandle GetCTable(short id);
OSErr Gestalt(uint32_t selector, long *response);
CFIndex CFArrayGetCount(CFArrayRef array);
const void *CFArrayGetValueAtIndex(CFArrayRef array, CFIndex index);
const void *CFDictionaryGetValue(CFDictionaryRef dict, const void *key);
Boolean CFNumberGetValue(CFNumberRef number, int type, void *value);
CFRange CFRangeMake(CFIndex location, CFIndex length);
void CFRelease(const void *value);
const void *CFRetain(const void *value);
int CFStringCompare(CFStringRef a, CFStringRef b, unsigned options);
CFStringRef CFStringCreateWithPascalString(void *allocator, ConstStr255Param text, CFStringEncoding encoding);
CFStringRef CFStringCreateWithSubstring(void *allocator, CFStringRef text, CFRange range);
CFRange CFStringFind(CFStringRef text, CFStringRef needle, unsigned options);
CFIndex CFStringGetLength(CFStringRef text);
ConstStringPtr CFStringGetPascalStringPtr(CFStringRef text, CFStringEncoding encoding);
CFStringEncoding CFStringGetSystemEncoding(void);
Boolean CFStringHasPrefix(CFStringRef text, CFStringRef prefix);
CFURLRef CFURLCreateCopyAppendingPathComponent(void *allocator, CFURLRef base, CFStringRef component, Boolean directory);

/* Obsolete display-mode API is excluded from Linux sources, not emulated. */

#include "U3LegacyDrawing.h"
