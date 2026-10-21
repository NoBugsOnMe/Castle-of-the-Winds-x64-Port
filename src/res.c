/*
 * res.c - converting Win16 resources (icons, cursors, bitmaps, menus, dialog
 *         templates) from the NE image into native Win32 objects.
 */
#include "win16.h"

#define RT16_CURSOR       1
#define RT16_BITMAP       2
#define RT16_ICON         3
#define RT16_MENU         4
#define RT16_DIALOG       5
#define RT16_GROUP_CURSOR 12
#define RT16_GROUP_ICON   14

/* cache of icons/cursors created from resources (LoadIcon returns shared handles) */
typedef struct { int res; HICON h; } IconCache;
static IconCache g_icache[512];
static int g_nicache;

int res_icon_is_shared(HICON h)
{
    int i;
    for (i = 0; i < g_nicache; i++) if (g_icache[i].h == h) return 1;
    return 0;
}

static int find_by_type(u16 type, u32 name)
{
    return ne_find_resource(type, name);
}

HICON load_icon_res(u32 name, int cursor)
{
    int gi, ii, i;
    const u8 *grp, *data;
    u32 glen, len;
    int id;
    HICON h;
    gi = find_by_type(cursor ? RT16_GROUP_CURSOR : RT16_GROUP_ICON, name);
    if (gi < 0) return NULL;
    for (i = 0; i < g_nicache; i++) if (g_icache[i].res == gi) return g_icache[i].h;
    grp = ne_res_data(gi, &glen);
    if (!grp || glen < 6) return NULL;
    id = LookupIconIdFromDirectoryEx((PBYTE)grp, !cursor, cursor ? 0 : 32, cursor ? 0 : 32, LR_DEFAULTCOLOR);
    if (!id) {
        /* fall back to the first entry */
        id = cursor ? (grp[6 + 12] | (grp[6 + 13] << 8)) : (grp[6 + 12] | (grp[6 + 13] << 8));
    }
    ii = ne_find_resource_c(cursor ? RT16_CURSOR : RT16_ICON, NULL, (u16)id, NULL);
    if (ii < 0) return NULL;
    data = ne_res_data(ii, &len);
    if (!data) return NULL;
    {
        /* private copy so the colour table can be snapped to VGA colours */
        u8 *copy = malloc(len);
        if (!copy) return NULL;
        memcpy(copy, data, len);
        dib_vga_inplace(copy + (cursor ? 4 : 0));
        h = CreateIconFromResourceEx(copy, len, !cursor, 0x00030000, 0, 0, LR_DEFAULTCOLOR);
        free(copy);
    }
    if (h && g_nicache < (int)(sizeof g_icache / sizeof g_icache[0])) {
        g_icache[g_nicache].res = gi;
        g_icache[g_nicache].h = h;
        g_nicache++;
    }
    return h;
}

static int dib_colors(const BITMAPINFOHEADER *bih)
{
    if (bih->biSize == sizeof(BITMAPCOREHEADER)) {
        const BITMAPCOREHEADER *c = (const BITMAPCOREHEADER *)bih;
        return c->bcBitCount <= 8 ? 1 << c->bcBitCount : 0;
    }
    if (bih->biClrUsed) return (int)bih->biClrUsed;
    if (bih->biBitCount <= 8) return 1 << bih->biBitCount;
    if (bih->biCompression == BI_BITFIELDS) return 3;
    return 0;
}

static int dib_is_mono(const BITMAPINFOHEADER *bih)
{
    if (bih->biSize == sizeof(BITMAPCOREHEADER)) {
        const BITMAPCOREINFO *ci = (const BITMAPCOREINFO *)bih;
        const RGBTRIPLE *c = ci->bmciColors;
        if (ci->bmciHeader.bcBitCount != 1) return 0;
        return !c[0].rgbtRed && !c[0].rgbtGreen && !c[0].rgbtBlue &&
               c[1].rgbtRed == 0xFF && c[1].rgbtGreen == 0xFF && c[1].rgbtBlue == 0xFF;
    } else {
        const RGBQUAD *c = ((const BITMAPINFO *)bih)->bmiColors;
        if (bih->biBitCount != 1) return 0;
        return !c[0].rgbRed && !c[0].rgbGreen && !c[0].rgbBlue &&
               c[1].rgbRed == 0xFF && c[1].rgbGreen == 0xFF && c[1].rgbBlue == 0xFF;
    }
}

const void *dib_bits_after_header(const BITMAPINFOHEADER *bih)
{
    int ncol = dib_colors(bih);
    if (bih->biSize == sizeof(BITMAPCOREHEADER)) return (const u8 *)bih + bih->biSize + ncol * 3;
    return (const u8 *)bih + bih->biSize + ncol * 4;
}

HBITMAP dib_to_bitmap(const BITMAPINFOHEADER *bih, const void *bits, UINT usage, HDC hdc)
{
    HDC dc = hdc ? hdc : GetDC(NULL);
    HBITMAP hbm;
    int w, h;
    if (bih->biSize == sizeof(BITMAPCOREHEADER)) {
        w = ((const BITMAPCOREHEADER *)bih)->bcWidth;
        h = ((const BITMAPCOREHEADER *)bih)->bcHeight;
    } else {
        w = bih->biWidth;
        h = bih->biHeight < 0 ? -bih->biHeight : bih->biHeight;
    }
    if (!bits) bits = dib_bits_after_header(bih);
    if (dib_is_mono(bih)) hbm = CreateBitmap(w, h, 1, 1, NULL);
    else hbm = CreateCompatibleBitmap(dc, w, h);
    if (hbm) {
        DibVgaBuf vb;
        SetDIBits(dc, hbm, 0, h, bits, (const BITMAPINFO *)dib_vga(bih, usage, &vb), usage);
    }
    if (!hdc) ReleaseDC(NULL, dc);
    return hbm;
}

HBITMAP load_bitmap_res(u32 name)
{
    int i = find_by_type(RT16_BITMAP, name);
    const u8 *data;
    u32 len;
    if (i < 0) return NULL;
    data = ne_res_data(i, &len);
    if (!data) return NULL;
    return dib_to_bitmap((const BITMAPINFOHEADER *)data, NULL, DIB_RGB_COLORS, NULL);
}

/* ---- menus ---- */

static const u8 *menu_items(HMENU hm, const u8 *p, const u8 *end)
{
    while (p + 2 <= end) {
        u16 flags = (u16)(p[0] | (p[1] << 8));
        u16 id = 0;
        const char *text;
        p += 2;
        if (!(flags & MF_POPUP)) { id = (u16)(p[0] | (p[1] << 8)); p += 2; }
        text = (const char *)p;
        p += strlen(text) + 1;
        if (flags & MF_POPUP) {
            HMENU sub = CreatePopupMenu();
            p = menu_items(sub, p, end);
            AppendMenuA(hm, (flags & ~(MF_END)) | MF_POPUP, (UINT_PTR)sub, text);
        } else if (!id && !text[0] && !(flags & ~MF_END)) {
            AppendMenuA(hm, MF_SEPARATOR, 0, NULL);
        } else {
            AppendMenuA(hm, flags & ~(MF_END | MF_POPUP), id, text);
        }
        if (flags & MF_END) break;
    }
    return p;
}

HMENU build_menu(const u8 *data, u32 len)
{
    HMENU hm = CreateMenu();
    u16 hdrsz;
    if (len < 4) return hm;
    hdrsz = (u16)(data[2] | (data[3] << 8));
    menu_items(hm, data + 4 + hdrsz, data + len);
    return hm;
}

HMENU load_menu_res(u32 name)
{
    int i = find_by_type(RT16_MENU, name);
    const u8 *data;
    u32 len;
    if (i < 0) return NULL;
    data = ne_res_data(i, &len);
    if (!data) return NULL;
    return build_menu(data, len);
}

/* ---- dialogs ---- */

typedef struct { u8 *buf; u32 len, cap; } Buf;

static void bput(Buf *b, const void *p, u32 n)
{
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 256;
        b->buf = realloc(b->buf, b->cap);
    }
    memcpy(b->buf + b->len, p, n);
    b->len += n;
}
static void bput16(Buf *b, u16 v) { bput(b, &v, 2); }
static void bput32(Buf *b, u32 v) { bput(b, &v, 4); }
static void balign(Buf *b, int a) { while (b->len % a) { u8 z = 0; bput(b, &z, 1); } }
static void bputwstr(Buf *b, const char *s)
{
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    WCHAR *w = malloc((n + 1) * sizeof(WCHAR));
    MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
    bput(b, w, n * sizeof(WCHAR));
    free(w);
}

/* converts a name field (0 / 0xFF ord / string); returns pointer past it */
static const u8 *conv_name(Buf *b, const u8 *p, int drop)
{
    if (*p == 0) { if (!drop) bput16(b, 0); return p + 1; }
    if (*p == 0xFF) {
        if (!drop) { bput16(b, 0xFFFF); bput16(b, (u16)(p[1] | (p[2] << 8))); }
        return p + 3;
    }
    if (!drop) bputwstr(b, (const char *)p);
    return p + strlen((const char *)p) + 1;
}

DlgIconFix g_dlg_icon_fix[32];
int g_dlg_nicon_fix;
u16 g_dlg_menu_id;
char g_dlg_menu_name[64];

/* converts a Win16 dialog template into a Win32 DLGTEMPLATEEX */
void *convert_dialog(const u8 *data, u32 len, u32 *outlen)
{
    Buf b = { 0 };
    const u8 *p = data, *end = data + len;
    u32 style = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
    int count = p[4], i;
    u16 x = (u16)(p[5] | (p[6] << 8)), y = (u16)(p[7] | (p[8] << 8));
    u16 cx = (u16)(p[9] | (p[10] << 8)), cy = (u16)(p[11] | (p[12] << 8));
    u32 style_at;
    int points = 0, weight = FW_NORMAL, had_font;
    char face[64] = "";
    g_dlg_nicon_fix = 0;
    p += 13;
    style &= ~0x20u;   /* DS_LOCALEDIT is meaningless on Win32 */
    bput16(&b, 1);                /* dlgVer */
    bput16(&b, 0xFFFF);           /* signature */
    bput32(&b, 0);                /* helpID */
    bput32(&b, 0);                /* exStyle */
    style_at = b.len;
    bput32(&b, style);
    bput16(&b, (u16)count);
    bput16(&b, x); bput16(&b, y); bput16(&b, cx); bput16(&b, cy);
    /* menu: dialogs with menus get them attached in WM_INITDIALOG; drop here */
    if (*p) {
        g_dlg_menu_name[0] = 0;
        if (*p == 0xFF) g_dlg_menu_id = (u16)(p[1] | (p[2] << 8));
        else { g_dlg_menu_id = 0; snprintf(g_dlg_menu_name, sizeof g_dlg_menu_name, "%s", (const char *)p); }
    } else { g_dlg_menu_id = 0; g_dlg_menu_name[0] = 0; }
    p = conv_name(&b, p, 1);
    bput16(&b, 0);
    p = conv_name(&b, p, 0);                 /* class */
    bputwstr(&b, (const char *)p);           /* caption */
    p += strlen((const char *)p) + 1;
    had_font = (style & DS_SETFONT) != 0;
    if (had_font) {
        points = (u16)(p[0] | (p[1] << 8));
        p += 2;
        snprintf(face, sizeof face, "%s", (const char *)p);
        p += strlen((const char *)p) + 1;
    }
    zoom_dialog_font(&points, &weight, face, sizeof face, &had_font);   /* larger font when zoomed */
    if (had_font) {
        if (!(style & DS_SETFONT)) {
            style |= DS_SETFONT;
            memcpy(b.buf + style_at, &style, 4);
        }
        bput16(&b, (u16)points);
        bput16(&b, (u16)weight);
        {
            u8 italic_charset[2] = { 0, DEFAULT_CHARSET };
            bput(&b, italic_charset, 2);
        }
        bputwstr(&b, face);
    }
    for (i = 0; i < count && p + 14 <= end; i++) {
        u16 ix = (u16)(p[0] | (p[1] << 8)), iy = (u16)(p[2] | (p[3] << 8));
        u16 icx = (u16)(p[4] | (p[5] << 8)), icy = (u16)(p[6] | (p[7] << 8));
        u16 id = (u16)(p[8] | (p[9] << 8));
        u32 istyle = (u32)p[10] | ((u32)p[11] << 8) | ((u32)p[12] << 16) | ((u32)p[13] << 24);
        int clsatom = 0;
        u8 extra;
        p += 14;
        balign(&b, 4);
        bput32(&b, 0);                       /* helpID */
        bput32(&b, 0);                       /* exStyle */
        bput32(&b, istyle);
        bput16(&b, ix); bput16(&b, iy); bput16(&b, icx); bput16(&b, icy);
        bput32(&b, id);
        if (*p & 0x80) {
            clsatom = *p;
            bput16(&b, 0xFFFF);
            bput16(&b, (u16)*p);
            p++;
        } else {
            bputwstr(&b, (const char *)p);
            p += strlen((const char *)p) + 1;
        }
        if (*p == 0xFF) {
            u16 ord = (u16)(p[1] | (p[2] << 8));
            if (clsatom == 0x82 && (istyle & 0x1F) == SS_ICON && g_dlg_nicon_fix < 32) {
                g_dlg_icon_fix[g_dlg_nicon_fix].ctl_id = id;
                g_dlg_icon_fix[g_dlg_nicon_fix].icon_id = ord;
                g_dlg_icon_fix[g_dlg_nicon_fix].icon_name[0] = 0;
                g_dlg_nicon_fix++;
                bput16(&b, 0);    /* empty text: the icon is set after creation */
            } else {
                bput16(&b, 0xFFFF);
                bput16(&b, ord);
            }
            p += 3;
        } else {
            if (clsatom == 0x82 && (istyle & 0x1F) == SS_ICON && g_dlg_nicon_fix < 32) {
                g_dlg_icon_fix[g_dlg_nicon_fix].ctl_id = id;
                g_dlg_icon_fix[g_dlg_nicon_fix].icon_id = 0;
                snprintf(g_dlg_icon_fix[g_dlg_nicon_fix].icon_name, 32, "%s", (const char *)p);
                g_dlg_nicon_fix++;
                bput16(&b, 0);
            } else bputwstr(&b, (const char *)p);
            p += strlen((const char *)p) + 1;
        }
        extra = *p++;
        p += extra;
        bput16(&b, 0);
    }
    *outlen = b.len;
    return b.buf;
}
