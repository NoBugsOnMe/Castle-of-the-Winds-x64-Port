/*
 * win16.h - common declarations for the Win16 -> Win32 API layer.
 */
#ifndef WIN16_H
#define WIN16_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "cpu.h"
#include "mem.h"
#include "ne.h"

/* ---- API table ---------------------------------------------------------- */

typedef struct ApiEntry {
    const char *module;
    u16 ordinal;
    const char *name;
    u16 argsize;
    void (*fn)(void);
} ApiEntry;

extern const ApiEntry g_api_table[];
extern int g_api_argsz;
extern int g_api_current;   /* index of the API being executed */

/* argument access inside an API handler (Pascal calling convention:
   first parameter at the highest address) */
#define ARGS() const int _argsz = g_api_argsz; u16 _ao = (u16)(SP + 4 + _argsz); (void)_ao
#define A16()  (_ao = (u16)(_ao - 2), rd16(g_cpu.base[S_SS] + _ao))
#define AS16() ((s16)A16())
#define A32()  (_ao = (u16)(_ao - 4), rd32(g_cpu.base[S_SS] + _ao))
void api_return(int argsz);
#define RET0()    api_return(_argsz)
#define RET16(v)  do { u16 _r = (u16)(v); AX = _r; api_return(_argsz); } while (0)
#define RET32(v)  do { u32 _r = (u32)(v); AX = (u16)_r; DX = (u16)(_r >> 16); api_return(_argsz); } while (0)

/* ---- calling 16-bit code -------------------------------------------------- */

u32  call16(u32 fn, u16 ax, int nwords, const u16 *words);
u32  call16_wndproc(u32 proc, u16 hinst, u16 hwnd, u16 msg, u16 wp, u32 lp);

/* ---- diagnostics ------------------------------------------------------------ */

extern int g_log_enabled;
void emu_log(const char *fmt, ...);
void emu_trace_api(const char *fmt, ...);
void cpu_fatal(const char *fmt, ...);
void emu_shutdown(int code);

/* ---- handles ------------------------------------------------------------------ */

/* Two-way map between native handles and 16-bit handle values. */
#define HMAP_HASH 131072
typedef struct HMap {
    void *h[65536];              /* id -> native handle */
    void *key[HMAP_HASH];        /* hash: native handle -> id */
    u16   val[HMAP_HASH];
    u16   next;                  /* next id to try */
    int   kind;                  /* 0 hwnd, 1 gdi, 2 menu, 3 icon */
    int   count;
} HMap;
extern HMap g_hwnd_map, g_gdi_map, g_menu_map, g_icon_map;

u16   hmap16(HMap *m, void *h);
void *hmap32(HMap *m, u16 id);
void  hmap_remove(HMap *m, void *h);
void  hmaps_init(void);

#define HWND_16(h)   hmap16(&g_hwnd_map, (void *)(h))
#define HWND_32(h)   ((HWND)hmap32(&g_hwnd_map, (u16)(h)))
#define HGDI_16(h)   hmap16(&g_gdi_map, (void *)(h))
#define HGDI_32(h)   ((HGDIOBJ)hmap32(&g_gdi_map, (u16)(h)))
#define HDC_16(h)    HGDI_16(h)
#define HDC_32(h)    ((HDC)HGDI_32(h))
#define HMENU_16(h)  hmap16(&g_menu_map, (void *)(h))
#define HMENU_32(h)  ((HMENU)hmap32(&g_menu_map, (u16)(h)))
#define HICON_16(h)  hmap16(&g_icon_map, (void *)(h))
#define HICON_32(h)  ((HICON)hmap32(&g_icon_map, (u16)(h)))

HBRUSH brush_32(u16 h);      /* handles COLOR_xxx+1 pseudo brushes */

/* ---- 16-bit structures ----------------------------------------------------- */

#pragma pack(push, 1)
typedef struct { s16 left, top, right, bottom; } RECT16;
typedef struct { s16 x, y; } POINT16;
typedef struct { u16 hwnd; u16 message; u16 wParam; u32 lParam; u32 time; POINT16 pt; } MSG16;
typedef struct {
    u16 style; u32 lpfnWndProc; s16 cbClsExtra; s16 cbWndExtra; u16 hInstance;
    u16 hIcon; u16 hCursor; u16 hbrBackground; u32 lpszMenuName; u32 lpszClassName;
} WNDCLASS16;
typedef struct { u16 hdc; s16 fErase; RECT16 rcPaint; s16 fRestore; s16 fIncUpdate; u8 rgbReserved[16]; } PAINTSTRUCT16;
typedef struct {
    u32 lpCreateParams; u16 hInstance; u16 hMenu; u16 hwndParent;
    s16 cy, cx, y, x; u32 style; u32 lpszName; u32 lpszClass; u32 dwExStyle;
} CREATESTRUCT16;
typedef struct {
    s16 lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    u8 lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
    char lfFaceName[32];
} LOGFONT16;
typedef struct {
    s16 tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading, tmAveCharWidth, tmMaxCharWidth, tmWeight;
    u8 tmItalic, tmUnderlined, tmStruckOut, tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar, tmPitchAndFamily, tmCharSet;
    s16 tmOverhang, tmDigitizedAspectX, tmDigitizedAspectY;
} TEXTMETRIC16;
typedef struct { s16 bmType, bmWidth, bmHeight, bmWidthBytes; u8 bmPlanes, bmBitsPixel; u32 bmBits; } BITMAP16;
typedef struct { u16 lbStyle; u32 lbColor; s16 lbHatch; } LOGBRUSH16;
typedef struct { u16 lopnStyle; POINT16 lopnWidth; u32 lopnColor; } LOGPEN16;
typedef struct { POINT16 ptReserved, ptMaxSize, ptMaxPosition, ptMinTrackSize, ptMaxTrackSize; } MINMAXINFO16;
typedef struct { u16 hwnd, hwndInsertAfter; s16 x, y, cx, cy; u16 flags; } WINDOWPOS16;
typedef struct { RECT16 rgrc[3]; u32 lppos; } NCCALCSIZE_PARAMS16;
typedef struct { u16 CtlType, CtlID, itemID, itemAction, itemState, hwndItem, hDC; RECT16 rcItem; u32 itemData; } DRAWITEMSTRUCT16;
typedef struct { u16 CtlType, CtlID, itemID, itemWidth, itemHeight; u32 itemData; } MEASUREITEMSTRUCT16;
typedef struct { u16 CtlType, CtlID, hwndItem, itemID1; u32 itemData1; u16 itemID2; u32 itemData2; } COMPAREITEMSTRUCT16;
typedef struct { u16 CtlType, CtlID, itemID, hwndItem; u32 itemData; } DELETEITEMSTRUCT16;
typedef struct { u8 cBytes, fFixedDisk; u16 nErrCode; u8 reserved[4]; char szPathName[128]; } OFSTRUCT16;
typedef struct {
    u32 lStructSize; u16 hwndOwner; u16 hInstance; u32 lpstrFilter; u32 lpstrCustomFilter;
    u32 nMaxCustFilter; u32 nFilterIndex; u32 lpstrFile; u32 nMaxFile; u32 lpstrFileTitle;
    u32 nMaxFileTitle; u32 lpstrInitialDir; u32 lpstrTitle; u32 Flags; u16 nFileOffset;
    u16 nFileExtension; u32 lpstrDefExt; u32 lCustData; u32 lpfnHook; u32 lpTemplateName;
} OPENFILENAME16;
#pragma pack(pop)

static inline void rect16to32(const RECT16 *a, RECT *b) { b->left = a->left; b->top = a->top; b->right = a->right; b->bottom = a->bottom; }
static inline void rect32to16(const RECT *a, RECT16 *b) { b->left = (s16)a->left; b->top = (s16)a->top; b->right = (s16)a->right; b->bottom = (s16)a->bottom; }

/* ---- USER internals (user.c / msg.c) ----------------------------------------- */

typedef struct ZSurf ZSurf;
typedef struct WndInfo {
    HWND hwnd;
    u32  proc16;          /* 16-bit window procedure (0 = none) */
    WNDPROC native_old;   /* original native proc for subclassed native windows */
    u32  dlgproc16;       /* 16-bit dialog procedure */
    int  is_dialog;
    ZSurf *zs;            /* zoom backing surface (zoom.c) */
    HFONT font16;         /* font the game set on a native control (unscaled) */
} WndInfo;

WndInfo *wnd_info(HWND hwnd, int create);
void     wnd_forget(HWND hwnd);
LRESULT CALLBACK bridge_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
INT_PTR CALLBACK bridge_dlgproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

u32  native_proc_stub(WNDPROC proc);     /* 16-bit callable thunk for a native wndproc */
WNDPROC stub_native_proc(u32 segptr);    /* reverse; NULL if not a stub */

/* message conversion (msg.c) */
LRESULT send_to_16(u32 proc16, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, int is_dlg);
u32  send_to_32(WNDPROC proc, HWND hwnd, u16 msg16, u16 wp16, u32 lp16, int defproc);
u32  send_message_16(u16 hwnd16, u16 msg16, u16 wp16, u32 lp16, int post);
int  msg_is_known16(UINT msg);
void msg32_to_msg16(const MSG *m, MSG16 *m16);
void msg16_to_msg32(const MSG16 *m16, MSG *m);
const char *msg_name(UINT msg);

/* resources (res.c) */
typedef struct { u16 ctl_id; u16 icon_id; char icon_name[32]; } DlgIconFix;
extern DlgIconFix g_dlg_icon_fix[32];
extern int g_dlg_nicon_fix;
extern u16 g_dlg_menu_id;
extern char g_dlg_menu_name[64];
typedef struct { u8 b[124 + 256 * 4]; } DibVgaBuf;
const void *dib_vga(const void *hdr, UINT usage, DibVgaBuf *buf);
void dib_vga_inplace(void *hdr);
HICON create_icon16(int w, int h, int planes, int bpp, const u8 *andbits, const u8 *xorbits);
HICON  load_icon_res(u32 name, int cursor);
HBITMAP load_bitmap_res(u32 name);
HMENU  load_menu_res(u32 name);
HMENU  build_menu(const u8 *data, u32 len);
void  *convert_dialog(const u8 *data, u32 len, u32 *outlen);
HBITMAP dib_to_bitmap(const BITMAPINFOHEADER *bih, const void *bits, UINT usage, HDC hdc);

/* ---- zoom (zoom.c) ------------------------------------------------------------ */

#define ZOOM_MAX 6
extern int g_zoom;                 /* scale factor S; 1 = off */
extern int g_zoom_setting;         /* 0 = automatic */
static inline int zfloor(int v) { return v >= 0 ? v / g_zoom : -((-v + g_zoom - 1) / g_zoom); }
static inline int zceil(int v) { return v >= 0 ? (v + g_zoom - 1) / g_zoom : -((-v) / g_zoom); }
void zoom_init(void);
int  zoom_auto_factor(void);
void zoom_nc(HWND h, RECT *nc);
void zoom_nc_style(DWORD style, DWORD exstyle, BOOL menu, RECT *nc);
void zoom_win_to_logical(const RECT *nc, int *x, int *y, int *cx, int *cy);
void zoom_win_to_real(const RECT *nc, int *x, int *y, int *cx, int *cy);
void zoom_window_rect(HWND h, RECT *out);
int  zoom_window_has_surface(HWND h);
void zoom_surface_free(HWND h);
void zoom_flush_all(void);
int debug_dump_ds(void);

/* the player's state, read from the game's variables (patch.c) */
typedef struct {
    int ok, has_time, has_mons;
    u16 mode, px, py, vx, vy, vx3, vy3, hp, hpmax, mp, mpmax, xplo, xphi, level, diff, kconst, time;
    u16 mlist;                      /* handle of the level's monster list */
} GameVars;
extern GameVars g_gv;
typedef struct {
    int in_game;
    int hp, hpmax, mp, mpmax, level, x, y, vx, vy, mode;
    u32 xp, next, clock;
} PlayerState;
int game_read_player(PlayerState *st);
/* the monsters of the current level */
typedef struct {
    u16 id;                         /* the monster's local handle */
    u16 ptr;                        /* its block in DGROUP */
    int x, y, hp, hpmax;
    int seen;                       /* drawn on the map (visible to the player) */
} MonsterState;
int game_read_monsters(MonsterState *m, int max);
int game_monster_visible(const MonsterState *m);

/* hud.c */
extern int g_hud_bars, g_hud_title, g_hud_enemy;
void hud_init(void);
void hud_set_map(HWND h);
void hud_map_class(ATOM a);
void hud_set_main(HWND h);
void hud_window_destroyed(HWND h);
int hud_wants_surface(HWND h);
int hud_is_main(HWND h);
void hud_game_title(HWND h, const char *s);
const char *hud_game_title_text(void);
void hud_settings_changed(void);
void hud_poll(void);
int hud_take_bars(HWND h, RECT *r);
void hud_overlay(HWND h, HDC dc);
void zoom_redraw_logical(HWND h, const RECT *r);
void zoom_flush_drawing(DWORD min_age);
extern int g_upscale;
int zoom_upscale_mode(void);
int zoom_scale_pixels(const u32 *src, int w, int h, u32 *dst, int s, int mode, u32 mask);
HDC  zoom_getdc(HWND h);
int  zoom_releasedc(HWND h, HDC dc);
HDC  zoom_beginpaint(HWND h, PAINTSTRUCT *ps, RECT *lpaint);
void zoom_endpaint(HWND h, HDC dc, PAINTSTRUCT *ps);
HDC  zoom_erase_begin(HWND h, HDC real, int *level);
void zoom_erase_end(HWND h, HDC mem, int level);
HDC  zoom_drawitem_begin(HWND h, HDC real, RECT *item_real);
void zoom_drawitem_end(HDC mem);
void zoom_scroll(HWND h, int dx, int dy, const RECT *lrc, const RECT *lclip);
HICON zoom_icon(HICON src, BOOL cursor);
HICON zoom_icon_orig(HICON scaled);
void zoom_icon_destroyed(HICON src);
void zoom_mark_system_cursor(HCURSOR h);
HFONT zoom_font(HFONT src);
HFONT zoom_unscale_font(HFONT f);
void zoom_dialog_font(int *points, int *weight, char *face, int facelen, int *had_font);
void wdc_drawn(u16 dc16, const RECT *r);     /* GDI layer: something was drawn */
int  wdc_dark(u16 dc16);                      /* recolour drawing on this DC? */
COLORREF wdc_set_text(u16 dc16, COLORREF c);
COLORREF wdc_set_bk(u16 dc16, COLORREF c);
int  wdc_game_colors(u16 dc16, COLORREF *text, COLORREF *bk);
void wdc_temp_begin(HWND h, HDC dc);
void wdc_temp_end(HDC dc);

/* ---- dark mode (dark.c) ------------------------------------------------------------ */

extern int g_dark;                 /* active */
extern int g_dark_dim_on;          /* dim the map views in dark mode */
int  dark_dim_active(void);
const u8 *dark_dim_lut(void);
void zoom_surfaces_reset(void);
extern int g_dark_setting;         /* -1 follow Windows, 0 off, 1 on */
void dark_init(void);
int  dark_update(void);
COLORREF dark_bg(COLORREF c);
COLORREF dark_fg(COLORREF c);
COLORREF dark_syscolor(int i);
HBRUSH dark_solid_brush(COLORREF c);
HBRUSH dark_brush_for(HBRUSH b);
enum { DARK_UI = 1, DARK_MAP = 2 };
int  dark_policy_for(HWND h);
COLORREF dark_bg_p(int policy, COLORREF c);
COLORREF dark_fg_p(int policy, COLORREF c);
HBRUSH dark_brush_p(int policy, HBRUSH b);
HBRUSH dark_class_brush(u16 b16);
HPEN dark_pen_for(HPEN p);
void dark_object_deleted(HGDIOBJ o);
void dark_apply_window(HWND h);
int  dark_menubar_msg(HWND h, UINT msg, WPARAM wp, LPARAM lp, LRESULT *res);
void dark_menubar_after_ncpaint(HWND h);
void dark_paint_button(HWND h, HDC dc);
void dark_subclass_button(HWND h);
void dark_child_ncactivate(HWND h, int active);
void dark_child_caption(HWND h);

int  port_message_box(HWND owner, const char *text, const char *caption, UINT type);   /* msgbox.c */

/* user.c helpers */
void user_window_sync(HWND h, int fonts);
int  user_is_game_window(HWND h);
int  user_window_light_bg(HWND h);
int  wdc_is_window_dc(u16 dc16);
HWND wdc_window(u16 dc16);

/* misc */
extern HINSTANCE g_hinst32;
extern char g_game_dir[MAX_PATH];
extern char g_data_dir[MAX_PATH];
int path_for_win16(const WCHAR *path, char *out, int maxlen);
extern char g_ini_path[MAX_PATH];
extern char g_exe_name[64];         /* e.g. CASTLE1.EXE */
void   help_show(HWND owner, const char *file, u16 cmd, u32 data);
const char *map_ini_name(const char *name, char *buf, int buflen);
const char *map_data_path(const char *name, char *buf, int buflen);

#endif
