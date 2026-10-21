/*
 * msg.c - window message translation between Win32 and Win16 conventions.
 */
#include "win16.h"

#define WM16_CTLCOLOR 0x0019
#define WM16_USER     0x0400

/* active Win32 -> Win16 deliveries, used so that DefWindowProc16 and
 * CallWindowProc16 can pass the original Win32 parameters through */
typedef struct MsgCtx {
    HWND hwnd;
    UINT msg;
    WPARAM wp;
    LPARAM lp;
    u16 msg16, wp16;
    u32 lp16;
    int kind;            /* structure kind */
    int passthrough;     /* set when the original params were handed to native code */
    union {              /* converted structure as first handed to the 16-bit code (zoom merge) */
        WINDOWPOS16 wpos;
        MINMAXINFO16 mmi;
    } orig;
    struct MsgCtx *prev;
} MsgCtx;

static MsgCtx *g_ctx;

enum { K_NONE, K_MINMAX, K_NCCALC, K_NCCALC_RECT, K_WINPOS, K_CREATE, K_GETTEXT, K_MEASURE, K_DRAW, K_COMPARE, K_DELETE };

/* ---- known Win16 messages ------------------------------------------------- */

int msg_is_known16(UINT msg)
{
    if (msg >= WM16_USER && msg < 0x8000) return 1;
    if (msg >= 0x0132 && msg <= 0x0138) return 1;   /* WM_CTLCOLORxxx */
    switch (msg) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x05: case 0x06: case 0x07: case 0x08:
    case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11:
    case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x18: case 0x1A: case 0x1B:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x26: case 0x27: case 0x28: case 0x2A: case 0x2B: case 0x2C: case 0x2D:
    case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x37: case 0x39: case 0x41:
    case 0x46: case 0x47:
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xA8: case 0xA9:
    case 0x100: case 0x101: case 0x102: case 0x103: case 0x104: case 0x105: case 0x106: case 0x107:
    case 0x110: case 0x111: case 0x112: case 0x113: case 0x114: case 0x115: case 0x116: case 0x117:
    case 0x11F: case 0x120: case 0x121:
    case 0x200: case 0x201: case 0x202: case 0x203: case 0x204: case 0x205: case 0x206: case 0x207: case 0x208: case 0x209:
    case 0x210: case 0x211: case 0x212:
    case 0x300: case 0x301: case 0x302: case 0x303: case 0x304: case 0x305: case 0x306: case 0x307:
    case 0x308: case 0x309: case 0x30A: case 0x30B: case 0x30C: case 0x30D: case 0x30E:
    case 0x30F: case 0x310: case 0x311:
        return 1;
    }
    return 0;
}

const char *msg_name(UINT msg)
{
    static char buf[16];
    switch (msg) {
    case WM_CREATE: return "WM_CREATE"; case WM_DESTROY: return "WM_DESTROY"; case WM_PAINT: return "WM_PAINT";
    case WM_COMMAND: return "WM_COMMAND"; case WM_INITDIALOG: return "WM_INITDIALOG"; case WM_TIMER: return "WM_TIMER";
    case WM_KEYDOWN: return "WM_KEYDOWN"; case WM_CHAR: return "WM_CHAR"; case WM_MOUSEMOVE: return "WM_MOUSEMOVE";
    case WM_LBUTTONDOWN: return "WM_LBUTTONDOWN"; case WM_LBUTTONUP: return "WM_LBUTTONUP"; case WM_SIZE: return "WM_SIZE";
    case WM_NCCREATE: return "WM_NCCREATE"; case WM_CLOSE: return "WM_CLOSE"; case WM_SETCURSOR: return "WM_SETCURSOR";
    }
    snprintf(buf, sizeof buf, "0x%04X", msg);
    return buf;
}

/* ---- helpers ------------------------------------------------------------------ */

static int is_in_emu(const void *p)
{
    return (const u8 *)p >= g_mem && (const u8 *)p < g_mem + MEM_SIZE;
}

/* copies a native string onto the 16-bit stack; returns segptr */
static u32 push_string16(const char *s, int maxlen)
{
    size_t n;
    u32 sp;
    if (!s) return 0;
    n = strlen(s);
    if ((int)n > maxlen) n = (size_t)maxlen;
    sp = stack_alloc((u16)(n + 1));
    memcpy(ptrseg(sp), s, n);
    ((char *)ptrseg(sp))[n] = 0;
    return sp;
}

/* Folds a 16-bit coordinate the Win16 code saw (and maybe changed) back
   into the original 32-bit value.  Coordinates outside the 16-bit range
   (e.g. a window temporarily parked far off screen) would otherwise be
   truncated on the round trip; small edits are applied as deltas. */
static LONG merge16(LONG cur, s16 v)
{
    s16 d = (s16)(v - (s16)cur);
    if (d == 0) return cur;
    if (d > -4096 && d < 4096) return cur + d;
    return v;
}
static void rect16to32_merge(const RECT16 *s, RECT *d)
{
    d->left = merge16(d->left, s->left);
    d->top = merge16(d->top, s->top);
    d->right = merge16(d->right, s->right);
    d->bottom = merge16(d->bottom, s->bottom);
}

static void minmax32to16(const MINMAXINFO *a, MINMAXINFO16 *b)
{
    const POINT *s = &a->ptReserved;
    POINT16 *d = &b->ptReserved;
    int i;
    for (i = 0; i < 5; i++) {
        d[i].x = (s16)(s[i].x > 32767 ? 32767 : s[i].x < -32768 ? -32768 : s[i].x);
        d[i].y = (s16)(s[i].y > 32767 ? 32767 : s[i].y < -32768 ? -32768 : s[i].y);
    }
}
static void minmax16to32(const MINMAXINFO16 *b, MINMAXINFO *a)
{
    const POINT16 *s = &b->ptReserved;
    POINT *d = &a->ptReserved;
    int i;
    for (i = 0; i < 5; i++) { d[i].x = s[i].x; d[i].y = s[i].y; }
}

static u16 hwnd_after_16(HWND h)
{
    if (h == HWND_TOP) return 0;
    if (h == HWND_BOTTOM) return 1;
    if (h == HWND_TOPMOST) return 0xFFFF;
    if (h == HWND_NOTOPMOST) return 0xFFFE;
    return HWND_16(h);
}
HWND hwnd_after_32(u16 h)
{
    switch (h) {
    case 0: return HWND_TOP;
    case 1: return HWND_BOTTOM;
    case 0xFFFF: return HWND_TOPMOST;
    case 0xFFFE: return HWND_NOTOPMOST;
    }
    return HWND_32(h);
}

static void winpos32to16(const WINDOWPOS *a, WINDOWPOS16 *b)
{
    b->hwnd = HWND_16(a->hwnd);
    b->hwndInsertAfter = hwnd_after_16(a->hwndInsertAfter);
    b->x = (s16)a->x; b->y = (s16)a->y; b->cx = (s16)a->cx; b->cy = (s16)a->cy;
    b->flags = (u16)a->flags;
}
static void winpos16to32(const WINDOWPOS16 *b, WINDOWPOS *a)
{
    a->hwnd = HWND_32(b->hwnd);
    a->hwndInsertAfter = hwnd_after_32(b->hwndInsertAfter);
    a->x = b->x; a->y = b->y; a->cx = b->cx; a->cy = b->cy;
    a->flags = (a->flags & 0xFFFF0000u) | b->flags;
}
/* copy-back into an existing Win32 WINDOWPOS (keeps out-of-range values) */
static void winpos16to32_merge(const WINDOWPOS16 *b, WINDOWPOS *a)
{
    if (HWND_16(a->hwnd) != b->hwnd) a->hwnd = HWND_32(b->hwnd);
    if (hwnd_after_16(a->hwndInsertAfter) != b->hwndInsertAfter) a->hwndInsertAfter = hwnd_after_32(b->hwndInsertAfter);
    a->x = merge16(a->x, b->x); a->y = merge16(a->y, b->y);
    a->cx = merge16(a->cx, b->cx); a->cy = merge16(a->cy, b->cy);
    a->flags = (a->flags & 0xFFFF0000u) | b->flags;
}

/* ---- zoom: structures with window coordinates ----------------------------- */

static void winpos32to16_z(const WINDOWPOS *a, WINDOWPOS16 *b)
{
    winpos32to16(a, b);
    if (g_zoom > 1 && a->hwnd) {
        RECT nc;
        int x = a->x, y = a->y, cx = a->cx, cy = a->cy;
        zoom_nc(a->hwnd, &nc);
        zoom_win_to_logical(&nc, &x, &y, &cx, &cy);
        b->x = (s16)x; b->y = (s16)y; b->cx = (s16)cx; b->cy = (s16)cy;
    }
}

/* copy back what the 16-bit code changed; untouched values keep their exact
   real value */
static void winpos16to32_zmerge(const WINDOWPOS16 *b, const WINDOWPOS16 *orig, WINDOWPOS *a)
{
    RECT nc;
    if (g_zoom == 1) { winpos16to32_merge(b, a); return; }
    if (HWND_16(a->hwnd) != b->hwnd) a->hwnd = HWND_32(b->hwnd);
    if (hwnd_after_16(a->hwndInsertAfter) != b->hwndInsertAfter) a->hwndInsertAfter = hwnd_after_32(b->hwndInsertAfter);
    a->flags = (a->flags & 0xFFFF0000u) | b->flags;
    if (b->x == orig->x && b->y == orig->y && b->cx == orig->cx && b->cy == orig->cy) return;
    zoom_nc(a->hwnd, &nc);
    if (b->x != orig->x || b->y != orig->y) {
        int x = b->x, y = b->y;
        zoom_win_to_real(&nc, &x, &y, NULL, NULL);
        if (b->x != orig->x) a->x = x;
        if (b->y != orig->y) a->y = y;
    }
    if (b->cx != orig->cx || b->cy != orig->cy) {
        int cx = b->cx, cy = b->cy;
        zoom_win_to_real(&nc, NULL, NULL, &cx, &cy);
        if (b->cx != orig->cx) a->cx = cx;
        if (b->cy != orig->cy) a->cy = cy;
    }
}

static void winpos16to32_z(const WINDOWPOS16 *b, WINDOWPOS *a)
{
    winpos16to32(b, a);
    if (g_zoom > 1 && a->hwnd) {
        RECT nc;
        int x = a->x, y = a->y, cx = a->cx, cy = a->cy;
        zoom_nc(a->hwnd, &nc);
        zoom_win_to_real(&nc, &x, &y, &cx, &cy);
        a->x = x; a->y = y; a->cx = cx; a->cy = cy;
    }
}

static void minmax32to16_z(HWND h, const MINMAXINFO *a, MINMAXINFO16 *b)
{
    MINMAXINFO t = *a;
    if (g_zoom > 1) {
        RECT nc;
        int x, y;
        zoom_nc(h, &nc);
        x = t.ptMaxSize.x; y = t.ptMaxSize.y;
        zoom_win_to_logical(&nc, NULL, NULL, &x, &y); t.ptMaxSize.x = x; t.ptMaxSize.y = y;
        x = t.ptMaxPosition.x; y = t.ptMaxPosition.y;
        zoom_win_to_logical(&nc, &x, &y, NULL, NULL); t.ptMaxPosition.x = x; t.ptMaxPosition.y = y;
        x = t.ptMinTrackSize.x; y = t.ptMinTrackSize.y;
        zoom_win_to_logical(&nc, NULL, NULL, &x, &y); t.ptMinTrackSize.x = x; t.ptMinTrackSize.y = y;
        x = t.ptMaxTrackSize.x; y = t.ptMaxTrackSize.y;
        zoom_win_to_logical(&nc, NULL, NULL, &x, &y); t.ptMaxTrackSize.x = x; t.ptMaxTrackSize.y = y;
    }
    minmax32to16(&t, b);
}

static void minmax16to32_zmerge(HWND h, const MINMAXINFO16 *b, const MINMAXINFO16 *orig, MINMAXINFO *a)
{
    RECT nc;
    int x, y;
    if (g_zoom == 1) { minmax16to32(b, a); return; }
    zoom_nc(h, &nc);
#define ZM_SIZE(f) if (b->f.x != orig->f.x || b->f.y != orig->f.y) { x = b->f.x; y = b->f.y; \
        zoom_win_to_real(&nc, NULL, NULL, &x, &y); if (b->f.x != orig->f.x) a->f.x = x; if (b->f.y != orig->f.y) a->f.y = y; }
    ZM_SIZE(ptMaxSize)
    ZM_SIZE(ptMinTrackSize)
    ZM_SIZE(ptMaxTrackSize)
#undef ZM_SIZE
    if (b->ptMaxPosition.x != orig->ptMaxPosition.x || b->ptMaxPosition.y != orig->ptMaxPosition.y) {
        x = b->ptMaxPosition.x; y = b->ptMaxPosition.y;
        zoom_win_to_real(&nc, &x, &y, NULL, NULL);
        if (b->ptMaxPosition.x != orig->ptMaxPosition.x) a->ptMaxPosition.x = x;
        if (b->ptMaxPosition.y != orig->ptMaxPosition.y) a->ptMaxPosition.y = y;
    }
}

static u32 zpt_down(u32 lp)       /* real -> logical point (lParam) */
{
    if (g_zoom == 1) return lp;
    return MAKELONG((u16)(s16)zfloor((s16)LOWORD(lp)), (u16)(s16)zfloor((s16)HIWORD(lp)));
}
static LPARAM zpt_up(u32 lp)      /* logical -> real point (lParam) */
{
    if (g_zoom == 1) return (LPARAM)(LONG)lp;
    return MAKELPARAM((u16)(s16)((s16)LOWORD(lp) * g_zoom), (u16)(s16)((s16)HIWORD(lp) * g_zoom));
}
static int is_mouse_msg(UINT m) { return (m >= WM_MOUSEFIRST && m <= 0x209) || (m >= 0xA0 && m <= 0xA9) || m == WM_NCHITTEST; }

static void create32to16(HWND hwnd, const CREATESTRUCTA *cs, CREATESTRUCT16 *c)
{
    c->lpCreateParams = (u32)(ULONG_PTR)cs->lpCreateParams;
    c->hInstance = g_mod.hInstance;
    if (cs->style & WS_CHILD) c->hMenu = (u16)(ULONG_PTR)cs->hMenu;
    else c->hMenu = HMENU_16(cs->hMenu);
    c->hwndParent = HWND_16(cs->hwndParent);
    c->cy = (s16)cs->cy; c->cx = (s16)cs->cx; c->y = (s16)cs->y; c->x = (s16)cs->x;
    c->style = (u32)cs->style;
    c->dwExStyle = (u32)cs->dwExStyle;
    if (g_zoom > 1) {
        RECT nc;
        int x = cs->x, y = cs->y, cx = cs->cx, cy = cs->cy;
        zoom_nc_style(cs->style, cs->dwExStyle, cs->hMenu && !(cs->style & WS_CHILD), &nc);
        zoom_win_to_logical(&nc, &x, &y, &cx, &cy);
        if (cs->x != CW_USEDEFAULT) { c->x = (s16)x; c->y = (s16)y; }
        if (cs->cx != CW_USEDEFAULT) { c->cx = (s16)cx; c->cy = (s16)cy; }
    }
    (void)hwnd;
}

/* ---- Win32 -> Win16 (native code calls a 16-bit window/dialog procedure) ---- */

LRESULT send_to_16(u32 proc16, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, int is_dlg)
{
    MsgCtx ctx;
    u16 save_sp = SP;
    u16 hwnd16, msg16, wp16;
    u32 lp16, r16;
    LRESULT result;
    u32 pstruct = 0;   /* segptr of the converted structure */
    u16 hbig = 0;      /* global block for big text buffers */
    WPARAM ctx_wp = wp;
    HDC zdc = NULL;    /* zoom: substitute DC */
    int zlevel = 0;

    msg16 = (u16)msg;
    if (!msg_is_known16(msg)) {
        WndInfo *wi;
        if (is_dlg) return 0;
        wi = wnd_info(hwnd, 0);
        /* button control messages are WM_USER based in Win16; let a 16-bit
           subclass procedure see them, everything else goes straight to the
           native procedure */
        if (wi && wi->native_old && msg >= BM_GETCHECK && msg <= BM_SETSTYLE) {
            char cls[16];
            if (GetClassNameA(hwnd, cls, sizeof cls) && !_stricmp(cls, "Button")) msg16 = (u16)(WM16_USER + (msg - BM_GETCHECK));
        }
        if (msg16 == (u16)msg) {
            if (wi && wi->native_old) return CallWindowProcA(wi->native_old, hwnd, msg, wp, lp);
            return DefWindowProcA(hwnd, msg, wp, lp);
        }
    }

    hwnd16 = HWND_16(hwnd);
    wp16 = (u16)wp;
    lp16 = (u32)lp;
    memset(&ctx, 0, sizeof ctx);

    switch (msg) {
    case WM_ACTIVATE: case WM_CHARTOITEM: case WM_VKEYTOITEM: case WM_COMMAND:
        wp16 = LOWORD(wp);
        lp16 = MAKELONG(HWND_16((HWND)lp), HIWORD(wp));
        break;
    case WM_HSCROLL: case WM_VSCROLL:
        wp16 = LOWORD(wp);
        lp16 = MAKELONG(HIWORD(wp), HWND_16((HWND)lp));
        break;
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC:
        msg16 = WM16_CTLCOLOR;
        wp16 = HDC_16((HDC)wp);
        lp16 = MAKELONG(HWND_16((HWND)lp), msg - WM_CTLCOLORMSGBOX);
        wdc_temp_begin((HWND)lp, (HDC)wp);     /* colours the game sets get the dark treatment */
        break;
    case WM_MENUSELECT: {
        UINT flags = HIWORD(wp);
        HMENU hm = (HMENU)lp;
        if (flags == 0xFFFF && !hm) { wp16 = 0; lp16 = MAKELONG(0xFFFF, 0); break; }
        if ((flags & MF_POPUP) && hm) wp16 = HMENU_16(GetSubMenu(hm, LOWORD(wp)));
        else wp16 = LOWORD(wp);
        lp16 = MAKELONG(flags, HMENU_16(hm));
        break;
    }
    case WM_MENUCHAR:
        wp16 = LOWORD(wp);
        lp16 = MAKELONG(HIWORD(wp), HMENU_16((HMENU)lp));
        break;
    case WM_PARENTNOTIFY:
        if (LOWORD(wp) == WM_CREATE || LOWORD(wp) == WM_DESTROY) {
            wp16 = LOWORD(wp);
            lp16 = MAKELONG(HWND_16((HWND)lp), HIWORD(wp));
        } else if (LOWORD(wp) >= WM_MOUSEFIRST && LOWORD(wp) <= 0x209) {
            lp16 = zpt_down((u32)lp);
        }
        break;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_NCHITTEST: case WM_NCMOUSEMOVE: case WM_NCLBUTTONDOWN: case WM_NCLBUTTONUP: case WM_NCLBUTTONDBLCLK:
    case WM_NCRBUTTONDOWN: case WM_NCRBUTTONUP: case WM_NCRBUTTONDBLCLK:
    case WM_NCMBUTTONDOWN: case WM_NCMBUTTONUP: case WM_NCMBUTTONDBLCLK:
        lp16 = zpt_down((u32)lp);
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_SETCURSOR: case WM_MOUSEACTIVATE:
    case WM_PALETTECHANGED: case WM_PALETTEISCHANGING: case WM_INITDIALOG:
        wp16 = HWND_16((HWND)wp);
        break;
    case WM_ERASEBKGND: case WM_ICONERASEBKGND:
        if (msg == WM_ERASEBKGND && (zdc = zoom_erase_begin(hwnd, (HDC)wp, &zlevel)) != NULL) {
            wp16 = HDC_16(zdc);
            ctx_wp = (WPARAM)zdc;
        } else
            wp16 = HDC_16((HDC)wp);
        break;
    case WM_PAINT:
        wp16 = wp ? HDC_16((HDC)wp) : 0;
        break;
    case WM_SIZE:
        if (g_zoom > 1) lp16 = MAKELONG((u16)zceil(LOWORD(lp)), (u16)zceil(HIWORD(lp)));
        break;
    case WM_MOVE:
        lp16 = zpt_down((u32)lp);
        break;
    case WM_SETFONT:
        wp16 = HGDI_16((HGDIOBJ)zoom_unscale_font((HFONT)wp));
        break;
    case WM_NCPAINT:
        wp16 = (wp == 1) ? 1 : HGDI_16((HGDIOBJ)wp);
        break;
    case WM_NCACTIVATE:
        lp16 = 0;
        break;
    case WM_INITMENU: case WM_INITMENUPOPUP:
        wp16 = HMENU_16((HMENU)wp);
        break;
    case WM_ENTERIDLE:
        lp16 = HWND_16((HWND)lp);
        break;
    case WM_NEXTDLGCTL:
        if (LOWORD(lp)) wp16 = HWND_16((HWND)wp);
        break;
    case WM_TIMER: case WM_ACTIVATEAPP:
        lp16 = 0;
        break;
    case WM_SETTEXT: case WM_WININICHANGE: case WM_DEVMODECHANGE:
        if (lp) {
            const char *s = (const char *)lp;
            lp16 = push_string16(s, 1024);
        }
        break;
    case WM_GETTEXT: {
        u16 n = (u16)(wp > 0x7FFF ? 0x7FFF : wp);
        if (n <= 512) pstruct = stack_alloc((u16)(n + 2));
        else { hbig = glob_alloc(G_FIXED, n + 2u, GK_SYSTEM, 0, 0); pstruct = (u32)glob_sel(hbig) << 16; }
        if (pstruct) ((char *)ptrseg(pstruct))[0] = 0;
        lp16 = pstruct;
        wp16 = n;
        ctx.kind = K_GETTEXT;
        break;
    }
    case WM_GETMINMAXINFO:
        pstruct = stack_alloc(sizeof(MINMAXINFO16));
        minmax32to16_z(hwnd, (MINMAXINFO *)lp, (MINMAXINFO16 *)ptrseg(pstruct));
        ctx.orig.mmi = *(MINMAXINFO16 *)ptrseg(pstruct);
        lp16 = pstruct;
        ctx.kind = K_MINMAX;
        break;
    case WM_WINDOWPOSCHANGING: case WM_WINDOWPOSCHANGED:
        pstruct = stack_alloc(sizeof(WINDOWPOS16));
        winpos32to16_z((WINDOWPOS *)lp, (WINDOWPOS16 *)ptrseg(pstruct));
        ctx.orig.wpos = *(WINDOWPOS16 *)ptrseg(pstruct);
        lp16 = pstruct;
        ctx.kind = K_WINPOS;
        break;
    case WM_NCCALCSIZE:
        if (wp) {
            NCCALCSIZE_PARAMS *nc = (NCCALCSIZE_PARAMS *)lp;
            u32 pwp = stack_alloc(sizeof(WINDOWPOS16));
            NCCALCSIZE_PARAMS16 *n16;
            pstruct = stack_alloc(sizeof(NCCALCSIZE_PARAMS16));
            n16 = (NCCALCSIZE_PARAMS16 *)ptrseg(pstruct);
            rect32to16(&nc->rgrc[0], &n16->rgrc[0]);
            rect32to16(&nc->rgrc[1], &n16->rgrc[1]);
            rect32to16(&nc->rgrc[2], &n16->rgrc[2]);
            if (nc->lppos) winpos32to16(nc->lppos, (WINDOWPOS16 *)ptrseg(pwp));
            n16->lppos = nc->lppos ? pwp : 0;
            ctx.kind = K_NCCALC;
        } else {
            pstruct = stack_alloc(sizeof(RECT16));
            rect32to16((RECT *)lp, (RECT16 *)ptrseg(pstruct));
            ctx.kind = K_NCCALC_RECT;
        }
        lp16 = pstruct;
        break;
    case WM_NCCREATE: case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
        CREATESTRUCT16 c;
        u32 pname = 0, pclass = 0;
        memset(&c, 0, sizeof c);
        create32to16(hwnd, cs, &c);
        if (cs->lpszName) {
            if (!HIWORD((ULONG_PTR)cs->lpszName)) pname = (u32)(ULONG_PTR)cs->lpszName;
            else pname = push_string16(cs->lpszName, 512);
        }
        if (cs->lpszClass) {
            if (!HIWORD((ULONG_PTR)cs->lpszClass)) pclass = (u32)(ULONG_PTR)cs->lpszClass;
            else pclass = push_string16(cs->lpszClass, 256);
        }
        c.lpszName = pname;
        c.lpszClass = pclass;
        pstruct = stack_alloc(sizeof(CREATESTRUCT16));
        memcpy(ptrseg(pstruct), &c, sizeof c);
        lp16 = pstruct;
        ctx.kind = K_CREATE;
        break;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)lp;
        DRAWITEMSTRUCT16 d16;
        d16.CtlType = (u16)d->CtlType; d16.CtlID = (u16)d->CtlID; d16.itemID = (u16)d->itemID;
        d16.itemAction = (u16)d->itemAction; d16.itemState = (u16)d->itemState;
        d16.hwndItem = (d->CtlType == ODT_MENU) ? HMENU_16((HMENU)d->hwndItem) : HWND_16(d->hwndItem);
        d16.hDC = HDC_16(d->hDC);
        rect32to16(&d->rcItem, &d16.rcItem);
        d16.itemData = (u32)d->itemData;
        if (g_zoom > 1 && d->CtlType != ODT_MENU && (zdc = zoom_drawitem_begin(d->hwndItem, d->hDC, &d->rcItem)) != NULL) {
            d16.hDC = HDC_16(zdc);
            d16.rcItem.left = (s16)zfloor(d->rcItem.left);
            d16.rcItem.top = (s16)zfloor(d->rcItem.top);
            d16.rcItem.right = (s16)zceil(d->rcItem.right);
            d16.rcItem.bottom = (s16)zceil(d->rcItem.bottom);
        } else {
            wdc_temp_begin(d->hwndItem, d->hDC);   /* dark mode colours */
            ctx.kind = K_DRAW;
        }
        pstruct = stack_alloc(sizeof d16);
        memcpy(ptrseg(pstruct), &d16, sizeof d16);
        lp16 = pstruct;
        ctx.kind = K_DRAW;
        break;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *m = (MEASUREITEMSTRUCT *)lp;
        MEASUREITEMSTRUCT16 m16;
        m16.CtlType = (u16)m->CtlType; m16.CtlID = (u16)m->CtlID; m16.itemID = (u16)m->itemID;
        m16.itemWidth = (u16)m->itemWidth; m16.itemHeight = (u16)m->itemHeight;
        m16.itemData = (u32)m->itemData;
        pstruct = stack_alloc(sizeof m16);
        memcpy(ptrseg(pstruct), &m16, sizeof m16);
        lp16 = pstruct;
        ctx.kind = K_MEASURE;
        break;
    }
    case WM_COMPAREITEM: {
        COMPAREITEMSTRUCT *c = (COMPAREITEMSTRUCT *)lp;
        COMPAREITEMSTRUCT16 c16;
        c16.CtlType = (u16)c->CtlType; c16.CtlID = (u16)c->CtlID; c16.hwndItem = HWND_16(c->hwndItem);
        c16.itemID1 = (u16)c->itemID1; c16.itemData1 = (u32)c->itemData1;
        c16.itemID2 = (u16)c->itemID2; c16.itemData2 = (u32)c->itemData2;
        pstruct = stack_alloc(sizeof c16);
        memcpy(ptrseg(pstruct), &c16, sizeof c16);
        lp16 = pstruct;
        ctx.kind = K_COMPARE;
        break;
    }
    case WM_DELETEITEM: {
        DELETEITEMSTRUCT *d = (DELETEITEMSTRUCT *)lp;
        DELETEITEMSTRUCT16 d16;
        d16.CtlType = (u16)d->CtlType; d16.CtlID = (u16)d->CtlID; d16.itemID = (u16)d->itemID;
        d16.hwndItem = HWND_16(d->hwndItem); d16.itemData = (u32)d->itemData;
        pstruct = stack_alloc(sizeof d16);
        memcpy(ptrseg(pstruct), &d16, sizeof d16);
        lp16 = pstruct;
        ctx.kind = K_DELETE;
        break;
    }
    case WM_GETDLGCODE:
        if (lp) {
            MSG16 m16;
            msg32_to_msg16((const MSG *)lp, &m16);
            pstruct = stack_alloc(sizeof m16);
            memcpy(ptrseg(pstruct), &m16, sizeof m16);
            lp16 = pstruct;
        }
        break;
    }

    ctx.hwnd = hwnd; ctx.msg = msg; ctx.wp = ctx_wp; ctx.lp = lp;
    ctx.msg16 = msg16; ctx.wp16 = wp16; ctx.lp16 = lp16;
    ctx.prev = g_ctx;
    g_ctx = &ctx;
    r16 = call16_wndproc(proc16, g_mod.hInstance, hwnd16, msg16, wp16, lp16);
    g_ctx = ctx.prev;

    /* copy back results */
    switch (ctx.kind) {
    case K_MINMAX:
        minmax16to32_zmerge(hwnd, (MINMAXINFO16 *)ptrseg(pstruct), &ctx.orig.mmi, (MINMAXINFO *)lp);
        break;
    case K_WINPOS:
        winpos16to32_zmerge((WINDOWPOS16 *)ptrseg(pstruct), &ctx.orig.wpos, (WINDOWPOS *)lp);
        break;
    case K_NCCALC: {
        NCCALCSIZE_PARAMS *nc = (NCCALCSIZE_PARAMS *)lp;
        NCCALCSIZE_PARAMS16 *n16 = (NCCALCSIZE_PARAMS16 *)ptrseg(pstruct);
        rect16to32_merge(&n16->rgrc[0], &nc->rgrc[0]);
        rect16to32_merge(&n16->rgrc[1], &nc->rgrc[1]);
        rect16to32_merge(&n16->rgrc[2], &nc->rgrc[2]);
        if (nc->lppos && n16->lppos) winpos16to32_merge((WINDOWPOS16 *)ptrseg(n16->lppos), nc->lppos);
        break;
    }
    case K_NCCALC_RECT:
        rect16to32_merge((RECT16 *)ptrseg(pstruct), (RECT *)lp);
        break;
    case K_MEASURE: {
        MEASUREITEMSTRUCT *m = (MEASUREITEMSTRUCT *)lp;
        MEASUREITEMSTRUCT16 *m16 = (MEASUREITEMSTRUCT16 *)ptrseg(pstruct);
        m->itemWidth = m16->itemWidth * g_zoom;
        m->itemHeight = m16->itemHeight * g_zoom;
        break;
    }
    case K_GETTEXT:
        if (!ctx.passthrough && pstruct && wp) {
            size_t n = strlen((const char *)ptrseg(pstruct));
            if (n >= wp) n = wp - 1;
            memcpy((char *)lp, ptrseg(pstruct), n);
            ((char *)lp)[n] = 0;
            r16 = (u32)n;
        }
        break;
    }
    if (hbig) glob_free(hbig);
    SP = save_sp;
    if (zdc) {
        if (msg == WM_ERASEBKGND) zoom_erase_end(hwnd, zdc, zlevel);
        else zoom_drawitem_end(zdc);
    } else if (msg == WM_DRAWITEM && ctx.kind == K_DRAW) {
        wdc_temp_end(((DRAWITEMSTRUCT *)lp)->hDC);
    }

    /* result */
    switch (msg) {
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC: {
        HBRUSH b = brush_32((u16)r16);
        wdc_temp_end((HDC)wp);
        if (g_dark && b) {
            if (ctx.passthrough) {
                /* Windows' defaults, in dark colours */
                int ci = (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) ? COLOR_WINDOW : COLOR_BTNFACE;
                SetTextColor((HDC)wp, dark_syscolor(COLOR_WINDOWTEXT));
                SetBkColor((HDC)wp, dark_syscolor(ci));
                b = dark_solid_brush(dark_syscolor(ci));
            } else {
                HBRUSH d = dark_brush_for(b);
                if (d) b = d;
            }
        }
        return (LRESULT)b;
    }
    case WM_QUERYDRAGICON:
        return (LRESULT)HICON_32((u16)r16);
    case WM_GETFONT: {
        /* the game sees unscaled fonts; whoever asks on the 32-bit side
           (our own painting, the dialog manager) wants the real one */
        HFONT f = (HFONT)HGDI_32((u16)r16);
        return (LRESULT)(f && g_zoom > 1 ? zoom_font(f) : f);
    }
    case WM_NCHITTEST: case WM_MOUSEACTIVATE:
        return (LRESULT)(s16)(u16)r16;
    case WM_GETDLGCODE: case WM_NCCALCSIZE:
        return (LRESULT)(u16)r16;
    }
    if (is_dlg) {
        switch (msg) {
        case WM_COMPAREITEM: case WM_VKEYTOITEM: case WM_CHARTOITEM: case WM_INITDIALOG:
            return (LRESULT)(s16)(u16)r16;
        }
        return (LRESULT)(s16)(u16)r16;
    }
    result = (LRESULT)(s32)r16;
    return result;
}

/* ---- Win16 -> Win32 ------------------------------------------------------------ */

typedef struct Conv32 {
    UINT msg;
    WPARAM wp;
    LPARAM lp;
    int kind;
    u32 lp16;
    union {
        CREATESTRUCTA cs;
        MINMAXINFO mmi;
        WINDOWPOS wpos;
        struct { NCCALCSIZE_PARAMS nc; WINDOWPOS wp; } ncs;
        RECT rc;
        DRAWITEMSTRUCT dis;
        MEASUREITEMSTRUCT mis;
        COMPAREITEMSTRUCT cis;
        DELETEITEMSTRUCT dels;
        MSG m;
        INT ints[256];
        ULONG_PTR data;
    } u;
} Conv32;

enum { CLS_OTHER, CLS_BUTTON, CLS_EDIT, CLS_STATIC, CLS_LISTBOX, CLS_COMBO, CLS_SCROLLBAR };

static int control_class(HWND hwnd)
{
    char name[32];
    if (!GetClassNameA(hwnd, name, sizeof name)) return CLS_OTHER;
    if (!_stricmp(name, "Button")) return CLS_BUTTON;
    if (!_stricmp(name, "Edit")) return CLS_EDIT;
    if (!_stricmp(name, "Static")) return CLS_STATIC;
    if (!_stricmp(name, "ListBox") || !_stricmp(name, "ComboLBox")) return CLS_LISTBOX;
    if (!_stricmp(name, "ComboBox")) return CLS_COMBO;
    if (!_stricmp(name, "ScrollBar")) return CLS_SCROLLBAR;
    return CLS_OTHER;
}

enum { K32_NONE, K32_MINMAX, K32_WINPOS, K32_NCCALC, K32_NCCALC_RECT, K32_MEASURE, K32_RECT16, K32_SELITEMS, K32_ITEMDATA, K32_GETTEXT_DATA };

static int lb_has_strings(HWND hwnd, int combo)
{
    LONG st = GetWindowLongA(hwnd, GWL_STYLE);
    if (combo) return !(st & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) || (st & CBS_HASSTRINGS);
    return !(st & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) || (st & LBS_HASSTRINGS);
}

/* convert control messages (WM_USER based in Win16) */
static int conv_control16to32(HWND hwnd, u16 msg16, u16 wp16, u32 lp16, Conv32 *c)
{
    int cls = control_class(hwnd);
    int k = msg16 - WM16_USER;
    switch (cls) {
    case CLS_BUTTON:
        if (k >= 0 && k <= 4) { c->msg = BM_GETCHECK + k; return 1; }
        break;
    case CLS_STATIC:
        if (k == 0) { c->msg = STM_SETICON; c->wp = (WPARAM)zoom_icon(HICON_32(wp16), FALSE); return 1; }
        if (k == 1) { c->msg = STM_GETICON; return 1; }
        break;
    case CLS_EDIT:
        if (k < 0 || k > 34) break;
        c->msg = EM_GETSEL + k;
        switch (c->msg) {
        case EM_GETSEL: c->wp = 0; c->lp = 0; break;
        case EM_SETSEL: c->wp = (WPARAM)(INT_PTR)(s16)LOWORD(lp16); c->lp = (LPARAM)(s16)HIWORD(lp16); break;
        case EM_GETRECT:
            c->lp = (LPARAM)&c->u.rc; c->kind = K32_RECT16; break;
        case EM_SETRECT: case EM_SETRECTNP:
            if (lp16) { rect16to32((RECT16 *)ptrseg(lp16), &c->u.rc); c->lp = (LPARAM)&c->u.rc; }
            break;
        case EM_LINESCROLL: c->wp = (WPARAM)(INT_PTR)(s16)HIWORD(lp16); c->lp = (LPARAM)(s16)LOWORD(lp16); break;
        case EM_REPLACESEL: case EM_GETLINE: c->lp = (LPARAM)ptrseg(lp16); break;
        case EM_SETTABSTOPS: {
            int i, n = wp16 > 256 ? 256 : wp16;
            const s16 *t = (const s16 *)ptrseg(lp16);
            for (i = 0; i < n && t; i++) c->u.ints[i] = t[i];
            c->lp = t ? (LPARAM)c->u.ints : 0;
            break;
        }
        case EM_LIMITTEXT: c->wp = wp16 ? wp16 : 0xFFFF; break;
        }
        return 1;
    case CLS_LISTBOX: {
        int hs = lb_has_strings(hwnd, 0);
        if (k < 1 || k > 35) break;
        c->msg = LB_ADDSTRING + (k - 1);
        switch (c->msg) {
        case LB_ADDSTRING: case LB_INSERTSTRING: case LB_FINDSTRING: case LB_SELECTSTRING: case LB_FINDSTRINGEXACT:
            c->lp = hs ? (LPARAM)ptrseg(lp16) : (LPARAM)lp16;
            c->wp = (WPARAM)(INT_PTR)(s16)wp16;
            break;
        case LB_DIR: c->lp = (LPARAM)ptrseg(lp16); break;
        case LB_GETTEXT:
            c->wp = (WPARAM)(INT_PTR)(s16)wp16;
            if (hs) c->lp = (LPARAM)ptrseg(lp16);
            else { c->lp = (LPARAM)&c->u.data; c->kind = K32_ITEMDATA; }
            break;
        case LB_GETITEMRECT: c->lp = (LPARAM)&c->u.rc; c->kind = K32_RECT16; break;
        case LB_GETSELITEMS: c->wp = wp16 > 256 ? 256 : wp16; c->lp = (LPARAM)c->u.ints; c->kind = K32_SELITEMS; break;
        case LB_SETTABSTOPS: {
            int i, n = wp16 > 256 ? 256 : wp16;
            const s16 *t = (const s16 *)ptrseg(lp16);
            for (i = 0; i < n && t; i++) c->u.ints[i] = t[i];
            c->lp = t ? (LPARAM)c->u.ints : 0;
            break;
        }
        case LB_SETSEL: case LB_SETCURSEL: case LB_GETSEL: case LB_GETTEXTLEN: case LB_SETTOPINDEX:
        case LB_GETITEMDATA: case LB_SETITEMDATA: case LB_SETCARETINDEX: case LB_GETITEMHEIGHT:
        case LB_SETITEMHEIGHT: case LB_DELETESTRING:
            c->wp = (WPARAM)(INT_PTR)(s16)wp16;
            break;
        case LB_SELITEMRANGE:
            c->wp = wp16;
            break;
        }
        return 1;
    }
    case CLS_COMBO: {
        int hs = lb_has_strings(hwnd, 1);
        if (k < 0 || k > 24) break;
        c->msg = CB_GETEDITSEL + k;
        switch (c->msg) {
        case CB_GETEDITSEL: c->wp = 0; c->lp = 0; break;
        case CB_ADDSTRING: case CB_INSERTSTRING: case CB_FINDSTRING: case CB_SELECTSTRING: case CB_FINDSTRINGEXACT:
            c->lp = hs ? (LPARAM)ptrseg(lp16) : (LPARAM)lp16;
            c->wp = (WPARAM)(INT_PTR)(s16)wp16;
            break;
        case CB_DIR: c->lp = (LPARAM)ptrseg(lp16); break;
        case CB_GETLBTEXT:
            c->wp = (WPARAM)(INT_PTR)(s16)wp16;
            if (hs) c->lp = (LPARAM)ptrseg(lp16);
            else { c->lp = (LPARAM)&c->u.data; c->kind = K32_ITEMDATA; }
            break;
        case CB_GETDROPPEDCONTROLRECT: c->lp = (LPARAM)&c->u.rc; c->kind = K32_RECT16; break;
        case CB_SETCURSEL: case CB_GETLBTEXTLEN: case CB_GETITEMDATA: case CB_SETITEMDATA: case CB_DELETESTRING:
            c->wp = (WPARAM)(INT_PTR)(s16)wp16;
            break;
        }
        return 1;
    }
    }
    return 0;
}

static void conv16to32(HWND hwnd, u16 msg16, u16 wp16, u32 lp16, Conv32 *c, int to_native_window)
{
    memset(c, 0, sizeof *c);
    c->msg = msg16;
    c->wp = wp16;
    c->lp = (LPARAM)(LONG)lp16;
    c->lp16 = lp16;
    if (msg16 >= WM16_USER && to_native_window) {
        if (conv_control16to32(hwnd, msg16, wp16, lp16, c)) return;
        c->msg = msg16; c->wp = wp16; c->lp = (LPARAM)(LONG)lp16;
        return;
    }
    if (g_zoom > 1 && is_mouse_msg(msg16)) {
        c->lp = zpt_up(lp16);
        return;
    }
    switch (msg16) {
    case WM_ACTIVATE: case WM_CHARTOITEM: case WM_VKEYTOITEM: case WM_COMMAND:
        c->wp = MAKEWPARAM(wp16, HIWORD(lp16));
        c->lp = (LPARAM)HWND_32(LOWORD(lp16));
        break;
    case WM_SIZE:
        if (g_zoom > 1) c->lp = MAKELPARAM(LOWORD(lp16) * g_zoom, HIWORD(lp16) * g_zoom);
        break;
    case WM_MOVE:
        c->lp = zpt_up(lp16);
        break;
    case WM_HSCROLL: case WM_VSCROLL:
        c->wp = MAKEWPARAM(wp16, LOWORD(lp16));
        c->lp = (LPARAM)HWND_32(HIWORD(lp16));
        break;
    case WM16_CTLCOLOR:
        c->msg = WM_CTLCOLORMSGBOX + (HIWORD(lp16) <= 6 ? HIWORD(lp16) : 0);
        c->wp = (WPARAM)HDC_32(wp16);
        c->lp = (LPARAM)HWND_32(LOWORD(lp16));
        break;
    case WM_MENUSELECT: {
        u16 flags = LOWORD(lp16);
        HMENU hm = HMENU_32(HIWORD(lp16));
        UINT item = wp16;
        if (flags == 0xFFFF && !HIWORD(lp16)) { c->wp = MAKEWPARAM(0, 0xFFFF); c->lp = 0; break; }
        if ((flags & MF_POPUP) && hm) {
            int i, n = GetMenuItemCount(hm);
            HMENU sub = HMENU_32(wp16);
            for (i = 0; i < n; i++) if (GetSubMenu(hm, i) == sub) { item = (UINT)i; break; }
        }
        c->wp = MAKEWPARAM(item, flags);
        c->lp = (LPARAM)hm;
        break;
    }
    case WM_MENUCHAR:
        c->wp = MAKEWPARAM(wp16, LOWORD(lp16));
        c->lp = (LPARAM)HMENU_32(HIWORD(lp16));
        break;
    case WM_PARENTNOTIFY:
        if (wp16 == WM_CREATE || wp16 == WM_DESTROY) {
            c->wp = MAKEWPARAM(wp16, HIWORD(lp16));
            c->lp = (LPARAM)HWND_32(LOWORD(lp16));
        }
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_SETCURSOR: case WM_MOUSEACTIVATE:
    case WM_PALETTECHANGED: case WM_PALETTEISCHANGING: case WM_INITDIALOG:
        c->wp = (WPARAM)HWND_32(wp16);
        break;
    case WM_ERASEBKGND: case WM_ICONERASEBKGND:
        c->wp = (WPARAM)HDC_32(wp16);
        break;
    case WM_PAINT:
        c->wp = wp16 ? (WPARAM)HDC_32(wp16) : 0;
        break;
    case WM_SETFONT:
        c->wp = (WPARAM)HGDI_32(wp16);
        if (to_native_window) {
            WndInfo *wi = wnd_info(hwnd, 1);
            if (wi) wi->font16 = (HFONT)c->wp;
            if (g_zoom > 1) c->wp = (WPARAM)zoom_font((HFONT)c->wp);
            emu_log("WM_SETFONT %04X -> native %p: %p", wp16, (void *)hwnd, (void *)c->wp);
        }
        break;
    case WM_NCPAINT:
        c->wp = (wp16 == 1) ? 1 : (WPARAM)HGDI_32(wp16);
        break;
    case WM_NCACTIVATE:
        c->lp = 0;
        break;
    case WM_INITMENU: case WM_INITMENUPOPUP:
        c->wp = (WPARAM)HMENU_32(wp16);
        break;
    case WM_ENTERIDLE:
        c->lp = (LPARAM)HWND_32(LOWORD(lp16));
        break;
    case WM_NEXTDLGCTL:
        if (LOWORD(lp16)) c->wp = (WPARAM)HWND_32(wp16);
        break;
    case WM_TIMER: case WM_ACTIVATEAPP:
        c->lp = 0;
        break;
    case WM_SETTEXT: case WM_GETTEXT: case WM_WININICHANGE: case WM_DEVMODECHANGE:
        c->lp = (LPARAM)ptrseg(lp16);
        break;
    case WM_GETMINMAXINFO:
        minmax16to32((MINMAXINFO16 *)ptrseg(lp16), &c->u.mmi);
        c->lp = (LPARAM)&c->u.mmi;
        c->kind = K32_MINMAX;
        break;
    case WM_WINDOWPOSCHANGING: case WM_WINDOWPOSCHANGED:
        memset(&c->u.wpos, 0, sizeof c->u.wpos);
        winpos16to32_z((WINDOWPOS16 *)ptrseg(lp16), &c->u.wpos);
        c->lp = (LPARAM)&c->u.wpos;
        c->kind = K32_WINPOS;
        break;
    case WM_NCCALCSIZE:
        if (wp16) {
            NCCALCSIZE_PARAMS16 *n16 = (NCCALCSIZE_PARAMS16 *)ptrseg(lp16);
            rect16to32(&n16->rgrc[0], &c->u.ncs.nc.rgrc[0]);
            rect16to32(&n16->rgrc[1], &c->u.ncs.nc.rgrc[1]);
            rect16to32(&n16->rgrc[2], &c->u.ncs.nc.rgrc[2]);
            if (n16->lppos) { winpos16to32((WINDOWPOS16 *)ptrseg(n16->lppos), &c->u.ncs.wp); c->u.ncs.nc.lppos = &c->u.ncs.wp; }
            c->lp = (LPARAM)&c->u.ncs.nc;
            c->kind = K32_NCCALC;
        } else {
            rect16to32((RECT16 *)ptrseg(lp16), &c->u.rc);
            c->lp = (LPARAM)&c->u.rc;
            c->kind = K32_NCCALC_RECT;
        }
        break;
    case WM_NCCREATE: case WM_CREATE: {
        CREATESTRUCT16 *s = (CREATESTRUCT16 *)ptrseg(lp16);
        CREATESTRUCTA *d = &c->u.cs;
        d->lpCreateParams = (LPVOID)(ULONG_PTR)s->lpCreateParams;
        d->hInstance = g_hinst32;
        d->style = (LONG)s->style;
        d->hMenu = (s->style & WS_CHILD) ? (HMENU)(ULONG_PTR)s->hMenu : HMENU_32(s->hMenu);
        d->hwndParent = HWND_32(s->hwndParent);
        d->cy = s->cy; d->cx = s->cx; d->y = s->y; d->x = s->x;
        d->lpszName = (s->lpszName >> 16) ? (LPCSTR)ptrseg(s->lpszName) : (LPCSTR)(ULONG_PTR)s->lpszName;
        d->lpszClass = (s->lpszClass >> 16) ? (LPCSTR)ptrseg(s->lpszClass) : (LPCSTR)(ULONG_PTR)s->lpszClass;
        d->dwExStyle = s->dwExStyle;
        c->lp = (LPARAM)d;
        break;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT16 *s = (DRAWITEMSTRUCT16 *)ptrseg(lp16);
        DRAWITEMSTRUCT *d = &c->u.dis;
        d->CtlType = s->CtlType; d->CtlID = s->CtlID; d->itemID = s->itemID;
        d->itemAction = s->itemAction; d->itemState = s->itemState;
        d->hwndItem = (s->CtlType == ODT_MENU) ? (HWND)HMENU_32(s->hwndItem) : HWND_32(s->hwndItem);
        d->hDC = HDC_32(s->hDC);
        rect16to32(&s->rcItem, &d->rcItem);
        d->itemData = s->itemData;
        c->lp = (LPARAM)d;
        break;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT16 *s = (MEASUREITEMSTRUCT16 *)ptrseg(lp16);
        MEASUREITEMSTRUCT *d = &c->u.mis;
        d->CtlType = s->CtlType; d->CtlID = s->CtlID; d->itemID = s->itemID;
        d->itemWidth = s->itemWidth; d->itemHeight = s->itemHeight; d->itemData = s->itemData;
        c->lp = (LPARAM)d;
        c->kind = K32_MEASURE;
        break;
    }
    case WM_COMPAREITEM: {
        COMPAREITEMSTRUCT16 *s = (COMPAREITEMSTRUCT16 *)ptrseg(lp16);
        COMPAREITEMSTRUCT *d = &c->u.cis;
        d->CtlType = s->CtlType; d->CtlID = s->CtlID; d->hwndItem = HWND_32(s->hwndItem);
        d->itemID1 = s->itemID1; d->itemData1 = s->itemData1;
        d->itemID2 = s->itemID2; d->itemData2 = s->itemData2;
        d->dwLocaleId = 0;
        c->lp = (LPARAM)d;
        break;
    }
    case WM_DELETEITEM: {
        DELETEITEMSTRUCT16 *s = (DELETEITEMSTRUCT16 *)ptrseg(lp16);
        DELETEITEMSTRUCT *d = &c->u.dels;
        d->CtlType = s->CtlType; d->CtlID = s->CtlID; d->itemID = s->itemID;
        d->hwndItem = HWND_32(s->hwndItem); d->itemData = s->itemData;
        c->lp = (LPARAM)d;
        break;
    }
    case WM_GETDLGCODE:
        if (lp16) { msg16_to_msg32((const MSG16 *)ptrseg(lp16), &c->u.m); c->lp = (LPARAM)&c->u.m; }
        break;
    }
}

static u32 conv16to32_result(HWND hwnd, u16 msg16, Conv32 *c, LRESULT r)
{
    (void)hwnd;
    switch (c->kind) {
    case K32_MINMAX:
        minmax32to16(&c->u.mmi, (MINMAXINFO16 *)ptrseg(c->lp16));
        break;
    case K32_WINPOS:
        winpos32to16_z(&c->u.wpos, (WINDOWPOS16 *)ptrseg(c->lp16));
        break;
    case K32_NCCALC: {
        NCCALCSIZE_PARAMS16 *n16 = (NCCALCSIZE_PARAMS16 *)ptrseg(c->lp16);
        rect32to16(&c->u.ncs.nc.rgrc[0], &n16->rgrc[0]);
        rect32to16(&c->u.ncs.nc.rgrc[1], &n16->rgrc[1]);
        rect32to16(&c->u.ncs.nc.rgrc[2], &n16->rgrc[2]);
        if (n16->lppos) winpos32to16(&c->u.ncs.wp, (WINDOWPOS16 *)ptrseg(n16->lppos));
        break;
    }
    case K32_NCCALC_RECT:
    case K32_RECT16:
        if (c->lp16) rect32to16(&c->u.rc, (RECT16 *)ptrseg(c->lp16));
        break;
    case K32_MEASURE: {
        MEASUREITEMSTRUCT16 *m16 = (MEASUREITEMSTRUCT16 *)ptrseg(c->lp16);
        m16->itemWidth = (u16)c->u.mis.itemWidth;
        m16->itemHeight = (u16)c->u.mis.itemHeight;
        break;
    }
    case K32_SELITEMS: {
        s16 *d = (s16 *)ptrseg(c->lp16);
        int i;
        for (i = 0; i < (int)r && i < 256 && d; i++) d[i] = (s16)c->u.ints[i];
        break;
    }
    case K32_ITEMDATA:
        if (c->lp16) { u32 v = (u32)c->u.data; memcpy(ptrseg(c->lp16), &v, 4); }
        if (r != LB_ERR) r = 4;
        break;
    }
    switch (c->msg) {
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC:
        return HGDI_16((HGDIOBJ)r);
    case WM_GETFONT:
        return HGDI_16((HGDIOBJ)zoom_unscale_font((HFONT)r));
    case WM_QUERYDRAGICON: case STM_GETICON: case STM_SETICON:
        return HICON_16(zoom_icon_orig((HICON)r));
    }
    (void)msg16;
    return (u32)r;
}

/* sync a structure the 16-bit code may have changed back into the original
   Win32 structure before handing the original parameters to native code */
static void ctx_sync_16to32(MsgCtx *x)
{
    switch (x->kind) {
    case K_MINMAX: minmax16to32_zmerge(x->hwnd, (MINMAXINFO16 *)ptrseg(x->lp16), &x->orig.mmi, (MINMAXINFO *)x->lp); break;
    case K_WINPOS: winpos16to32_zmerge((WINDOWPOS16 *)ptrseg(x->lp16), &x->orig.wpos, (WINDOWPOS *)x->lp); break;
    case K_NCCALC: {
        NCCALCSIZE_PARAMS *nc = (NCCALCSIZE_PARAMS *)x->lp;
        NCCALCSIZE_PARAMS16 *n16 = (NCCALCSIZE_PARAMS16 *)ptrseg(x->lp16);
        rect16to32_merge(&n16->rgrc[0], &nc->rgrc[0]);
        rect16to32_merge(&n16->rgrc[1], &nc->rgrc[1]);
        rect16to32_merge(&n16->rgrc[2], &nc->rgrc[2]);
        if (nc->lppos && n16->lppos) winpos16to32_merge((WINDOWPOS16 *)ptrseg(n16->lppos), nc->lppos);
        break;
    }
    case K_NCCALC_RECT: rect16to32_merge((RECT16 *)ptrseg(x->lp16), (RECT *)x->lp); break;
    }
}
static void ctx_sync_32to16(MsgCtx *x)
{
    switch (x->kind) {
    case K_MINMAX:
        minmax32to16_z(x->hwnd, (MINMAXINFO *)x->lp, (MINMAXINFO16 *)ptrseg(x->lp16));
        x->orig.mmi = *(MINMAXINFO16 *)ptrseg(x->lp16);
        break;
    case K_WINPOS:
        winpos32to16_z((WINDOWPOS *)x->lp, (WINDOWPOS16 *)ptrseg(x->lp16));
        x->orig.wpos = *(WINDOWPOS16 *)ptrseg(x->lp16);
        break;
    case K_NCCALC: {
        NCCALCSIZE_PARAMS *nc = (NCCALCSIZE_PARAMS *)x->lp;
        NCCALCSIZE_PARAMS16 *n16 = (NCCALCSIZE_PARAMS16 *)ptrseg(x->lp16);
        rect32to16(&nc->rgrc[0], &n16->rgrc[0]);
        rect32to16(&nc->rgrc[1], &n16->rgrc[1]);
        rect32to16(&nc->rgrc[2], &n16->rgrc[2]);
        if (nc->lppos && n16->lppos) winpos32to16(nc->lppos, (WINDOWPOS16 *)ptrseg(n16->lppos));
        break;
    }
    case K_NCCALC_RECT: rect32to16((RECT *)x->lp, (RECT16 *)ptrseg(x->lp16)); break;
    }
}

static u32 result32to16_passthrough(MsgCtx *x, LRESULT r)
{
    switch (x->msg) {
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC:
    case WM_GETFONT:
        return HGDI_16((HGDIOBJ)zoom_unscale_font((HFONT)r));
    case WM_QUERYDRAGICON:
        return HICON_16((HICON)r);
    }
    return (u32)r;
}

/* kind: 0 = DefWindowProcA, 1 = CallWindowProcA(proc) */
u32 send_to_32(WNDPROC proc, HWND hwnd, u16 msg16, u16 wp16, u32 lp16, int defproc)
{
    Conv32 c;
    LRESULT r;
    MsgCtx *x = g_ctx;
    /* exact passthrough of a message we are currently delivering */
    if (x && x->hwnd == hwnd && x->msg16 == msg16 && x->wp16 == wp16 && x->lp16 == lp16) {
        ctx_sync_16to32(x);
        x->passthrough = 1;
        if (proc) r = CallWindowProcA(proc, hwnd, x->msg, x->wp, x->lp);
        else r = DefWindowProcA(hwnd, x->msg, x->wp, x->lp);
        ctx_sync_32to16(x);
        return result32to16_passthrough(x, r);
    }
    (void)defproc;
    conv16to32(hwnd, msg16, wp16, lp16, &c, proc != NULL);
    if (proc) r = CallWindowProcA(proc, hwnd, c.msg, c.wp, c.lp);
    else r = DefWindowProcA(hwnd, c.msg, c.wp, c.lp);
    return conv16to32_result(hwnd, msg16, &c, r);
}

/* SendMessage16 / PostMessage16 */
u32 send_message_16(u16 hwnd16, u16 msg16, u16 wp16, u32 lp16, int post)
{
    HWND hwnd = (hwnd16 == 0xFFFF) ? HWND_BROADCAST : HWND_32(hwnd16);
    WndInfo *wi = (hwnd16 == 0xFFFF) ? NULL : wnd_info(hwnd, 0);
    Conv32 c;
    LRESULT r;
    int native_target = !(wi && wi->proc16 && !wi->native_old && !wi->is_dialog);

    if (post) {
        conv16to32(hwnd, msg16, wp16, lp16, &c, native_target);
        return PostMessageA(hwnd, c.msg, c.wp, c.lp) ? 1 : 0;
    }
    if (!native_target && GetWindowThreadProcessId(hwnd, NULL) == GetCurrentThreadId()) {
        /* our own 16-bit window: call its procedure directly, like Win16 */
        return call16_wndproc(wi->proc16, g_mod.hInstance, hwnd16, msg16, wp16, lp16);
    }
    conv16to32(hwnd, msg16, wp16, lp16, &c, native_target);
    r = SendMessageA(hwnd, c.msg, c.wp, c.lp);
    return conv16to32_result(hwnd, msg16, &c, r);
}

/* ---- MSG structures ---- */

void msg32_to_msg16(const MSG *m, MSG16 *m16)
{
    WPARAM wp = m->wParam;
    LPARAM lp = m->lParam;
    m16->hwnd = HWND_16(m->hwnd);
    m16->message = (u16)m->message;
    m16->wParam = (u16)wp;
    m16->lParam = (u32)lp;
    m16->time = m->time;
    m16->pt.x = (s16)zfloor(m->pt.x);
    m16->pt.y = (s16)zfloor(m->pt.y);
    if (is_mouse_msg(m->message)) m16->lParam = zpt_down((u32)lp);
    switch (m->message) {
    case WM_COMMAND: case WM_ACTIVATE: case WM_CHARTOITEM: case WM_VKEYTOITEM:
        m16->wParam = LOWORD(wp);
        m16->lParam = MAKELONG(HWND_16((HWND)lp), HIWORD(wp));
        break;
    case WM_HSCROLL: case WM_VSCROLL:
        m16->wParam = LOWORD(wp);
        m16->lParam = MAKELONG(HIWORD(wp), HWND_16((HWND)lp));
        break;
    case WM_TIMER:
        m16->lParam = 0;
        break;
    case WM_PAINT:
        m16->wParam = wp ? HDC_16((HDC)wp) : 0;
        break;
    }
}

void msg16_to_msg32(const MSG16 *m16, MSG *m)
{
    Conv32 c;
    HWND hwnd = HWND_32(m16->hwnd);
    conv16to32(hwnd, m16->message, m16->wParam, m16->lParam, &c, 0);
    m->hwnd = hwnd;
    m->message = c.msg;
    m->wParam = c.wp;
    m->lParam = c.lp;
    m->time = m16->time;
    m->pt.x = m16->pt.x * g_zoom;
    m->pt.y = m16->pt.y * g_zoom;
}
