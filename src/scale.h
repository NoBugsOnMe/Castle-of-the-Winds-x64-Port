/* scale.h - edge-aware pixel art magnification (see scale.c) */
#ifndef SCALE_H
#define SCALE_H
#include <stdint.h>
#include <stddef.h>

#ifndef SCALE_NO_U32
typedef uint32_t u32;
#endif

enum { SCALE_SHARP = 1, SCALE_SOFT = 2 };

typedef struct ScaleCtx ScaleCtx;      /* working memory; one per thread */
ScaleCtx *scale_ctx_new(void);
void scale_ctx_free(ScaleCtx *c);

/* scales src (w x h pixels, 0xAARRGGBB, pitches in pixels) by s (2..8) into
   dst (w*s x h*s); mask is ANDed with every source pixel first (0x00FFFFFF
   ignores alpha); returns 0 if out of memory */
int pixel_scale(ScaleCtx *c, const u32 *src, int w, int h, int spitch, u32 *dst, int dpitch, int s, int mode, u32 mask);

/* the same for source rows y0..y1-1 only (dst receives output rows y0*s..) */
int pixel_scale_rows(ScaleCtx *c, const u32 *src, int w, int h, int spitch, int y0, int y1,
                     u32 *dst, int dpitch, int s, int mode, u32 mask);

/* source pixels needed around a region for it to match a whole-image scaling */
int pixel_scale_margin(int s, int mode);

#endif
