/*
 * user.c - USER.EXE functions used by Castle of the Winds.
 */
#include "win16.h"

HWND hwnd_after_32(u16 h);
int  res_icon_is_shared(HICON h);
extern DlgIconFix g_dlg_icon_fix[32];
extern int g_dlg_nicon_fix;
extern u16 g_dlg_menu_id;
extern char g_dlg_menu_name[64];

static const char *str16(u32 p) { return p ? (const char *)ptrseg(p) : NULL; }

/* ---- per window info --------------------------------------------------------- */

static WndInfo g_wnd[65536];

WndInfo *wnd_info(HWND hwnd, int create)
{
    u16 id;
    WndInfo *w;
    if (!hwnd) return NULL;
    id = HWND_16(hwnd);
    w = &g_wnd[id];
    if (w->hwnd != hwnd) {
        if (!create) return NULL;
        memset(w, 0, sizeof *w);
        w->hwnd = hwnd;
    }
    return w;
}

void wnd_forget(HWND hwnd)
{
    u16 id = HWND_16(hwnd);
    if (g_wnd[id].hwnd == hwnd) zoom_surface_free(hwnd);
    memset(&g_wnd[id], 0, sizeof g_wnd[id]);
    hmap_remove(&g_hwnd_map, hwnd);
}

/* ---- classes ---------------------------------------------------------------------- */

typedef struct {
    ATOM atom;
    char name[64];
    u32 proc16;
    u16 menu_id;
    char menu_name[64];
    HCURSOR cursor;       /* class cursor as the game registered it (unscaled) */
    u16 brush16;          /* class background as the game registered it */
} ClassInfo;

static ClassInfo g_classes[64];
static int g_nclasses;

static ClassInfo *class_by_atom(ATOM a)
{
    int i;
    for (i = 0; i < g_nclasses; i++) if (g_classes[i].atom == a) return &g_classes[i];
    return NULL;
}

static ClassInfo *class_by_name(const char *n)
{
    int i;
    for (i = 0; i < g_nclasses; i++) if (!_stricmp(g_classes[i].name, n)) return &g_classes[i];
    return NULL;
}

/* brings a window in line with the current zoom / dark settings: class
   cursor and background of the game's classes, fonts of native controls */
void user_window_sync(HWND h, int fonts)
{
    ClassInfo *ci = class_by_atom((ATOM)GetClassWord(h, GCW_ATOM));
    if (ci) {
        HCURSOR c = (HCURSOR)zoom_icon(ci->cursor, TRUE);
        HBRUSH b = dark_class_brush(ci->brush16);
        if ((HCURSOR)GetClassLongPtrA(h, GCLP_HCURSOR) != c) SetClassLongPtrA(h, GCLP_HCURSOR, (LONG_PTR)c);
        if ((HBRUSH)GetClassLongPtrA(h, GCLP_HBRBACKGROUND) != b) SetClassLongPtrA(h, GCLP_HBRBACKGROUND, (LONG_PTR)b);
    } else if (fonts && (GetWindowLongA(h, GWL_STYLE) & WS_CHILD)) {
        WndInfo *wi = wnd_info(h, 0);
        char cls[32];
        if (GetClassNameA(h, cls, sizeof cls) && strcmp(cls, "#32770")) {
            HFONT f = wi ? wi->font16 : NULL;
            SendMessageA(h, WM_SETFONT, (WPARAM)(g_zoom > 1 ? zoom_font(f) : (f ? f : GetStockObject(SYSTEM_FONT))), FALSE);
        }
    }
}

/* did the game give this window a light background (the default for
   windows it does not register, such as dialog boxes)? */
int user_window_light_bg(HWND h)
{
    ClassInfo *ci;
    HBRUSH b;
    LOGBRUSH lb;
    if (!h) return 1;
    ci = class_by_atom((ATOM)GetClassWord(h, GCW_ATOM));
    if (!ci) return 1;
    b = brush_32(ci->brush16);
    if (!b) return 1;
    if ((ULONG_PTR)b <= COLOR_MENUTEXT + 2) lb.lbColor = GetSysColor((int)(ULONG_PTR)b - 1);
    else if (!GetObjectA(b, sizeof lb, &lb) || lb.lbStyle != BS_SOLID) return 1;
    return (GetRValue(lb.lbColor) * 299 + GetGValue(lb.lbColor) * 587 + GetBValue(lb.lbColor) * 114) / 1000 > 128;
}

int user_is_game_window(HWND h)
{
    return class_by_atom((ATOM)GetClassWord(h, GCW_ATOM)) != NULL;
}

void settings_init_sysmenu(HWND h, HMENU m);
int  settings_init_popup(HWND h, HMENU m);
int  settings_owns_menuitem(WPARAM wp, LPARAM lp);
void settings_menu_attached(HWND h, HMENU bar);
int  settings_command(HWND h, UINT cmd);
void settings_theme_changed(void);
void settings_window_created(HWND h);

LRESULT CALLBACK bridge_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WndInfo *wi = wnd_info(hwnd, 1);
    LRESULT r;
    if (!wi->proc16 && !wi->native_old) {
        ClassInfo *ci = class_by_atom((ATOM)GetClassWord(hwnd, GCW_ATOM));
        if (ci) wi->proc16 = ci->proc16;
    }
    /* the port's own options and dark-mode painting come first */
    switch (msg) {
    case WM_SYSCOMMAND:
        if (settings_command(hwnd, (UINT)wp)) return 0;
        break;
    case WM_COMMAND:
        /* items in submenus of the system menu arrive as WM_COMMAND on some systems */
        if (!HIWORD(wp) && !lp && LOWORD(wp) >= 0x2A00 && LOWORD(wp) < 0x2C00 && settings_command(hwnd, LOWORD(wp))) return 0;
        break;
    case WM_INITMENUPOPUP:
        if (HIWORD(lp)) settings_init_sysmenu(hwnd, (HMENU)wp);
        else if (settings_init_popup(hwnd, (HMENU)wp)) return 0;    /* the View menu */
        break;
    case WM_MENUSELECT:
        if (settings_owns_menuitem(wp, lp)) return DefWindowProcA(hwnd, msg, wp, lp);
        break;
    case WM_SETTINGCHANGE:
        if (lp && !lstrcmpiA((const char *)lp, "ImmersiveColorSet")) settings_theme_changed();
        break;
    case 0x0091: case 0x0092:    /* WM_UAHDRAWMENU, WM_UAHDRAWMENUITEM */
        if (dark_menubar_msg(hwnd, msg, wp, lp, &r)) return r;
        break;
    }
    if (wi->proc16) r = send_to_16(wi->proc16, hwnd, msg, wp, lp, 0);
    else if (wi->native_old) r = CallWindowProcA(wi->native_old, hwnd, msg, wp, lp);
    else r = DefWindowProcA(hwnd, msg, wp, lp);
    if (msg == WM_NCPAINT || msg == WM_NCACTIVATE) dark_menubar_after_ncpaint(hwnd);
    if (msg == WM_NCACTIVATE) dark_child_ncactivate(hwnd, (int)wp);
    if (msg == WM_NCPAINT || msg == WM_NCACTIVATE || msg == WM_SETTEXT || msg == WM_NCLBUTTONUP || msg == WM_NCMOUSELEAVE)
        dark_child_caption(hwnd);
    if (msg == WM_ERASEBKGND && !r && g_dark && g_zoom == 1 && wi->proc16 && !GetClassLongPtrA(hwnd, GCLP_HBRBACKGROUND) &&
        !zoom_window_has_surface(hwnd)) {
        /* a window without a background brush: whatever the game does not
           paint would show the light default, keep it dark */
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, dark_solid_brush(dark_bg(RGB(255, 255, 255))));
        r = 1;
    }
    if (msg == WM_NCDESTROY) {
        WndInfo *w2 = wnd_info(hwnd, 0);
        hud_window_destroyed(hwnd);
        if (w2 && w2->native_old) SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)w2->native_old);
        wnd_forget(hwnd);
    }
    return r;
}

static u32 g_pending_dlgproc;

typedef struct {
    u32 proc16;
    u32 param;
    int nicon;
    DlgIconFix icons[32];
    u16 menu_id;
    char menu_name[64];
} DlgCreate;

INT_PTR CALLBACK bridge_dlgproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WndInfo *wi = wnd_info(hwnd, 1);
    INT_PTR r;
    if (!wi->dlgproc16 && g_pending_dlgproc) {
        wi->dlgproc16 = g_pending_dlgproc;
        wi->is_dialog = 1;
        g_pending_dlgproc = 0;
    }
    if (msg == WM_INITDIALOG) {
        DlgCreate *dc = (DlgCreate *)lp;
        int i;
        wi->dlgproc16 = dc->proc16;
        wi->is_dialog = 1;
        for (i = 0; i < dc->nicon; i++) {
            HICON hi;
            u32 name = dc->icons[i].icon_id ? dc->icons[i].icon_id : 0;
            if (!name) {
                /* named icon: put the name on the 16-bit stack temporarily */
                u16 save = SP;
                u32 p = stack_alloc(32);
                snprintf((char *)ptrseg(p), 32, "%s", dc->icons[i].icon_name);
                hi = load_icon_res(p, 0);
                SP = save;
            } else hi = load_icon_res(name, 0);
            if (hi) SendDlgItemMessageA(hwnd, dc->icons[i].ctl_id, STM_SETICON, (WPARAM)zoom_icon(hi, FALSE), 0);
        }
        if (dc->menu_id || dc->menu_name[0]) {
            HMENU hm;
            if (dc->menu_id) hm = load_menu_res(dc->menu_id);
            else {
                u16 save = SP;
                u32 p = stack_alloc(64);
                snprintf((char *)ptrseg(p), 64, "%s", dc->menu_name);
                hm = load_menu_res(p);
                SP = save;
            }
            if (hm) SetMenu(hwnd, hm);
        }
        lp = (LPARAM)dc->param;
    }
    if (!wi->dlgproc16) return FALSE;
    r = (INT_PTR)send_to_16(wi->dlgproc16, hwnd, msg, wp, lp, 1);
    if (msg == WM_INITDIALOG) {
        HWND c;
        dark_apply_window(hwnd);
        for (c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            dark_subclass_button(c);
            if (g_dark) dark_apply_window(c);
        }
    }
    if (g_dark && !r && msg >= WM_CTLCOLORMSGBOX && msg <= WM_CTLCOLORSTATIC) {
        /* the game left the colours to Windows: dark ones */
        HDC dc = (HDC)wp;
        int edit = msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX;
        SetTextColor(dc, dark_syscolor(COLOR_WINDOWTEXT));
        SetBkColor(dc, edit ? dark_syscolor(COLOR_WINDOW) : dark_syscolor(COLOR_BTNFACE));
        r = (INT_PTR)dark_solid_brush(edit ? dark_syscolor(COLOR_WINDOW) : dark_syscolor(COLOR_BTNFACE));
    }
    if (msg == WM_NCDESTROY) wnd_forget(hwnd);
    return r;
}

void USER_RegisterClass(void)
{
    ARGS();
    u32 p = A32();
    WNDCLASS16 *wc = (WNDCLASS16 *)ptrseg(p);
    WNDCLASSA w;
    ATOM a;
    ClassInfo *ci;
    const char *name = str16(wc->lpszClassName);
    memset(&w, 0, sizeof w);
    w.style = wc->style & ~(0x0004u | 0x0100u);
    w.lpfnWndProc = bridge_wndproc;
    w.cbClsExtra = wc->cbClsExtra;
    w.cbWndExtra = wc->cbWndExtra;
    w.hInstance = g_hinst32;
    w.hIcon = HICON_32(wc->hIcon);
    w.hCursor = (HCURSOR)zoom_icon(HICON_32(wc->hCursor), TRUE);
    w.hbrBackground = dark_class_brush(wc->hbrBackground);
    w.lpszClassName = name;
    a = RegisterClassA(&w);
    emu_log("RegisterClass('%s', brush %04X, cursor %04X) -> %04X", name ? name : "?", wc->hbrBackground, wc->hCursor, a);
    if (a && name && !_stricmp(name, "Dungeon")) hud_map_class(a);
    if (!a) { RET16(0); return; }
    ci = class_by_name(name);
    if (!ci) {
        if (g_nclasses == 64) { RET16(0); return; }
        ci = &g_classes[g_nclasses++];
    }
    memset(ci, 0, sizeof *ci);
    ci->atom = a;
    snprintf(ci->name, sizeof ci->name, "%s", name);
    ci->proc16 = wc->lpfnWndProc;
    ci->cursor = (HCURSOR)HICON_32(wc->hCursor);
    ci->brush16 = wc->hbrBackground;
    if (wc->lpszMenuName) {
        if (wc->lpszMenuName >> 16) snprintf(ci->menu_name, sizeof ci->menu_name, "%s", str16(wc->lpszMenuName));
        else ci->menu_id = (u16)wc->lpszMenuName;
    }
    RET16(a);
}

void USER_CreateWindow(void)
{
    ARGS();
    u32 cls = A32(), title = A32(), style = A32();
    s16 x = AS16(), y = AS16(), cx = AS16(), cy = AS16();
    u16 parent = A16(), menu = A16(), hinst = A16();
    u32 param = A32();
    char clsbuf[64];
    LPCSTR clsname;
    HMENU hm = NULL;
    HWND hwnd;
    ClassInfo *ci = NULL;
    int wx = x, wy = y, wcx = cx, wcy = cy;
    (void)hinst;
    if (cls >> 16) {
        clsname = str16(cls);
        ci = class_by_name(clsname);
    } else {
        ci = class_by_atom((ATOM)cls);
        clsname = (LPCSTR)(ULONG_PTR)(u16)cls;
        if (ci) { snprintf(clsbuf, sizeof clsbuf, "%s", ci->name); clsname = clsbuf; }
    }
    if (g_zoom > 1) {
        RECT nc;
        zoom_nc_style(style, 0, !(style & WS_CHILD) && (menu || (ci && (ci->menu_id || ci->menu_name[0]))), &nc);
        if (x != (s16)0x8000) zoom_win_to_real(&nc, &wx, &wy, NULL, NULL);
        if (cx != (s16)0x8000) zoom_win_to_real(&nc, NULL, NULL, &wcx, &wcy);
    }
    if (x == (s16)0x8000) { wx = CW_USEDEFAULT; wy = (style & WS_CHILD) ? 0 : y; }
    if (cx == (s16)0x8000) { wcx = CW_USEDEFAULT; wcy = 0; }
    if (style & WS_CHILD) hm = (HMENU)(ULONG_PTR)menu;
    else {
        hm = HMENU_32(menu);
        if (!menu && ci && (ci->menu_id || ci->menu_name[0])) {
            if (ci->menu_id) hm = load_menu_res(ci->menu_id);
            else {
                u16 save = SP;
                u32 p = stack_alloc(64);
                snprintf((char *)ptrseg(p), 64, "%s", ci->menu_name);
                hm = load_menu_res(p);
                SP = save;
            }
        }
    }
    hwnd = CreateWindowExA(0, clsname, str16(title), style, wx, wy, wcx, wcy,
                           HWND_32(parent), hm, g_hinst32, (LPVOID)(ULONG_PTR)param);
    if (hwnd && g_zoom > 1 && !ci) {
        SendMessageA(hwnd, WM_SETFONT, (WPARAM)zoom_font(NULL), FALSE);   /* native control */
    }
    if (hwnd) {
        user_window_sync(hwnd, 0);
        settings_window_created(hwnd);
        if (!ci) dark_subclass_button(hwnd);
    }
    if (g_log_enabled) {
        RECT wr = { 0, 0, 0, 0 };
        GetWindowRect(hwnd, &wr);
        emu_log("CreateWindow(%s, '%s', %08X, %d,%d %dx%d) -> %p [%04X] at (%d,%d)-(%d,%d)",
                HIWORD((ULONG_PTR)clsname) ? clsname : "#atom", title ? str16(title) : "", style,
                wx, wy, wcx, wcy, (void *)hwnd, HWND_16(hwnd), (int)wr.left, (int)wr.top, (int)wr.right, (int)wr.bottom);
    }
    RET16(HWND_16(hwnd));
}

void USER_DefWindowProc(void)
{
    ARGS();
    u16 hwnd = A16(), msg = A16(), wp = A16();
    u32 lp = A32();
    RET32(send_to_32(NULL, HWND_32(hwnd), msg, wp, lp, 1));
}

void USER_CallWindowProc(void)
{
    ARGS();
    u32 proc = A32();
    u16 hwnd = A16(), msg = A16(), wp = A16();
    u32 lp = A32();
    WNDPROC np = stub_native_proc(proc);
    if (np) { RET32(send_to_32(np, HWND_32(hwnd), msg, wp, lp, 0)); return; }
    RET32(call16_wndproc(proc, g_mod.hInstance, hwnd, msg, wp, lp));
}

void USER_SendMessage(void)
{
    ARGS();
    u16 hwnd = A16(), msg = A16(), wp = A16();
    u32 lp = A32();
    RET32(send_message_16(hwnd, msg, wp, lp, 0));
}

void USER_PostMessage(void)
{
    ARGS();
    u16 hwnd = A16(), msg = A16(), wp = A16();
    u32 lp = A32();
    RET16(send_message_16(hwnd, msg, wp, lp, 1));
}

void USER_SendDlgItemMessage(void)
{
    ARGS();
    u16 hdlg = A16(), id = A16(), msg = A16(), wp = A16();
    u32 lp = A32();
    HWND h = GetDlgItem(HWND_32(hdlg), id);
    if (!h) { RET32(0); return; }
    RET32(send_message_16(HWND_16(h), msg, wp, lp, 0));
}

/* ---- message loop ---------------------------------------------------------------- */

typedef struct { MSG16 m16; MSG m; } MsgCache;
static MsgCache g_mcache[32];
static unsigned g_mcache_pos;

static void cache_msg(const MSG *m, const MSG16 *m16)
{
    MsgCache *c = &g_mcache[g_mcache_pos++ & 31];
    c->m = *m;
    c->m16 = *m16;
}

static const MSG *find_cached(const MSG16 *m16)
{
    unsigned i;
    for (i = 0; i < 32; i++) {
        MsgCache *c = &g_mcache[(g_mcache_pos - 1 - i) & 31];
        if (!memcmp(&c->m16, m16, sizeof *m16)) return &c->m;
    }
    return NULL;
}

void USER_GetMessage(void)
{
    ARGS();
    u32 pmsg = A32();
    u16 hwnd = A16(), mn = A16(), mx = A16();
    MSG m;
    MSG16 m16;
    BOOL r;
    hud_poll();
    zoom_flush_all();
    for (;;) {
        r = GetMessageA(&m, HWND_32(hwnd), mn, mx);
        if (r == -1) { RET16(0); return; }
        if (r && m.message == WM_KEYDOWN && m.wParam == VK_F12 && debug_dump_ds()) continue;
        if (r && !msg_is_known16(m.message)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
            continue;
        }
        break;
    }
    msg32_to_msg16(&m, &m16);
    cache_msg(&m, &m16);
    memcpy(ptrseg(pmsg), &m16, sizeof m16);
    RET16(r ? 1 : 0);
}

void USER_PeekMessage(void)
{
    ARGS();
    u32 pmsg = A32();
    u16 hwnd = A16(), mn = A16(), mx = A16(), flags = A16();
    MSG m;
    MSG16 m16;
    hud_poll();
    zoom_flush_all();
    for (;;) {
        if (!PeekMessageA(&m, HWND_32(hwnd), mn, mx, flags & 3)) {
            RET16(0);
            return;
        }
        if (!msg_is_known16(m.message)) {
            if (!(flags & PM_REMOVE)) PeekMessageA(&m, m.hwnd, m.message, m.message, PM_REMOVE);
            TranslateMessage(&m);
            DispatchMessageA(&m);
            continue;
        }
        break;
    }
    msg32_to_msg16(&m, &m16);
    cache_msg(&m, &m16);
    memcpy(ptrseg(pmsg), &m16, sizeof m16);
    RET16(1);
}

void USER_TranslateMessage(void)
{
    ARGS();
    u32 pmsg = A32();
    MSG16 *m16 = (MSG16 *)ptrseg(pmsg);
    const MSG *c = find_cached(m16);
    MSG m;
    if (c) m = *c; else msg16_to_msg32(m16, &m);
    RET16(TranslateMessage(&m));
}

void USER_DispatchMessage(void)
{
    ARGS();
    u32 pmsg = A32();
    MSG16 m16 = *(MSG16 *)ptrseg(pmsg);
    const MSG *c = find_cached(&m16);
    MSG m;
    if (c) m = *c; else msg16_to_msg32(&m16, &m);
    RET32((u32)DispatchMessageA(&m));
}

void USER_PostQuitMessage(void) { ARGS(); u16 c = A16(); PostQuitMessage((s16)c); RET0(); }
void USER_InitApp(void) { ARGS(); (void)A16(); RET16(1); }

/* ---- dialogs ------------------------------------------------------------------------ */

void USER_DialogBox(void)
{
    ARGS();
    u16 hinst = A16();
    u32 tmpl = A32();
    u16 owner = A16();
    u32 proc = A32();
    int idx = ne_find_resource(5, tmpl);
    const u8 *data;
    u32 len, tlen;
    void *t;
    DlgCreate dc;
    INT_PTR r;
    (void)hinst;
    zoom_flush_all();
    if (idx < 0 || !(data = ne_res_data(idx, &len))) {
        emu_log("DialogBox: template not found");
        RET16(0xFFFF);
        return;
    }
    t = convert_dialog(data, len, &tlen);
    memset(&dc, 0, sizeof dc);
    dc.proc16 = proc;
    dc.param = 0;
    dc.nicon = g_dlg_nicon_fix;
    memcpy(dc.icons, g_dlg_icon_fix, sizeof dc.icons);
    dc.menu_id = g_dlg_menu_id;
    snprintf(dc.menu_name, sizeof dc.menu_name, "%s", g_dlg_menu_name);
    g_pending_dlgproc = proc;
    r = DialogBoxIndirectParamA(g_hinst32, (LPCDLGTEMPLATEA)t, HWND_32(owner), bridge_dlgproc, (LPARAM)&dc);
    g_pending_dlgproc = 0;
    free(t);
    emu_log("DialogBox -> %d", (int)r);
    RET16((u16)(s16)r);
}

void USER_EndDialog(void)
{
    ARGS();
    u16 h = A16();
    s16 r = AS16();
    RET16(EndDialog(HWND_32(h), r));
}

void USER_GetDlgItem(void) { ARGS(); u16 h = A16(), id = A16(); RET16(HWND_16(GetDlgItem(HWND_32(h), id))); }

void USER_SetDlgItemText(void)
{
    ARGS();
    u16 h = A16(), id = A16();
    u32 s = A32();
    SetDlgItemTextA(HWND_32(h), id, str16(s));
    RET0();
}

void USER_GetDlgItemText(void)
{
    ARGS();
    u16 h = A16(), id = A16();
    u32 buf = A32();
    u16 n = A16();
    RET16(GetDlgItemTextA(HWND_32(h), id, (char *)ptrseg(buf), n));
}

void USER_CheckRadioButton(void)
{
    ARGS();
    u16 h = A16(), first = A16(), last = A16(), check = A16();
    CheckRadioButton(HWND_32(h), first, last, check);
    RET0();
}

void USER_CheckDlgButton(void)
{
    ARGS();
    u16 h = A16(), id = A16(), check = A16();
    CheckDlgButton(HWND_32(h), id, check);
    RET0();
}

void USER_GetDlgCtrlID(void) { ARGS(); u16 h = A16(); RET16(GetDlgCtrlID(HWND_32(h))); }

void USER_MessageBox(void)
{
    ARGS();
    u16 h = A16();
    u32 text = A32(), cap = A32();
    u16 type = A16();
    zoom_flush_all();
    RET16(port_message_box(HWND_32(h), str16(text), str16(cap), type));
}

void USER_MessageBeep(void) { ARGS(); u16 t = A16(); MessageBeep(t); RET0(); }

/* ---- windows ------------------------------------------------------------------------- */

void USER_ShowWindow(void)
{
    ARGS();
    u16 h = A16(), c = A16();
    BOOL r = ShowWindow(HWND_32(h), c);
    if (g_log_enabled) {
        RECT wr = { 0, 0, 0, 0 };
        GetWindowRect(HWND_32(h), &wr);
        emu_log("ShowWindow(%04X, %d) -> visible=%d rect (%d,%d)-(%d,%d)", h, c, IsWindowVisible(HWND_32(h)),
                (int)wr.left, (int)wr.top, (int)wr.right, (int)wr.bottom);
    }
    RET16(r);
}
void USER_UpdateWindow(void) { ARGS(); u16 h = A16(); UpdateWindow(HWND_32(h)); zoom_flush_all(); RET0(); }
void USER_IsIconic(void) { ARGS(); u16 h = A16(); RET16(IsIconic(HWND_32(h))); }
void USER_IsWindow(void) { ARGS(); u16 h = A16(); RET16(h && HWND_32(h) && IsWindow(HWND_32(h))); }
void USER_EnableWindow(void) { ARGS(); u16 h = A16(), e = A16(); RET16(EnableWindow(HWND_32(h), e)); }
void USER_IsWindowEnabled(void) { ARGS(); u16 h = A16(); RET16(IsWindowEnabled(HWND_32(h))); }
void USER_BringWindowToTop(void) { ARGS(); u16 h = A16(); RET16(BringWindowToTop(HWND_32(h))); }
void USER_GetActiveWindow(void) { ARGS(); RET16(HWND_16(GetActiveWindow())); }
void USER_SetFocus(void) { ARGS(); u16 h = A16(); RET16(HWND_16(SetFocus(HWND_32(h)))); }
void USER_SetCapture(void) { ARGS(); u16 h = A16(); RET16(HWND_16(SetCapture(HWND_32(h)))); }
void USER_ReleaseCapture(void) { ARGS(); ReleaseCapture(); RET0(); }
void USER_GetCapture(void) { ARGS(); RET16(HWND_16(GetCapture())); }
void USER_GetTopWindow(void) { ARGS(); u16 h = A16(); RET16(HWND_16(GetTopWindow(HWND_32(h)))); }

void USER_DestroyWindow(void)
{
    ARGS();
    u16 h = A16();
    RET16(DestroyWindow(HWND_32(h)));
}

void USER_MoveWindow(void)
{
    ARGS();
    u16 h = A16();
    s16 x = AS16(), y = AS16(), cx = AS16(), cy = AS16();
    u16 rep = A16();
    int wx = x, wy = y, wcx = cx, wcy = cy;
    if (g_zoom > 1) {
        RECT nc;
        zoom_nc(HWND_32(h), &nc);
        zoom_win_to_real(&nc, &wx, &wy, &wcx, &wcy);
    }
    RET16(MoveWindow(HWND_32(h), wx, wy, wcx, wcy, rep));
}

void USER_SetWindowPos(void)
{
    ARGS();
    u16 h = A16(), after = A16();
    s16 x = AS16(), y = AS16(), cx = AS16(), cy = AS16();
    u16 flags = A16();
    int wx = x, wy = y, wcx = cx, wcy = cy;
    if (g_zoom > 1) {
        RECT nc;
        zoom_nc(HWND_32(h), &nc);
        if (!(flags & SWP_NOMOVE)) zoom_win_to_real(&nc, &wx, &wy, NULL, NULL);
        if (!(flags & SWP_NOSIZE)) zoom_win_to_real(&nc, NULL, NULL, &wcx, &wcy);
    }
    RET16(SetWindowPos(HWND_32(h), hwnd_after_32(after), wx, wy, wcx, wcy, flags));
}

void USER_GetWindowRect(void)
{
    ARGS();
    u16 h = A16();
    u32 p = A32();
    RECT r;
    if (g_zoom > 1) zoom_window_rect(HWND_32(h), &r);
    else GetWindowRect(HWND_32(h), &r);
    rect32to16(&r, (RECT16 *)ptrseg(p));
    RET0();
}

void USER_GetClientRect(void)
{
    ARGS();
    u16 h = A16();
    u32 p = A32();
    RECT r;
    GetClientRect(HWND_32(h), &r);
    if (g_zoom > 1) { r.right = zceil(r.right); r.bottom = zceil(r.bottom); }
    rect32to16(&r, (RECT16 *)ptrseg(p));
    RET0();
}

void USER_ClientToScreen(void)
{
    ARGS();
    u16 h = A16();
    u32 p = A32();
    POINT16 *pt = (POINT16 *)ptrseg(p);
    POINT q = { pt->x, pt->y };
    if (g_zoom > 1) {
        POINT o = { 0, 0 };
        ClientToScreen(HWND_32(h), &o);
        q.x += zfloor(o.x); q.y += zfloor(o.y);
    } else ClientToScreen(HWND_32(h), &q);
    pt->x = (s16)q.x; pt->y = (s16)q.y;
    RET0();
}

void USER_ScreenToClient(void)
{
    ARGS();
    u16 h = A16();
    u32 p = A32();
    POINT16 *pt = (POINT16 *)ptrseg(p);
    POINT q = { pt->x, pt->y };
    if (g_zoom > 1) {
        POINT o = { 0, 0 };
        ClientToScreen(HWND_32(h), &o);
        q.x -= zfloor(o.x); q.y -= zfloor(o.y);
    } else ScreenToClient(HWND_32(h), &q);
    pt->x = (s16)q.x; pt->y = (s16)q.y;
    RET0();
}

void USER_ChildWindowFromPoint(void)
{
    ARGS();
    u16 h = A16();
    u32 pt = A32();
    POINT q = { (s16)LOWORD(pt), (s16)HIWORD(pt) };
    if (g_zoom > 1) { q.x = q.x * g_zoom + g_zoom / 2; q.y = q.y * g_zoom + g_zoom / 2; }
    RET16(HWND_16(ChildWindowFromPoint(HWND_32(h), q)));
}

void USER_GetWindowText(void)
{
    ARGS();
    u16 h = A16();
    u32 buf = A32();
    u16 n = A16();
    if (hud_is_main(HWND_32(h)) && n > 0) {
        /* the game gets back its own title, not the one with the stats */
        char *d = (char *)ptrseg(buf);
        snprintf(d, n, "%s", hud_game_title_text());
        RET16((u16)strlen(d));
        return;
    }
    RET16(GetWindowTextA(HWND_32(h), (char *)ptrseg(buf), n));
}

void USER_SetWindowText(void)
{
    ARGS();
    u16 h = A16();
    u32 s = A32();
    if (hud_is_main(HWND_32(h))) hud_game_title(HWND_32(h), str16(s));
    else SetWindowTextA(HWND_32(h), str16(s));
    RET0();
}

static int is_dialog_window(HWND h)
{
    char cls[16];
    return GetClassNameA(h, cls, sizeof cls) && !strcmp(cls, "#32770");
}

void USER_GetWindowWord(void)
{
    ARGS();
    u16 h = A16();
    s16 off = AS16();
    HWND hw = HWND_32(h);
    u16 r = 0;
    switch (off) {
    case -6: r = g_mod.hInstance; break;                                     /* GWW_HINSTANCE */
    case -8: r = HWND_16((HWND)GetWindowLongPtrA(hw, GWLP_HWNDPARENT)); break; /* GWW_HWNDPARENT */
    case -12: r = (u16)GetWindowLongPtrA(hw, GWLP_ID); break;                 /* GWW_ID */
    default:
        if (off >= 0) r = (u16)GetWindowWord(hw, off);
        else r = (u16)GetWindowLongA(hw, off);
    }
    RET16(r);
}

void USER_SetWindowWord(void)
{
    ARGS();
    u16 h = A16();
    s16 off = AS16();
    u16 v = A16();
    HWND hw = HWND_32(h);
    u16 r = 0;
    switch (off) {
    case -6: r = g_mod.hInstance; break;
    case -8: r = HWND_16((HWND)SetWindowLongPtrA(hw, GWLP_HWNDPARENT, (LONG_PTR)HWND_32(v))); break;
    case -12: r = (u16)SetWindowLongPtrA(hw, GWLP_ID, v); break;
    default:
        if (off >= 0) r = (u16)SetWindowWord(hw, off, v);
    }
    RET16(r);
}

void USER_GetWindowLong(void)
{
    ARGS();
    u16 h = A16();
    s16 off = AS16();
    HWND hw = HWND_32(h);
    u32 r = 0;
    if (off == -4) { /* GWL_WNDPROC */
        WndInfo *wi = wnd_info(hw, 0);
        if (wi && wi->proc16) r = wi->proc16;
        else r = native_proc_stub((WNDPROC)GetWindowLongPtrA(hw, GWLP_WNDPROC));
    } else if (off >= 0 && is_dialog_window(hw) && (off == 0 || off == 4 || off == 8)) {
        if (off == 0) r = (u32)GetWindowLongPtrA(hw, DWLP_MSGRESULT);
        else if (off == 4) { WndInfo *wi = wnd_info(hw, 0); r = wi ? wi->dlgproc16 : 0; }
        else r = (u32)GetWindowLongPtrA(hw, DWLP_USER);
    } else if (off >= 0) {
        r = (u32)GetWindowLongA(hw, off);
    } else {
        r = (u32)GetWindowLongA(hw, off);
    }
    RET32(r);
}

void USER_SetWindowLong(void)
{
    ARGS();
    u16 h = A16();
    s16 off = AS16();
    u32 v = A32();
    HWND hw = HWND_32(h);
    u32 r = 0;
    if (off == -4) {
        WndInfo *wi = wnd_info(hw, 1);
        WNDPROC np = stub_native_proc(v);
        if (wi->proc16) {
            r = wi->proc16;
        } else {
            WNDPROC cur = (WNDPROC)GetWindowLongPtrA(hw, GWLP_WNDPROC);
            r = native_proc_stub(wi->native_old ? wi->native_old : cur);
            if (!np && !wi->native_old) {
                wi->native_old = cur;
                SetWindowLongPtrA(hw, GWLP_WNDPROC, (LONG_PTR)bridge_wndproc);
            }
        }
        if (np) {
            /* restoring a native procedure */
            if (wi->native_old == np) {
                SetWindowLongPtrA(hw, GWLP_WNDPROC, (LONG_PTR)np);
                wi->native_old = NULL;
                wi->proc16 = 0;
            } else {
                wi->proc16 = 0;
                SetWindowLongPtrA(hw, GWLP_WNDPROC, (LONG_PTR)np);
            }
        } else {
            wi->proc16 = v;
        }
    } else if (off >= 0 && is_dialog_window(hw) && (off == 0 || off == 8)) {
        r = (u32)SetWindowLongPtrA(hw, off == 0 ? DWLP_MSGRESULT : DWLP_USER, (LONG_PTR)(LONG)v);
    } else if (off >= 0 && is_dialog_window(hw) && off == 4) {
        WndInfo *wi = wnd_info(hw, 1);
        r = wi->dlgproc16;
        wi->dlgproc16 = v;
    } else {
        r = (u32)SetWindowLongA(hw, off, (LONG)v);
    }
    RET32(r);
}

/* ---- painting ------------------------------------------------------------------------- */

typedef struct { HWND hwnd; PAINTSTRUCT ps; HDC dc; } PaintRec;
static PaintRec g_paint[64];
static int g_npaint;

void USER_BeginPaint(void)
{
    ARGS();
    u16 h = A16();
    u32 p = A32();
    PAINTSTRUCT ps;
    PAINTSTRUCT16 *p16 = (PAINTSTRUCT16 *)ptrseg(p);
    RECT lp;
    HDC dc = zoom_beginpaint(HWND_32(h), &ps, &lp);
    if (g_npaint < 64) { g_paint[g_npaint].hwnd = HWND_32(h); g_paint[g_npaint].ps = ps; g_paint[g_npaint].dc = dc; g_npaint++; }
    p16->hdc = HDC_16(dc);
    p16->fErase = (s16)ps.fErase;
    rect32to16(&lp, &p16->rcPaint);
    p16->fRestore = (s16)ps.fRestore;
    p16->fIncUpdate = (s16)ps.fIncUpdate;
    memset(p16->rgbReserved, 0, sizeof p16->rgbReserved);
    RET16(HDC_16(dc));
}

void USER_EndPaint(void)
{
    ARGS();
    u16 h = A16();
    u32 p = A32();
    PAINTSTRUCT16 *p16 = (PAINTSTRUCT16 *)ptrseg(p);
    HWND hw = HWND_32(h);
    int i;
    for (i = g_npaint - 1; i >= 0; i--) {
        if (g_paint[i].hwnd == hw && g_paint[i].dc == HDC_32(p16->hdc)) {
            zoom_endpaint(hw, g_paint[i].dc, &g_paint[i].ps);
            memmove(&g_paint[i], &g_paint[i + 1], (g_npaint - i - 1) * sizeof(PaintRec));
            g_npaint--;
            RET0();
            return;
        }
    }
    {
        PAINTSTRUCT ps;
        memset(&ps, 0, sizeof ps);
        ps.hdc = HDC_32(p16->hdc);
        rect16to32(&p16->rcPaint, &ps.rcPaint);
        EndPaint(hw, &ps);
    }
    RET0();
}

void USER_GetDC(void) { ARGS(); u16 h = A16(); RET16(HDC_16(zoom_getdc(HWND_32(h)))); }
void USER_ReleaseDC(void) { ARGS(); u16 h = A16(), dc = A16(); RET16(zoom_releasedc(HWND_32(h), HDC_32(dc))); }

void USER_InvalidateRect(void)
{
    ARGS();
    u16 h = A16();
    u32 p = A32();
    u16 erase = A16();
    RECT r;
    if (p) {
        rect16to32((RECT16 *)ptrseg(p), &r);
        if (g_zoom > 1) { r.left *= g_zoom; r.top *= g_zoom; r.right *= g_zoom; r.bottom *= g_zoom; }
    }
    InvalidateRect(HWND_32(h), p ? &r : NULL, erase);
    RET0();
}

void USER_ScrollWindow(void)
{
    ARGS();
    u16 h = A16();
    s16 dx = AS16(), dy = AS16();
    u32 prc = A32(), pclip = A32();
    RECT r, c;
    if (prc) rect16to32((RECT16 *)ptrseg(prc), &r);
    if (pclip) rect16to32((RECT16 *)ptrseg(pclip), &c);
    zoom_scroll(HWND_32(h), dx, dy, prc ? &r : NULL, pclip ? &c : NULL);
    RET0();
}

void USER_SetScrollPos(void)
{
    ARGS();
    u16 h = A16(), bar = A16();
    s16 pos = AS16();
    u16 redraw = A16();
    RET16(SetScrollPos(HWND_32(h), bar, pos, redraw));
}

void USER_SetScrollRange(void)
{
    ARGS();
    u16 h = A16(), bar = A16();
    s16 mn = AS16(), mx = AS16();
    u16 redraw = A16();
    SetScrollRange(HWND_32(h), bar, mn, mx, redraw);
    RET0();
}

void USER_FillRect(void)
{
    ARGS();
    u16 dc = A16();
    u32 p = A32();
    u16 br = A16();
    RECT r;
    int res;
    HBRUSH b = brush_32(br);
    if (wdc_dark(dc)) { HBRUSH d = dark_brush_p(wdc_dark(dc), b); if (d) b = d; }
    rect16to32((RECT16 *)ptrseg(p), &r);
    res = FillRect(HDC_32(dc), &r, b);
    wdc_drawn(dc, &r);
    RET16(res);
}

void USER_DrawIcon(void)
{
    ARGS();
    u16 dc = A16();
    s16 x = AS16(), y = AS16();
    u16 ic = A16();
    RECT r = { x, y, x + 32, y + 32 };
    BOOL res = DrawIcon(HDC_32(dc), x, y, HICON_32(ic));
    wdc_drawn(dc, &r);
    RET16(res);
}

void USER_DrawText(void)
{
    ARGS();
    u16 dc = A16();
    u32 s = A32();
    s16 n = AS16();
    u32 p = A32();
    u16 fmt = A16();
    RECT r;
    int res;
    rect16to32((RECT16 *)ptrseg(p), &r);
    res = DrawTextA(HDC_32(dc), str16(s), n, &r, fmt);
    if (!(fmt & DT_CALCRECT)) {
        RECT d = r;
        if (fmt & DT_NOCLIP) { d.left -= 2048; d.right += 2048; d.bottom += 2048; }
        wdc_drawn(dc, &d);
    }
    rect32to16(&r, (RECT16 *)ptrseg(p));
    RET16(res);
}

void USER_TabbedTextOut(void)
{
    ARGS();
    u16 dc = A16();
    s16 x = AS16(), y = AS16();
    u32 s = A32();
    s16 n = AS16(), ntabs = AS16();
    u32 ptabs = A32();
    s16 origin = AS16();
    INT tabs[64];
    int i;
    LONG r;
    const s16 *t = (const s16 *)ptrseg(ptabs);
    if (ntabs > 64) ntabs = 64;
    for (i = 0; i < ntabs && t; i++) tabs[i] = t[i];
    r = TabbedTextOutA(HDC_32(dc), x, y, str16(s), n, t ? ntabs : 0, t ? tabs : NULL, origin);
    {
        RECT d = { x - LOWORD(r), y - HIWORD(r), x + LOWORD(r), y + HIWORD(r) };
        wdc_drawn(dc, &d);
    }
    RET32((u32)r);
}

/* ---- rectangles ----------------------------------------------------------------------- */

void USER_SetRect(void)
{
    ARGS();
    u32 p = A32();
    s16 l = AS16(), t = AS16(), r = AS16(), b = AS16();
    RECT16 *rc = (RECT16 *)ptrseg(p);
    rc->left = l; rc->top = t; rc->right = r; rc->bottom = b;
    RET0();
}

void USER_PtInRect(void)
{
    ARGS();
    u32 p = A32();
    u32 pt = A32();
    RECT16 *rc = (RECT16 *)ptrseg(p);
    s16 x = (s16)LOWORD(pt), y = (s16)HIWORD(pt);
    RET16(x >= rc->left && x < rc->right && y >= rc->top && y < rc->bottom);
}

void USER_OffsetRect(void)
{
    ARGS();
    u32 p = A32();
    s16 dx = AS16(), dy = AS16();
    RECT16 *rc = (RECT16 *)ptrseg(p);
    rc->left += dx; rc->right += dx; rc->top += dy; rc->bottom += dy;
    RET0();
}

void USER_IntersectRect(void)
{
    ARGS();
    u32 pd = A32(), p1 = A32(), p2 = A32();
    RECT a, b, d;
    BOOL r;
    rect16to32((RECT16 *)ptrseg(p1), &a);
    rect16to32((RECT16 *)ptrseg(p2), &b);
    r = IntersectRect(&d, &a, &b);
    rect32to16(&d, (RECT16 *)ptrseg(pd));
    RET16(r);
}

/* ---- input ---------------------------------------------------------------------------- */

void USER_GetKeyState(void) { ARGS(); u16 vk = A16(); RET16((u16)GetKeyState(vk)); }
void USER_GetAsyncKeyState(void) { ARGS(); u16 vk = A16(); RET16((u16)GetAsyncKeyState(vk)); }

void USER_GetCursorPos(void)
{
    ARGS();
    u32 p = A32();
    POINT q;
    POINT16 *pt = (POINT16 *)ptrseg(p);
    GetCursorPos(&q);
    if (g_zoom > 1) { q.x = zfloor(q.x); q.y = zfloor(q.y); }
    pt->x = (s16)q.x; pt->y = (s16)q.y;
    RET0();
}

void USER_SetCursorPos(void)
{
    ARGS();
    s16 x = AS16(), y = AS16();
    if (g_zoom > 1) SetCursorPos(x * g_zoom + g_zoom / 2, y * g_zoom + g_zoom / 2);
    else SetCursorPos(x, y);
    RET0();
}
void USER_ShowCursor(void) { ARGS(); u16 s = A16(); RET16(ShowCursor(s)); }
void USER_SetCursor(void)
{
    ARGS();
    u16 c = A16();
    HCURSOR prev = SetCursor((HCURSOR)zoom_icon(HICON_32(c), TRUE));
    RET16(HICON_16(zoom_icon_orig(prev)));
}

void USER_GetSystemMetrics(void)
{
    ARGS();
    s16 i = AS16();
    int v = GetSystemMetrics(i);
    if (g_zoom > 1 && (i == SM_CXSCREEN || i == SM_CYSCREEN || i == SM_CXFULLSCREEN || i == SM_CYFULLSCREEN))
        v /= g_zoom;
    RET16(v);
}
void USER_GetSysColor(void) { ARGS(); u16 i = A16(); RET32(GetSysColor(i)); }

/* ---- resources ------------------------------------------------------------------------- */

void USER_LoadIcon(void)
{
    ARGS();
    u16 hinst = A16();
    u32 name = A32();
    HICON h;
    if (!hinst) h = LoadIconA(NULL, (LPCSTR)(ULONG_PTR)(u16)name);
    else h = load_icon_res(name, 0);
    if (!h) emu_log("LoadIcon(%08X) failed", name);
    RET16(HICON_16(h));
}

void USER_LoadCursor(void)
{
    ARGS();
    u16 hinst = A16();
    u32 name = A32();
    HCURSOR h;
    if (!hinst) { h = LoadCursorA(NULL, (LPCSTR)(ULONG_PTR)(u16)name); zoom_mark_system_cursor(h); }
    else h = (HCURSOR)load_icon_res(name, 1);
    if (!h) emu_log("LoadCursor(%08X) failed", name);
    RET16(HICON_16(h));
}

void USER_LoadBitmap(void)
{
    ARGS();
    u16 hinst = A16();
    u32 name = A32();
    HBITMAP h;
    if (!hinst) h = LoadBitmapA(NULL, (LPCSTR)(ULONG_PTR)(u16)name);
    else h = load_bitmap_res(name);
    if (!h) emu_log("LoadBitmap(%08X) failed", name);
    RET16(HGDI_16(h));
}

void USER_DestroyIcon(void)
{
    ARGS();
    u16 h = A16();
    HICON hi = HICON_32(h);
    if (!hi) { RET16(0); return; }
    if (res_icon_is_shared(hi)) { RET16(1); return; }
    zoom_icon_destroyed(hi);
    RET16(DestroyIcon(hi));
}

/* convert DDB-format (top-down, word aligned rows) 4bpp or 1bpp bits into an icon */
static const RGBQUAD vga16[16] = {
    {0,0,0,0},{0,0,0x80,0},{0,0x80,0,0},{0,0x80,0x80,0},{0x80,0,0,0},{0x80,0,0x80,0},{0x80,0x80,0,0},{0xC0,0xC0,0xC0,0},
    {0x80,0x80,0x80,0},{0,0,0xFF,0},{0,0xFF,0,0},{0,0xFF,0xFF,0},{0xFF,0,0,0},{0xFF,0,0xFF,0},{0xFF,0xFF,0,0},{0xFF,0xFF,0xFF,0}
};

HICON create_icon16(int w, int h, int planes, int bpp, const u8 *andbits, const u8 *xorbits)
{
    ICONINFO ii;
    HICON hi;
    BITMAPINFO bi;
    void *dibbits;
    HDC dc;
    int x, y;
    int abytes = ((w + 15) / 16) * 2;
    if (planes == 1 && bpp == 1) return CreateIcon(g_hinst32, w, h, 1, 1, andbits, xorbits);
    memset(&ii, 0, sizeof ii);
    ii.fIcon = TRUE;
    ii.hbmMask = CreateBitmap(w, h, 1, 1, andbits);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    dc = GetDC(NULL);
    ii.hbmColor = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &dibbits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!ii.hbmColor || !dibbits) { DeleteObject(ii.hbmMask); return NULL; }
    for (y = 0; y < h; y++) {
        u32 *row = (u32 *)dibbits + (size_t)y * w;
        for (x = 0; x < w; x++) {
            int idx = 0;
            if (planes == 1 && bpp == 4) {
                int xb = ((w * 4 + 15) / 16) * 2;
                u8 b = xorbits[y * xb + x / 2];
                idx = (x & 1) ? (b & 15) : (b >> 4);
            } else if (planes == 4 && bpp == 1) {
                int p, pb = ((w + 15) / 16) * 2;
                for (p = 0; p < 4; p++) if (xorbits[(p * h + y) * pb + x / 8] & (0x80 >> (x & 7))) idx |= 1 << p;
            } else if (bpp == 8) {
                int xb = ((w + 1) / 2) * 2;
                idx = xorbits[y * xb + x] & 15;
            } else if (bpp == 32 || bpp == 24) {
                int bpx = bpp / 8, xb = ((w * bpx + 1) / 2) * 2;
                const u8 *px = xorbits + y * xb + x * bpx;
                row[x] = (u32)px[0] | ((u32)px[1] << 8) | ((u32)px[2] << 16);
                continue;
            }
            row[x] = (u32)vga16[idx].rgbBlue | ((u32)vga16[idx].rgbGreen << 8) | ((u32)vga16[idx].rgbRed << 16);
        }
    }
    (void)abytes;
    hi = CreateIconIndirect(&ii);
    DeleteObject(ii.hbmMask);
    DeleteObject(ii.hbmColor);
    return hi;
}

void USER_CreateIcon(void)
{
    ARGS();
    u16 hinst = A16(), w = A16(), h = A16();
    u16 planes = A16(), bpp = A16();
    u32 pand = A32(), pxor = A32();
    (void)hinst;
    planes &= 0xFF; bpp &= 0xFF;
    RET16(HICON_16(create_icon16(w, h, planes, bpp, (const u8 *)ptrseg(pand), (const u8 *)ptrseg(pxor))));
}

/* ---- menus -------------------------------------------------------------------------- */

void USER_LoadMenu(void)
{
    ARGS();
    u16 hinst = A16();
    u32 name = A32();
    (void)hinst;
    RET16(HMENU_16(load_menu_res(name)));
}

void USER_CreateMenu(void) { ARGS(); RET16(HMENU_16(CreateMenu())); }
void USER_DestroyMenu(void) { ARGS(); u16 h = A16(); RET16(DestroyMenu(HMENU_32(h))); }
void USER_GetMenu(void) { ARGS(); u16 h = A16(); RET16(HMENU_16(GetMenu(HWND_32(h)))); }
void USER_SetMenu(void)
{
    ARGS();
    u16 h = A16(), m = A16();
    BOOL r = SetMenu(HWND_32(h), HMENU_32(m));
    if (r) settings_menu_attached(HWND_32(h), HMENU_32(m));
    RET16(r);
}
void USER_GetSubMenu(void) { ARGS(); u16 m = A16(); s16 p = AS16(); RET16(HMENU_16(GetSubMenu(HMENU_32(m), p))); }
void USER_DrawMenuBar(void) { ARGS(); u16 h = A16(); DrawMenuBar(HWND_32(h)); RET0(); }
void USER_GetSystemMenu(void) { ARGS(); u16 h = A16(), rev = A16(); RET16(HMENU_16(GetSystemMenu(HWND_32(h), rev))); }

void USER_CheckMenuItem(void)
{
    ARGS();
    u16 m = A16(), id = A16(), fl = A16();
    RET16((u16)CheckMenuItem(HMENU_32(m), id, fl));
}

void USER_EnableMenuItem(void)
{
    ARGS();
    u16 m = A16(), id = A16(), fl = A16();
    RET16((u16)EnableMenuItem(HMENU_32(m), id, fl));
}

static UINT_PTR menu_id32(u16 flags, u16 id) { return (flags & MF_POPUP) ? (UINT_PTR)HMENU_32(id) : id; }
static LPCSTR menu_data32(u16 flags, u32 data)
{
    if (flags & (MF_BITMAP)) return (LPCSTR)HGDI_32((u16)data);
    if (flags & MF_OWNERDRAW) return (LPCSTR)(ULONG_PTR)data;
    if (flags & MF_SEPARATOR) return NULL;
    return str16(data);
}

static void menu_log(const char *op, u16 m, u16 pos, u16 flags, u16 id, u32 data)
{
    if (g_log_enabled)
        emu_log("%s(%04X, %04X, flags %04X, id %04X, '%s')", op, m, pos, flags, id,
                (flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR)) ? "" : (data ? str16(data) : ""));
}

static BOOL menu_insert(u16 m, u16 pos, u16 flags, u16 id, u32 data)
{
    menu_log("InsertMenu", m, pos, flags, id, data);
    return InsertMenuA(HMENU_32(m), pos, flags, menu_id32(flags, id), menu_data32(flags, data));
}
static BOOL menu_append(u16 m, u16 flags, u16 id, u32 data)
{
    menu_log("AppendMenu", m, 0, flags, id, data);
    return AppendMenuA(HMENU_32(m), flags, menu_id32(flags, id), menu_data32(flags, data));
}
static BOOL menu_modify(u16 m, u16 pos, u16 flags, u16 id, u32 data)
{
    menu_log("ModifyMenu", m, pos, flags, id, data);
    return ModifyMenuA(HMENU_32(m), pos, flags, menu_id32(flags, id), menu_data32(flags, data));
}

void USER_InsertMenu(void)
{
    ARGS();
    u16 m = A16(), pos = A16(), flags = A16(), id = A16();
    u32 data = A32();
    RET16(menu_insert(m, pos, flags, id, data));
}

void USER_AppendMenu(void)
{
    ARGS();
    u16 m = A16(), flags = A16(), id = A16();
    u32 data = A32();
    RET16(menu_append(m, flags, id, data));
}

void USER_ModifyMenu(void)
{
    ARGS();
    u16 m = A16(), pos = A16(), flags = A16(), id = A16();
    u32 data = A32();
    RET16(menu_modify(m, pos, flags, id, data));
}

void USER_RemoveMenu(void) { ARGS(); u16 m = A16(), pos = A16(), fl = A16(); RET16(RemoveMenu(HMENU_32(m), pos, fl)); }
void USER_DeleteMenu(void) { ARGS(); u16 m = A16(), pos = A16(), fl = A16(); RET16(DeleteMenu(HMENU_32(m), pos, fl)); }

void USER_ChangeMenu(void)
{
    ARGS();
    u16 m = A16(), pos = A16();
    u32 data = A32();
    u16 id = A16(), flags = A16();
    BOOL r;
    if (flags & 0x0100) r = menu_append(m, flags & ~0x0100, id, data);                 /* MF_APPEND */
    else if (flags & 0x0200) r = DeleteMenu(HMENU_32(m), pos, flags & ~0x0200);         /* MF_DELETE */
    else if (flags & 0x0080) r = menu_modify(m, pos, flags & ~0x0080, id, data);        /* MF_CHANGE */
    else if (flags & 0x1000) r = RemoveMenu(HMENU_32(m), (flags & MF_BYPOSITION) ? pos : id, flags & ~0x1000); /* MF_REMOVE */
    else r = menu_insert(m, pos, flags, id, data);
    RET16(r);
}

/* ---- palettes (USER side) ----------------------------------------------------------- */

void USER_SelectPalette(void)
{
    ARGS();
    u16 dc = A16(), pal = A16(), bk = A16();
    RET16(HGDI_16(SelectPalette(HDC_32(dc), (HPALETTE)HGDI_32(pal), bk)));
}

void USER_RealizePalette(void) { ARGS(); u16 dc = A16(); RET16(RealizePalette(HDC_32(dc))); }

/* ---- help --------------------------------------------------------------------------- */

void USER_WinHelp(void)
{
    ARGS();
    u16 h = A16();
    u32 file = A32();
    u16 cmd = A16();
    u32 data = A32();
    help_show(HWND_32(h), str16(file), cmd, data);
    RET16(1);
}
