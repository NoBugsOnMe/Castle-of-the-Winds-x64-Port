/*
 * msgbox.c - message boxes that follow the zoom factor and dark mode.
 *
 * With zoom 1 and dark mode off the real MessageBox is used.  Otherwise a
 * small dialog with the same buttons, icon and keyboard behaviour is built
 * here, with fonts and sizes multiplied by the zoom factor.
 */
#include "win16.h"

typedef struct {
    const char *text, *caption;
    UINT type;
    int iy;                   /* icon position */
    int nbtn, ids[3], def;
    const char *labels[3];
    HICON icon;
    int iconsz;
    HFONT font;
    RECT rtext;               /* text area (client coordinates) */
    int has_cancel;
} MBox;

static int S(int v) { return v * g_zoom; }

static void mb_buttons(MBox *m)
{
    static const char *const L_OK = "OK", *const L_CANCEL = "Cancel", *const L_YES = "&Yes", *const L_NO = "&No",
                             *const L_RETRY = "&Retry", *const L_ABORT = "&Abort", *const L_IGNORE = "&Ignore";
    switch (m->type & MB_TYPEMASK) {
    case MB_OKCANCEL: m->nbtn = 2; m->ids[0] = IDOK; m->labels[0] = L_OK; m->ids[1] = IDCANCEL; m->labels[1] = L_CANCEL; break;
    case MB_ABORTRETRYIGNORE: m->nbtn = 3;
        m->ids[0] = IDABORT; m->labels[0] = L_ABORT; m->ids[1] = IDRETRY; m->labels[1] = L_RETRY; m->ids[2] = IDIGNORE; m->labels[2] = L_IGNORE; break;
    case MB_YESNOCANCEL: m->nbtn = 3;
        m->ids[0] = IDYES; m->labels[0] = L_YES; m->ids[1] = IDNO; m->labels[1] = L_NO; m->ids[2] = IDCANCEL; m->labels[2] = L_CANCEL; break;
    case MB_YESNO: m->nbtn = 2; m->ids[0] = IDYES; m->labels[0] = L_YES; m->ids[1] = IDNO; m->labels[1] = L_NO; break;
    case MB_RETRYCANCEL: m->nbtn = 2; m->ids[0] = IDRETRY; m->labels[0] = L_RETRY; m->ids[1] = IDCANCEL; m->labels[1] = L_CANCEL; break;
    default: m->nbtn = 1; m->ids[0] = IDOK; m->labels[0] = L_OK; break;
    }
    m->def = (m->type & MB_DEFMASK) == MB_DEFBUTTON2 ? 1 : (m->type & MB_DEFMASK) == MB_DEFBUTTON3 ? 2 : 0;
    if (m->def >= m->nbtn) m->def = 0;
    m->has_cancel = 0;
    {
        int i;
        for (i = 0; i < m->nbtn; i++) if (m->ids[i] == IDCANCEL) m->has_cancel = 1;
    }
}

static INT_PTR CALLBACK mb_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    MBox *m = (MBox *)GetWindowLongPtrA(h, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        HDC dc;
        RECT rt, wr, orc;
        int margin = S(12), gap = S(10), bw = S(78), bh = S(26), maxw, cw, ch, x, i, textx, contenth, owner_ok;
        HWND owner = GetWindow(h, GW_OWNER), deff = NULL;
        HMONITOR mon;
        MONITORINFO mi;
        m = (MBox *)lp;
        SetWindowLongPtrA(h, DWLP_USER, (LONG_PTR)m);
        mon = MonitorFromWindow(owner ? owner : h, MONITOR_DEFAULTTONEAREST);
        mi.cbSize = sizeof mi;
        GetMonitorInfoA(mon, &mi);
        maxw = (mi.rcWork.right - mi.rcWork.left) * 2 / 3;
        if (maxw > S(560)) maxw = S(560);
        /* measure the text */
        dc = GetDC(h);
        SelectObject(dc, m->font);
        SetRect(&rt, 0, 0, maxw, 0);
        DrawTextA(dc, m->text, -1, &rt, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS);
        ReleaseDC(h, dc);
        textx = margin + (m->icon ? m->iconsz + gap : 0);
        contenth = rt.bottom > m->iconsz ? rt.bottom : m->iconsz;
        cw = textx + rt.right + margin;
        i = m->nbtn * bw + (m->nbtn - 1) * gap + 2 * margin;
        if (cw < i) cw = i;
        ch = margin + contenth + gap + bh + margin;
        m->iy = margin + (contenth - m->iconsz) / 2;
        SetWindowTextA(h, m->caption);
        m->rtext.left = textx;
        m->rtext.top = margin + (contenth - rt.bottom) / 2;
        m->rtext.right = textx + rt.right;
        m->rtext.bottom = m->rtext.top + rt.bottom;
        CreateWindowExA(0, "STATIC", m->text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                        m->rtext.left, m->rtext.top, rt.right + 2, rt.bottom, h, (HMENU)100, g_hinst32, NULL);
        SendDlgItemMessageA(h, 100, WM_SETFONT, (WPARAM)m->font, FALSE);
        x = (cw - (m->nbtn * bw + (m->nbtn - 1) * gap)) / 2;
        for (i = 0; i < m->nbtn; i++) {
            HWND b = CreateWindowExA(0, "BUTTON", m->labels[i],
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | (i == m->def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON) | (i == 0 ? WS_GROUP : 0),
                                     x, margin + contenth + gap, bw, bh, h, (HMENU)(INT_PTR)m->ids[i], g_hinst32, NULL);
            SendMessageA(b, WM_SETFONT, (WPARAM)m->font, FALSE);
            dark_subclass_button(b);
            if (i == m->def) deff = b;
            x += bw + gap;
        }
        if (!m->has_cancel && m->nbtn > 1) EnableMenuItem(GetSystemMenu(h, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
        /* size and centre over the owner (or the screen) */
        SetRect(&wr, 0, 0, cw, ch);
        AdjustWindowRectEx(&wr, GetWindowLongA(h, GWL_STYLE), FALSE, GetWindowLongA(h, GWL_EXSTYLE));
        owner_ok = owner && IsWindowVisible(owner) && !IsIconic(owner) && GetWindowRect(owner, &orc);
        if (!owner_ok) orc = mi.rcWork;
        x = (orc.left + orc.right - (wr.right - wr.left)) / 2;
        i = (orc.top + orc.bottom - (wr.bottom - wr.top)) / 2;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (i < mi.rcWork.top) i = mi.rcWork.top;
        SetWindowPos(h, HWND_TOP, x, i, wr.right - wr.left, wr.bottom - wr.top, 0);
        dark_apply_window(h);
        SendMessageA(h, DM_SETDEFID, m->ids[m->def], 0);
        if (deff) SetFocus(deff);
        return FALSE;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (m && m->icon) DrawIconEx(dc, S(12), m->iy, m->icon, m->iconsz, m->iconsz, 0, NULL, DI_NORMAL);
        EndPaint(h, &ps);
        return TRUE;
    }
    case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
        if (g_dark) {
            SetTextColor((HDC)wp, dark_syscolor(COLOR_WINDOWTEXT));
            SetBkColor((HDC)wp, dark_syscolor(COLOR_BTNFACE));
            return (INT_PTR)dark_solid_brush(dark_syscolor(COLOR_BTNFACE));
        }
        return FALSE;
    case WM_COMMAND: {
        int id = LOWORD(wp), i;
        if (id == IDCANCEL && !m->has_cancel) {
            /* Escape / close: only boxes with a single OK button close on it */
            if (m->nbtn == 1) EndDialog(h, IDOK);
            return TRUE;
        }
        for (i = 0; i < m->nbtn; i++) if (m->ids[i] == id) { EndDialog(h, id); return TRUE; }
        return TRUE;
    }
    }
    return FALSE;
}

int port_message_box(HWND owner, const char *text, const char *caption, UINT type)
{
    static WORD tmpl[16];
    MBox m;
    NONCLIENTMETRICSA ncm;
    LOGFONTA lf;
    INT_PTR r;
    HWND top;
    LPCSTR ic = NULL;
    emu_log("MessageBox('%s', '%s', %04X)", text ? text : "", caption ? caption : "", type);
    if (g_zoom == 1 && !g_dark) return MessageBoxA(owner, text, caption, type);
    memset(&m, 0, sizeof m);
    m.text = text ? text : "";
    m.caption = caption ? caption : "Error";
    m.type = type;
    mb_buttons(&m);
    switch (type & MB_ICONMASK) {
    case MB_ICONHAND: ic = IDI_HAND; break;
    case MB_ICONQUESTION: ic = IDI_QUESTION; break;
    case MB_ICONEXCLAMATION: ic = IDI_EXCLAMATION; break;
    case MB_ICONASTERISK: ic = IDI_ASTERISK; break;
    }
    m.iconsz = S(32);
    if (ic) {
        m.icon = (HICON)LoadImageA(NULL, ic, IMAGE_ICON, m.iconsz, m.iconsz, LR_SHARED);
        if (!m.icon) m.icon = LoadIconA(NULL, ic);
    }
    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) lf = ncm.lfMessageFont;
    else {
        memset(&lf, 0, sizeof lf);
        lf.lfHeight = -12;
        snprintf(lf.lfFaceName, sizeof lf.lfFaceName, "Microsoft Sans Serif");
    }
    lf.lfHeight *= g_zoom;
    m.font = CreateFontIndirectA(&lf);
    /* an empty dialog; the controls are made in WM_INITDIALOG */
    memset(tmpl, 0, sizeof tmpl);
    {
        DLGTEMPLATE *t = (DLGTEMPLATE *)tmpl;
        t->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_NOIDLEMSG;
        t->dwExtendedStyle = WS_EX_DLGMODALFRAME;
    }
    if (type & MB_ICONMASK) MessageBeep(type & MB_ICONMASK);
    zoom_flush_all();
    top = owner ? GetAncestor(owner, GA_ROOT) : NULL;
    r = DialogBoxIndirectParamA(g_hinst32, (LPCDLGTEMPLATEA)tmpl, top, mb_proc, (LPARAM)&m);
    DeleteObject(m.font);
    if (r <= 0) r = m.has_cancel ? IDCANCEL : m.ids[0];
    return (int)r;
}
