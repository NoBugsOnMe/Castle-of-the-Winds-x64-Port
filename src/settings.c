/*
 * settings.c - the port's own options: zoom, upscaling and dark mode.
 *
 * They are kept in CASTLE.INI, section [Port] (Zoom=Auto|1..6,
 * Upscaling=Original|Sharp|Soft, DarkMode=Auto|On|Off, DimMap=0|1), and can
 * be changed while playing from the View menu the port adds to the main
 * window's menu bar, or from its system menu (the icon at the left of the
 * title bar, or Alt+Space).
 */
#include "win16.h"
#define SCALE_NO_U32
#include "scale.h"

#define IDM_ZOOM_AUTO  0x2A00
#define IDM_ZOOM_1     0x2A10      /* IDM_ZOOM_1 + (n-1)*0x10 for n = 1..4 */
#define IDM_DARK_AUTO  0x2A80
#define IDM_DARK_ON    0x2A90
#define IDM_DARK_OFF   0x2AA0
#define IDM_DARK_DIM   0x2AB0
#define IDM_UP_OFF     0x2AC0
#define IDM_UP_SHARP   0x2AD0
#define IDM_UP_SOFT    0x2AE0
#define IDM_HUD_BARS   0x2B00
#define IDM_HUD_TITLE  0x2B10
#define IDM_HUD_ENEMY  0x2B20
#define IDM_ZOOM_MENU_MAX 4

void help_settings_changed(void);

static void save_settings(void)
{
    char v[8];
    if (g_zoom_setting) snprintf(v, sizeof v, "%d", g_zoom_setting);
    else snprintf(v, sizeof v, "Auto");
    WritePrivateProfileStringA("Port", "Zoom", v, g_ini_path);
    WritePrivateProfileStringA("Port", "DarkMode", g_dark_setting < 0 ? "Auto" : g_dark_setting ? "On" : "Off", g_ini_path);
    WritePrivateProfileStringA("Port", "DimMap", g_dark_dim_on ? "1" : "0", g_ini_path);
    WritePrivateProfileStringA("Port", "Upscaling", g_upscale == SCALE_SOFT ? "Soft" : g_upscale ? "Sharp" : "Original", g_ini_path);
    WritePrivateProfileStringA("Port", "CombatBars", g_hud_bars ? "1" : "0", g_ini_path);
    WritePrivateProfileStringA("Port", "TitleStats", g_hud_title ? "1" : "0", g_ini_path);
    WritePrivateProfileStringA("Port", "EnemyBars", g_hud_enemy ? "1" : "0", g_ini_path);
}

/* ---- window enumeration ------------------------------------------------------- */

typedef struct {
    HWND h, parent;
    int x, y, cx, cy;         /* logical; children relative to the parent's client area */
    int top, maxed, iconic;
    int pw, ph, pmax;         /* children: parent's logical client size; parent maximized */
} WGeo;

typedef struct { WGeo *v; int n, cap; } WList;

static void wl_add(WList *l, const WGeo *g)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->v = realloc(l->v, l->cap * sizeof *l->v);
    }
    l->v[l->n++] = *g;
}

static void geo_of(HWND h, WGeo *g, int top)
{
    RECT r;
    memset(g, 0, sizeof *g);
    g->h = h;
    g->top = top;
    if (g_zoom > 1) zoom_window_rect(h, &r);
    else GetWindowRect(h, &r);
    if (!top) {
        POINT o = { 0, 0 };
        RECT pr;
        g->parent = GetParent(h);
        ClientToScreen(g->parent, &o);
        GetClientRect(g->parent, &pr);
        g->pw = zceil(pr.right);
        g->ph = zceil(pr.bottom);
        g->pmax = IsZoomed(g->parent) && !(GetWindowLongA(g->parent, GWL_STYLE) & WS_CHILD);
        r.left -= zfloor(o.x); r.right -= zfloor(o.x);
        r.top -= zfloor(o.y); r.bottom -= zfloor(o.y);
    } else {
        g->maxed = IsZoomed(h);
        g->iconic = IsIconic(h);
    }
    g->x = r.left; g->y = r.top;
    g->cx = r.right - r.left; g->cy = r.bottom - r.top;
}

static BOOL CALLBACK enum_child(HWND h, LPARAM lp)
{
    WGeo g;
    geo_of(h, &g, 0);
    wl_add((WList *)lp, &g);
    return TRUE;
}

static BOOL CALLBACK enum_top(HWND h, LPARAM lp)
{
    WGeo g;
    char cls[64];
    GetClassNameA(h, cls, sizeof cls);
    if (!IsWindowVisible(h) && !user_is_game_window(h)) return TRUE;
    geo_of(h, &g, 1);
    wl_add((WList *)lp, &g);
    EnumChildWindows(h, enum_child, lp);
    return TRUE;
}

static void collect(WList *l)
{
    memset(l, 0, sizeof *l);
    EnumThreadWindows(GetCurrentThreadId(), enum_top, (LPARAM)l);
}

/* ---- zoom ------------------------------------------------------------------------ */

static HWND g_main;

static int factor_for(int setting)
{
    if (setting) return setting;
    if (g_main) {
        HMONITOR m = MonitorFromWindow(g_main, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (m && GetMonitorInfoA(m, &mi)) {
            int w = mi.rcMonitor.right - mi.rcMonitor.left, h = mi.rcMonitor.bottom - mi.rcMonitor.top, s;
            s = h / 700;
            if (w / 1024 < s) s = w / 1024;
            if (s < 1) s = 1;
            if (s > ZOOM_MAX) s = ZOOM_MAX;
            return s;
        }
    }
    return zoom_auto_factor();
}

static void set_zoom(int setting)
{
    WList l;
    int news, i;
    g_zoom_setting = setting;
    save_settings();
    news = factor_for(setting);
    if (news == g_zoom) return;
    emu_log("zoom: switching from %d to %d", g_zoom, news);
    zoom_flush_all();
    collect(&l);
    for (i = 0; i < l.n; i++) zoom_surface_free(l.v[i].h);
    g_zoom = news;

    /* children keep their logical place inside their parent; in a maximized
       window (whose logical size changes) those that spanned the whole width
       or height are stretched with it - the game sizes its button bar from
       the screen width once, when it creates it */
    for (i = 0; i < l.n; i++) {
        WGeo *g = &l.v[i];
        RECT nc, pr;
        int x = g->x, y = g->y, cx = g->cx, cy = g->cy;
        if (g->top) continue;
        if (g->pmax && GetClientRect(g->parent, &pr)) {
            if (x <= 0 && x + cx >= g->pw) cx += zceil(pr.right) - g->pw;
            if (y <= 0 && y + cy >= g->ph) cy += zceil(pr.bottom) - g->ph;
        }
        zoom_nc(g->h, &nc);
        zoom_win_to_real(&nc, &x, &y, &cx, &cy);
        SetWindowPos(g->h, NULL, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
        user_window_sync(g->h, 1);
    }
    /* top-level windows: maximized ones keep their size (the game sees a new
       logical size), others keep their logical size and their position */
    for (i = 0; i < l.n; i++) {
        WGeo *g = &l.v[i];
        RECT nc, cr;
        if (!g->top) continue;
        user_window_sync(g->h, 1);
        if (g->iconic) continue;
        if (g->maxed) {
            GetClientRect(g->h, &cr);
            SendMessageA(g->h, WM_SIZE, SIZE_MAXIMIZED, MAKELPARAM(cr.right, cr.bottom));
        } else {
            int cx = g->cx, cy = g->cy;
            HMONITOR m = MonitorFromWindow(g->h, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi;
            zoom_nc(g->h, &nc);
            zoom_win_to_real(&nc, NULL, NULL, &cx, &cy);
            mi.cbSize = sizeof mi;
            if (m && GetMonitorInfoA(m, &mi)) {
                if (cx > mi.rcWork.right - mi.rcWork.left) cx = mi.rcWork.right - mi.rcWork.left;
                if (cy > mi.rcWork.bottom - mi.rcWork.top) cy = mi.rcWork.bottom - mi.rcWork.top;
            }
            SetWindowPos(g->h, NULL, 0, 0, cx, cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    SetCursor(zoom_icon(zoom_icon_orig(GetCursor()), TRUE));
    for (i = 0; i < l.n; i++)
        if (l.v[i].top) RedrawWindow(l.v[i].h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    free(l.v);
    help_settings_changed();
}

/* ---- dark mode ---------------------------------------------------------------- */

static void refresh_dark(void)
{
    WList l;
    int i;
    zoom_surfaces_reset();       /* the map windows need a surface while dimmed */
    collect(&l);
    for (i = 0; i < l.n; i++) {
        user_window_sync(l.v[i].h, 0);
        dark_apply_window(l.v[i].h);
        if (l.v[i].top) SetWindowPos(l.v[i].h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    for (i = 0; i < l.n; i++)
        if (l.v[i].top) {
            RedrawWindow(l.v[i].h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
            DrawMenuBar(l.v[i].h);
        }
    free(l.v);
    help_settings_changed();
}

static void set_dark(int setting)
{
    g_dark_setting = setting;
    save_settings();
    if (dark_update()) refresh_dark();
}

/* Windows' light/dark app setting changed */
void settings_theme_changed(void)
{
    if (g_dark_setting < 0 && dark_update()) refresh_dark();
}

/* ---- menus ---------------------------------------------------------------------- */

/* The options appear in two places on the main window: a "View" menu in the
   menu bar (before Help; added again whenever the game switches menu bars,
   e.g. for the inventory) and the system menu (title bar icon / Alt+Space). */

static int has_options(HMENU m)
{
    return m && GetMenuState(m, IDM_ZOOM_AUTO, MF_BYCOMMAND) != (UINT)-1;
}

static void append_options(HMENU m)
{
    HMENU zm = CreatePopupMenu(), dm = CreatePopupMenu(), um = CreatePopupMenu();
    char text[16];
    int i;
    AppendMenuA(zm, MF_STRING, IDM_ZOOM_AUTO, "&Automatic");
    AppendMenuA(zm, MF_SEPARATOR, 0, NULL);
    for (i = 1; i <= IDM_ZOOM_MENU_MAX; i++) {
        snprintf(text, sizeof text, "&%dx", i);
        AppendMenuA(zm, MF_STRING, IDM_ZOOM_1 + (i - 1) * 0x10, text);
    }
    AppendMenuA(dm, MF_STRING, IDM_DARK_AUTO, "&Same as Windows");
    AppendMenuA(dm, MF_SEPARATOR, 0, NULL);
    AppendMenuA(dm, MF_STRING, IDM_DARK_ON, "&On");
    AppendMenuA(dm, MF_STRING, IDM_DARK_OFF, "O&ff");
    AppendMenuA(dm, MF_SEPARATOR, 0, NULL);
    AppendMenuA(dm, MF_STRING, IDM_DARK_DIM, "&Dim the Map");
    AppendMenuA(um, MF_STRING, IDM_UP_OFF, "&Original Pixels");
    AppendMenuA(um, MF_STRING, IDM_UP_SHARP, "&Sharp");
    AppendMenuA(um, MF_STRING, IDM_UP_SOFT, "So&ft");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)zm, "&Zoom");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)um, "&Upscaling");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)dm, "&Dark Mode");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, IDM_HUD_BARS, "Combat &Bars");
    AppendMenuA(m, MF_STRING, IDM_HUD_ENEMY, "&Enemy Health Bars");
    AppendMenuA(m, MF_STRING, IDM_HUD_TITLE, "Stats in &Title Bar");
}

/* radio marks and the current automatic factor */
static void update_options(HMENU m)
{
    char text[64];
    int i;
    snprintf(text, sizeof text, "&Automatic (%dx)", factor_for(0));
    ModifyMenuA(m, IDM_ZOOM_AUTO, MF_BYCOMMAND | MF_STRING, IDM_ZOOM_AUTO, text);
    CheckMenuItem(m, IDM_ZOOM_AUTO, MF_BYCOMMAND | (g_zoom_setting == 0 ? MF_CHECKED : MF_UNCHECKED));
    for (i = 1; i <= IDM_ZOOM_MENU_MAX; i++)
        CheckMenuItem(m, IDM_ZOOM_1 + (i - 1) * 0x10, MF_BYCOMMAND | (g_zoom_setting == i ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_DARK_AUTO, MF_BYCOMMAND | (g_dark_setting < 0 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_DARK_ON, MF_BYCOMMAND | (g_dark_setting == 1 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_DARK_OFF, MF_BYCOMMAND | (g_dark_setting == 0 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_DARK_DIM, MF_BYCOMMAND | (g_dark_dim_on ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_HUD_BARS, MF_BYCOMMAND | (g_hud_bars ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_HUD_TITLE, MF_BYCOMMAND | (g_hud_title ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_HUD_ENEMY, MF_BYCOMMAND | (g_hud_enemy ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_UP_OFF, MF_BYCOMMAND | (g_upscale == 0 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_UP_SHARP, MF_BYCOMMAND | (g_upscale == SCALE_SHARP ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_UP_SOFT, MF_BYCOMMAND | (g_upscale == SCALE_SOFT ? MF_CHECKED : MF_UNCHECKED));
    /* upscaling only does something when the game is zoomed */
    for (i = 0; i < 3; i++)
        EnableMenuItem(m, IDM_UP_OFF + i * 0x10, MF_BYCOMMAND | (g_zoom > 1 ? MF_ENABLED : MF_GRAYED));
}

static void add_to_sysmenu(HWND h)
{
    HMENU sm = GetSystemMenu(h, FALSE);       /* the window's own copy */
    if (!sm || has_options(sm)) return;
    AppendMenuA(sm, MF_SEPARATOR, 0, NULL);
    append_options(sm);
}

/* a menu bar the main window got: add the View menu in front of the
   right-aligned Help menu (the game addresses its menus by position from
   the left, so the positions it uses do not change) */
void settings_menu_attached(HWND h, HMENU bar)
{
    int n, i, pos;
    HMENU v;
    if (!g_main || h != g_main || !bar || has_options(bar)) return;
    n = GetMenuItemCount(bar);
    if (n < 0) return;
    pos = n;
    for (i = 0; i < n; i++) {
        MENUITEMINFOA mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        mi.fMask = MIIM_FTYPE;
        if (GetMenuItemInfoA(bar, i, TRUE, &mi) && (mi.fType & MFT_RIGHTJUSTIFY)) { pos = i; break; }
    }
    v = CreatePopupMenu();
    append_options(v);
    InsertMenuA(bar, pos, MF_BYPOSITION | MF_POPUP, (UINT_PTR)v, "Vi&ew");
    DrawMenuBar(h);
}

void settings_window_created(HWND h)
{
    LONG st = GetWindowLongA(h, GWL_STYLE);
    if (!g_main && !(st & WS_CHILD) && (st & WS_SYSMENU) && user_is_game_window(h)) {
        g_main = h;
        hud_set_main(h);
        add_to_sysmenu(h);
        settings_menu_attached(h, GetMenu(h));
    }
    dark_apply_window(h);
}

/* WM_INITMENUPOPUP for the system menu */
void settings_init_sysmenu(HWND h, HMENU m)
{
    if (h != g_main || !m) return;
    if (!has_options(m)) {               /* reset by the system (or never added) */
        AppendMenuA(m, MF_SEPARATOR, 0, NULL);
        append_options(m);
    }
    update_options(m);
}

/* WM_INITMENUPOPUP for other popups: returns 1 for the port's own menus
   (which the game then does not see) */
int settings_init_popup(HWND h, HMENU m)
{
    (void)h;
    if (!has_options(m)) return 0;
    update_options(m);
    return 1;
}

/* WM_MENUSELECT: is the highlighted item one of the port's? */
int settings_owns_menuitem(WPARAM wp, LPARAM lp)
{
    UINT item = LOWORD(wp), flags = HIWORD(wp);
    HMENU m = (HMENU)lp;
    if (flags == 0xFFFF && !m) return 0;     /* menu closed */
    if (flags & MF_POPUP) return m && has_options(GetSubMenu(m, (int)item));
    return item >= 0x2A00 && item < 0x2C00;
}

/* ---- upscaling ------------------------------------------------------------------ */

static void set_upscale(int mode)
{
    WList l;
    int i;
    if (mode == g_upscale) return;
    g_upscale = mode;
    save_settings();
    emu_log("upscaling: %d", mode);
    if (g_zoom == 1) return;
    SetCursor(zoom_icon(zoom_icon_orig(GetCursor()), TRUE));
    collect(&l);
    for (i = 0; i < l.n; i++)
        if (l.v[i].top) RedrawWindow(l.v[i].h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    free(l.v);
    help_settings_changed();
}

/* WM_SYSCOMMAND; returns 1 if it was one of ours */
int settings_command(HWND h, UINT cmd)
{
    (void)h;
    cmd &= 0xFFF0;
    if (cmd == IDM_ZOOM_AUTO) { set_zoom(0); return 1; }
    if (cmd >= IDM_ZOOM_1 && cmd < IDM_ZOOM_1 + IDM_ZOOM_MENU_MAX * 0x10) { set_zoom((cmd - IDM_ZOOM_1) / 0x10 + 1); return 1; }
    if (cmd == IDM_DARK_AUTO) { set_dark(-1); return 1; }
    if (cmd == IDM_DARK_ON) { set_dark(1); return 1; }
    if (cmd == IDM_DARK_OFF) { set_dark(0); return 1; }
    if (cmd == IDM_HUD_BARS || cmd == IDM_HUD_TITLE || cmd == IDM_HUD_ENEMY) {
        if (cmd == IDM_HUD_BARS) g_hud_bars = !g_hud_bars;
        else if (cmd == IDM_HUD_ENEMY) g_hud_enemy = !g_hud_enemy;
        else g_hud_title = !g_hud_title;
        save_settings();
        hud_settings_changed();
        return 1;
    }
    if (cmd == IDM_UP_OFF) { set_upscale(0); return 1; }
    if (cmd == IDM_UP_SHARP) { set_upscale(SCALE_SHARP); return 1; }
    if (cmd == IDM_UP_SOFT) { set_upscale(SCALE_SOFT); return 1; }
    if (cmd == IDM_DARK_DIM) {
        g_dark_dim_on = !g_dark_dim_on;
        save_settings();
        refresh_dark();
        return 1;
    }
    return 0;
}
