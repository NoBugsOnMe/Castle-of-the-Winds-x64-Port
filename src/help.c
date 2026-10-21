/*
 * help.c - built-in replacement for WinHelp (WINHLP32 is not part of
 *          Windows 10).  Renders the game's help file, converted at build
 *          time by tools/hlp2bin.py and embedded as resource "HELPDATA".
 */
#include "win16.h"
#include <shellapi.h>

/* ---- help data model ------------------------------------------------------ */

enum { IT_TEXT, IT_FONT, IT_BR, IT_TAB, IT_LINK, IT_ENDLINK, IT_BMP };

typedef struct { u8 kind; u8 a; s16 b; u8 c; const char *text; int len; } HItem;
typedef struct { s16 pos; u8 type; } HTab;
typedef struct {
    u8 align; u8 ntabs;
    s16 left, right, first, above, below;
    HTab *tabs;
    int nitems;
    HItem *items;
} HPara;
typedef struct {
    u16 num; s16 prev, next;
    char *title;
    int npara;
    HPara *paras;
} HTopic;
typedef struct { u8 flags, halfpoints, family; char face[40]; } HFontDesc;
typedef struct {
    const BITMAPINFOHEADER *bih; HBITMAP hbm; int w, h; const void *bits; DibVgaBuf *vga;
    u32 *up; int up_s, up_mode;          /* upscaled copy (32 bpp, top-down) */
} HBmp;
typedef struct { u32 id; u16 topic; } HMapEnt;
typedef struct { char *word; int ntopics; u16 *topics; } HKeyword;

static struct {
    int loaded;
    char title[128];
    int ntopics, nfonts, nbmps, nmap, nkw;
    HTopic *topics;
    HFontDesc *fonts;
    HBmp *bmps;
    HMapEnt *map;
    HKeyword *kws;
} H;

static const u8 *hp;
static u16 rdw(void) { u16 v = (u16)(hp[0] | (hp[1] << 8)); hp += 2; return v; }
static u32 rdl(void) { u32 v = (u32)hp[0] | ((u32)hp[1] << 8) | ((u32)hp[2] << 16) | ((u32)hp[3] << 24); hp += 4; return v; }
static char *rds(int *plen)
{
    int n = rdw();
    char *s = malloc(n + 1);
    memcpy(s, hp, n);
    s[n] = 0;
    hp += n;
    if (plen) *plen = n;
    return s;
}

static int help_load(void)
{
    HRSRC r;
    const u8 *base;
    int i, j, k;
    u16 ver;
    if (H.loaded) return H.loaded > 0;
    H.loaded = -1;
    r = FindResourceA(g_hinst32, "HELPDATA", (LPCSTR)RT_RCDATA);
    if (!r) return 0;
    base = (const u8 *)LockResource(LoadResource(g_hinst32, r));
    if (!base || memcmp(base, "CHLP", 4)) return 0;
    hp = base + 4;
    ver = rdw();
    (void)ver;
    H.ntopics = rdw(); H.nfonts = rdw(); H.nbmps = rdw(); H.nmap = rdw(); H.nkw = rdw(); rdw();
    {
        char *t = rds(NULL);
        snprintf(H.title, sizeof H.title, "%s", t);
        free(t);
    }
    H.fonts = calloc(H.nfonts + 1, sizeof(HFontDesc));
    for (i = 0; i < H.nfonts; i++) {
        char *f;
        H.fonts[i].flags = hp[0]; H.fonts[i].halfpoints = hp[1]; H.fonts[i].family = hp[2];
        hp += 4;
        f = rds(NULL);
        snprintf(H.fonts[i].face, sizeof H.fonts[i].face, "%s", f);
        free(f);
    }
    H.topics = calloc(H.ntopics + 1, sizeof(HTopic));
    for (i = 0; i < H.ntopics; i++) {
        HTopic *t = &H.topics[i];
        t->num = rdw(); t->prev = (s16)rdw(); t->next = (s16)rdw();
        t->title = rds(NULL);
        t->npara = rdw();
        t->paras = calloc(t->npara + 1, sizeof(HPara));
        for (j = 0; j < t->npara; j++) {
            HPara *p = &t->paras[j];
            p->align = hp[0]; p->ntabs = hp[1]; hp += 2;
            p->left = (s16)rdw(); p->right = (s16)rdw(); p->first = (s16)rdw(); p->above = (s16)rdw(); p->below = (s16)rdw();
            p->tabs = calloc(p->ntabs + 1, sizeof(HTab));
            for (k = 0; k < p->ntabs; k++) { p->tabs[k].pos = (s16)rdw(); p->tabs[k].type = *hp++; }
            p->nitems = rdw();
            p->items = calloc(p->nitems + 1, sizeof(HItem));
            for (k = 0; k < p->nitems; k++) {
                HItem *it = &p->items[k];
                it->kind = *hp++;
                switch (it->kind) {
                case IT_TEXT: it->text = rds(&it->len); break;
                case IT_FONT: it->b = (s16)rdw(); break;
                case IT_LINK: it->a = hp[0]; hp++; it->b = (s16)rdw(); it->c = *hp++; break;
                case IT_BMP: it->a = *hp++; it->b = (s16)rdw(); break;
                }
            }
        }
    }
    H.bmps = calloc(H.nbmps + 1, sizeof(HBmp));
    for (i = 0; i < H.nbmps; i++) {
        u32 n = rdl();
        H.bmps[i].bih = (const BITMAPINFOHEADER *)hp;
        H.bmps[i].w = H.bmps[i].bih->biWidth;
        H.bmps[i].h = H.bmps[i].bih->biHeight;
        H.bmps[i].bits = (const u8 *)hp + H.bmps[i].bih->biSize +
            (H.bmps[i].bih->biClrUsed ? H.bmps[i].bih->biClrUsed : (H.bmps[i].bih->biBitCount <= 8 ? 1u << H.bmps[i].bih->biBitCount : 0)) * 4;
        H.bmps[i].vga = malloc(sizeof(DibVgaBuf));
        if (H.bmps[i].vga) H.bmps[i].bih = (const BITMAPINFOHEADER *)dib_vga(H.bmps[i].bih, DIB_RGB_COLORS, H.bmps[i].vga);
        hp += n;
    }
    H.map = calloc(H.nmap + 1, sizeof(HMapEnt));
    for (i = 0; i < H.nmap; i++) { H.map[i].id = rdl(); H.map[i].topic = rdw(); }
    H.kws = calloc(H.nkw + 1, sizeof(HKeyword));
    for (i = 0; i < H.nkw; i++) {
        H.kws[i].word = rds(NULL);
        H.kws[i].ntopics = rdw();
        H.kws[i].topics = calloc(H.kws[i].ntopics + 1, sizeof(u16));
        for (k = 0; k < H.kws[i].ntopics; k++) H.kws[i].topics[k] = rdw();
    }
    H.loaded = 1;
    return 1;
}

/* ---- fonts --------------------------------------------------------------------- */

static HFONT g_hfonts[32 * 3];
static HFONT g_btnfont;
static int g_dpi = 96;      /* layout resolution: screen DPI x zoom factor */
static int g_bmpscale = 1;  /* pictures are enlarged with the zoom factor */

/* draws a picture enlarged by g_bmpscale - through the upscaler when it is on */
static void draw_bmp(HDC dc, int x, int y, HBmp *b)
{
    int s = g_bmpscale, mode = zoom_upscale_mode();
    if (s > 1 && mode && b->w > 0 && b->h > 0 && b->w <= 4096 && b->h <= 4096) {
        if (!b->up || b->up_s != s || b->up_mode != mode) {
            BITMAPINFO bi;
            void *bits = NULL;
            HBITMAP hb;
            free(b->up);
            b->up = NULL;
            memset(&bi, 0, sizeof bi);
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = b->w;
            bi.bmiHeader.biHeight = -b->h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            hb = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
            if (hb && bits) {
                HDC m = CreateCompatibleDC(NULL);
                HGDIOBJ o = SelectObject(m, hb);
                StretchDIBits(m, 0, 0, b->w, b->h, 0, 0, b->w, b->h, b->bits, (const BITMAPINFO *)b->bih, DIB_RGB_COLORS, SRCCOPY);
                GdiFlush();
                b->up = malloc((size_t)b->w * s * b->h * s * 4);
                if (b->up && !zoom_scale_pixels((const u32 *)bits, b->w, b->h, b->up, s, mode, 0x00FFFFFF)) {
                    free(b->up);
                    b->up = NULL;
                }
                SelectObject(m, o);
                DeleteDC(m);
            }
            if (hb) DeleteObject(hb);
            b->up_s = s;
            b->up_mode = mode;
        }
        if (b->up) {
            BITMAPINFO bi;
            memset(&bi, 0, sizeof bi);
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = b->w * s;
            bi.bmiHeader.biHeight = -b->h * s;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            StretchDIBits(dc, x, y, b->w * s, b->h * s, 0, 0, b->w * s, b->h * s, b->up, &bi, DIB_RGB_COLORS, SRCCOPY);
            return;
        }
    }
    StretchDIBits(dc, x, y, b->w * s, b->h * s, 0, 0, b->w, b->h, b->bits, (const BITMAPINFO *)b->bih, DIB_RGB_COLORS, SRCCOPY);
}

/* colours of the help text (light or dark) */
static COLORREF help_bg(void) { return g_dark ? RGB(30, 30, 30) : RGB(255, 255, 255); }
static COLORREF help_text(void) { return g_dark ? RGB(222, 222, 222) : RGB(0, 0, 0); }
static COLORREF help_link(void) { return g_dark ? RGB(110, 210, 110) : RGB(0, 128, 0); }
static COLORREF help_frame(void) { return g_dark ? RGB(110, 110, 110) : RGB(0, 0, 0); }

static const char *map_face(const HFontDesc *f)
{
    const char *n = f->face;
    if (!_stricmp(n, "Helv") || !_strnicmp(n, "Univers", 7) || !_stricmp(n, "Arial") || !_strnicmp(n, "MS Sans", 7)) return "Arial";
    if (!_stricmp(n, "Tms Rmn") || !_strnicmp(n, "CG Times", 8) || !_stricmp(n, "Times New Roman") || !_strnicmp(n, "MS Serif", 8)) return "Times New Roman";
    if (!_stricmp(n, "Symbol")) return "Symbol";
    if (!_strnicmp(n, "Cour", 4)) return "Courier New";
    switch (f->family) {
    case 1: return "Courier New";
    case 2: return "Times New Roman";
    default: return "Arial";
    }
}

static HFONT get_font(int idx, int link)
{
    int key;
    if (idx < 0 || idx >= H.nfonts) idx = 0;
    key = (idx & 31) * 3 + link;
    if (!g_hfonts[key]) {
        LOGFONTA lf;
        const HFontDesc *f = &H.fonts[idx];
        memset(&lf, 0, sizeof lf);
        lf.lfHeight = -MulDiv(f->halfpoints ? f->halfpoints : 20, g_dpi, 144);
        lf.lfWeight = (f->flags & 1) ? FW_BOLD : FW_NORMAL;
        lf.lfItalic = (f->flags & 2) ? 1 : 0;
        lf.lfUnderline = ((f->flags & 4) || link == 1) ? 1 : 0;
        lf.lfStrikeOut = (f->flags & 8) ? 1 : 0;
        lf.lfCharSet = !_stricmp(f->face, "Symbol") ? SYMBOL_CHARSET : DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        snprintf(lf.lfFaceName, sizeof lf.lfFaceName, "%s", map_face(f));
        g_hfonts[key] = CreateFontIndirectA(&lf);
    }
    return g_hfonts[key];
}

/* ---- layout ------------------------------------------------------------------------- */

typedef struct { RECT rc; int popup; int target; } LinkRect;

typedef struct {
    HWND hwnd;
    int topic;
    int scroll;
    int height;            /* total document height */
    LinkRect *links;
    int nlinks, caplinks;
    int is_popup;
    int focus_link;
} View;

static int hp2px(int v) { return MulDiv(v, g_dpi, 144); }

static void add_link(View *v, int x0, int y0, int x1, int y1, int popup, int target)
{
    if (v->nlinks == v->caplinks) {
        v->caplinks = v->caplinks ? v->caplinks * 2 : 64;
        v->links = realloc(v->links, v->caplinks * sizeof(LinkRect));
    }
    SetRect(&v->links[v->nlinks].rc, x0, y0, x1, y1);
    v->links[v->nlinks].popup = popup;
    v->links[v->nlinks].target = target;
    v->nlinks++;
}

/* pending pieces of the current line */
typedef struct { int x, w; HFONT font; const char *s; int n; int link; int popup; int target; int bmp; int asc, desc; } Piece;

#define MAXPIECES 512

typedef struct {
    HDC dc;
    View *v;
    int paint;
    int width;           /* content width */
    int margin;
    int y;
    Piece pc[MAXPIECES];
    int npc;
    int line_x0, line_x;
    int float_bottom, float_left;   /* left-floating bitmap */
    int float_rbottom, float_right; /* right-floating bitmap */
    int yoff;            /* scroll offset for painting */
} Layout;

static void flush_line(Layout *L, const HPara *p, int last_in_para)
{
    int i, asc = 0, desc = 0, shift = 0;
    int limit = L->width - L->margin - hp2px(p->right) - (L->y < L->float_rbottom ? L->float_right : 0);
    (void)last_in_para;
    for (i = 0; i < L->npc; i++) {
        if (L->pc[i].asc > asc) asc = L->pc[i].asc;
        if (L->pc[i].desc > desc) desc = L->pc[i].desc;
    }
    if (!L->npc) {
        /* empty line: height of the current font */
        TEXTMETRICA tm;
        GetTextMetricsA(L->dc, &tm);
        asc = tm.tmAscent; desc = tm.tmDescent;
    }
    if (L->npc && p->align) {
        int used = L->pc[L->npc - 1].x + L->pc[L->npc - 1].w - L->line_x0;
        int avail = limit - L->line_x0;
        if (p->align == 1) shift = (avail - used) / 2;
        else shift = avail - used;
        if (shift < 0) shift = 0;
    }
    for (i = 0; i < L->npc; i++) {
        Piece *pc = &L->pc[i];
        int x = pc->x + shift;
        int top = L->y + asc - pc->asc;
        if (pc->bmp >= 0) {
            if (L->paint) {
                draw_bmp(L->dc, x, top - L->yoff, &H.bmps[pc->bmp]);
            }
            continue;
        }
        if (L->paint) {
            SelectObject(L->dc, pc->font);
            SetTextColor(L->dc, pc->link ? help_link() : help_text());
            TextOutA(L->dc, x, top - L->yoff, pc->s, pc->n);
            if (pc->link && pc->popup) {
                /* dotted underline for popup links */
                int yy = L->y + asc + 1 - L->yoff, xx, t;
                for (xx = x; xx < x + pc->w; xx += 2 * g_bmpscale)
                    for (t = 0; t < g_bmpscale * g_bmpscale; t++) SetPixel(L->dc, xx + t % g_bmpscale, yy + t / g_bmpscale, help_link());
            }
        }
        if (pc->link) add_link(L->v, x, L->y, x + pc->w, L->y + asc + desc, pc->popup, pc->target);
    }
    L->npc = 0;
    L->y += asc + desc;
}

static int left_edge(Layout *L, const HPara *p, int first)
{
    int x = L->margin + hp2px(p->left) + (first ? hp2px(p->first) : 0);
    if (L->y < L->float_bottom && x < L->margin + L->float_left) x = L->margin + L->float_left;
    if (x < 0) x = 0;
    return x;
}

static int next_tab(Layout *L, const HPara *p, int x)
{
    int i, rel = x - L->margin;
    for (i = 0; i < p->ntabs; i++) {
        int t = hp2px(p->tabs[i].pos & 0x3FFF);
        if (t > rel) return L->margin + t;
    }
    /* hanging indent acts as a tab stop */
    if (p->first < 0 && hp2px(p->left) > rel) return L->margin + hp2px(p->left);
    {
        int step = hp2px(72);
        return L->margin + ((rel / step) + 1) * step;
    }
}

static void add_piece(Layout *L, const HPara *p, HFONT f, const char *s, int n, int w, int link, int popup, int target)
{
    Piece *pc;
    TEXTMETRICA tm;
    if (L->npc == MAXPIECES) flush_line(L, p, 0);
    pc = &L->pc[L->npc++];
    SelectObject(L->dc, f);
    GetTextMetricsA(L->dc, &tm);
    pc->x = L->line_x; pc->w = w; pc->font = f; pc->s = s; pc->n = n;
    pc->link = link; pc->popup = popup; pc->target = target; pc->bmp = -1;
    pc->asc = tm.tmAscent + tm.tmExternalLeading; pc->desc = tm.tmDescent;
    L->line_x += w;
}

static int layout_topic(View *v, HDC dc, int width, int paint, int yoff)
{
    Layout *L = calloc(1, sizeof(Layout));
    HTopic *t;
    int i, j, k, total;
    int font = 0, link = 0, popup = 0, target = -1;
    if (v->topic < 0 || v->topic >= H.ntopics) { free(L); return 0; }
    t = &H.topics[v->topic];
    L->dc = dc; L->v = v; L->paint = paint; L->width = width; L->yoff = yoff;
    L->margin = v->is_popup ? 8 : 12;
    L->y = v->is_popup ? 6 : 8;
    v->nlinks = 0;
    SetBkMode(dc, TRANSPARENT);
    for (i = 0; i < t->npara; i++) {
        HPara *p = &t->paras[i];
        int first_line = 1;
        L->y += hp2px(p->above);
        L->line_x0 = L->line_x = left_edge(L, p, 1);
        for (j = 0; j < p->nitems; j++) {
            HItem *it = &p->items[j];
            switch (it->kind) {
            case IT_FONT: font = it->b; break;
            case IT_LINK: link = 1; popup = it->a; target = it->b; break;
            case IT_ENDLINK: link = 0; break;
            case IT_BR:
                flush_line(L, p, 0);
                first_line = 0;
                L->line_x0 = L->line_x = left_edge(L, p, 0);
                break;
            case IT_TAB: {
                int nx = next_tab(L, p, L->line_x);
                int limit = L->width - L->margin - hp2px(p->right);
                if (nx >= limit) {
                    flush_line(L, p, 0);
                    first_line = 0;
                    L->line_x0 = L->line_x = left_edge(L, p, 0);
                } else L->line_x = nx;
                break;
            }
            case IT_BMP: {
                HBmp *b;
                if (it->b < 0 || it->b >= H.nbmps) break;
                b = &H.bmps[it->b];
                int bw = b->w * g_bmpscale, bh = b->h * g_bmpscale;
                if (it->a == 1) {          /* left float */
                    if (paint) draw_bmp(dc, L->margin + hp2px(p->left), L->y - yoff, b);
                    L->float_bottom = L->y + bh + 4;
                    L->float_left = hp2px(p->left) + bw + 8;
                    if (L->npc == 0) L->line_x0 = L->line_x = left_edge(L, p, first_line);
                } else if (it->a == 2) {   /* right float */
                    if (paint) draw_bmp(dc, L->width - L->margin - bw, L->y - yoff, b);
                    L->float_rbottom = L->y + bh + 4;
                    L->float_right = bw + 8;
                } else {                   /* inline, like a character */
                    Piece *pc;
                    if (L->npc == MAXPIECES) flush_line(L, p, 0);
                    pc = &L->pc[L->npc++];
                    memset(pc, 0, sizeof *pc);
                    pc->x = L->line_x; pc->w = bw; pc->bmp = it->b; pc->asc = bh; pc->desc = 0;
                    L->line_x += bw;
                }
                break;
            }
            case IT_TEXT: {
                HFONT f = get_font(font, link ? (popup ? 2 : 1) : 0);
                const char *s = it->text;
                int n = it->len;
                SelectObject(dc, f);
                while (n > 0) {
                    /* take the next word including trailing spaces */
                    int wlen = 0, fit;
                    SIZE sz;
                    int limit = L->width - L->margin - hp2px(p->right) - (L->y < L->float_rbottom ? L->float_right : 0);
                    while (wlen < n && s[wlen] != ' ') wlen++;
                    while (wlen < n && s[wlen] == ' ') wlen++;
                    GetTextExtentPoint32A(dc, s, wlen, &sz);
                    if (L->line_x + sz.cx > limit && L->npc > 0) {
                        /* trailing spaces may overflow */
                        int trimmed = wlen;
                        SIZE s2;
                        while (trimmed > 0 && s[trimmed - 1] == ' ') trimmed--;
                        GetTextExtentPoint32A(dc, s, trimmed, &s2);
                        if (L->line_x + s2.cx > limit) {
                            flush_line(L, p, 0);
                            first_line = 0;
                            L->line_x0 = L->line_x = left_edge(L, p, 0);
                            continue;
                        }
                    }
                    if (L->line_x + sz.cx > limit && L->npc == 0) {
                        /* a single word longer than the line: split it */
                        int maxw = limit - L->line_x;
                        if (maxw < 10) maxw = 10;
                        GetTextExtentExPointA(dc, s, wlen, maxw, &fit, NULL, &sz);
                        if (fit < 1) fit = 1;
                        GetTextExtentPoint32A(dc, s, fit, &sz);
                        add_piece(L, p, f, s, fit, sz.cx, link, popup, target);
                        s += fit; n -= fit;
                        flush_line(L, p, 0);
                        first_line = 0;
                        L->line_x0 = L->line_x = left_edge(L, p, 0);
                        continue;
                    }
                    add_piece(L, p, f, s, wlen, sz.cx, link, popup, target);
                    s += wlen; n -= wlen;
                }
                break;
            }
            }
        }
        flush_line(L, p, 1);
        L->y += hp2px(p->below);
        (void)k;
    }
    if (L->y < L->float_bottom) L->y = L->float_bottom;
    total = L->y + (v->is_popup ? 6 : 12);
    free(L);
    return total;
}

/* ---- windows -------------------------------------------------------------------------- */

#define BTN_CONTENTS 101
#define BTN_SEARCH   102
#define BTN_BACK     103
#define BTN_PREV     104
#define BTN_NEXT     105
#define BAR_H        30

static HWND g_helpwnd, g_popupwnd;
static View g_view, g_pview;
static int g_hist[256], g_hist_scroll[256], g_nhist;
static int g_classes_ok;

static void view_relayout(View *v)
{
    RECT rc;
    HDC dc = GetDC(v->hwnd);
    GetClientRect(v->hwnd, &rc);
    v->height = layout_topic(v, dc, rc.right, 0, 0);
    ReleaseDC(v->hwnd, dc);
}

static void main_update_scroll(void)
{
    RECT rc;
    SCROLLINFO si;
    HWND body = GetDlgItem(g_helpwnd, 200);
    GetClientRect(body, &rc);
    if (g_view.scroll > g_view.height - rc.bottom) g_view.scroll = g_view.height - rc.bottom;
    if (g_view.scroll < 0) g_view.scroll = 0;
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nMax = g_view.height - 1;
    si.nPage = rc.bottom;
    si.nPos = g_view.scroll;
    SetScrollInfo(body, SB_VERT, &si, TRUE);
}

static void update_buttons(void)
{
    HTopic *t = &H.topics[g_view.topic];
    char cap[300];
    EnableWindow(GetDlgItem(g_helpwnd, BTN_BACK), g_nhist > 0);
    EnableWindow(GetDlgItem(g_helpwnd, BTN_PREV), t->prev >= 0 && t->prev < H.ntopics);
    EnableWindow(GetDlgItem(g_helpwnd, BTN_NEXT), t->next >= 0 && t->next < H.ntopics);
    snprintf(cap, sizeof cap, "%s Help - %s", H.title, t->title);
    SetWindowTextA(g_helpwnd, cap);
}

static void show_topic(int topic, int push)
{
    HWND body = GetDlgItem(g_helpwnd, 200);
    if (topic < 0 || topic >= H.ntopics) return;
    if (push && g_view.topic >= 0 && g_view.topic != topic) {
        if (g_nhist == 256) { memmove(g_hist, g_hist + 1, 255 * sizeof(int)); memmove(g_hist_scroll, g_hist_scroll + 1, 255 * sizeof(int)); g_nhist--; }
        g_hist[g_nhist] = g_view.topic;
        g_hist_scroll[g_nhist] = g_view.scroll;
        g_nhist++;
    }
    g_view.topic = topic;
    g_view.scroll = 0;
    g_view.hwnd = body;
    view_relayout(&g_view);
    main_update_scroll();
    update_buttons();
    InvalidateRect(body, NULL, TRUE);
}

static void paint_view(View *v, HWND hwnd)
{
    PAINTSTRUCT ps;
    RECT rc;
    HDC dc = BeginPaint(hwnd, &ps), mdc;
    HBITMAP bm, old;
    GetClientRect(hwnd, &rc);
    mdc = CreateCompatibleDC(dc);
    bm = CreateCompatibleBitmap(dc, rc.right ? rc.right : 1, rc.bottom ? rc.bottom : 1);
    old = SelectObject(mdc, bm);
    FillRect(mdc, &rc, dark_solid_brush(help_bg()));
    v->height = layout_topic(v, mdc, rc.right, 1, v->scroll);
    if (v->is_popup) {
        HBRUSH b = CreateSolidBrush(help_frame());
        FrameRect(mdc, &rc, b);
        DeleteObject(b);
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, old);
    DeleteObject(bm);
    DeleteDC(mdc);
    EndPaint(hwnd, &ps);
}

static LinkRect *hit_link(View *v, int x, int y)
{
    int i;
    y += v->scroll;
    for (i = 0; i < v->nlinks; i++) {
        RECT *r = &v->links[i].rc;
        if (x >= r->left && x < r->right && y >= r->top && y < r->bottom) return &v->links[i];
    }
    return NULL;
}

static void show_popup(int topic, POINT at);

static LRESULT CALLBACK popup_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        paint_view(&g_pview, hwnd);
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_KEYDOWN:
        DestroyWindow(hwnd);
        return 0;
    case WM_KILLFOCUS:
        if (g_popupwnd == hwnd) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_popupwnd == hwnd) g_popupwnd = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_popup(int topic, POINT at)
{
    HDC dc;
    int w, h;
    RECT wa;
    if (topic < 0 || topic >= H.ntopics) return;
    if (g_popupwnd) DestroyWindow(g_popupwnd);
    memset(&g_pview, 0, sizeof g_pview);
    g_pview.topic = topic;
    g_pview.is_popup = 1;
    w = MulDiv(380, g_dpi, 96);
    g_popupwnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, "CotWHelpPopup", "", WS_POPUP,
                                 at.x, at.y, w, 50, g_helpwnd, NULL, g_hinst32, NULL);
    g_pview.hwnd = g_popupwnd;
    dc = GetDC(g_popupwnd);
    h = layout_topic(&g_pview, dc, w, 0, 0);
    ReleaseDC(g_popupwnd, dc);
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    if (at.x + w > wa.right) at.x = wa.right - w;
    if (at.y + h > wa.bottom) at.y = wa.bottom - h;
    if (at.x < wa.left) at.x = wa.left;
    if (at.y < wa.top) at.y = wa.top;
    SetWindowPos(g_popupwnd, HWND_TOPMOST, at.x, at.y, w, h, SWP_SHOWWINDOW);
    SetFocus(g_popupwnd);
}

static LRESULT CALLBACK body_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        g_view.hwnd = hwnd;
        paint_view(&g_view, hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        view_relayout(&g_view);
        main_update_scroll();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_VSCROLL: {
        RECT rc;
        int old = g_view.scroll;
        GetClientRect(hwnd, &rc);
        switch (LOWORD(wp)) {
        case SB_LINEUP: g_view.scroll -= 20 * g_bmpscale; break;
        case SB_LINEDOWN: g_view.scroll += 20 * g_bmpscale; break;
        case SB_PAGEUP: g_view.scroll -= rc.bottom - 20 * g_bmpscale; break;
        case SB_PAGEDOWN: g_view.scroll += rc.bottom - 20 * g_bmpscale; break;
        case SB_TOP: g_view.scroll = 0; break;
        case SB_BOTTOM: g_view.scroll = g_view.height; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: {
            SCROLLINFO si;
            si.cbSize = sizeof si; si.fMask = SIF_TRACKPOS;
            GetScrollInfo(hwnd, SB_VERT, &si);
            g_view.scroll = si.nTrackPos;
            break;
        }
        }
        main_update_scroll();
        if (old != g_view.scroll) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        int old = g_view.scroll;
        g_view.scroll -= d * 60 * g_bmpscale / WHEEL_DELTA;
        main_update_scroll();
        if (old != g_view.scroll) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        switch (wp) {
        case VK_UP: SendMessageA(hwnd, WM_VSCROLL, SB_LINEUP, 0); break;
        case VK_DOWN: SendMessageA(hwnd, WM_VSCROLL, SB_LINEDOWN, 0); break;
        case VK_PRIOR: SendMessageA(hwnd, WM_VSCROLL, SB_PAGEUP, 0); break;
        case VK_NEXT: SendMessageA(hwnd, WM_VSCROLL, SB_PAGEDOWN, 0); break;
        case VK_HOME: SendMessageA(hwnd, WM_VSCROLL, SB_TOP, 0); break;
        case VK_END: SendMessageA(hwnd, WM_VSCROLL, SB_BOTTOM, 0); break;
        case VK_BACK: SendMessageA(g_helpwnd, WM_COMMAND, BTN_BACK, 0); break;
        case VK_ESCAPE: DestroyWindow(g_helpwnd); break;
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            SetCursor(LoadCursorA(NULL, hit_link(&g_view, pt.x, pt.y) ? (LPCSTR)IDC_HAND : (LPCSTR)IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        LinkRect *l;
        SetFocus(hwnd);
        l = hit_link(&g_view, (short)LOWORD(lp), (short)HIWORD(lp));
        if (l) {
            if (l->popup) {
                POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) + 12 * g_bmpscale };
                ClientToScreen(hwnd, &pt);
                show_popup(l->target, pt);
            } else show_topic(l->target, 1);
        }
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- search dialog --------------------------------------------------------------------- */

static int g_search_result = -1;
static HWND g_search_wnd;

static void search_fill_topics(HWND dlg)
{
    HWND kl = GetDlgItem(dlg, 11), tl = GetDlgItem(dlg, 12);
    int sel = (int)SendMessageA(kl, LB_GETCURSEL, 0, 0), i;
    SendMessageA(tl, LB_RESETCONTENT, 0, 0);
    if (sel < 0) return;
    sel = (int)SendMessageA(kl, LB_GETITEMDATA, sel, 0);
    for (i = 0; i < H.kws[sel].ntopics; i++) {
        int t = H.kws[sel].topics[i];
        int pos = (int)SendMessageA(tl, LB_ADDSTRING, 0, (LPARAM)H.topics[t].title);
        SendMessageA(tl, LB_SETITEMDATA, pos, t);
    }
    SendMessageA(tl, LB_SETCURSEL, 0, 0);
}

static LRESULT CALLBACK search_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        if (g_dark) {
            int ci = msg == WM_CTLCOLORSTATIC ? COLOR_BTNFACE : COLOR_WINDOW;
            SetTextColor((HDC)wp, dark_syscolor(COLOR_WINDOWTEXT));
            SetBkColor((HDC)wp, dark_syscolor(ci));
            return (LRESULT)dark_solid_brush(dark_syscolor(ci));
        }
        break;
    case WM_ERASEBKGND:
        if (g_dark) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, dark_solid_brush(dark_syscolor(COLOR_BTNFACE)));
            return 1;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == 10 && HIWORD(wp) == EN_CHANGE) {
            char buf[128];
            int idx;
            GetDlgItemTextA(hwnd, 10, buf, sizeof buf);
            idx = (int)SendDlgItemMessageA(hwnd, 11, LB_FINDSTRING, (WPARAM)-1, (LPARAM)buf);
            if (idx >= 0) { SendDlgItemMessageA(hwnd, 11, LB_SETCURSEL, idx, 0); search_fill_topics(hwnd); }
        } else if (LOWORD(wp) == 11 && HIWORD(wp) == LBN_SELCHANGE) {
            search_fill_topics(hwnd);
        } else if ((LOWORD(wp) == 11 || LOWORD(wp) == 12) && HIWORD(wp) == LBN_DBLCLK) {
            if (LOWORD(wp) == 11) search_fill_topics(hwnd);
            SendMessageA(hwnd, WM_COMMAND, IDOK, 0);
        } else if (LOWORD(wp) == IDOK) {
            int s = (int)SendDlgItemMessageA(hwnd, 12, LB_GETCURSEL, 0, 0);
            if (s >= 0) g_search_result = (int)SendDlgItemMessageA(hwnd, 12, LB_GETITEMDATA, s, 0);
            DestroyWindow(hwnd);
        } else if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_search_wnd = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void do_search(const char *initial)
{
    HWND dlg;
    MSG m;
    int i, y = 10, W = MulDiv(420, g_dpi, 96), lh = MulDiv(22, g_dpi, 96);
    HFONT f = g_btnfont;
    RECT pr;
    if (!H.nkw) return;
    g_search_result = -1;
    GetWindowRect(g_helpwnd, &pr);
    dlg = CreateWindowExA(WS_EX_DLGMODALFRAME, "CotWHelpSearch", "Search", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                          pr.left + 40, pr.top + 60, W, MulDiv(470, g_dpi, 96), g_helpwnd, NULL, g_hinst32, NULL);
    g_search_wnd = dlg;
#define MK(cls, txt, st, id, x, yy, w, h) SendMessageA(CreateWindowExA(!strcmp(cls, "EDIT") || !strcmp(cls, "LISTBOX") ? WS_EX_CLIENTEDGE : 0, cls, txt, WS_CHILD | WS_VISIBLE | (st), x, yy, w, h, dlg, (HMENU)(INT_PTR)(id), g_hinst32, NULL), WM_SETFONT, (WPARAM)f, 0)
    MK("STATIC", "Type a &word, or select one from the list:", 0, 9, 10, y, W - 30, lh); y += lh;
    MK("EDIT", initial ? initial : "", WS_TABSTOP | ES_AUTOHSCROLL, 10, 10, y, W - 30, lh + 2); y += lh + 8 * g_bmpscale;
    MK("LISTBOX", "", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_SORT, 11, 10, y, W - 30, MulDiv(190, g_dpi, 96)); y += MulDiv(196, g_dpi, 96);
    MK("STATIC", "&Topics found:", 0, 13, 10, y, W - 30, lh); y += lh;
    MK("LISTBOX", "", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY, 12, 10, y, W - 30, MulDiv(100, g_dpi, 96)); y += MulDiv(108, g_dpi, 96);
    MK("BUTTON", "&Go To", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, W - MulDiv(200, g_dpi, 96), y, MulDiv(80, g_dpi, 96), lh + 4);
    MK("BUTTON", "Cancel", WS_TABSTOP, IDCANCEL, W - MulDiv(110, g_dpi, 96), y, MulDiv(80, g_dpi, 96), lh + 4);
#undef MK
    {
        HWND c;
        dark_apply_window(dlg);
        for (c = GetWindow(dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            dark_subclass_button(c);
            if (g_dark) dark_apply_window(c);
        }
    }
    for (i = 0; i < H.nkw; i++) {
        int pos = (int)SendDlgItemMessageA(dlg, 11, LB_ADDSTRING, 0, (LPARAM)H.kws[i].word);
        SendDlgItemMessageA(dlg, 11, LB_SETITEMDATA, pos, i);
    }
    if (initial && initial[0]) {
        int idx = (int)SendDlgItemMessageA(dlg, 11, LB_FINDSTRING, (WPARAM)-1, (LPARAM)initial);
        if (idx >= 0) SendDlgItemMessageA(dlg, 11, LB_SETCURSEL, idx, 0);
    } else SendDlgItemMessageA(dlg, 11, LB_SETCURSEL, 0, 0);
    search_fill_topics(dlg);
    ShowWindow(dlg, SW_SHOW);
    EnableWindow(g_helpwnd, FALSE);
    SetFocus(GetDlgItem(dlg, 10));
    while (g_search_wnd && GetMessageA(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(dlg, &m)) { TranslateMessage(&m); DispatchMessageA(&m); }
    }
    EnableWindow(g_helpwnd, TRUE);
    SetForegroundWindow(g_helpwnd);
    if (g_search_result >= 0) show_topic(g_search_result, 1);
}

static void layout_bar(HWND hwnd)
{
    static const int ids[5] = { BTN_CONTENTS, BTN_SEARCH, BTN_BACK, BTN_PREV, BTN_NEXT };
    int i, x = 4, bw = MulDiv(72, g_dpi, 96), bh = MulDiv(BAR_H - 6, g_dpi, 96);
    for (i = 0; i < 5; i++) {
        HWND b = GetDlgItem(hwnd, ids[i]);
        int w = i >= 3 ? bw / 2 + 8 : bw;
        MoveWindow(b, x, MulDiv(3, g_dpi, 96), w, bh, TRUE);
        SendMessageA(b, WM_SETFONT, (WPARAM)g_btnfont, TRUE);
        x += w + 4;
    }
}

static void make_btnfont(void)
{
    NONCLIENTMETRICSA ncm;
    if (g_btnfont && g_btnfont != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(g_btnfont);
    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) {
        ncm.lfMessageFont.lfHeight *= g_zoom;
        g_btnfont = CreateFontIndirectA(&ncm.lfMessageFont);
    } else g_btnfont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static void set_scale(void)
{
    HDC dc = GetDC(NULL);
    int i;
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY) * g_zoom;
    ReleaseDC(NULL, dc);
    g_bmpscale = g_zoom;
    for (i = 0; i < 32 * 3; i++) if (g_hfonts[i]) { DeleteObject(g_hfonts[i]); g_hfonts[i] = NULL; }
    make_btnfont();
}

static LRESULT CALLBACK help_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        struct { int id; const char *txt; } btns[] = {
            { BTN_CONTENTS, "&Contents" }, { BTN_SEARCH, "&Search" }, { BTN_BACK, "&Back" },
            { BTN_PREV, "&<<" }, { BTN_NEXT, "&>>" } };
        int i;
        for (i = 0; i < 5; i++) {
            HWND b = CreateWindowExA(0, "BUTTON", btns[i].txt, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)btns[i].id, g_hinst32, NULL);
            dark_subclass_button(b);
        }
        layout_bar(hwnd);
        CreateWindowExA(WS_EX_CLIENTEDGE, "CotWHelpBody", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                        0, MulDiv(BAR_H, g_dpi, 96), 100, 100, hwnd, (HMENU)200, g_hinst32, NULL);
        dark_apply_window(hwnd);
        dark_apply_window(GetDlgItem(hwnd, 200));
        return 0;
    }
    case WM_ERASEBKGND:
        if (g_dark) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, dark_solid_brush(dark_syscolor(COLOR_BTNFACE)));
            return 1;
        }
        break;
    case WM_SIZE: {
        int bar = MulDiv(BAR_H, g_dpi, 96);
        MoveWindow(GetDlgItem(hwnd, 200), 0, bar, LOWORD(lp), HIWORD(lp) - bar, TRUE);
        return 0;
    }
    case WM_SETFOCUS:
        SetFocus(GetDlgItem(hwnd, 200));
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case BTN_CONTENTS: show_topic(0, 1); break;
        case BTN_SEARCH: do_search(NULL); break;
        case BTN_BACK:
            if (g_nhist > 0) {
                int s;
                g_nhist--;
                s = g_hist_scroll[g_nhist];
                show_topic(g_hist[g_nhist], 0);
                g_view.scroll = s;
                main_update_scroll();
                InvalidateRect(GetDlgItem(hwnd, 200), NULL, FALSE);
                update_buttons();
            }
            break;
        case BTN_PREV: show_topic(H.topics[g_view.topic].prev, 1); break;
        case BTN_NEXT: show_topic(H.topics[g_view.topic].next, 1); break;
        }
        SetFocus(GetDlgItem(hwnd, 200));
        return 0;
    case WM_DESTROY:
        if (g_popupwnd) DestroyWindow(g_popupwnd);
        g_helpwnd = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int ensure_window(void)
{
    if (!help_load()) return 0;
    if (!g_classes_ok) {
        WNDCLASSA wc;
        set_scale();
        memset(&wc, 0, sizeof wc);
        wc.hInstance = g_hinst32;
        wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.lpfnWndProc = help_proc;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon = LoadIconA(g_hinst32, MAKEINTRESOURCEA(1));
        wc.lpszClassName = "CotWHelp";
        RegisterClassA(&wc);
        wc.lpfnWndProc = body_proc;
        wc.hbrBackground = NULL;
        wc.hIcon = NULL;
        wc.lpszClassName = "CotWHelpBody";
        RegisterClassA(&wc);
        wc.lpfnWndProc = popup_proc;
        wc.style = CS_DROPSHADOW;
        wc.lpszClassName = "CotWHelpPopup";
        RegisterClassA(&wc);
        wc.style = 0;
        wc.lpfnWndProc = search_proc;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "CotWHelpSearch";
        RegisterClassA(&wc);
        g_classes_ok = 1;
    }
    if (!g_helpwnd) {
        RECT wa;
        int w, h;
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        w = MulDiv(640, g_dpi, 96);
        h = MulDiv(560, g_dpi, 96);
        if (w > wa.right - wa.left) w = wa.right - wa.left;
        if (h > wa.bottom - wa.top) h = wa.bottom - wa.top;
        memset(&g_view, 0, sizeof g_view);
        g_view.topic = -1;
        g_nhist = 0;
        g_helpwnd = CreateWindowExA(0, "CotWHelp", "Help", WS_OVERLAPPEDWINDOW,
                                    wa.right - w - 20, wa.top + 20, w, h, NULL, NULL, g_hinst32, NULL);
        g_view.hwnd = GetDlgItem(g_helpwnd, 200);
    }
    return g_helpwnd != NULL;
}

/* zoom or dark mode changed while the help window is open */
void help_settings_changed(void)
{
    RECT wr;
    int oldscale = g_bmpscale;
    if (!g_classes_ok) return;
    set_scale();
    if (g_popupwnd) DestroyWindow(g_popupwnd);
    if (!g_helpwnd) return;
    if (oldscale != g_bmpscale && !IsZoomed(g_helpwnd) && !IsIconic(g_helpwnd)) {
        RECT wa;
        int w, h;
        GetWindowRect(g_helpwnd, &wr);
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        w = (wr.right - wr.left) * g_bmpscale / oldscale;
        h = (wr.bottom - wr.top) * g_bmpscale / oldscale;
        if (w > wa.right - wa.left) w = wa.right - wa.left;
        if (h > wa.bottom - wa.top) h = wa.bottom - wa.top;
        SetWindowPos(g_helpwnd, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    layout_bar(g_helpwnd);
    GetClientRect(g_helpwnd, &wr);
    SendMessageA(g_helpwnd, WM_SIZE, 0, MAKELPARAM(wr.right, wr.bottom));
    dark_apply_window(g_helpwnd);
    dark_apply_window(GetDlgItem(g_helpwnd, 200));
    view_relayout(&g_view);
    main_update_scroll();
    RedrawWindow(g_helpwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

static int topic_for_map(u32 id)
{
    int i;
    for (i = 0; i < H.nmap; i++) if (H.map[i].id == id) return H.map[i].topic;
    return -1;
}

static int topic_for_keyword(const char *k)
{
    int i;
    for (i = 0; i < H.nkw; i++) if (!_stricmp(H.kws[i].word, k) && H.kws[i].ntopics) return H.kws[i].topics[0];
    return -1;
}

void help_show(HWND owner, const char *file, u16 cmd, u32 data)
{
    int topic = -1;
    emu_log("WinHelp(%s, %04X, %08X)", file ? file : "(null)", cmd, data);
    if (cmd == 2 /* HELP_QUIT */) {
        if (g_helpwnd) DestroyWindow(g_helpwnd);
        return;
    }
    if (cmd == 4 /* HELP_HELPONHELP */) {
        port_message_box(owner,
                    "Use the Contents button to see the help index, Search to look up keywords, "
                    "Back to return to the previous topic and << / >> to browse through related topics.\n\n"
                    "Click green underlined text to jump to another topic; green dotted text shows a definition.",
                    "Using Help", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!ensure_window()) {
        port_message_box(owner, "The help text is not available.", "Castle of the Winds Help", MB_OK | MB_ICONINFORMATION);
        return;
    }
    switch (cmd) {
    case 1:      /* HELP_CONTEXT */
    case 8:      /* HELP_CONTEXTPOPUP */
        topic = topic_for_map(data);
        if (topic < 0) topic = 0;
        if (cmd == 8) {
            POINT pt;
            GetCursorPos(&pt);
            if (!IsWindowVisible(g_helpwnd)) {
                /* popups need a parent; keep the main help window hidden */
            }
            show_popup(topic, pt);
            return;
        }
        break;
    case 0x101:  /* HELP_KEY */
    case 0x105:  /* HELP_PARTIALKEY */ {
        const char *k = data ? (const char *)ptrseg(data) : "";
        topic = topic_for_keyword(k);
        if (topic < 0 || cmd == 0x105) {
            ShowWindow(g_helpwnd, SW_SHOWNORMAL);
            if (g_view.topic < 0) show_topic(0, 0);
            do_search(k);
            return;
        }
        break;
    }
    default:     /* HELP_INDEX / HELP_CONTENTS and anything else */
        topic = 0;
        break;
    }
    if (IsIconic(g_helpwnd)) ShowWindow(g_helpwnd, SW_RESTORE);
    ShowWindow(g_helpwnd, SW_SHOWNORMAL);
    show_topic(topic, g_view.topic >= 0);
    SetForegroundWindow(g_helpwnd);
    (void)owner;
}
