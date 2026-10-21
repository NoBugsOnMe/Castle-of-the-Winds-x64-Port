/*
 * zoom.c - integer scaling ("zoom") of the game's windows.
 *
 * The 16-bit code keeps working in its original pixel coordinates (the
 * "logical" space); everything it sees is divided by the zoom factor S and
 * everything it asks for is multiplied by S:
 *
 *  - Each window painted by 16-bit code gets a backing bitmap of its logical
 *    client size.  GetDC/BeginPaint hand out a memory DC on that bitmap, the
 *    game draws exactly as on a 640x480 screen, and the changed area is copied
 *    to the real window with an integer nearest-neighbour stretch, so the
 *    result is pixel-identical to the original, only larger.  Drawing done
 *    outside WM_PAINT is tracked per DC and flushed when the DC is released
 *    and whenever the game polls for messages or reads the clock.
 *  - Windows painted by native code (dialog boxes and their controls,
 *    buttons, list boxes) are made S times larger by giving them S times
 *    larger fonts; anything the game draws into them (owner-drawn items,
 *    WM_PAINT of a dialog) goes through a temporary bitmap.
 *  - Client coordinates, client sizes, mouse positions and the screen size
 *    are divided by S; window frames (caption, borders, menu bar) keep their
 *    real size, so a logical window rectangle is "real frame + client/S".
 *  - Cursors made from the game's resources are scaled to match.
 *  - Instead of the nearest-neighbour stretch, the surfaces can be enlarged
 *    with an edge-aware pixel art scaler (scale.c, "Upscaling" setting):
 *    the same pixels, with smoother diagonals and curves.
 *
 * With S = 1 none of this is active and every call goes straight through.
 */
#include "win16.h"
#define SCALE_NO_U32
#include "scale.h"

int g_zoom = 1;
int g_zoom_setting = 0;          /* 0 = automatic, else fixed factor */
int g_upscale = SCALE_SHARP;     /* 0 = original pixels, SCALE_SHARP, SCALE_SOFT */

/* ---- settings ---------------------------------------------------------------- */

static int auto_zoom_for(HMONITOR mon)
{
    MONITORINFO mi;
    int w, h, s;
    mi.cbSize = sizeof mi;
    if (!mon || !GetMonitorInfoA(mon, &mi)) {
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
    } else {
        w = mi.rcMonitor.right - mi.rcMonitor.left;
        h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }
    /* the game was laid out for 640x480 .. 1024x768 screens: pick the largest
       factor that leaves at least 1024x700 logical pixels */
    s = h / 700;
    if (w / 1024 < s) s = w / 1024;
    if (s < 1) s = 1;
    if (s > ZOOM_MAX) s = ZOOM_MAX;
    return s;
}

int zoom_auto_factor(void)
{
    POINT pt = { 0, 0 };
    return auto_zoom_for(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY));
}

void zoom_init(void)
{
    char v[16];
    GetPrivateProfileStringA("Port", "Zoom", "Auto", v, sizeof v, g_ini_path);
    g_zoom_setting = atoi(v);
    if (g_zoom_setting < 0 || g_zoom_setting > ZOOM_MAX) g_zoom_setting = 0;
    g_zoom = g_zoom_setting ? g_zoom_setting : zoom_auto_factor();
    emu_log("zoom: setting '%s' -> factor %d", v, g_zoom);
    GetPrivateProfileStringA("Port", "Upscaling", "Sharp", v, sizeof v, g_ini_path);
    if (!_stricmp(v, "Original") || !_stricmp(v, "Off") || !_stricmp(v, "0")) g_upscale = 0;
    else if (!_stricmp(v, "Soft") || !_stricmp(v, "2")) g_upscale = SCALE_SOFT;
    else g_upscale = SCALE_SHARP;
    emu_log("upscaling: setting '%s' -> %d", v, g_upscale);
}

/* the upscaling in effect (only when zoomed) */
int zoom_upscale_mode(void) { return g_zoom > 1 ? g_upscale : 0; }

/* ---- window frames -------------------------------------------------------------- */

/* real thickness of a window's frame around its client area */
void zoom_nc(HWND h, RECT *nc)
{
    RECT wr, cr;
    POINT o = { 0, 0 };
    memset(nc, 0, sizeof *nc);
    if (!h || !GetWindowRect(h, &wr) || !GetClientRect(h, &cr)) return;
    ClientToScreen(h, &o);
    nc->left = o.x - wr.left;
    nc->top = o.y - wr.top;
    nc->right = wr.right - (o.x + cr.right);
    nc->bottom = wr.bottom - (o.y + cr.bottom);
    if (nc->left < 0 || nc->top < 0 || nc->right < 0 || nc->bottom < 0) memset(nc, 0, sizeof *nc);
}

void zoom_nc_style(DWORD style, DWORD exstyle, BOOL menu, RECT *nc)
{
    RECT r = { 0, 0, 100, 100 };
    AdjustWindowRectEx(&r, style, menu, exstyle);
    nc->left = -r.left;
    nc->top = -r.top;
    nc->right = r.right - 100;
    nc->bottom = r.bottom - 100;
    if (style & WS_VSCROLL) nc->right += GetSystemMetrics(SM_CXVSCROLL);
    if (style & WS_HSCROLL) nc->bottom += GetSystemMetrics(SM_CYHSCROLL);
}

/* outer position/size, real -> logical (positions are in the parent's client
   coordinates or on the screen; a logical window is frame + client/S) */
void zoom_win_to_logical(const RECT *nc, int *x, int *y, int *cx, int *cy)
{
    int ncw = nc->left + nc->right, nch = nc->top + nc->bottom;
    if (x) *x = zfloor(*x + nc->left) - nc->left;
    if (y) *y = zfloor(*y + nc->top) - nc->top;
    if (cx) *cx = (*cx > ncw) ? ncw + zceil(*cx - ncw) : *cx;
    if (cy) *cy = (*cy > nch) ? nch + zceil(*cy - nch) : *cy;
}

void zoom_win_to_real(const RECT *nc, int *x, int *y, int *cx, int *cy)
{
    int ncw = nc->left + nc->right, nch = nc->top + nc->bottom;
    if (x) *x = (*x + nc->left) * g_zoom - nc->left;
    if (y) *y = (*y + nc->top) * g_zoom - nc->top;
    if (cx) *cx = (*cx > ncw) ? ncw + (*cx - ncw) * g_zoom : *cx;
    if (cy) *cy = (*cy > nch) ? nch + (*cy - nch) * g_zoom : *cy;
}

/* logical window rectangle (screen coordinates) */
void zoom_window_rect(HWND h, RECT *out)
{
    RECT nc, cr;
    POINT o = { 0, 0 };
    if (!GetClientRect(h, &cr)) { memset(out, 0, sizeof *out); return; }
    zoom_nc(h, &nc);
    ClientToScreen(h, &o);
    out->left = zfloor(o.x) - nc.left;
    out->top = zfloor(o.y) - nc.top;
    out->right = zfloor(o.x) + zceil(cr.right) + nc.right;
    out->bottom = zfloor(o.y) + zceil(cr.bottom) + nc.bottom;
}

/* ---- backing surfaces --------------------------------------------------------- */

struct ZSurf {
    HWND hwnd;
    HDC dc;
    HBITMAP bmp, oldbmp;
    int w, h;                 /* logical size of the bitmap */
    RECT dirty;               /* logical area drawn since the last flush */
    DWORD dirty_since;        /* when dirty last became non-empty */
    int sessions;             /* open GetDC/BeginPaint sessions */
    int painting;             /* inside WM_ERASEBKGND / BeginPaint..EndPaint */
    u16 dc16;
};

/* everything the game holds a DC for: persistent surfaces, temporary
   surfaces for natively painted windows, and (S = 1) plain window DCs */
typedef struct WinDC {
    HWND hwnd;
    HDC dc;                   /* DC handed to the game */
    ZSurf *zs;                /* persistent surface, or NULL */
    int kind;                 /* WDC_xxx */
    int refs;
    /* temporary surface */
    HBITMAP tbmp, toldbmp;
    RECT treal;               /* real client area covered */
    RECT tlog;                /* the same area in logical coordinates */
    HDC target;               /* real DC to copy to (paint/owner-draw DC), or NULL */
    RECT dirty;
    /* colours the game asked for (dark mode shows others) */
    COLORREF text, bk;
    int policy;               /* DARK_UI / DARK_MAP (see dark.c) */
} WinDC;

enum { WDC_PLAIN, WDC_SURFACE, WDC_TEMP };

static WinDC *g_wdc[65536];   /* by 16-bit DC handle */

static ZSurf *g_surfs[512];
static int g_nsurfs;

static void rect_union_into(RECT *d, const RECT *r)
{
    if (r->left >= r->right || r->top >= r->bottom) return;
    if (d->left >= d->right || d->top >= d->bottom) { *d = *r; return; }
    if (r->left < d->left) d->left = r->left;
    if (r->top < d->top) d->top = r->top;
    if (r->right > d->right) d->right = r->right;
    if (r->bottom > d->bottom) d->bottom = r->bottom;
}

static int rect_empty(const RECT *r) { return r->left >= r->right || r->top >= r->bottom; }

/* windows painted by 16-bit code get a surface when zoomed, and the map
   windows also when they are dimmed (dark mode) */
int zoom_window_has_surface(HWND h)
{
    WndInfo *wi;
    if (!h) return 0;
    wi = wnd_info(h, 0);
    if (!(wi && wi->proc16 && !wi->native_old && !wi->is_dialog)) return 0;
    if (hud_wants_surface(h)) return 1;     /* (also notes which window is the map) */
    return g_zoom > 1 || (dark_dim_active() && dark_policy_for(h) == DARK_MAP);
}

static int surf_dimmed(HWND h) { return dark_dim_active() && dark_policy_for(h) == DARK_MAP; }

static void surf_logical_size(HWND h, int *w, int *hh)
{
    RECT cr;
    GetClientRect(h, &cr);
    *w = zceil(cr.right);
    *hh = zceil(cr.bottom);
    if (*w < 1) *w = 1;
    if (*hh < 1) *hh = 1;
}

static HBRUSH surf_background(HWND h)
{
    HBRUSH br = (HBRUSH)GetClassLongPtrA(h, GCLP_HBRBACKGROUND);
    if (br) return br;
    return g_dark ? dark_solid_brush(dark_bg(RGB(255, 255, 255))) : (HBRUSH)GetStockObject(WHITE_BRUSH);
}

/* creates or resizes the surface of a window */
static ZSurf *surf_get(HWND h, int create)
{
    WndInfo *wi = wnd_info(h, 0);
    ZSurf *zs;
    int w, ht;
    if (!wi) return NULL;
    zs = wi->zs;
    if (!zs) {
        HDC sdc;
        if (!create || g_nsurfs == (int)(sizeof g_surfs / sizeof g_surfs[0])) return NULL;
        zs = calloc(1, sizeof *zs);
        zs->hwnd = h;
        sdc = GetDC(NULL);
        zs->dc = CreateCompatibleDC(sdc);
        surf_logical_size(h, &zs->w, &zs->h);
        zs->bmp = CreateCompatibleBitmap(sdc, zs->w, zs->h);
        ReleaseDC(NULL, sdc);
        zs->oldbmp = SelectObject(zs->dc, zs->bmp);
        {
            /* start from the window's background so nothing flashes */
            RECT r = { 0, 0, zs->w, zs->h };
            FillRect(zs->dc, &r, surf_background(h));
        }
        zs->dc16 = HDC_16(zs->dc);
        wi->zs = zs;
        g_surfs[g_nsurfs++] = zs;
        return zs;
    }
    surf_logical_size(h, &w, &ht);
    if (w != zs->w || ht != zs->h) {
        HDC sdc = GetDC(NULL);
        HBITMAP nb = CreateCompatibleBitmap(sdc, w, ht);
        HDC tmp = CreateCompatibleDC(sdc);
        HGDIOBJ o = SelectObject(tmp, nb);
        RECT r = { 0, 0, w, ht };
        ReleaseDC(NULL, sdc);
        FillRect(tmp, &r, surf_background(h));
        BitBlt(tmp, 0, 0, w < zs->w ? w : zs->w, ht < zs->h ? ht : zs->h, zs->dc, 0, 0, SRCCOPY);
        SelectObject(tmp, o);
        DeleteDC(tmp);
        SelectObject(zs->dc, nb);
        DeleteObject(zs->bmp);
        zs->bmp = nb;
        zs->w = w;
        zs->h = ht;
        if (zs->dirty.right > w) zs->dirty.right = w;
        if (zs->dirty.bottom > ht) zs->dirty.bottom = ht;
    }
    return zs;
}

void zoom_surface_free(HWND h)
{
    WndInfo *wi = wnd_info(h, 0);
    ZSurf *zs;
    int i;
    if (!wi || !(zs = wi->zs)) return;
    for (i = 0; i < g_nsurfs; i++) if (g_surfs[i] == zs) { g_surfs[i] = g_surfs[--g_nsurfs]; break; }
    if (g_wdc[zs->dc16] && g_wdc[zs->dc16]->zs == zs) { free(g_wdc[zs->dc16]); g_wdc[zs->dc16] = NULL; }
    hmap_remove(&g_gdi_map, zs->dc);
    SelectObject(zs->dc, zs->oldbmp);
    DeleteDC(zs->dc);
    DeleteObject(zs->bmp);
    free(zs);
    wi->zs = NULL;
}

/* scratch 32 bpp bitmaps: source pixels (dimming, upscaling) and upscaled output */
typedef struct { HDC dc; HBITMAP bmp; u32 *bits; int w, h; } Scratch;
static Scratch g_scr, g_out;

static int scratch_get(Scratch *sc, int w, int h)
{
    if (w <= sc->w && h <= sc->h && sc->dc) return 1;
    if (sc->dc) { DeleteDC(sc->dc); DeleteObject(sc->bmp); sc->dc = NULL; }
    {
        BITMAPINFO bi;
        void *bits = NULL;
        if (w < sc->w) w = sc->w;
        if (h < sc->h) h = sc->h;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        sc->bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!sc->bmp || !bits) { if (sc->bmp) DeleteObject(sc->bmp); sc->w = sc->h = 0; return 0; }
        sc->dc = CreateCompatibleDC(NULL);
        SelectObject(sc->dc, sc->bmp);
        sc->bits = (u32 *)bits;
        sc->w = w;
        sc->h = h;
    }
    return 1;
}

static int scratch(int w, int h) { return scratch_get(&g_scr, w, h); }
#define g_scr_dc g_scr.dc
#define g_scr_bits g_scr.bits
#define g_scr_w g_scr.w

static void dim_pixels(u32 *bits, int pitch, int w, int h)
{
    const u8 *lut = dark_dim_lut();
    int x, y;
    for (y = 0; y < h; y++) {
        u32 *row = bits + (size_t)y * pitch;
        for (x = 0; x < w; x++) {
            u32 c = row[x];
            row[x] = (u32)lut[c & 0xFF] | ((u32)lut[(c >> 8) & 0xFF] << 8) | ((u32)lut[(c >> 16) & 0xFF] << 16);
        }
    }
}

/* ---- upscaling on worker threads ---------------------------------------------------- */

#define MAX_WORKERS 15
typedef struct {
    HANDLE thread, go, done;
    ScaleCtx *ctx;
} Worker;

static struct {
    const u32 *src; int w, h, spitch, y0, y1, band; u32 *dst; int dpitch, s, mode;
    volatile LONG next;
    int nbands;
    volatile LONG failed;
} g_job;

static Worker g_workers[MAX_WORKERS];
static int g_nworkers = -1;
static ScaleCtx *g_main_ctx;

/* bands are taken from a shared counter until none are left */
static void run_bands(ScaleCtx *ctx)
{
    for (;;) {
        LONG b = InterlockedIncrement(&g_job.next) - 1;
        int a, e;
        if (b >= g_job.nbands) break;
        a = g_job.y0 + (int)b * g_job.band;
        e = a + g_job.band < g_job.y1 ? a + g_job.band : g_job.y1;
        if (!pixel_scale_rows(ctx, g_job.src, g_job.w, g_job.h, g_job.spitch, a, e,
                              g_job.dst + (size_t)(a - g_job.y0) * g_job.s * g_job.dpitch, g_job.dpitch,
                              g_job.s, g_job.mode, 0x00FFFFFF))
            InterlockedExchange(&g_job.failed, 1);
    }
}

static DWORD WINAPI worker_main(LPVOID p)
{
    Worker *w = (Worker *)p;
    for (;;) {
        WaitForSingleObject(w->go, INFINITE);
        run_bands(w->ctx);
        SetEvent(w->done);
    }
    return 0;
}

static void workers_init(void)
{
    SYSTEM_INFO si;
    int i, n;
    GetSystemInfo(&si);
    n = (int)si.dwNumberOfProcessors - 1;
    if (n > MAX_WORKERS) n = MAX_WORKERS;
    if (n < 0) n = 0;
    g_main_ctx = scale_ctx_new();
    g_nworkers = 0;
    for (i = 0; i < n; i++) {
        Worker *w = &g_workers[g_nworkers];
        w->ctx = scale_ctx_new();
        w->go = CreateEventA(NULL, FALSE, FALSE, NULL);
        w->done = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (!w->ctx || !w->go || !w->done) break;
        w->thread = CreateThread(NULL, 0, worker_main, w, 0, NULL);
        if (!w->thread) break;
        g_nworkers++;
    }
    emu_log("upscaling: %d worker threads", g_nworkers);
}

/* scales source rows y0..y1 of src (w x h) into dst (its first row = output
   row y0*s), splitting the work over the worker threads when it is large */
static int scale_rows_parallel(const u32 *src, int w, int h, int spitch, int y0, int y1, u32 *dst, int dpitch, int s, int mode)
{
    long work = (long)w * (y1 - y0) * (mode == SCALE_SOFT ? 4 : 1);
    int i, nw;
    if (g_nworkers < 0) workers_init();
    if (!g_main_ctx) return 0;
    nw = g_nworkers;
    if (nw == 0 || work < 40000 || y1 - y0 < 16) {
        return pixel_scale_rows(g_main_ctx, src, w, h, spitch, y0, y1, dst, dpitch, s, mode, 0x00FFFFFF);
    }
    g_job.src = src; g_job.w = w; g_job.h = h; g_job.spitch = spitch;
    g_job.y0 = y0; g_job.y1 = y1; g_job.dst = dst; g_job.dpitch = dpitch; g_job.s = s; g_job.mode = mode;
    /* bands of about 48 rows (fewer for small jobs), at least one per thread */
    g_job.band = (y1 - y0 + nw) / (nw + 1);
    if (g_job.band > 48) g_job.band = 48;
    if (g_job.band < 8) g_job.band = 8;
    g_job.nbands = (y1 - y0 + g_job.band - 1) / g_job.band;
    g_job.next = 0;
    g_job.failed = 0;
    if (nw > g_job.nbands - 1) nw = g_job.nbands - 1;
    for (i = 0; i < nw; i++) SetEvent(g_workers[i].go);
    run_bands(g_main_ctx);
    for (i = 0; i < nw; i++) WaitForSingleObject(g_workers[i].done, INFINITE);
    return !g_job.failed;
}

/* single-threaded scaling of a small image (icons, help pictures) */
int zoom_scale_pixels(const u32 *src, int w, int h, u32 *dst, int s, int mode, u32 mask)
{
    if (g_nworkers < 0) workers_init();
    return g_main_ctx && pixel_scale(g_main_ctx, src, w, h, w, dst, w * s, s, mode, mask);
}

/* copies the logical area l of mem (mw x mh) to real with the upscaler:
   logical (x, y) lands at real ((x - ox) * S, (y - oy) * S).  With grow, the
   pixels around l whose look depends on l are redone as well.  Returns 0 if
   it could not (the caller then stretches plainly). */
static int scaled_blit(HDC real, HDC mem, int mw, int mh, const RECT *l, int ox, int oy, int dim, int grow)
{
    int s = g_zoom, mode = zoom_upscale_mode(), m;
    RECT o, in;
    int iw, ih, ow, oh;
    if (!mode || s < 2) return 0;
    m = pixel_scale_margin(s, mode);
    o = *l;
    if (grow) InflateRect(&o, m, m);
    if (o.left < 0) o.left = 0;
    if (o.top < 0) o.top = 0;
    if (o.right > mw) o.right = mw;
    if (o.bottom > mh) o.bottom = mh;
    if (o.left >= o.right || o.top >= o.bottom) return 1;
    in = o;
    InflateRect(&in, m, m);
    if (in.left < 0) in.left = 0;
    if (in.top < 0) in.top = 0;
    if (in.right > mw) in.right = mw;
    if (in.bottom > mh) in.bottom = mh;
    iw = in.right - in.left;
    ih = in.bottom - in.top;
    ow = o.right - o.left;
    oh = o.bottom - o.top;
    if (!scratch_get(&g_scr, iw, ih) || !scratch_get(&g_out, iw * s, oh * s)) return 0;
    BitBlt(g_scr.dc, 0, 0, iw, ih, mem, in.left, in.top, SRCCOPY);
    GdiFlush();
    if (dim) dim_pixels(g_scr.bits, g_scr.w, iw, ih);
    {
        LARGE_INTEGER t0, t1, fq;
        int big = g_log_enabled && (long)ow * oh >= 200000;
        if (big) QueryPerformanceCounter(&t0);
        if (!scale_rows_parallel(g_scr.bits, iw, ih, g_scr.w, o.top - in.top, o.bottom - in.top, g_out.bits, g_out.w, s, mode))
            return 0;
        if (big) {
            QueryPerformanceCounter(&t1);
            QueryPerformanceFrequency(&fq);
            emu_log("upscale %dx%d (x%d, mode %d): %.1f ms", ow, oh, s, mode, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart);
        }
    }
    BitBlt(real, (o.left - ox) * s, (o.top - oy) * s, ow * s, oh * s, g_out.dc, (o.left - in.left) * s, 0, SRCCOPY);
    return 1;
}

/* copies a logical area of a surface (mw x mh) to a real DC, optionally dimmed */
static void blit_up(HDC real, HDC mem, int mw, int mh, const RECT *l, int ox, int oy, int dim)
{
    int s = g_zoom, w = l->right - l->left, h = l->bottom - l->top;
    HDC src = mem;
    int sx = l->left, sy = l->top;
    if (w <= 0 || h <= 0) return;
    if (scaled_blit(real, mem, mw, mh, l, ox, oy, dim, 1)) return;
    if (dim && scratch(w, h)) {
        BitBlt(g_scr_dc, 0, 0, w, h, mem, l->left, l->top, SRCCOPY);
        GdiFlush();
        dim_pixels(g_scr_bits, g_scr_w, w, h);
        src = g_scr_dc;
        sx = sy = 0;
    }
    if (s == 1) {
        BitBlt(real, l->left - ox, l->top - oy, w, h, src, sx, sy, SRCCOPY);
        return;
    }
    SetStretchBltMode(real, COLORONCOLOR);
    StretchBlt(real, (l->left - ox) * s, (l->top - oy) * s, w * s, h * s, src, sx, sy, w, h, SRCCOPY);
}

static void surf_flush(ZSurf *zs, HDC real)
{
    HDC dc;
    if (rect_empty(&zs->dirty)) return;
    dc = real ? real : GetDC(zs->hwnd);
    if (dc) {
        blit_up(dc, zs->dc, zs->w, zs->h, &zs->dirty, 0, 0, surf_dimmed(zs->hwnd));
        hud_overlay(zs->hwnd, dc);
        if (!real) ReleaseDC(zs->hwnd, dc);
    }
    SetRectEmpty(&zs->dirty);
}

/* shows a logical area of a window again from its surface (the combat bars
   moved or went away) */
void zoom_redraw_logical(HWND h, const RECT *r)
{
    ZSurf *zs = zoom_window_has_surface(h) ? surf_get(h, 1) : NULL;
    RECT c = *r;
    if (!zs) {
        RECT rr = { r->left * g_zoom, r->top * g_zoom, r->right * g_zoom, r->bottom * g_zoom };
        InvalidateRect(h, &rr, FALSE);
        return;
    }
    if (c.left < 0) c.left = 0;
    if (c.top < 0) c.top = 0;
    if (c.right > zs->w) c.right = zs->w;
    if (c.bottom > zs->h) c.bottom = zs->h;
    if (rect_empty(&c)) return;
    if (rect_empty(&zs->dirty)) zs->dirty_since = GetTickCount();
    rect_union_into(&zs->dirty, &c);
    if (!zs->painting) surf_flush(zs, NULL);
}

/* settings changed: drop all surfaces (they are made again as needed) */
void zoom_surfaces_reset(void)
{
    int i;
    zoom_flush_all();
    for (i = g_nsurfs - 1; i >= 0; i--)
        if (!g_surfs[i]->sessions) zoom_surface_free(g_surfs[i]->hwnd);
}

void zoom_flush_all(void)
{
    int i;
    for (i = 0; i < g_nsurfs; i++)
        if (!rect_empty(&g_surfs[i]->dirty)) surf_flush(g_surfs[i], NULL);
}

/* the game is drawing outside WM_PAINT (animations) and has not handed
   control back for min_age ms: show it.  Windows being painted are left
   alone - EndPaint shows them in one piece, without flicker. */
void zoom_flush_drawing(DWORD min_age)
{
    int i;
    DWORD now = min_age ? GetTickCount() : 0;
    for (i = 0; i < g_nsurfs; i++) {
        ZSurf *zs = g_surfs[i];
        if (rect_empty(&zs->dirty) || zs->painting) continue;
        if (min_age && now - zs->dirty_since < min_age) continue;
        surf_flush(zs, NULL);
    }
}

/* ---- drawing notifications from the GDI layer ------------------------------------- */

/* r: area in the DC's logical coordinates, NULL = everything */
void wdc_drawn(u16 dc16, const RECT *r)
{
    WinDC *w = g_wdc[dc16];
    RECT d;
    if (!w || w->kind == WDC_PLAIN) return;
    if (r) {
        POINT p[2];
        p[0].x = r->left; p[0].y = r->top; p[1].x = r->right; p[1].y = r->bottom;
        LPtoDP(w->dc, p, 2);
        d.left = (p[0].x < p[1].x ? p[0].x : p[1].x) - 1;
        d.top = (p[0].y < p[1].y ? p[0].y : p[1].y) - 1;
        d.right = (p[0].x > p[1].x ? p[0].x : p[1].x) + 1;
        d.bottom = (p[0].y > p[1].y ? p[0].y : p[1].y) + 1;
    } else {
        d.left = d.top = -32768;
        d.right = d.bottom = 32767;
    }
    if (w->kind == WDC_SURFACE) {
        ZSurf *zs = w->zs;
        if (d.left < 0) d.left = 0;
        if (d.top < 0) d.top = 0;
        if (d.right > zs->w) d.right = zs->w;
        if (d.bottom > zs->h) d.bottom = zs->h;
        if (rect_empty(&zs->dirty)) zs->dirty_since = GetTickCount();
        rect_union_into(&zs->dirty, &d);
    } else {
        RECT b = { 0, 0, w->tlog.right - w->tlog.left, w->tlog.bottom - w->tlog.top };
        IntersectRect(&d, &d, &b);
        rect_union_into(&w->dirty, &d);
    }
}

int wdc_is_window_dc(u16 dc16) { return g_wdc[dc16] != NULL; }
HWND wdc_window(u16 dc16) { return g_wdc[dc16] ? g_wdc[dc16]->hwnd : NULL; }

/* ---- handing out DCs ---------------------------------------------------------------- */

static void wdc_drop(WinDC *w);

static WinDC *wdc_new(HWND h, HDC dc, int kind)
{
    u16 id = HDC_16(dc);
    WinDC *w = g_wdc[id];
    if (w && w->dc == dc) { w->refs++; return w; }
    if (w) free(w);
    w = calloc(1, sizeof *w);
    w->hwnd = h;
    w->dc = dc;
    w->kind = kind;
    w->refs = 1;
    w->text = GetTextColor(dc);
    w->bk = GetBkColor(dc);
    w->policy = dark_policy_for(h);
    if (g_dark) {
        SetTextColor(dc, dark_fg_p(w->policy, w->text));
        SetBkColor(dc, dark_bg_p(w->policy, w->bk));
    }
    g_wdc[id] = w;
    return w;
}

/* ---- colours (dark mode) ---- */

/* 0 = draw as the game says, else the window's dark-mode policy */
int wdc_dark(u16 dc16) { return (g_dark && g_wdc[dc16]) ? g_wdc[dc16]->policy : 0; }

COLORREF wdc_set_text(u16 dc16, COLORREF c)
{
    WinDC *w = g_wdc[dc16];
    COLORREF prev;
    if (!w) return SetTextColor(HDC_32(dc16), c);
    prev = w->text;
    w->text = c;
    SetTextColor(w->dc, g_dark ? dark_fg_p(w->policy, c) : c);
    return prev;
}

COLORREF wdc_set_bk(u16 dc16, COLORREF c)
{
    WinDC *w = g_wdc[dc16];
    COLORREF prev;
    if (!w) return SetBkColor(HDC_32(dc16), c);
    prev = w->bk;
    w->bk = c;
    SetBkColor(w->dc, g_dark ? dark_bg_p(w->policy, c) : c);
    return prev;
}

/* the colours the game set (for monochrome bitmaps, which must keep them) */
int wdc_game_colors(u16 dc16, COLORREF *text, COLORREF *bk)
{
    WinDC *w = g_wdc[dc16];
    if (!w) return 0;
    *text = w->text;
    *bk = w->bk;
    return 1;
}

/* a native DC handed to the game for a moment (WM_CTLCOLOR, owner drawing) */
void wdc_temp_begin(HWND h, HDC dc)
{
    if (dc) wdc_new(h, dc, WDC_PLAIN);
}

void wdc_temp_end(HDC dc)
{
    WinDC *w = dc ? g_wdc[HDC_16(dc)] : NULL;
    if (w && w->kind == WDC_PLAIN) wdc_drop(w);
}

static void wdc_drop(WinDC *w)
{
    u16 id = HDC_16(w->dc);
    if (--w->refs > 0) return;
    if (g_wdc[id] == w) g_wdc[id] = NULL;
    free(w);
}

/* temporary logical surface over a real area of a natively painted window;
   initialised from the real pixels (which came from an exact upscale, so
   sampling every S-th pixel recovers them) */
static WinDC *temp_surface(HWND h, HDC real, const RECT *rreal)
{
    HDC sdc = GetDC(NULL), mem;
    HBITMAP bmp;
    WinDC *w;
    RECT l;
    int lw, lh;
    l.left = zfloor(rreal->left);
    l.top = zfloor(rreal->top);
    l.right = zceil(rreal->right);
    l.bottom = zceil(rreal->bottom);
    lw = l.right - l.left; lh = l.bottom - l.top;
    if (lw < 1) lw = 1;
    if (lh < 1) lh = 1;
    mem = CreateCompatibleDC(sdc);
    bmp = CreateCompatibleBitmap(sdc, lw, lh);
    ReleaseDC(NULL, sdc);
    w = wdc_new(h, mem, WDC_TEMP);
    w->tbmp = bmp;
    w->toldbmp = SelectObject(mem, bmp);
    w->tlog = l;
    w->treal = *rreal;
    w->target = real;
    if (real) {
        SetStretchBltMode(mem, COLORONCOLOR);
        StretchBlt(mem, 0, 0, lw, lh, real, l.left * g_zoom, l.top * g_zoom, lw * g_zoom, lh * g_zoom, SRCCOPY);
    }
    /* the game draws in window-relative logical coordinates */
    SetWindowOrgEx(mem, l.left, l.top, NULL);
    return w;
}

static void temp_finish(WinDC *w, int all)
{
    HDC real = w->target, own = NULL;
    RECT d = all ? (RECT){ 0, 0, w->tlog.right - w->tlog.left, w->tlog.bottom - w->tlog.top } : w->dirty;
    if (!rect_empty(&d)) {
        if (!real) real = own = GetDC(w->hwnd);
        if (real) {
            SetWindowOrgEx(w->dc, 0, 0, NULL);
            if (!scaled_blit(real, w->dc, w->tlog.right - w->tlog.left, w->tlog.bottom - w->tlog.top, &d,
                             -w->tlog.left, -w->tlog.top, 0, 0)) {
                SetStretchBltMode(real, COLORONCOLOR);
                StretchBlt(real, (w->tlog.left + d.left) * g_zoom, (w->tlog.top + d.top) * g_zoom,
                           (d.right - d.left) * g_zoom, (d.bottom - d.top) * g_zoom,
                           w->dc, d.left, d.top, d.right - d.left, d.bottom - d.top, SRCCOPY);
            }
        }
        if (own) ReleaseDC(w->hwnd, own);
    }
    hmap_remove(&g_gdi_map, w->dc);
    SelectObject(w->dc, w->toldbmp);
    DeleteObject(w->tbmp);
    DeleteDC(w->dc);
    w->refs = 1;
    wdc_drop(w);
}

typedef struct DCSession { HDC dc; int level; WinDC *w; } DCSession;
static DCSession g_sess[64];
static int g_nsess;

static void sess_push(HDC dc, int level, WinDC *w)
{
    if (g_nsess < 64) { g_sess[g_nsess].dc = dc; g_sess[g_nsess].level = level; g_sess[g_nsess].w = w; g_nsess++; }
}

static DCSession *sess_find(HDC dc)
{
    int i;
    for (i = g_nsess - 1; i >= 0; i--) if (g_sess[i].dc == dc) return &g_sess[i];
    return NULL;
}

static void sess_remove(DCSession *s)
{
    int i = (int)(s - g_sess);
    memmove(&g_sess[i], &g_sess[i + 1], (g_nsess - i - 1) * sizeof *s);
    g_nsess--;
}

HDC zoom_getdc(HWND h)
{
    if (!h || (g_zoom == 1 && !zoom_window_has_surface(h))) {
        HDC dc = GetDC(h);
        if (dc && h) wdc_new(h, dc, WDC_PLAIN);
        return dc;
    }
    if (zoom_window_has_surface(h)) {
        ZSurf *zs = surf_get(h, 1);
        WinDC *w;
        int level;
        if (!zs) return GetDC(h);
        level = SaveDC(zs->dc);
        w = wdc_new(h, zs->dc, WDC_SURFACE);
        w->zs = zs;
        zs->sessions++;
        sess_push(zs->dc, level, w);
        return zs->dc;
    } else {
        RECT cr;
        HDC real = GetDC(h);
        WinDC *w;
        GetClientRect(h, &cr);
        w = temp_surface(h, real, &cr);
        ReleaseDC(h, real);
        w->target = NULL;          /* copied back through a fresh DC */
        sess_push(w->dc, 0, w);
        return w->dc;
    }
}

int zoom_releasedc(HWND h, HDC dc)
{
    DCSession *s = sess_find(dc);
    if (!s) {
        WinDC *w = dc ? g_wdc[HDC_16(dc)] : NULL;
        if (w && w->kind == WDC_PLAIN) wdc_drop(w);
        return ReleaseDC(h, dc);
    }
    if (s->w->kind == WDC_SURFACE) {
        ZSurf *zs = s->w->zs;
        RestoreDC(zs->dc, s->level);
        wdc_drop(s->w);
        sess_remove(s);
        zs->sessions--;
        surf_flush(zs, NULL);
    } else {
        WinDC *w = s->w;
        sess_remove(s);
        temp_finish(w, 0);
    }
    return 1;
}

/* BeginPaint: ps receives the real paint data, *lpaint the logical paint rect */
HDC zoom_beginpaint(HWND h, PAINTSTRUCT *ps, RECT *lpaint)
{
    HDC real = BeginPaint(h, ps);
    if (!real) return NULL;
    if (g_zoom == 1 && !zoom_window_has_surface(h)) {
        wdc_new(h, real, WDC_PLAIN);
        *lpaint = ps->rcPaint;
        return real;
    }
    lpaint->left = zfloor(ps->rcPaint.left);
    lpaint->top = zfloor(ps->rcPaint.top);
    lpaint->right = zceil(ps->rcPaint.right);
    lpaint->bottom = zceil(ps->rcPaint.bottom);
    if (zoom_window_has_surface(h)) {
        ZSurf *zs = surf_get(h, 1);
        WinDC *w;
        int level;
        if (!zs) return real;
        level = SaveDC(zs->dc);
        IntersectClipRect(zs->dc, lpaint->left, lpaint->top, lpaint->right, lpaint->bottom);
        w = wdc_new(h, zs->dc, WDC_SURFACE);
        w->zs = zs;
        zs->sessions++;
        zs->painting++;
        sess_push(zs->dc, level, w);
        return zs->dc;
    } else {
        WinDC *w = temp_surface(h, real, &ps->rcPaint);
        sess_push(w->dc, 0, w);
        IntersectClipRect(w->dc, lpaint->left, lpaint->top, lpaint->right, lpaint->bottom);
        return w->dc;
    }
}

void zoom_endpaint(HWND h, HDC dc, PAINTSTRUCT *ps)
{
    DCSession *s = dc ? sess_find(dc) : NULL;
    if (!s) {
        WinDC *w = dc ? g_wdc[HDC_16(dc)] : NULL;
        if (w && w->kind == WDC_PLAIN) wdc_drop(w);
        EndPaint(h, ps);
        return;
    }
    if (s->w->kind == WDC_SURFACE) {
        ZSurf *zs = s->w->zs;
        RECT l;
        l.left = zfloor(ps->rcPaint.left);
        l.top = zfloor(ps->rcPaint.top);
        l.right = zceil(ps->rcPaint.right);
        l.bottom = zceil(ps->rcPaint.bottom);
        RestoreDC(zs->dc, s->level);
        wdc_drop(s->w);
        sess_remove(s);
        zs->sessions--;
        if (zs->painting > 0) zs->painting--;
        if (zoom_upscale_mode()) {
            /* upscaled: the pixels around the update area change too, which
               the paint DC would clip away - go through a window DC */
            rect_union_into(&zs->dirty, &l);
            surf_flush(zs, NULL);
        } else {
            /* the update area goes through the paint DC; anything drawn
               elsewhere (other DCs, earlier GetDC drawing) through a window DC */
            blit_up(ps->hdc, zs->dc, zs->w, zs->h, &l, 0, 0, surf_dimmed(h));
            hud_overlay(h, ps->hdc);
            if (!rect_empty(&zs->dirty) &&
                (zs->dirty.left < l.left || zs->dirty.top < l.top || zs->dirty.right > l.right || zs->dirty.bottom > l.bottom))
                surf_flush(zs, NULL);
            else
                SetRectEmpty(&zs->dirty);
        }
    } else {
        WinDC *w = s->w;
        sess_remove(s);
        w->target = ps->hdc;
        temp_finish(w, 1);
    }
    EndPaint(h, ps);
}

/* WM_ERASEBKGND sent by BeginPaint: redirect to the backing surface */
HDC zoom_erase_begin(HWND h, HDC real, int *level)
{
    ZSurf *zs;
    RECT rc, l;
    if (!zoom_window_has_surface(h) || !(zs = surf_get(h, 1))) return NULL;
    if (GetClipBox(real, &rc) == ERROR) GetClientRect(h, &rc);
    l.left = zfloor(rc.left); l.top = zfloor(rc.top);
    l.right = zceil(rc.right); l.bottom = zceil(rc.bottom);
    *level = SaveDC(zs->dc);
    IntersectClipRect(zs->dc, l.left, l.top, l.right, l.bottom);
    wdc_new(h, zs->dc, WDC_SURFACE)->zs = zs;
    if (rect_empty(&zs->dirty)) zs->dirty_since = GetTickCount();
    rect_union_into(&zs->dirty, &l);
    zs->painting++;
    return zs->dc;
}

void zoom_erase_end(HWND h, HDC mem, int level)
{
    WinDC *w = g_wdc[HDC_16(mem)];
    ZSurf *zs = w ? w->zs : NULL;
    (void)h;
    RestoreDC(mem, level);
    if (zs && zs->painting > 0) zs->painting--;
    if (w) wdc_drop(w);
    /* the dirty area is copied when the paint session ends */
}

/* WM_DRAWITEM into a native control: temporary surface over the item */
HDC zoom_drawitem_begin(HWND h, HDC real, RECT *item_real)
{
    WinDC *w;
    if (g_zoom == 1) return NULL;
    w = temp_surface(h, real, item_real);
    sess_push(w->dc, 0, w);
    return w->dc;
}

void zoom_drawitem_end(HDC mem)
{
    DCSession *s = sess_find(mem);
    WinDC *w;
    if (!s) return;
    w = s->w;
    sess_remove(s);
    temp_finish(w, 1);
}

/* ScrollWindow on a window with a surface: scroll the bitmap, then the real
   window (whose pixels match the bitmap after a flush) */
void zoom_scroll(HWND h, int dx, int dy, const RECT *lrc, const RECT *lclip)
{
    ZSurf *zs = zoom_window_has_surface(h) ? surf_get(h, 1) : NULL;
    RECT r, c;
    if (!zs) { ScrollWindow(h, dx, dy, lrc, lclip); return; }
    {
        /* the combat bars must not be scrolled along with the map */
        RECT br;
        if (hud_take_bars(h, &br)) rect_union_into(&zs->dirty, &br);
    }
    surf_flush(zs, NULL);
    ScrollDC(zs->dc, dx, dy, lrc, lclip, NULL, NULL);
    if (zoom_upscale_mode()) {
        /* pixels that moved to the edge lost the neighbours their upscaled
           look was computed with */
        int m = pixel_scale_margin(g_zoom, zoom_upscale_mode());
        RECT a = lrc ? *lrc : (RECT){ 0, 0, zs->w, zs->h }, e;
        if (dx < 0) { e = a; e.right = e.left + m; rect_union_into(&zs->dirty, &e); }
        if (dx > 0) { e = a; e.left = e.right - m; rect_union_into(&zs->dirty, &e); }
        if (dy < 0) { e = a; e.bottom = e.top + m; rect_union_into(&zs->dirty, &e); }
        if (dy > 0) { e = a; e.top = e.bottom - m; rect_union_into(&zs->dirty, &e); }
        if (!rect_empty(&zs->dirty)) zs->dirty_since = GetTickCount();
    }
    if (lrc) { r.left = lrc->left * g_zoom; r.top = lrc->top * g_zoom; r.right = lrc->right * g_zoom; r.bottom = lrc->bottom * g_zoom; }
    if (lclip) { c.left = lclip->left * g_zoom; c.top = lclip->top * g_zoom; c.right = lclip->right * g_zoom; c.bottom = lclip->bottom * g_zoom; }
    ScrollWindow(h, dx * g_zoom, dy * g_zoom, lrc ? &r : NULL, lclip ? &c : NULL);
}

/* ---- cursors and icons ------------------------------------------------------------------ */

typedef struct { HICON src; HICON dst; int s, mode; } ScaledIcon;
static ScaledIcon g_sicons[256];
static int g_nsicons;

/* colour icon through the upscaler: transparent pixels become one
   "colour" (alpha 0) so outlines are smoothed against the background too.
   Returns 0 (caller stretches plainly) for icons with inverting pixels. */
static int scale_icon_smooth(const ICONINFO *ii, int w, int h, int s, ICONINFO *ni, HDC sdc)
{
    BITMAPINFO bi;
    u32 *col, *msk, *in, *out, *ocol, *omsk;
    int i, n = w * h, on = w * s * h * s, ok = 0;
    void *cbits = NULL;
    if (!zoom_upscale_mode() || !ii->hbmColor || w <= 0 || h <= 0 || w > 256 || h > 256) return 0;
    col = malloc((size_t)n * 4); msk = malloc((size_t)n * 4); in = malloc((size_t)n * 4);
    out = malloc((size_t)on * 4); omsk = malloc((size_t)on * 4);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    if (!col || !msk || !in || !out || !omsk ||
        GetDIBits(sdc, ii->hbmColor, 0, h, col, &bi, DIB_RGB_COLORS) != h ||
        GetDIBits(sdc, ii->hbmMask, 0, h, msk, &bi, DIB_RGB_COLORS) != h) goto done;
    for (i = 0; i < n; i++) {
        int transparent = (msk[i] & 0xFFFFFF) != 0;
        if (transparent && (col[i] & 0xFFFFFF)) goto done;       /* inverting pixel */
        in[i] = transparent ? 0 : (0xFF000000 | (col[i] & 0xFFFFFF));
    }
    /* the sharp variant keeps every pixel either opaque or transparent */
    if (!zoom_scale_pixels(in, w, h, out, s, SCALE_SHARP, 0xFFFFFFFF)) goto done;
    bi.bmiHeader.biWidth = w * s;
    bi.bmiHeader.biHeight = -h * s;
    ni->hbmColor = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &cbits, NULL, 0);
    if (!ni->hbmColor || !cbits) goto done;
    ocol = (u32 *)cbits;
    for (i = 0; i < on; i++) {
        int opaque = (out[i] >> 24) >= 128;
        ocol[i] = opaque ? (out[i] & 0xFFFFFF) : 0;
        omsk[i] = opaque ? 0 : 0xFFFFFF;
    }
    ni->hbmMask = CreateBitmap(w * s, h * s, 1, 1, NULL);
    if (!ni->hbmMask || SetDIBits(sdc, ni->hbmMask, 0, h * s, omsk, &bi, DIB_RGB_COLORS) != h * s) goto done;
    ok = 1;
done:
    if (!ok) {
        if (ni->hbmColor) DeleteObject(ni->hbmColor);
        if (ni->hbmMask) DeleteObject(ni->hbmMask);
        ni->hbmColor = ni->hbmMask = NULL;
    }
    free(col); free(msk); free(in); free(out); free(omsk);
    return ok;
}

static HICON scale_icon(HICON src, int s, BOOL cursor)
{
    ICONINFO ii, ni;
    BITMAP bm;
    HDC sdc, a, b;
    HICON r;
    int w, h, mono;
    if (!GetIconInfo(src, &ii)) return NULL;
    GetObjectA(ii.hbmMask, sizeof bm, &bm);
    mono = !ii.hbmColor;
    w = bm.bmWidth;
    h = mono ? bm.bmHeight / 2 : bm.bmHeight;
    memset(&ni, 0, sizeof ni);
    ni.fIcon = !cursor;
    ni.xHotspot = ii.xHotspot * s;
    ni.yHotspot = ii.yHotspot * s;
    sdc = GetDC(NULL);
    if (scale_icon_smooth(&ii, w, h, s, &ni, sdc)) {
        ReleaseDC(NULL, sdc);
        r = CreateIconIndirect(&ni);
        DeleteObject(ni.hbmMask);
        DeleteObject(ni.hbmColor);
        DeleteObject(ii.hbmMask);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        return r;
    }
    a = CreateCompatibleDC(sdc);
    b = CreateCompatibleDC(sdc);
    ni.hbmMask = CreateBitmap(w * s, (mono ? h * 2 : h) * s, 1, 1, NULL);
    SelectObject(a, ii.hbmMask);
    SelectObject(b, ni.hbmMask);
    StretchBlt(b, 0, 0, w * s, (mono ? h * 2 : h) * s, a, 0, 0, w, mono ? h * 2 : h, SRCCOPY);
    if (!mono) {
        ni.hbmColor = CreateCompatibleBitmap(sdc, w * s, h * s);
        SelectObject(a, ii.hbmColor);
        SelectObject(b, ni.hbmColor);
        SetStretchBltMode(b, COLORONCOLOR);
        StretchBlt(b, 0, 0, w * s, h * s, a, 0, 0, w, h, SRCCOPY);
    }
    DeleteDC(a);
    DeleteDC(b);
    ReleaseDC(NULL, sdc);
    r = CreateIconIndirect(&ni);
    DeleteObject(ni.hbmMask);
    if (ni.hbmColor) DeleteObject(ni.hbmColor);
    DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    return r;
}

static HCURSOR g_syscur[32];
static int g_nsyscur;

/* cursors loaded from the system stay at their normal size */
void zoom_mark_system_cursor(HCURSOR h)
{
    int i;
    if (!h) return;
    for (i = 0; i < g_nsyscur; i++) if (g_syscur[i] == h) return;
    if (g_nsyscur < 32) g_syscur[g_nsyscur++] = h;
}

HICON zoom_icon_orig(HICON scaled)
{
    int i;
    for (i = 0; i < g_nsicons; i++) if (g_sicons[i].dst == scaled) return g_sicons[i].src;
    return scaled;
}

/* scaled version of a cursor/icon created from the game's data (cached) */
HICON zoom_icon(HICON src, BOOL cursor)
{
    int i;
    if (g_zoom == 1 || !src) return src;
    for (i = 0; i < g_nsyscur; i++) if (g_syscur[i] == src) return src;
    for (i = 0; i < g_nsicons; i++)
        if (g_sicons[i].src == src && g_sicons[i].s == g_zoom && g_sicons[i].mode == zoom_upscale_mode())
            return g_sicons[i].dst;
    {
        HICON d = scale_icon(src, g_zoom, cursor);
        if (!d) return src;
        if (g_nsicons == 256) {
            DestroyIcon(g_sicons[0].dst);
            memmove(&g_sicons[0], &g_sicons[1], 255 * sizeof g_sicons[0]);
            g_nsicons--;
        }
        g_sicons[g_nsicons].src = src;
        g_sicons[g_nsicons].dst = d;
        g_sicons[g_nsicons].s = g_zoom;
        g_sicons[g_nsicons].mode = zoom_upscale_mode();
        g_nsicons++;
        return d;
    }
}

/* the game destroyed an icon it made: drop its scaled copies */
void zoom_icon_destroyed(HICON src)
{
    int i;
    for (i = 0; i < g_nsicons; ) {
        if (g_sicons[i].src == src) {
            if (GetCursor() != g_sicons[i].dst) DestroyIcon(g_sicons[i].dst);
            g_sicons[i] = g_sicons[--g_nsicons];
        } else i++;
    }
}

/* ---- fonts for native controls -------------------------------------------------------------- */

typedef struct { HFONT src; HFONT dst; int s; } ScaledFont;
static ScaledFont g_sfonts[64];
static int g_nsfonts;

/* TrueType stand-ins for the raster fonts, which cannot be scaled smoothly */
static const char *tt_face(const char *face, LONG *weight)
{
    if (!face[0] || !_stricmp(face, "System")) { if (*weight < FW_BOLD) *weight = FW_BOLD; return "Arial"; }
    if (!_stricmp(face, "Helv") || !_stricmp(face, "MS Sans Serif")) return "Microsoft Sans Serif";
    if (!_stricmp(face, "Tms Rmn") || !_stricmp(face, "MS Serif")) return "Times New Roman";
    if (!_stricmp(face, "Courier") || !_stricmp(face, "Fixedsys") || !_stricmp(face, "Terminal")) return "Courier New";
    return face;
}

HFONT zoom_font(HFONT src)
{
    LOGFONTA lf;
    HFONT d;
    int i;
    if (g_zoom == 1) return src;
    if (!src) src = (HFONT)GetStockObject(SYSTEM_FONT);
    for (i = 0; i < g_nsfonts; i++)
        if (g_sfonts[i].src == src && g_sfonts[i].s == g_zoom) return g_sfonts[i].dst;
    if (!GetObjectA(src, sizeof lf, &lf)) return src;
    if (lf.lfHeight == 0) lf.lfHeight = 16;
    lf.lfHeight *= g_zoom;
    lf.lfWidth *= g_zoom;
    snprintf(lf.lfFaceName, sizeof lf.lfFaceName, "%s", tt_face(lf.lfFaceName, &lf.lfWeight));
    lf.lfOutPrecision = OUT_TT_PRECIS;
    d = CreateFontIndirectA(&lf);
    if (!d) return src;
    if (g_nsfonts == 64) { DeleteObject(g_sfonts[0].dst); memmove(&g_sfonts[0], &g_sfonts[1], 63 * sizeof g_sfonts[0]); g_nsfonts--; }
    g_sfonts[g_nsfonts].src = src;
    g_sfonts[g_nsfonts].dst = d;
    g_sfonts[g_nsfonts].s = g_zoom;
    g_nsfonts++;
    return d;
}

HFONT zoom_unscale_font(HFONT f)
{
    int i;
    for (i = 0; i < g_nsfonts; i++) if (g_sfonts[i].dst == f) return g_sfonts[i].src;
    return f;
}

/* dialog template fonts: point size, weight and face for the zoomed dialog */
void zoom_dialog_font(int *points, int *weight, char *face, int facelen, int *had_font)
{
    LONG w = *weight;
    if (g_zoom == 1) return;
    if (!*had_font) {
        /* no DS_SETFONT: the bold System font; Arial Bold 10 pt has nearly
           the same dialog base units and looks alike */
        *points = 10;
        *weight = FW_BOLD;
        snprintf(face, facelen, "Arial");
        *had_font = 1;
    } else {
        char tmp[64];
        snprintf(tmp, sizeof tmp, "%s", face);
        snprintf(face, facelen, "%s", tt_face(tmp, &w));
        *weight = (int)w;
    }
    *points *= g_zoom;
}
