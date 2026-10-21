/*
 * dark.c - dark mode.
 *
 * The game's own drawing is left alone where it is artwork (bitmaps, icons,
 * metafiles) and recoloured where it is user interface: when the game draws
 * into one of its windows, light background colours (window and panel
 * backgrounds, fills, text backgrounds) are made dark and dark foreground
 * colours (text, lines) are made light, keeping their hue.  Colours that are
 * already dark stay dark as backgrounds (the dungeon stays black) and light
 * text stays light.  Native parts get the matching Windows dark-mode
 * treatment where Windows offers one: title bars, menus, scroll bars, plus
 * dark colours for dialog boxes and our own painting of push buttons and the
 * menu bar.
 */
#include "win16.h"

int g_dark;
int g_dark_setting = -1;    /* -1 = follow Windows, 0 = off, 1 = on */
int g_dark_dim = 60;        /* brightness of white in the map in dark mode, percent (100 = no dimming) */
int g_dark_dim_on = 1;      /* "Dim the Map" option */
static u8 g_dimlut[256];
static int g_dimlut_pct = -1;

int dark_dim_active(void) { return g_dark && g_dark_dim_on && g_dark_dim < 100; }

/* per-channel brightness table: very dark values stay, the rest is scaled so
   that white ends at g_dark_dim percent */
const u8 *dark_dim_lut(void)
{
    if (g_dimlut_pct != g_dark_dim) {
        int v, top = 255 * g_dark_dim / 100;
        for (v = 0; v < 256; v++) g_dimlut[v] = (u8)(v <= 40 ? v : 40 + (v - 40) * (top - 40) / 215);
        g_dimlut_pct = g_dark_dim;
    }
    return g_dimlut;
}

/* ---- colour transforms ------------------------------------------------------- */

static void rgb_to_hsl(COLORREF c, double *h, double *s, double *l)
{
    double r = GetRValue(c) / 255.0, g = GetGValue(c) / 255.0, b = GetBValue(c) / 255.0;
    double mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    double mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double d = mx - mn;
    *l = (mx + mn) / 2;
    if (d < 1e-9) { *h = 0; *s = 0; return; }
    *s = *l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == r) *h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) *h = (b - r) / d + 2;
    else *h = (r - g) / d + 4;
    *h /= 6;
}

static double hue2rgb(double p, double q, double t)
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

static COLORREF hsl_to_rgb(double h, double s, double l)
{
    double r, g, b;
    if (s < 1e-9) r = g = b = l;
    else {
        double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
        r = hue2rgb(p, q, h + 1.0 / 3);
        g = hue2rgb(p, q, h);
        b = hue2rgb(p, q, h - 1.0 / 3);
    }
    return RGB((int)(r * 255 + 0.5), (int)(g * 255 + 0.5), (int)(b * 255 + 0.5));
}

/* perceived lightness 0..1 */
static double luma(COLORREF c) { return (0.299 * GetRValue(c) + 0.587 * GetGValue(c) + 0.114 * GetBValue(c)) / 255.0; }

#define BG_KEEP  0.40    /* backgrounds at or below this lightness stay as they are */
#define BG_DARK  0.12    /* white becomes this */
#define FG_KEEP  0.60    /* foregrounds at or above this lightness stay as they are */
#define FG_LIGHT 0.88    /* black becomes this */

/* background / fill colour */
COLORREF dark_bg(COLORREF c)
{
    double h, s, l, y = luma(c), ny;
    if ((c >> 24) || y <= BG_KEEP) return c;     /* palette-relative / already dark */
    ny = BG_KEEP - (y - BG_KEEP) * (BG_KEEP - BG_DARK) / (1.0 - BG_KEEP);
    rgb_to_hsl(c, &h, &s, &l);
    return hsl_to_rgb(h, s * 0.8, l * ny / y);
}

/* text / line colour */
COLORREF dark_fg(COLORREF c)
{
    double h, s, l, y = luma(c), ny;
    if ((c >> 24) || y >= FG_KEEP) return c;
    ny = FG_KEEP + (FG_KEEP - y) * (FG_LIGHT - FG_KEEP) / FG_KEEP;
    rgb_to_hsl(c, &h, &s, &l);
    /* raise lightness until the luma target is met (hue kept) */
    {
        double lo = l, hi = 1.0;
        int i;
        for (i = 0; i < 20; i++) {
            double mid = (lo + hi) / 2;
            if (luma(hsl_to_rgb(h, s, mid)) < ny) lo = mid; else hi = mid;
        }
        return hsl_to_rgb(h, s, hi);
    }
}

/* Windows that show the game's map (the dungeon view and the map overview)
   are artwork: only their white background is darkened there. */
int dark_policy_for(HWND h)
{
    char cls[32];
    if (h && GetClassNameA(h, cls, sizeof cls) && (!_stricmp(cls, "Dungeon") || !_stricmp(cls, "Zoom"))) return DARK_MAP;
    return DARK_UI;
}

static int is_white(COLORREF c) { return luma(c) > 0.97; }

COLORREF dark_bg_p(int policy, COLORREF c) { return (policy == DARK_MAP && !is_white(c)) ? c : dark_bg(c); }
COLORREF dark_fg_p(int policy, COLORREF c) { return policy == DARK_MAP ? c : dark_fg(c); }

/* system colours as the dark scheme shows them */
COLORREF dark_syscolor(int i)
{
    switch (i) {
    case COLOR_WINDOW: case COLOR_MENU: case COLOR_APPWORKSPACE: case COLOR_BACKGROUND: return RGB(30, 30, 30);
    case COLOR_BTNFACE: case COLOR_SCROLLBAR: case COLOR_INACTIVECAPTION: return RGB(51, 51, 51);
    case COLOR_BTNSHADOW: return RGB(24, 24, 24);
    case COLOR_BTNHIGHLIGHT: return RGB(96, 96, 96);
    case COLOR_WINDOWTEXT: case COLOR_MENUTEXT: case COLOR_BTNTEXT: case COLOR_CAPTIONTEXT:
    case COLOR_INACTIVECAPTIONTEXT: return RGB(224, 224, 224);
    case COLOR_GRAYTEXT: return RGB(128, 128, 128);
    case COLOR_HIGHLIGHT: return RGB(0, 90, 158);
    case COLOR_HIGHLIGHTTEXT: return RGB(255, 255, 255);
    case COLOR_WINDOWFRAME: case COLOR_ACTIVEBORDER: case COLOR_INACTIVEBORDER: return RGB(70, 70, 70);
    }
    return dark_bg(GetSysColor(i));
}

/* ---- brushes and pens ----------------------------------------------------------- */

typedef struct { COLORREF c; HBRUSH b; } SolidCache;
static SolidCache g_solid[64];
static int g_nsolid;

HBRUSH dark_solid_brush(COLORREF c)
{
    int i;
    for (i = 0; i < g_nsolid; i++) if (g_solid[i].c == c) return g_solid[i].b;
    if (g_nsolid == 64) { DeleteObject(g_solid[0].b); memmove(&g_solid[0], &g_solid[1], 63 * sizeof g_solid[0]); g_nsolid--; }
    g_solid[g_nsolid].c = c;
    g_solid[g_nsolid].b = CreateSolidBrush(c);
    return g_solid[g_nsolid++].b;
}

/* dark replacement for a brush used to fill part of a window (NULL = keep) */
HBRUSH dark_brush_p(int policy, HBRUSH b)
{
    LOGBRUSH lb;
    ULONG_PTR v = (ULONG_PTR)b;
    if (!b || !policy) return NULL;
    if (v <= COLOR_MENUTEXT + 2 && v > 0) {                         /* COLOR_xxx + 1 */
        COLORREF c = GetSysColor((int)v - 1);
        if (policy == DARK_MAP && !is_white(c)) return NULL;
        return dark_solid_brush(dark_syscolor((int)v - 1));
    }
    if (!GetObjectA(b, sizeof lb, &lb)) return NULL;
    if (lb.lbStyle == BS_SOLID) {
        COLORREF d = dark_bg_p(policy, lb.lbColor);
        return d == lb.lbColor ? NULL : dark_solid_brush(d);
    }
    return NULL;
}

HBRUSH dark_brush_for(HBRUSH b) { return dark_brush_p(DARK_UI, b); }

/* class background brush for the current mode */
HBRUSH dark_class_brush(u16 b16)
{
    HBRUSH b = brush_32(b16);
    if (g_dark) {
        HBRUSH d = dark_brush_for(b);
        if (d) return d;
    }
    return b;
}

typedef struct { HPEN src; int dark; HPEN dst; } PenCache;
static PenCache g_pens[64];
static int g_npens;

/* dark replacement for a pen (NULL = keep) */
HPEN dark_pen_for(HPEN p)
{
    LOGPEN lp;
    COLORREF d;
    int i;
    if (!p) return NULL;
    for (i = 0; i < g_npens; i++) if (g_pens[i].src == p) return g_pens[i].dst;
    if (!GetObjectA(p, sizeof lp, &lp) || lp.lopnStyle == PS_NULL) return NULL;
    d = dark_fg(lp.lopnColor);
    if (g_npens == 64) { if (g_pens[0].dst) DeleteObject(g_pens[0].dst); memmove(&g_pens[0], &g_pens[1], 63 * sizeof g_pens[0]); g_npens--; }
    g_pens[g_npens].src = p;
    g_pens[g_npens].dst = (d == lp.lopnColor) ? NULL : CreatePen(lp.lopnStyle, lp.lopnWidth.x, d);
    return g_pens[g_npens++].dst;
}

/* the game deleted a pen: forget its replacement */
void dark_object_deleted(HGDIOBJ o)
{
    int i;
    for (i = 0; i < g_npens; i++)
        if (g_pens[i].src == (HPEN)o) {
            if (g_pens[i].dst) DeleteObject(g_pens[i].dst);
            g_pens[i] = g_pens[--g_npens];
            return;
        }
}

/* ---- native parts ------------------------------------------------------------------ */

typedef BOOL (WINAPI *AllowDarkModeForWindow_t)(HWND, BOOL);
typedef int (WINAPI *SetPreferredAppMode_t)(int);
typedef void (WINAPI *FlushMenuThemes_t)(void);
typedef HRESULT (WINAPI *SetWindowTheme_t)(HWND, LPCWSTR, LPCWSTR);
typedef HRESULT (WINAPI *DwmSetWindowAttribute_t)(HWND, DWORD, LPCVOID, DWORD);

static AllowDarkModeForWindow_t pAllowDarkModeForWindow;
static SetPreferredAppMode_t pSetPreferredAppMode;
static FlushMenuThemes_t pFlushMenuThemes;
static SetWindowTheme_t pSetWindowTheme;
static DwmSetWindowAttribute_t pDwmSetWindowAttribute;
static DWORD g_build;

static DWORD windows_build(void)
{
    typedef LONG (WINAPI *RtlGetVersion_t)(OSVERSIONINFOW *);
    OSVERSIONINFOW vi;
    RtlGetVersion_t f = (RtlGetVersion_t)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    memset(&vi, 0, sizeof vi);
    vi.dwOSVersionInfoSize = sizeof vi;
    if (!f || f(&vi) != 0 || vi.dwMajorVersion < 10) return 0;
    return vi.dwBuildNumber;
}

static void load_apis(void)
{
    static int done;
    HMODULE ux, dwm;
    if (done) return;
    done = 1;
    g_build = windows_build();
    ux = LoadLibraryA("uxtheme.dll");
    dwm = LoadLibraryA("dwmapi.dll");
    if (ux) {
        pSetWindowTheme = (SetWindowTheme_t)(void *)GetProcAddress(ux, "SetWindowTheme");
        /* undocumented, by ordinal; Windows 10 1809 (17763) and later */
        if (g_build >= 17763) {
            pAllowDarkModeForWindow = (AllowDarkModeForWindow_t)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(133));
            pFlushMenuThemes = (FlushMenuThemes_t)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
            /* 1903+: SetPreferredAppMode; 1809: AllowDarkModeForApp (BOOL) at the same ordinal */
            pSetPreferredAppMode = (SetPreferredAppMode_t)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
        }
    }
    if (dwm) pDwmSetWindowAttribute = (DwmSetWindowAttribute_t)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
}

/* Windows' "apps use light theme" setting */
static int windows_prefers_dark(void)
{
    DWORD v = 1, sz = sizeof v;
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExA(k, "AppsUseLightTheme", NULL, NULL, (BYTE *)&v, &sz) != ERROR_SUCCESS) v = 1;
    RegCloseKey(k);
    return v == 0;
}

void dark_init(void)
{
    char v[16];
    load_apis();
    GetPrivateProfileStringA("Port", "DarkMode", "Auto", v, sizeof v, g_ini_path);
    if (!_stricmp(v, "1") || !_stricmp(v, "On") || !_stricmp(v, "Yes")) g_dark_setting = 1;
    else if (!_stricmp(v, "0") || !_stricmp(v, "Off") || !_stricmp(v, "No")) g_dark_setting = 0;
    else g_dark_setting = -1;
    g_dark_dim = GetPrivateProfileIntA("Port", "DimMapBrightness", 60, g_ini_path);
    if (g_dark_dim < 30) g_dark_dim = 30;
    if (g_dark_dim > 100) g_dark_dim = 100;
    g_dark_dim_on = GetPrivateProfileIntA("Port", "DimMap", 1, g_ini_path) != 0;
    g_dark = g_dark_setting < 0 ? windows_prefers_dark() : g_dark_setting;
    if (pSetPreferredAppMode) pSetPreferredAppMode(g_dark ? 2 : 0);   /* ForceDark / Default */
    if (pFlushMenuThemes) pFlushMenuThemes();
    emu_log("dark mode: setting %d -> %d (Windows build %lu)", g_dark_setting, g_dark, g_build);
}

/* re-evaluates the mode (setting changed, or Windows' theme changed);
   returns 1 if it changed */
int dark_update(void)
{
    int d = g_dark_setting < 0 ? windows_prefers_dark() : g_dark_setting;
    if (d == g_dark) return 0;
    g_dark = d;
    if (pSetPreferredAppMode) pSetPreferredAppMode(g_dark ? 2 : 0);
    if (pFlushMenuThemes) pFlushMenuThemes();
    return 1;
}

/* title bar, scroll bars and theme of one window */
void dark_apply_window(HWND h)
{
    LONG st = GetWindowLongA(h, GWL_STYLE);
    load_apis();
    if (!(st & WS_CHILD) && pDwmSetWindowAttribute) {
        BOOL on = g_dark;
        if (FAILED(pDwmSetWindowAttribute(h, 20, &on, sizeof on)))   /* DWMWA_USE_IMMERSIVE_DARK_MODE */
            pDwmSetWindowAttribute(h, 19, &on, sizeof on);          /* before Windows 10 20H1 */
    }
    if (pAllowDarkModeForWindow) pAllowDarkModeForWindow(h, g_dark);
    if (pSetWindowTheme && (st & (WS_VSCROLL | WS_HSCROLL)))
        pSetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
}

/* ---- menu bar (owner painting through the undocumented UAH messages that
        themed windows receive on Windows 10/11) ---------------------------------------- */

#define WM_UAHDRAWMENU         0x0091
#define WM_UAHDRAWMENUITEM     0x0092
#define WM_UAHMEASUREMENUITEM  0x0094

typedef struct { HMENU hmenu; HDC hdc; DWORD dwFlags; } UAHMENU;
typedef struct { DWORD cx, cy; } UAHSIZE;
typedef union { UAHSIZE rgsizeBar[2]; UAHSIZE rgsizePopup[4]; } UAHMENUITEMMETRICS;
typedef struct { DWORD rgcx[4]; DWORD fUpdateMaxWidths; } UAHMENUPOPUPMETRICS;
typedef struct { int iPosition; UAHMENUITEMMETRICS umim; UAHMENUPOPUPMETRICS umpm; } UAHMENUITEM;
typedef struct { DRAWITEMSTRUCT dis; UAHMENU um; UAHMENUITEM umi; } UAHDRAWMENUITEM;

static const COLORREF MENUBAR_BG = RGB(43, 43, 43), MENUBAR_HOT = RGB(65, 65, 65), MENUBAR_TEXT = RGB(224, 224, 224),
                      MENUBAR_GRAY = RGB(128, 128, 128);

static int menubar_rect(HWND h, RECT *r)
{
    MENUBARINFO mbi;
    RECT wr;
    memset(&mbi, 0, sizeof mbi);
    mbi.cbSize = sizeof mbi;
    if (!GetMenuBarInfo(h, OBJID_MENU, 0, &mbi)) return 0;
    GetWindowRect(h, &wr);
    *r = mbi.rcBar;
    OffsetRect(r, -wr.left, -wr.top);
    return 1;
}

/* returns 1 and *res if the message was handled */
int dark_menubar_msg(HWND h, UINT msg, WPARAM wp, LPARAM lp, LRESULT *res)
{
    (void)wp;
    if (!g_dark) return 0;
    switch (msg) {
    case WM_UAHDRAWMENU: {
        UAHMENU *um = (UAHMENU *)lp;
        RECT r;
        if (!menubar_rect(h, &r)) return 0;
        r.top -= 1;
        FillRect(um->hdc, &r, dark_solid_brush(MENUBAR_BG));
        *res = TRUE;
        return 1;
    }
    case WM_UAHDRAWMENUITEM: {
        UAHDRAWMENUITEM *it = (UAHDRAWMENUITEM *)lp;
        WCHAR text[256];
        MENUITEMINFOW mi;
        UINT st = it->dis.itemState;
        DWORD flags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
        COLORREF tc = MENUBAR_TEXT, bg = MENUBAR_BG;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        mi.fMask = MIIM_STRING;
        mi.dwTypeData = text;
        mi.cch = 255;
        text[0] = 0;
        GetMenuItemInfoW(it->um.hmenu, it->umi.iPosition, TRUE, &mi);
        if (st & (ODS_HOTLIGHT | ODS_SELECTED)) bg = MENUBAR_HOT;
        if (st & (ODS_GRAYED | ODS_DISABLED)) tc = MENUBAR_GRAY;
        if (st & ODS_NOACCEL) flags |= DT_HIDEPREFIX;
        FillRect(it->um.hdc, &it->dis.rcItem, dark_solid_brush(bg));
        SetBkMode(it->um.hdc, TRANSPARENT);
        SetTextColor(it->um.hdc, tc);
        DrawTextW(it->um.hdc, text, -1, &it->dis.rcItem, flags);
        *res = TRUE;
        return 1;
    }
    }
    return 0;
}

/* after the default non-client painting: cover the light line between the
   menu bar and the client area */
void dark_menubar_after_ncpaint(HWND h)
{
    RECT cr, wr;
    HDC dc;
    if (!g_dark || !GetMenu(h) || (GetWindowLongA(h, GWL_STYLE) & WS_CHILD)) return;
    GetClientRect(h, &cr);
    MapWindowPoints(h, NULL, (POINT *)&cr, 2);
    GetWindowRect(h, &wr);
    OffsetRect(&cr, -wr.left, -wr.top);
    cr.bottom = cr.top;
    cr.top--;
    dc = GetWindowDC(h);
    FillRect(dc, &cr, dark_solid_brush(MENUBAR_BG));
    ReleaseDC(h, dc);
}

/* ---- push buttons ------------------------------------------------------------------------ */

/* paints a native push button in dark colours (subclassed in user.c) */
void dark_paint_button(HWND h, HDC dc)
{
    RECT r;
    char text[256];
    LRESULT state = SendMessageA(h, BM_GETSTATE, 0, 0);
    LONG st = GetWindowLongA(h, GWL_STYLE);
    HFONT f = (HFONT)SendMessageA(h, WM_GETFONT, 0, 0), of = NULL;
    BOOL pushed = (state & BST_PUSHED) != 0, focus = (state & BST_FOCUS) != 0;
    BOOL enabled = IsWindowEnabled(h);
    BOOL def = (st & 0x0F) == BS_DEFPUSHBUTTON;
    HPEN pen, op;
    HBRUSH ob;
    GetClientRect(h, &r);
    pen = CreatePen(PS_SOLID, 1, def || focus ? RGB(120, 160, 210) : RGB(100, 100, 100));
    op = SelectObject(dc, pen);
    ob = SelectObject(dc, dark_solid_brush(pushed ? RGB(32, 32, 32) : RGB(58, 58, 58)));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    if (def) { InflateRect(&r, -1, -1); FrameRect(dc, &r, dark_solid_brush(RGB(120, 160, 210))); InflateRect(&r, 1, 1); }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
    GetWindowTextA(h, text, sizeof text);
    if (f) of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? RGB(230, 230, 230) : RGB(120, 120, 120));
    if (pushed) OffsetRect(&r, 1, 1);
    DrawTextA(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE |
              ((SendMessageA(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0));
    if (focus) {
        RECT fr = r;
        InflateRect(&fr, -4, -4);
        SetTextColor(dc, RGB(200, 200, 200));
        DrawFocusRect(dc, &fr);
    }
    if (of) SelectObject(dc, of);
}

static const char BTN_PROP[] = "cotw.dark.oldproc";

/* check boxes and radio buttons, painted at the zoom factor and in the
   current colours (the classic glyphs are a fixed 13 pixels) */
static void paint_check(HWND h, HDC dc)
{
    RECT r, box, tr;
    char text[256];
    LONG st = GetWindowLongA(h, GWL_STYLE);
    int type = st & 0x0F, radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    LRESULT state = SendMessageA(h, BM_GETSTATE, 0, 0);
    int checked = (int)(state & 3), focus = (state & BST_FOCUS) != 0, enabled = IsWindowEnabled(h);
    int s = g_zoom, sz = 13 * s, gap = 4 * s;
    HFONT f = (HFONT)SendMessageA(h, WM_GETFONT, 0, 0), of = NULL;
    HWND parent = GetParent(h);
    HBRUSH bg;
    COLORREF tc, face, edge;
    HPEN pen, op;
    HGDIOBJ ob;
    UINT dt;
    GetClientRect(h, &r);
    /* background and text colour as the parent decides (WM_CTLCOLORSTATIC) */
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
    bg = parent ? (HBRUSH)SendMessageA(parent, WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)h) : NULL;
    if (!bg) bg = g_dark ? dark_solid_brush(dark_syscolor(COLOR_BTNFACE)) : GetSysColorBrush(COLOR_BTNFACE);
    FillRect(dc, &r, bg);
    tc = GetTextColor(dc);
    face = g_dark ? RGB(45, 45, 45) : RGB(255, 255, 255);
    edge = g_dark ? RGB(150, 150, 150) : RGB(64, 64, 64);
    if (!enabled) { tc = g_dark ? RGB(110, 110, 110) : RGB(128, 128, 128); edge = tc; }
    box.left = (st & BS_LEFTTEXT) ? r.right - sz : r.left;
    box.top = r.top + (r.bottom - r.top - sz) / 2;
    box.right = box.left + sz;
    box.bottom = box.top + sz;
    pen = CreatePen(PS_SOLID, s, edge);
    op = SelectObject(dc, pen);
    ob = SelectObject(dc, dark_solid_brush(face));
    if (radio) Ellipse(dc, box.left, box.top, box.right, box.bottom);
    else Rectangle(dc, box.left, box.top, box.right, box.bottom);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
    if (checked) {
        HPEN mp = CreatePen(PS_SOLID, s + s / 2 + 1, tc);
        HGDIOBJ o2 = SelectObject(dc, mp);
        if (radio) {
            HGDIOBJ o3 = SelectObject(dc, dark_solid_brush(tc));
            int in = sz * 3 / 10;
            Ellipse(dc, box.left + in, box.top + in, box.right - in, box.bottom - in);
            SelectObject(dc, o3);
        } else if (checked == BST_INDETERMINATE) {
            RECT ir = { box.left + 3 * s, box.top + 3 * s, box.right - 3 * s, box.bottom - 3 * s };
            FillRect(dc, &ir, dark_solid_brush(edge));
        } else {
            MoveToEx(dc, box.left + sz * 22 / 100, box.top + sz * 52 / 100, NULL);
            LineTo(dc, box.left + sz * 42 / 100, box.top + sz * 72 / 100);
            LineTo(dc, box.left + sz * 78 / 100, box.top + sz * 30 / 100);
        }
        SelectObject(dc, o2);
        DeleteObject(mp);
    }
    GetWindowTextA(h, text, sizeof text);
    tr = r;
    if (st & BS_LEFTTEXT) tr.right = box.left - gap; else tr.left = box.right + gap;
    if (f) of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, tc);
    dt = DT_LEFT | ((st & BS_MULTILINE) ? DT_WORDBREAK : DT_SINGLELINE | DT_VCENTER) |
         ((SendMessageA(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0);
    if (st & BS_MULTILINE) {
        RECT m = tr;
        int th;
        DrawTextA(dc, text, -1, &m, dt | DT_CALCRECT);
        th = m.bottom - m.top;
        tr.top += (tr.bottom - tr.top - th) / 2;
    }
    DrawTextA(dc, text, -1, &tr, dt);
    if (focus && text[0]) {
        RECT fr = tr;
        DrawTextA(dc, text, -1, &fr, dt | DT_CALCRECT);
        if (!(st & BS_MULTILINE)) { fr.top = tr.top + (tr.bottom - tr.top - (fr.bottom - fr.top)) / 2; fr.bottom = fr.top + (fr.bottom - fr.top) + 0; }
        InflateRect(&fr, s, s / 2 + 1);
        SetTextColor(dc, tc);
        SetBkColor(dc, g_dark ? RGB(30, 30, 30) : RGB(255, 255, 255));
        DrawFocusRect(dc, &fr);
    }
    if (of) SelectObject(dc, of);
}

static int custom_kind(HWND h)
{
    LONG type = GetWindowLongA(h, GWL_STYLE) & 0x0F;
    if (GetWindowLongA(h, GWL_STYLE) & BS_PUSHLIKE) return (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) && g_dark ? 1 : 0;
    if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) return g_dark ? 1 : 0;
    if (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX || type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON ||
        type == BS_3STATE || type == BS_AUTO3STATE)
        return (g_dark || g_zoom > 1) ? 2 : 0;
    return 0;
}

static void paint_custom(HWND h, HDC dc, int kind)
{
    if (kind == 1) dark_paint_button(h, dc);
    else paint_check(h, dc);
}

static LRESULT CALLBACK button_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC old = (WNDPROC)GetPropA(h, BTN_PROP);
    LRESULT r;
    int kind;
    if (!old) return DefWindowProcA(h, msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        RemovePropA(h, BTN_PROP);
        SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)old);
        return CallWindowProcA(old, h, msg, wp, lp);
    }
    kind = custom_kind(h);
    if (!kind) return CallWindowProcA(old, h, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint_custom(h, dc, kind);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case BM_SETSTATE: case BM_SETSTYLE: case BM_SETCHECK: case WM_SETFOCUS: case WM_KILLFOCUS:
    case WM_ENABLE: case WM_SETTEXT: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_KEYDOWN: case WM_KEYUP: case WM_MOUSEMOVE: case WM_CAPTURECHANGED: case WM_UPDATEUISTATE: {
        /* classic buttons repaint themselves directly; paint over it */
        HDC dc;
        r = CallWindowProcA(old, h, msg, wp, lp);
        if (IsWindow(h) && IsWindowVisible(h) && (dc = GetDC(h)) != NULL) {
            paint_custom(h, dc, custom_kind(h));
            ReleaseDC(h, dc);
        }
        return r;
    }
    }
    return CallWindowProcA(old, h, msg, wp, lp);
}

/* lets a native button paint itself dark when dark mode is on */
void dark_subclass_button(HWND h)
{
    char cls[16];
    if (!h || GetPropA(h, BTN_PROP)) return;
    if (!GetClassNameA(h, cls, sizeof cls) || _stricmp(cls, "Button")) return;
    SetPropA(h, BTN_PROP, (HANDLE)GetWindowLongPtrA(h, GWLP_WNDPROC));
    SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)button_proc);
}

/* ---- captions of child windows (the inventory's object lists) ------------------- */

static const char NCACT_PROP[] = "cotw.dark.ncactive";

void dark_child_ncactivate(HWND h, int active)
{
    if (active) SetPropA(h, NCACT_PROP, (HANDLE)1);
    else RemovePropA(h, NCACT_PROP);
}

/* repaints the frame and caption of a child window with a caption in dark
   colours, over what the default non-client painting drew */
void dark_child_caption(HWND h)
{
    LONG st = GetWindowLongA(h, GWL_STYLE), ex = GetWindowLongA(h, GWL_EXSTYLE);
    RECT wr, cr, cap, r;
    POINT o = { 0, 0 };
    int bl, br, bb, captop, capbottom, w, ht, caph, x, active;
    HDC dc;
    HBRUSH fb;
    char text[256];
    NONCLIENTMETRICSA ncm;
    HFONT f = NULL, of = NULL;
    if (!g_dark || !(st & WS_CHILD) || (st & WS_CAPTION) != WS_CAPTION || IsIconic(h)) return;
    GetWindowRect(h, &wr);
    GetClientRect(h, &cr);
    ClientToScreen(h, &o);
    w = wr.right - wr.left;
    ht = wr.bottom - wr.top;
    bl = o.x - wr.left;
    br = wr.right - (o.x + cr.right);
    bb = wr.bottom - (o.y + cr.bottom);
    captop = bl;                         /* the top border is as thick as the side ones */
    capbottom = o.y - wr.top;
    if (st & WS_VSCROLL) br -= GetSystemMetrics(SM_CXVSCROLL);
    if (st & WS_HSCROLL) bb -= GetSystemMetrics(SM_CYHSCROLL);
    if (capbottom <= captop || bl < 0 || br < 0 || bb < 0) return;
    caph = capbottom - captop;
    active = GetPropA(h, NCACT_PROP) != NULL;
    dc = GetWindowDC(h);
    if (!dc) return;
    fb = dark_solid_brush(RGB(64, 64, 64));
    SetRect(&r, 0, 0, w, captop); FillRect(dc, &r, fb);
    SetRect(&r, 0, 0, bl, ht); FillRect(dc, &r, fb);
    SetRect(&r, w - br, 0, w, ht); FillRect(dc, &r, fb);
    SetRect(&r, 0, ht - bb, w, ht); FillRect(dc, &r, fb);
    SetRect(&cap, bl, captop, w - br, capbottom);
    FillRect(dc, &cap, dark_solid_brush(active ? RGB(74, 74, 74) : RGB(50, 50, 50)));
    x = cap.left + 2;
    if (st & WS_SYSMENU) {
        int isz = GetSystemMetrics(SM_CXSMICON), cx;
        HICON ic = (HICON)SendMessageA(h, WM_GETICON, ICON_SMALL, 0);
        if (!ic) ic = (HICON)GetClassLongPtrA(h, GCLP_HICONSM);
        if (!ic) ic = (HICON)GetClassLongPtrA(h, GCLP_HICON);
        if (!ic && !(ex & WS_EX_DLGMODALFRAME))   /* what the system draws for windows without an icon */
            ic = (HICON)LoadImageA(NULL, IDI_APPLICATION, IMAGE_ICON, isz, isz, LR_SHARED);
        if (isz > caph - 2) isz = caph - 2;
        if (ic && isz > 0) DrawIconEx(dc, x, cap.top + (caph - isz) / 2, ic, isz, isz, 0, NULL, DI_NORMAL);
        x += isz + 4;
        /* close button */
        {
            int g = caph * 2 / 5, pw = caph >= 40 ? 2 : 1, ox, oy;
            HPEN p = CreatePen(PS_SOLID, pw, active ? RGB(230, 230, 230) : RGB(160, 160, 160));
            HGDIOBJ op = SelectObject(dc, p);
            if (g < 7) g = 7;
            cx = cap.right - caph;
            ox = cx + (caph - g) / 2;
            oy = cap.top + (caph - g) / 2;
            MoveToEx(dc, ox, oy, NULL); LineTo(dc, ox + g, oy + g);
            MoveToEx(dc, ox + g - 1, oy, NULL); LineTo(dc, ox - 1, oy + g);
            SelectObject(dc, op);
            DeleteObject(p);
            cap.right = cx;
        }
    }
    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0))
        f = CreateFontIndirectA((ex & WS_EX_TOOLWINDOW) ? &ncm.lfSmCaptionFont : &ncm.lfCaptionFont);
    if (f) of = SelectObject(dc, f);
    GetWindowTextA(h, text, sizeof text);
    cap.left = x;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, active ? RGB(240, 240, 240) : RGB(170, 170, 170));
    DrawTextA(dc, text, -1, &cap, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (of) SelectObject(dc, of);
    if (f) DeleteObject(f);
    ReleaseDC(h, dc);
}
