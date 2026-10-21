/*
 * gdi.c - GDI.EXE functions used by Castle of the Winds.
 */
#include "win16.h"

static const char *str16(u32 p) { return p ? (const char *)ptrseg(p) : NULL; }
static u32 pt32(POINT p) { return MAKELONG((u16)(s16)p.x, (u16)(s16)p.y); }

#define DC(x) HDC_32(x)

/* ---- DC attributes ---------------------------------------------------------- */

void GDI_SetBkColor(void) { ARGS(); u16 dc = A16(); u32 c = A32(); RET32(wdc_set_bk(dc, c)); }
void GDI_SetBkMode(void) { ARGS(); u16 dc = A16(), m = A16(); RET16(SetBkMode(DC(dc), m)); }
void GDI_SetMapMode(void) { ARGS(); u16 dc = A16(), m = A16(); RET16(SetMapMode(DC(dc), m)); }
void GDI_SetROP2(void) { ARGS(); u16 dc = A16(), m = A16(); RET16(SetROP2(DC(dc), m)); }
void GDI_SetStretchBltMode(void) { ARGS(); u16 dc = A16(), m = A16(); RET16(SetStretchBltMode(DC(dc), m)); }
void GDI_SetTextColor(void) { ARGS(); u16 dc = A16(); u32 c = A32(); RET32(wdc_set_text(dc, c)); }

/* dark mode: recoloured pen / brush selected for one drawing call into a window */
typedef struct { HDC dc; HGDIOBJ oldpen, oldbrush; } DarkSel;

static void dark_begin(u16 dc, DarkSel *d, int pen, int brush)
{
    int policy = wdc_dark(dc);
    d->dc = NULL;
    d->oldpen = d->oldbrush = NULL;
    if (!policy) return;
    d->dc = DC(dc);
    if (pen && policy == DARK_UI) {
        HPEN p = dark_pen_for((HPEN)GetCurrentObject(d->dc, OBJ_PEN));
        if (p) d->oldpen = SelectObject(d->dc, p);
    }
    if (brush) {
        HBRUSH b = dark_brush_p(policy, (HBRUSH)GetCurrentObject(d->dc, OBJ_BRUSH));
        if (b) d->oldbrush = SelectObject(d->dc, b);
    }
}

static void dark_end(DarkSel *d)
{
    if (!d->dc) return;
    if (d->oldpen) SelectObject(d->dc, d->oldpen);
    if (d->oldbrush) SelectObject(d->dc, d->oldbrush);
}

static int dc_has_mono_bitmap(HDC dc)
{
    BITMAP bm;
    HGDIOBJ b = dc ? GetCurrentObject(dc, OBJ_BITMAP) : NULL;
    return b && GetObjectA(b, sizeof bm, &bm) && bm.bmBitsPixel == 1 && bm.bmPlanes == 1;
}

void GDI_SetWindowOrg(void)
{
    ARGS();
    u16 dc = A16(); s16 x = AS16(), y = AS16();
    POINT p = { 0, 0 };
    SetWindowOrgEx(DC(dc), x, y, &p);
    RET32(pt32(p));
}
void GDI_SetWindowExt(void)
{
    ARGS();
    u16 dc = A16(); s16 x = AS16(), y = AS16();
    SIZE s = { 0, 0 };
    SetWindowExtEx(DC(dc), x, y, &s);
    RET32(MAKELONG((u16)(s16)s.cx, (u16)(s16)s.cy));
}
void GDI_SetViewportOrg(void)
{
    ARGS();
    u16 dc = A16(); s16 x = AS16(), y = AS16();
    POINT p = { 0, 0 };
    SetViewportOrgEx(DC(dc), x, y, &p);
    RET32(pt32(p));
}
void GDI_SetViewportExt(void)
{
    ARGS();
    u16 dc = A16(); s16 x = AS16(), y = AS16();
    SIZE s = { 0, 0 };
    SetViewportExtEx(DC(dc), x, y, &s);
    RET32(MAKELONG((u16)(s16)s.cx, (u16)(s16)s.cy));
}
void GDI_SetBrushOrg(void)
{
    ARGS();
    u16 dc = A16(); s16 x = AS16(), y = AS16();
    POINT p = { 0, 0 };
    SetBrushOrgEx(DC(dc), x, y, &p);
    RET32(pt32(p));
}

static void conv_points(u16 dc, u32 pp, s16 n, int to_lp)
{
    POINT16 *p16 = (POINT16 *)ptrseg(pp);
    POINT tmp[64], *p = tmp;
    int i;
    if (n <= 0 || !p16) return;
    if (n > 64) p = malloc(n * sizeof(POINT));
    for (i = 0; i < n; i++) { p[i].x = p16[i].x; p[i].y = p16[i].y; }
    if (to_lp) DPtoLP(DC(dc), p, n); else LPtoDP(DC(dc), p, n);
    for (i = 0; i < n; i++) { p16[i].x = (s16)p[i].x; p16[i].y = (s16)p[i].y; }
    if (p != tmp) free(p);
}
void GDI_DPtoLP(void) { ARGS(); u16 dc = A16(); u32 p = A32(); s16 n = AS16(); conv_points(dc, p, n, 1); RET16(1); }
void GDI_LPtoDP(void) { ARGS(); u16 dc = A16(); u32 p = A32(); s16 n = AS16(); conv_points(dc, p, n, 0); RET16(1); }

/* ---- drawing ------------------------------------------------------------------- */

void GDI_MoveTo(void)
{
    ARGS();
    u16 dc = A16(); s16 x = AS16(), y = AS16();
    POINT p = { 0, 0 };
    MoveToEx(DC(dc), x, y, &p);
    RET32(pt32(p));
}
/* notify the zoom layer of the area a drawing call touched */
static void drawn(u16 dc, int x1, int y1, int x2, int y2)
{
    RECT r;
    r.left = x1 < x2 ? x1 : x2; r.right = x1 < x2 ? x2 : x1;
    r.top = y1 < y2 ? y1 : y2; r.bottom = y1 < y2 ? y2 : y1;
    wdc_drawn(dc, &r);
}

void GDI_LineTo(void)
{
    ARGS();
    u16 dc = A16(); s16 x = AS16(), y = AS16();
    POINT cur = { 0, 0 };
    BOOL r;
    DarkSel ds;
    GetCurrentPositionEx(DC(dc), &cur);
    dark_begin(dc, &ds, 1, 0);
    r = LineTo(DC(dc), x, y);
    dark_end(&ds);
    drawn(dc, cur.x - 2, cur.y - 2, x + 2, y + 2);
    RET16(r);
}
void GDI_Rectangle(void)
{
    ARGS();
    u16 dc = A16(); s16 l = AS16(), t = AS16(), r = AS16(), b = AS16();
    DarkSel ds;
    BOOL res;
    dark_begin(dc, &ds, 1, 1);
    res = Rectangle(DC(dc), l, t, r, b);
    dark_end(&ds);
    drawn(dc, l - 2, t - 2, r + 2, b + 2);
    RET16(res);
}
void GDI_Arc(void)
{
    ARGS();
    u16 dc = A16();
    s16 l = AS16(), t = AS16(), r = AS16(), b = AS16(), x1 = AS16(), y1 = AS16(), x2 = AS16(), y2 = AS16();
    DarkSel ds;
    BOOL res;
    dark_begin(dc, &ds, 1, 0);
    res = Arc(DC(dc), l, t, r, b, x1, y1, x2, y2);
    dark_end(&ds);
    drawn(dc, l - 2, t - 2, r + 2, b + 2);
    RET16(res);
}
void GDI_PatBlt(void)
{
    ARGS();
    u16 dc = A16(); s16 x = AS16(), y = AS16(), w = AS16(), h = AS16(); u32 rop = A32();
    DarkSel ds;
    BOOL r;
    if (rop == WHITENESS && wdc_dark(dc)) {
        HGDIOBJ ob = SelectObject(DC(dc), dark_solid_brush(dark_bg(RGB(255, 255, 255))));
        r = PatBlt(DC(dc), x, y, w, h, PATCOPY);
        SelectObject(DC(dc), ob);
    } else {
        dark_begin(dc, &ds, 0, rop == PATCOPY);
        r = PatBlt(DC(dc), x, y, w, h, rop);
        dark_end(&ds);
    }
    drawn(dc, x, y, x + w, y + h);
    RET16(r);
}
void GDI_SetPixel(void)
{
    ARGS();
    u16 dc = A16(); s16 x = AS16(), y = AS16(); u32 c = A32();
    COLORREF r = SetPixel(DC(dc), x, y, c);
    drawn(dc, x, y, x + 1, y + 1);
    RET32(r);
}
void GDI_GetPixel(void)
{
    ARGS();
    u16 dc = A16(); s16 x = AS16(), y = AS16();
    RET32(GetPixel(DC(dc), x, y));
}
void GDI_TextOut(void)
{
    ARGS();
    u16 dc = A16(); s16 x = AS16(), y = AS16(); u32 s = A32(); s16 n = AS16();
    BOOL r = TextOutA(DC(dc), x, y, str16(s), n);
    if (wdc_is_window_dc(dc)) {
        SIZE sz = { 0, 0 };
        GetTextExtentPoint32A(DC(dc), str16(s), n, &sz);
        drawn(dc, x - sz.cx, y - sz.cy, x + sz.cx, y + sz.cy);
    }
    RET16(r);
}
void GDI_BitBlt(void)
{
    ARGS();
    u16 dd = A16(); s16 x = AS16(), y = AS16(), w = AS16(), h = AS16();
    u16 sd = A16(); s16 xs = AS16(), ys = AS16(); u32 rop = A32();
    BOOL r;
    COLORREF gt, gb, ot = 0, ob = 0;
    int mono = 0;
    if (wdc_dark(dd) && rop == WHITENESS) {
        HGDIOBJ old = SelectObject(DC(dd), dark_solid_brush(dark_bg(RGB(255, 255, 255))));
        r = PatBlt(DC(dd), x, y, w, h, PATCOPY);
        SelectObject(DC(dd), old);
        drawn(dd, x, y, x + w, y + h);
        RET16(r);
        return;
    }
    /* monochrome bitmaps (masks) must see the game's own colours */
    if (wdc_dark(dd) && sd && dc_has_mono_bitmap(DC(sd)) && wdc_game_colors(dd, &gt, &gb)) {
        mono = 1;
        ot = SetTextColor(DC(dd), gt);
        ob = SetBkColor(DC(dd), gb);
    }
    r = BitBlt(DC(dd), x, y, w, h, DC(sd), xs, ys, rop);
    if (mono) { SetTextColor(DC(dd), ot); SetBkColor(DC(dd), ob); }
    drawn(dd, x, y, x + w, y + h);
    RET16(r);
}
void GDI_StretchBlt(void)
{
    ARGS();
    u16 dd = A16(); s16 x = AS16(), y = AS16(), w = AS16(), h = AS16();
    u16 sd = A16(); s16 xs = AS16(), ys = AS16(), ws = AS16(), hs = AS16(); u32 rop = A32();
    BOOL r;
    COLORREF gt, gb, ot = 0, ob = 0;
    int mono = 0;
    if (wdc_dark(dd) && sd && dc_has_mono_bitmap(DC(sd)) && wdc_game_colors(dd, &gt, &gb)) {
        mono = 1;
        ot = SetTextColor(DC(dd), gt);
        ob = SetBkColor(DC(dd), gb);
    }
    r = StretchBlt(DC(dd), x, y, w, h, DC(sd), xs, ys, ws, hs, rop);
    if (mono) { SetTextColor(DC(dd), ot); SetBkColor(DC(dd), ob); }
    drawn(dd, x, y, x + w, y + h);
    RET16(r);
}

/* ---- objects ---------------------------------------------------------------------- */

void GDI_SelectObject(void)
{
    ARGS();
    u16 dc = A16(), obj = A16();
    HGDIOBJ o = HGDI_32(obj), prev;
    if (o && GetObjectType(o) == OBJ_REGION) {
        RET16(SelectClipRgn(DC(dc), (HRGN)o));
        return;
    }
    prev = SelectObject(DC(dc), o);
    RET16(HGDI_16(prev));
}

void GDI_SelectClipRgn(void) { ARGS(); u16 dc = A16(), r = A16(); RET16(SelectClipRgn(DC(dc), (HRGN)HGDI_32(r))); }
void GDI_CreateCompatibleBitmap(void) { ARGS(); u16 dc = A16(), w = A16(), h = A16(); RET16(HGDI_16(CreateCompatibleBitmap(DC(dc), w, h))); }
void GDI_CreateCompatibleDC(void) { ARGS(); u16 dc = A16(); RET16(HGDI_16(CreateCompatibleDC(DC(dc)))); }
void GDI_CreateEllipticRgn(void)
{
    ARGS();
    s16 l = AS16(), t = AS16(), r = AS16(), b = AS16();
    RET16(HGDI_16(CreateEllipticRgn(l, t, r, b)));
}
void GDI_CreatePatternBrush(void) { ARGS(); u16 bm = A16(); RET16(HGDI_16(CreatePatternBrush((HBITMAP)HGDI_32(bm)))); }
void GDI_CreatePen(void) { ARGS(); s16 st = AS16(), w = AS16(); u32 c = A32(); RET16(HGDI_16(CreatePen(st, w, c))); }
void GDI_CreateSolidBrush(void) { ARGS(); u32 c = A32(); RET16(HGDI_16(CreateSolidBrush(c))); }
void GDI_DeleteDC(void) { ARGS(); u16 dc = A16(); RET16(DeleteDC(DC(dc))); }
void GDI_DeleteObject(void) { ARGS(); u16 h = A16(); dark_object_deleted(HGDI_32(h)); RET16(DeleteObject(HGDI_32(h))); }
void GDI_UnrealizeObject(void) { ARGS(); u16 h = A16(); RET16(UnrealizeObject(HGDI_32(h))); }

void GDI_GetStockObject(void)
{
    ARGS();
    u16 i = A16();
    RET16(HGDI_16(GetStockObject(i)));
}

void GDI_CreateFontIndirect(void)
{
    ARGS();
    u32 p = A32();
    LOGFONT16 *f = (LOGFONT16 *)ptrseg(p);
    LOGFONTA lf;
    if (!f) { RET16(0); return; }
    memset(&lf, 0, sizeof lf);
    lf.lfHeight = f->lfHeight; lf.lfWidth = f->lfWidth; lf.lfEscapement = f->lfEscapement;
    lf.lfOrientation = f->lfOrientation; lf.lfWeight = f->lfWeight; lf.lfItalic = f->lfItalic;
    lf.lfUnderline = f->lfUnderline; lf.lfStrikeOut = f->lfStrikeOut; lf.lfCharSet = f->lfCharSet;
    lf.lfOutPrecision = f->lfOutPrecision; lf.lfClipPrecision = f->lfClipPrecision;
    lf.lfQuality = f->lfQuality; lf.lfPitchAndFamily = f->lfPitchAndFamily;
    memcpy(lf.lfFaceName, f->lfFaceName, 32);
    lf.lfFaceName[31] = 0;
    {
        HFONT hf = CreateFontIndirectA(&lf);
        emu_log("CreateFontIndirect(%d,%d,wt %d,'%s') -> %04X", (int)lf.lfHeight, (int)lf.lfWidth, (int)lf.lfWeight, lf.lfFaceName, HGDI_16(hf));
        RET16(HGDI_16(hf));
    }
}

void GDI_GetTextExtent(void)
{
    ARGS();
    u16 dc = A16(); u32 s = A32(); s16 n = AS16();
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32A(DC(dc), str16(s), n, &sz);
    RET32(MAKELONG((u16)sz.cx, (u16)sz.cy));
}

void GDI_GetTextMetrics(void)
{
    ARGS();
    u16 dc = A16(); u32 p = A32();
    TEXTMETRICA tm;
    TEXTMETRIC16 *t = (TEXTMETRIC16 *)ptrseg(p);
    BOOL r = GetTextMetricsA(DC(dc), &tm);
    if (r && t) {
        t->tmHeight = (s16)tm.tmHeight; t->tmAscent = (s16)tm.tmAscent; t->tmDescent = (s16)tm.tmDescent;
        t->tmInternalLeading = (s16)tm.tmInternalLeading; t->tmExternalLeading = (s16)tm.tmExternalLeading;
        t->tmAveCharWidth = (s16)tm.tmAveCharWidth; t->tmMaxCharWidth = (s16)tm.tmMaxCharWidth;
        t->tmWeight = (s16)tm.tmWeight; t->tmItalic = tm.tmItalic; t->tmUnderlined = tm.tmUnderlined;
        t->tmStruckOut = tm.tmStruckOut; t->tmFirstChar = tm.tmFirstChar; t->tmLastChar = tm.tmLastChar;
        t->tmDefaultChar = tm.tmDefaultChar; t->tmBreakChar = tm.tmBreakChar;
        t->tmPitchAndFamily = tm.tmPitchAndFamily; t->tmCharSet = tm.tmCharSet;
        t->tmOverhang = (s16)tm.tmOverhang; t->tmDigitizedAspectX = (s16)tm.tmDigitizedAspectX;
        t->tmDigitizedAspectY = (s16)tm.tmDigitizedAspectY;
    }
    RET16(r);
}

void GDI_GetDeviceCaps(void)
{
    ARGS();
    u16 dc = A16(); s16 i = AS16();
    int v = GetDeviceCaps(DC(dc), i);
    if (g_zoom > 1 && (i == HORZRES || i == VERTRES)) v /= g_zoom;
    if (v > 32767) v = 32767;
    RET16((u16)(s16)v);
}

/* report colour bitmaps as packed 4bpp (a 16 colour display); mono as 1bpp */
static int bitmap_is_mono(HBITMAP h, BITMAP *bm)
{
    GetObjectA(h, sizeof *bm, bm);
    return bm->bmBitsPixel == 1 && bm->bmPlanes == 1;
}

void GDI_GetObject(void)
{
    ARGS();
    u16 h = A16(); s16 size = AS16(); u32 buf = A32();
    HGDIOBJ o = HGDI_32(h);
    u8 *out = (u8 *)ptrseg(buf);
    int n = 0;
    if (!o || !out || size <= 0) { RET16(0); return; }
    switch (GetObjectType(o)) {
    case OBJ_BITMAP: {
        BITMAP bm;
        BITMAP16 b;
        int mono = bitmap_is_mono((HBITMAP)o, &bm);
        b.bmType = 0;
        b.bmWidth = (s16)bm.bmWidth;
        b.bmHeight = (s16)bm.bmHeight;
        b.bmPlanes = 1;
        b.bmBitsPixel = mono ? 1 : 4;
        b.bmWidthBytes = (s16)(((bm.bmWidth * b.bmBitsPixel + 15) / 16) * 2);
        b.bmBits = 0;
        n = size < (int)sizeof b ? size : (int)sizeof b;
        memcpy(out, &b, n);
        break;
    }
    case OBJ_FONT: {
        LOGFONTA lf;
        LOGFONT16 f;
        GetObjectA(o, sizeof lf, &lf);
        f.lfHeight = (s16)lf.lfHeight; f.lfWidth = (s16)lf.lfWidth; f.lfEscapement = (s16)lf.lfEscapement;
        f.lfOrientation = (s16)lf.lfOrientation; f.lfWeight = (s16)lf.lfWeight; f.lfItalic = lf.lfItalic;
        f.lfUnderline = lf.lfUnderline; f.lfStrikeOut = lf.lfStrikeOut; f.lfCharSet = lf.lfCharSet;
        f.lfOutPrecision = lf.lfOutPrecision; f.lfClipPrecision = lf.lfClipPrecision; f.lfQuality = lf.lfQuality;
        f.lfPitchAndFamily = lf.lfPitchAndFamily;
        memcpy(f.lfFaceName, lf.lfFaceName, 32);
        n = size < (int)sizeof f ? size : (int)sizeof f;
        memcpy(out, &f, n);
        break;
    }
    case OBJ_PEN: {
        LOGPEN lp;
        LOGPEN16 p;
        GetObjectA(o, sizeof lp, &lp);
        p.lopnStyle = (u16)lp.lopnStyle; p.lopnWidth.x = (s16)lp.lopnWidth.x; p.lopnWidth.y = (s16)lp.lopnWidth.y;
        p.lopnColor = lp.lopnColor;
        n = size < (int)sizeof p ? size : (int)sizeof p;
        memcpy(out, &p, n);
        break;
    }
    case OBJ_BRUSH: {
        LOGBRUSH lb;
        LOGBRUSH16 b;
        GetObjectA(o, sizeof lb, &lb);
        b.lbStyle = (u16)lb.lbStyle; b.lbColor = lb.lbColor;
        b.lbHatch = (lb.lbStyle == BS_PATTERN) ? (s16)HGDI_16((HGDIOBJ)lb.lbHatch) : (s16)lb.lbHatch;
        n = size < (int)sizeof b ? size : (int)sizeof b;
        memcpy(out, &b, n);
        break;
    }
    case OBJ_PAL: {
        WORD cnt = 0;
        GetObjectA(o, sizeof cnt, &cnt);
        n = size < 2 ? size : 2;
        memcpy(out, &cnt, n);
        break;
    }
    }
    RET16(n);
}

static const u8 vga_rgb[16][3] = {
    {0,0,0},{0x80,0,0},{0,0x80,0},{0x80,0x80,0},{0,0,0x80},{0x80,0,0x80},{0,0x80,0x80},{0xC0,0xC0,0xC0},
    {0x80,0x80,0x80},{0xFF,0,0},{0,0xFF,0},{0xFF,0xFF,0},{0,0,0xFF},{0xFF,0,0xFF},{0,0xFF,0xFF},{0xFF,0xFF,0xFF}
};

static int nearest_vga(u8 r, u8 g, u8 b)
{
    int i, best = 0;
    long bd = 0x7FFFFFFF;
    for (i = 0; i < 16; i++) {
        long dr = r - vga_rgb[i][0], dg = g - vga_rgb[i][1], db = b - vga_rgb[i][2];
        long d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* The game's artwork was drawn for 16-colour VGA, and part of it uses a colour
   table whose "dark" entries are 0xBF instead of 0x80.  Windows 3.x mapped
   such entries to the nearest device colour, so on the displays of the day
   they looked identical to the standard dark colours.  On a true-colour
   display they come out noticeably brighter (e.g. the high-score scroll
   turns two-tone), so the colour tables of <= 4 bpp DIBs are snapped to the
   16 VGA colours exactly as the original drivers did.
   Returns either the original header or a patched copy in buf. */
const void *dib_vga(const void *hdr, UINT usage, DibVgaBuf *buf)
{
    const BITMAPINFOHEADER *bih = (const BITMAPINFOHEADER *)hdr;
    int i, n;
    if (!hdr || usage != DIB_RGB_COLORS) return hdr;
    if (bih->biSize == sizeof(BITMAPCOREHEADER)) {
        const BITMAPCOREHEADER *ch = (const BITMAPCOREHEADER *)hdr;
        RGBTRIPLE *c;
        if (ch->bcBitCount > 4) return hdr;
        n = 1 << ch->bcBitCount;
        memcpy(buf->b, hdr, sizeof(BITMAPCOREHEADER) + n * sizeof(RGBTRIPLE));
        c = (RGBTRIPLE *)(buf->b + sizeof(BITMAPCOREHEADER));
        for (i = 0; i < n; i++) {
            int k = nearest_vga(c[i].rgbtRed, c[i].rgbtGreen, c[i].rgbtBlue);
            c[i].rgbtRed = vga_rgb[k][0]; c[i].rgbtGreen = vga_rgb[k][1]; c[i].rgbtBlue = vga_rgb[k][2];
        }
        return buf->b;
    }
    if (bih->biSize < sizeof(BITMAPINFOHEADER) || bih->biSize > 124 || bih->biBitCount > 4 || bih->biBitCount == 0)
        return hdr;
    n = bih->biClrUsed ? (int)bih->biClrUsed : 1 << bih->biBitCount;
    if (n > 16) n = 16;
    memcpy(buf->b, hdr, bih->biSize + n * sizeof(RGBQUAD));
    {
        RGBQUAD *c = (RGBQUAD *)(buf->b + bih->biSize);
        for (i = 0; i < n; i++) {
            int k = nearest_vga(c[i].rgbRed, c[i].rgbGreen, c[i].rgbBlue);
            c[i].rgbRed = vga_rgb[k][0]; c[i].rgbGreen = vga_rgb[k][1]; c[i].rgbBlue = vga_rgb[k][2];
        }
    }
    return buf->b;
}

/* in-place variant for a private copy of a DIB (e.g. an icon image) */
void dib_vga_inplace(void *hdr)
{
    DibVgaBuf tmp;
    const void *r = dib_vga(hdr, DIB_RGB_COLORS, &tmp);
    if (r != hdr) {
        const BITMAPINFOHEADER *bih = (const BITMAPINFOHEADER *)hdr;
        size_t n;
        if (bih->biSize == sizeof(BITMAPCOREHEADER)) n = sizeof(BITMAPCOREHEADER) + ((size_t)1 << ((const BITMAPCOREHEADER *)hdr)->bcBitCount) * sizeof(RGBTRIPLE);
        else n = bih->biSize + (size_t)(bih->biClrUsed && bih->biClrUsed < 16 ? bih->biClrUsed : 1u << bih->biBitCount) * sizeof(RGBQUAD);
        memcpy(hdr, tmp.b, n);
    }
}

void GDI_GetBitmapBits(void)
{
    ARGS();
    u16 h = A16(); u32 count = A32(); u32 buf = A32();
    HBITMAP hb = (HBITMAP)HGDI_32(h);
    BITMAP bm;
    u8 *out = (u8 *)ptrseg(buf);
    if (!hb || !out) { RET32(0); return; }
    if (bitmap_is_mono(hb, &bm)) {
        RET32((u32)GetBitmapBits(hb, (LONG)count, out));
        return;
    } else {
        int w = bm.bmWidth, ht = bm.bmHeight, x, y;
        int wb = ((w * 4 + 15) / 16) * 2;
        u32 total = (u32)wb * ht, n;
        BITMAPINFO bi;
        u32 *px = malloc((size_t)w * ht * 4);
        u8 *tmp = calloc(1, total);
        HDC dc = GetDC(NULL);
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -ht;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        GetDIBits(dc, hb, 0, ht, px, &bi, DIB_RGB_COLORS);
        ReleaseDC(NULL, dc);
        for (y = 0; y < ht; y++) {
            for (x = 0; x < w; x++) {
                u32 c = px[(size_t)y * w + x];
                int idx = nearest_vga((u8)(c >> 16), (u8)(c >> 8), (u8)c);
                if (x & 1) tmp[y * wb + x / 2] |= (u8)idx;
                else tmp[y * wb + x / 2] |= (u8)(idx << 4);
            }
        }
        n = count < total ? count : total;
        memcpy(out, tmp, n);
        free(px);
        free(tmp);
        RET32(n);
    }
}

void GDI_CreateDIBitmap(void)
{
    ARGS();
    u16 dc = A16(); u32 pbih = A32(); u32 init = A32(); u32 pbits = A32(); u32 pbmi = A32(); u16 usage = A16();
    DibVgaBuf vb;
    HBITMAP h = CreateDIBitmap(DC(dc), (const BITMAPINFOHEADER *)ptrseg(pbih), init,
                               ptrseg(pbits), (const BITMAPINFO *)dib_vga(ptrseg(pbmi), usage, &vb), usage);
    RET16(HGDI_16(h));
}

void GDI_MulDiv(void)
{
    ARGS();
    s16 a = AS16(), b = AS16(), c = AS16();
    long r;
    if (!c) { RET16(0x8000); return; }
    r = (long)a * b;
    if ((r > 0 && c > 0) || (r < 0 && c < 0)) r = (r + c / 2) / c;
    else r = (r - c / 2) / c;
    if (r > 32767 || r < -32767) { RET16(0x8000); return; }
    RET16((u16)(s16)r);
}

/* ---- palettes ---------------------------------------------------------------------- */

void GDI_CreatePalette(void)
{
    ARGS();
    u32 p = A32();
    RET16(HGDI_16(CreatePalette((const LOGPALETTE *)ptrseg(p))));
}

void GDI_AnimatePalette(void)
{
    ARGS();
    u16 pal = A16(), start = A16(), n = A16(); u32 p = A32();
    AnimatePalette((HPALETTE)HGDI_32(pal), start, n, (const PALETTEENTRY *)ptrseg(p));
    RET0();
}

/* ---- metafiles ------------------------------------------------------------------------ */

void GDI_SetMetaFileBits(void)
{
    ARGS();
    u16 hmem = A16();
    u16 sel = glob_sel(hmem);
    u32 size;
    const u8 *bits;
    HMETAFILE mf;
    if (!sel) { RET16(0); return; }
    bits = (const u8 *)ptr16(sel, 0);
    size = (u32)(bits[6] | (bits[7] << 8) | (bits[8] << 16) | ((u32)bits[9] << 24)) * 2;   /* mtSize in words */
    if (!size || size > glob_size(hmem)) size = glob_size(hmem);
    mf = SetMetaFileBitsEx(size, bits);
    emu_log("SetMetaFileBits(%04X, %u bytes) -> %p", hmem, size, (void *)mf);
    RET16(HGDI_16(mf));
}

/* dark mode: a copy of a metafile with its pen, brush and text colours
   recoloured (pictures drawn on the game's white backgrounds, such as the
   figure in the inventory window) */
typedef struct { HMETAFILE src, dst; } MfCache;
static MfCache g_mfcache[32];
static int g_nmfcache;

static u32 get32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

static HMETAFILE dark_metafile(HMETAFILE mf)
{
    UINT n, o;
    u8 *b;
    HMETAFILE d;
    int i;
    for (i = 0; i < g_nmfcache; i++) if (g_mfcache[i].src == mf) return g_mfcache[i].dst;
    n = GetMetaFileBitsEx(mf, 0, NULL);
    if (!n || !(b = malloc(n))) return mf;
    GetMetaFileBitsEx(mf, n, b);
    o = (UINT)(b[2] | (b[3] << 8)) * 2;     /* header size in words */
    while (o + 6 <= n) {
        u32 sz = get32(b + o);
        u16 fn = (u16)(b[o + 4] | (b[o + 5] << 8));
        u8 *p = b + o + 6;
        if (sz < 3 || fn == 0 || o + sz * 2 > n) break;
        switch (fn) {
        case 0x0201: put32(p, dark_bg(get32(p))); break;                       /* SETBKCOLOR */
        case 0x0209: put32(p, dark_fg(get32(p))); break;                       /* SETTEXTCOLOR */
        case 0x02FA: if (sz >= 8) put32(p + 6, dark_fg(get32(p + 6))); break;   /* CREATEPENINDIRECT */
        case 0x02FC:                                                           /* CREATEBRUSHINDIRECT */
            if (sz >= 7 && p[0] == BS_SOLID && p[1] == 0) put32(p + 2, dark_bg(get32(p + 2)));
            break;
        }
        o += sz * 2;
    }
    d = SetMetaFileBitsEx(n, b);
    free(b);
    if (!d) return mf;
    if (g_nmfcache == 32) { DeleteMetaFile(g_mfcache[0].dst); memmove(&g_mfcache[0], &g_mfcache[1], 31 * sizeof g_mfcache[0]); g_nmfcache--; }
    g_mfcache[g_nmfcache].src = mf;
    g_mfcache[g_nmfcache].dst = d;
    g_nmfcache++;
    return d;
}

void GDI_PlayMetaFile(void)
{
    ARGS();
    u16 dc = A16(), mf = A16();
    HMETAFILE m = (HMETAFILE)HGDI_32(mf);
    BOOL r;
    if (m && wdc_dark(dc) == DARK_UI && user_window_light_bg(wdc_window(dc))) m = dark_metafile(m);
    r = PlayMetaFile(DC(dc), m);
    wdc_drawn(dc, NULL);
    RET16(r);
}
void GDI_DeleteMetaFile(void)
{
    ARGS();
    u16 mf = A16();
    HMETAFILE m = (HMETAFILE)HGDI_32(mf);
    int i;
    for (i = 0; i < g_nmfcache; i++)
        if (g_mfcache[i].src == m) {
            DeleteMetaFile(g_mfcache[i].dst);
            g_mfcache[i] = g_mfcache[--g_nmfcache];
            break;
        }
    RET16(DeleteMetaFile(m));
}
