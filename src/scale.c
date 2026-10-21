/*
 * scale.c - edge-aware magnification of pixel art.
 *
 * MMPX ("MMPX Style-Preserving Pixel Art Magnification", Morgan McGuire and
 * Mara Gagiu, Journal of Computer Graphics Techniques 10(2), 2021) doubles an
 * image by deciding, for each pixel, which neighbouring colour each of its
 * four sub-pixels takes.  It smooths diagonal edges and curves but never
 * invents colours: every output pixel is a copy of an input pixel, so the
 * original palette, dithering and outlines are kept.
 *
 * Other factors are made from repeated doubling:
 *   sharp: double to the next power of two >= s, then (if that is larger)
 *          reduce by picking pixels - the palette stays exact;
 *   soft:  double to a power of two above s and reduce by averaging areas,
 *          which anti-aliases the smoothed edges.
 *
 * Pixels are 0xAARRGGBB; A is opacity (255 = opaque).  For screen images A is
 * simply 0 everywhere, which leaves the comparisons unchanged.
 *
 * All working memory is in a ScaleCtx, so separate threads can scale
 * separate bands of one image.
 */
#include <stdlib.h>
#include <string.h>
#include "scale.h"

struct ScaleCtx {
    u32 *buf[4];
    size_t cap[4];
};

ScaleCtx *scale_ctx_new(void) { return (ScaleCtx *)calloc(1, sizeof(ScaleCtx)); }

void scale_ctx_free(ScaleCtx *c)
{
    int i;
    if (!c) return;
    for (i = 0; i < 4; i++) free(c->buf[i]);
    free(c);
}

static u32 *ctx_buf(ScaleCtx *c, int i, size_t n)
{
    if (n > c->cap[i]) {
        free(c->buf[i]);
        c->buf[i] = (u32 *)malloc(n * sizeof(u32));
        c->cap[i] = c->buf[i] ? n : 0;
    }
    return c->buf[i];
}

enum { B_PAD, B_A, B_B, B_OUT };

static inline u32 luma(u32 c)
{
    return (((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF) + 1) * (256 - (c >> 24));
}

static inline int all_eq2(u32 b, u32 a0, u32 a1) { return ((b ^ a0) | (b ^ a1)) == 0; }
static inline int all_eq3(u32 b, u32 a0, u32 a1, u32 a2) { return ((b ^ a0) | (b ^ a1) | (b ^ a2)) == 0; }
static inline int all_eq4(u32 b, u32 a0, u32 a1, u32 a2, u32 a3) { return ((b ^ a0) | (b ^ a1) | (b ^ a2) | (b ^ a3)) == 0; }
static inline int any_eq3(u32 b, u32 a0, u32 a1, u32 a2) { return b == a0 || b == a1 || b == a2; }
static inline int none_eq2(u32 b, u32 a0, u32 a1) { return b != a0 && b != a1; }
static inline int none_eq4(u32 b, u32 a0, u32 a1, u32 a2, u32 a3) { return b != a0 && b != a1 && b != a2 && b != a3; }

#define PAD 3

/* one MMPX doubling: src w x h (pitch in pixels) -> dst 2w x 2h */
static int mmpx2x(ScaleCtx *c, const u32 *src, int w, int h, int spitch, u32 *dst, int dpitch, u32 mask)
{
    int pw = w + 2 * PAD, ph = h + 2 * PAD, x, y;
    u32 *p = ctx_buf(c, B_PAD, (size_t)pw * ph);
    if (!p) return 0;
    /* copy with a replicated border, so neighbours never need clamping */
    for (y = 0; y < ph; y++) {
        int sy = y - PAD < 0 ? 0 : y - PAD >= h ? h - 1 : y - PAD;
        const u32 *s = src + (size_t)sy * spitch;
        u32 *d = p + (size_t)y * pw;
        for (x = 0; x < PAD; x++) d[x] = s[0] & mask;
        for (x = 0; x < w; x++) d[PAD + x] = s[x] & mask;
        for (x = 0; x < PAD; x++) d[PAD + w + x] = s[w - 1] & mask;
    }
    for (y = 0; y < h; y++) {
        const u32 *row = p + (size_t)(y + PAD) * pw + PAD;
        u32 *o0 = dst + (size_t)(2 * y) * dpitch, *o1 = o0 + dpitch;
        for (x = 0; x < w; x++) {
            const u32 *e = row + x;
#define SRC(dx, dy) e[(ptrdiff_t)(dy) * pw + (dx)]
            u32 A = SRC(-1, -1), B = SRC(0, -1), C = SRC(1, -1);
            u32 D = SRC(-1, 0), E = e[0], F = SRC(1, 0);
            u32 G = SRC(-1, 1), H = SRC(0, 1), I = SRC(1, 1);
            u32 J = E, K = E, L = E, M = E;
            if (((A ^ E) | (B ^ E) | (C ^ E) | (D ^ E) | (F ^ E) | (G ^ E) | (H ^ E) | (I ^ E)) != 0) {
                u32 P = SRC(0, -2), S = SRC(0, 2), Q = SRC(-2, 0), R = SRC(2, 0);
                u32 Bl = luma(B), Dl = luma(D), El = luma(E), Fl = luma(F), Hl = luma(H);

                /* 1:1 slope rules */
                if ((D == B && D != H && D != F) && (El >= Dl || E == A) && any_eq3(E, A, C, G) &&
                    ((El < Dl) || A != D || E != P || E != Q)) J = D;
                if ((B == F && B != D && B != H) && (El >= Bl || E == C) && any_eq3(E, A, C, I) &&
                    ((El < Bl) || C != B || E != P || E != R)) K = B;
                if ((H == D && H != F && H != B) && (El >= Hl || E == G) && any_eq3(E, A, G, I) &&
                    ((El < Hl) || G != H || E != S || E != Q)) L = H;
                if ((F == H && F != B && F != D) && (El >= Fl || E == I) && any_eq3(E, C, G, I) &&
                    ((El < Fl) || I != H || E != R || E != S)) M = F;

                /* intersection rules */
                if ((E != F && all_eq4(E, C, I, D, Q) && all_eq2(F, B, H)) && F != SRC(3, 0)) K = M = F;
                if ((E != D && all_eq4(E, A, G, F, R) && all_eq2(D, B, H)) && D != SRC(-3, 0)) J = L = D;
                if ((E != H && all_eq4(E, G, I, B, P) && all_eq2(H, D, F)) && H != SRC(0, 3)) L = M = H;
                if ((E != B && all_eq4(E, A, C, H, S) && all_eq2(B, D, F)) && B != SRC(0, -3)) J = K = B;

                /* triangle tip rules */
                if (Bl < El && all_eq4(E, G, H, I, S) && none_eq4(E, A, D, C, F)) J = K = B;
                if (Hl < El && all_eq4(E, A, B, C, P) && none_eq4(E, D, G, I, F)) L = M = H;
                if (Fl < El && all_eq4(E, A, D, G, Q) && none_eq4(E, B, C, I, H)) K = M = F;
                if (Dl < El && all_eq4(E, C, F, I, R) && none_eq4(E, B, A, G, H)) J = L = D;

                /* 2:1 slope rules */
                if (H != B) {
                    if (H != A && H != E && H != C) {
                        if (all_eq3(H, G, F, R) && none_eq2(H, D, SRC(2, -1))) L = M;
                        if (all_eq3(H, I, D, Q) && none_eq2(H, F, SRC(-2, -1))) M = L;
                    }
                    if (B != I && B != G && B != E) {
                        if (all_eq3(B, A, F, R) && none_eq2(B, D, SRC(2, 1))) J = K;
                        if (all_eq3(B, C, D, Q) && none_eq2(B, F, SRC(-2, 1))) K = J;
                    }
                }
                if (F != D) {
                    if (D != I && D != E && D != C) {
                        if (all_eq3(D, A, H, S) && none_eq2(D, B, SRC(1, 2))) J = L;
                        if (all_eq3(D, G, B, P) && none_eq2(D, H, SRC(1, -2))) L = J;
                    }
                    if (F != E && F != A && F != G) {
                        if (all_eq3(F, C, H, S) && none_eq2(F, B, SRC(-1, 2))) K = M;
                        if (all_eq3(F, I, B, P) && none_eq2(F, H, SRC(-1, -2))) M = K;
                    }
                }
            }
#undef SRC
            o0[2 * x] = J; o0[2 * x + 1] = K;
            o1[2 * x] = L; o1[2 * x + 1] = M;
        }
    }
    return 1;
}

/* f -> s per source pixel (f > s), keeping exact colours: each output pixel
   takes the intermediate pixel under its centre */
static void reduce_pick(const u32 *src, int spitch, int w, int h, int f, int s, u32 *dst, int dpitch)
{
    int x, y, ow = w * s, oh = h * s;
    for (y = 0; y < oh; y++) {
        int sy = (int)(((long)y * 2 + 1) * f / (2L * s));
        const u32 *r = src + (size_t)sy * spitch;
        u32 *d = dst + (size_t)y * dpitch;
        for (x = 0; x < ow; x++) d[x] = r[((long)x * 2 + 1) * f / (2L * s)];
    }
}

/* f -> s per source pixel (f > s) by area averaging (separable box filter);
   tmp holds (w*s) x (h*f) pixels */
static void reduce_area(const u32 *src, int spitch, int w, int h, int f, int s, u32 *dst, int dpitch, u32 *tmp)
{
    int sw = w * f, sh = h * f, dw = w * s, dh = h * s, x, y, i;
    if (f == 2 * s) {
        /* the usual case: plain 2x2 averages */
        for (y = 0; y < dh; y++) {
            const u32 *r0 = src + (size_t)(2 * y) * spitch, *r1 = r0 + spitch;
            u32 *d = dst + (size_t)y * dpitch;
            for (x = 0; x < dw; x++) {
                u32 a = r0[2 * x], b = r0[2 * x + 1], c = r1[2 * x], e = r1[2 * x + 1];
                u32 lo = (a & 0x00FF00FF) + (b & 0x00FF00FF) + (c & 0x00FF00FF) + (e & 0x00FF00FF) + 0x00020002;
                u32 hi = ((a >> 8) & 0x00FF00FF) + ((b >> 8) & 0x00FF00FF) + ((c >> 8) & 0x00FF00FF) +
                         ((e >> 8) & 0x00FF00FF) + 0x00020002;
                d[x] = ((lo >> 2) & 0x00FF00FF) | (((hi >> 2) & 0x00FF00FF) << 8);
            }
        }
        return;
    }
    for (y = 0; y < sh; y++) {
        const u32 *r = src + (size_t)y * spitch;
        u32 *t = tmp + (size_t)y * dw;
        for (x = 0; x < dw; x++) {
            long a0 = (long)x * sw, a1 = a0 + sw;       /* in 1/dw source pixel units */
            int j0 = (int)(a0 / dw), j1 = (int)((a1 + dw - 1) / dw);
            unsigned c0 = 0, c1 = 0, c2 = 0, c3 = 0;
            for (i = j0; i < j1; i++) {
                long lo = (long)i * dw > a0 ? (long)i * dw : a0;
                long hi = (long)(i + 1) * dw < a1 ? (long)(i + 1) * dw : a1;
                unsigned wt = (unsigned)(hi - lo);
                u32 c = r[i];
                c0 += (c & 0xFF) * wt; c1 += ((c >> 8) & 0xFF) * wt;
                c2 += ((c >> 16) & 0xFF) * wt; c3 += (c >> 24) * wt;
            }
            t[x] = ((c0 + sw / 2) / sw) | (((c1 + sw / 2) / sw) << 8) | (((c2 + sw / 2) / sw) << 16) |
                   (((c3 + sw / 2) / sw) << 24);
        }
    }
    for (y = 0; y < dh; y++) {
        long a0 = (long)y * sh, a1 = a0 + sh;
        int j0 = (int)(a0 / dh), j1 = (int)((a1 + dh - 1) / dh);
        u32 *d = dst + (size_t)y * dpitch;
        for (x = 0; x < dw; x++) {
            unsigned c0 = 0, c1 = 0, c2 = 0, c3 = 0;
            for (i = j0; i < j1; i++) {
                long lo = (long)i * dh > a0 ? (long)i * dh : a0;
                long hi = (long)(i + 1) * dh < a1 ? (long)(i + 1) * dh : a1;
                unsigned wt = (unsigned)(hi - lo);
                u32 c = tmp[(size_t)i * dw + x];
                c0 += (c & 0xFF) * wt; c1 += ((c >> 8) & 0xFF) * wt;
                c2 += ((c >> 16) & 0xFF) * wt; c3 += (c >> 24) * wt;
            }
            d[x] = ((c0 + sh / 2) / sh) | (((c1 + sh / 2) / sh) << 8) | (((c2 + sh / 2) / sh) << 16) |
                   (((c3 + sh / 2) / sh) << 24);
        }
    }
}

/* power-of-two intermediate factor used for s */
static int inter_factor(int s, int mode)
{
    int f = 2;
    while (f < s) f *= 2;
    if (mode == SCALE_SOFT && f == s && f < 8) f *= 2;
    return f;
}

int pixel_scale_margin(int s, int mode)
{
    int f = inter_factor(s, mode);
    return f <= 2 ? 3 : f <= 4 ? 5 : 6;
}

/* whole image: src w x h -> dst (w*s) x (h*s) */
int pixel_scale(ScaleCtx *c, const u32 *src, int w, int h, int spitch, u32 *dst, int dpitch, int s, int mode, u32 mask)
{
    int f, cw = w, ch = h, cpitch = spitch, k = 0;
    const u32 *cur = src;
    if (s < 2 || s > 8 || w <= 0 || h <= 0 || !c) return 0;
    f = inter_factor(s, mode);
    while (cw < w * f) {
        int last = cw * 2 == w * f;
        u32 *out;
        int opitch;
        if (last && f == s) { out = dst; opitch = dpitch; }
        else {
            out = ctx_buf(c, (k & 1) ? B_B : B_A, (size_t)cw * 2 * ch * 2);
            opitch = cw * 2;
            if (!out) return 0;
        }
        if (!mmpx2x(c, cur, cw, ch, cpitch, out, opitch, mask)) return 0;
        cur = out; cpitch = opitch; cw *= 2; ch *= 2; k++;
        mask = 0xFFFFFFFF;
    }
    if (f != s) {
        if (mode == SCALE_SOFT) {
            u32 *t = ctx_buf(c, (cur == c->buf[B_A]) ? B_B : B_A, (size_t)(w * s) * (h * f));
            if (!t) return 0;
            reduce_area(cur, cpitch, w, h, f, s, dst, dpitch, t);
        } else {
            reduce_pick(cur, cpitch, w, h, f, s, dst, dpitch);
        }
    }
    return 1;
}

/* rows y0..y1-1 of src (w x h): scales them, reading up to the margin above
   and below, into dst, whose first row receives output row y0*s - exactly
   as a whole-image scaling would produce them */
int pixel_scale_rows(ScaleCtx *c, const u32 *src, int w, int h, int spitch, int y0, int y1,
                     u32 *dst, int dpitch, int s, int mode, u32 mask)
{
    int m = pixel_scale_margin(s, mode), a = y0 - m, b = y1 + m, y;
    u32 *out;
    if (a < 0) a = 0;
    if (b > h) b = h;
    if (y0 >= y1) return 1;
    out = ctx_buf(c, B_OUT, (size_t)(w * s) * ((b - a) * s));
    if (!out) return 0;
    if (!pixel_scale(c, src + (size_t)a * spitch, w, b - a, spitch, out, w * s, s, mode, mask)) return 0;
    for (y = (y0 - a) * s; y < (y1 - a) * s; y++)
        memcpy(dst + (size_t)(y - (y0 - a) * s) * dpitch, out + (size_t)y * (w * s), (size_t)(w * s) * sizeof(u32));
    return 1;
}
