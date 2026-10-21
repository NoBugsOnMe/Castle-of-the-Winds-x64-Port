/*
 * mem.c - linear memory, LDT selectors, Win16 global and local heaps.
 */
#include "mem.h"
#include <stdlib.h>
#include <string.h>

void cpu_refresh_segs(void);
void emu_log(const char *fmt, ...);

#define LIN_START 0x10000u

/* ---- linear allocator ---------------------------------------------------- */

typedef struct { u32 base, size; } Range;
static Range *g_free;
static int g_nfree, g_capfree;

static void free_insert(int at, u32 base, u32 size)
{
    if (g_nfree == g_capfree) {
        g_capfree = g_capfree ? g_capfree * 2 : 64;
        g_free = realloc(g_free, g_capfree * sizeof(Range));
    }
    memmove(&g_free[at + 1], &g_free[at], (g_nfree - at) * sizeof(Range));
    g_free[at].base = base;
    g_free[at].size = size;
    g_nfree++;
}

static void free_remove(int at)
{
    memmove(&g_free[at], &g_free[at + 1], (g_nfree - at - 1) * sizeof(Range));
    g_nfree--;
}

static u32 round16(u32 v) { return (v + 15u) & ~15u; }

u32 lin_alloc(u32 size)
{
    int i;
    size = round16(size ? size : 16);
    for (i = 0; i < g_nfree; i++) {
        if (g_free[i].size >= size) {
            u32 b = g_free[i].base;
            g_free[i].base += size;
            g_free[i].size -= size;
            if (!g_free[i].size) free_remove(i);
            memset(g_mem + b, 0, size);
            return b;
        }
    }
    return 0;
}

void lin_free(u32 base, u32 size)
{
    int i;
    size = round16(size ? size : 16);
    for (i = 0; i < g_nfree && g_free[i].base < base; i++) ;
    free_insert(i, base, size);
    /* merge with next */
    if (i + 1 < g_nfree && g_free[i].base + g_free[i].size == g_free[i + 1].base) {
        g_free[i].size += g_free[i + 1].size;
        free_remove(i + 1);
    }
    /* merge with previous */
    if (i > 0 && g_free[i - 1].base + g_free[i - 1].size == g_free[i].base) {
        g_free[i - 1].size += g_free[i].size;
        free_remove(i);
    }
}

/* try to extend an allocation in place */
static int lin_grow(u32 base, u32 oldsize, u32 newsize)
{
    int i;
    u32 end;
    oldsize = round16(oldsize ? oldsize : 16);
    newsize = round16(newsize);
    if (newsize <= oldsize) return 1;
    end = base + oldsize;
    for (i = 0; i < g_nfree; i++) {
        if (g_free[i].base == end) {
            u32 need = newsize - oldsize;
            if (g_free[i].size < need) return 0;
            memset(g_mem + end, 0, need);
            g_free[i].base += need;
            g_free[i].size -= need;
            if (!g_free[i].size) free_remove(i);
            return 1;
        }
    }
    return 0;
}

u32 glob_free_space(void)
{
    u32 total = 0;
    int i;
    for (i = 0; i < g_nfree; i++) total += g_free[i].size;
    return total;
}

/* ---- selectors ----------------------------------------------------------- */

static u8 sel_used[LDT_ENTRIES];

u16 sel_alloc(int count)
{
    int i, j;
    for (i = 1; i + count <= LDT_ENTRIES; i++) {
        for (j = 0; j < count; j++) if (sel_used[i + j]) break;
        if (j == count) {
            for (j = 0; j < count; j++) {
                sel_used[i + j] = 1;
                memset(&g_ldt[i + j], 0, sizeof(Descriptor));
            }
            return INDEX_SEL(i);
        }
        i += j;
    }
    return 0;
}

void sel_free(u16 sel, int count)
{
    int i, idx = SEL_INDEX(sel);
    for (i = 0; i < count; i++) {
        sel_used[idx + i] = 0;
        memset(&g_ldt[idx + i], 0, sizeof(Descriptor));
    }
}

void sel_set(u16 sel, u32 base, u32 limit, int code)
{
    Descriptor *d = &g_ldt[SEL_INDEX(sel)];
    d->base = base;
    d->limit = limit;
    d->flags = DESC_PRESENT | (code ? DESC_CODE : DESC_DATA);
}

/* ---- global heap ----------------------------------------------------------- */

typedef struct {
    u8  used;
    u8  kind;
    u8  code;
    u16 flags;
    u16 lock;
    u16 tiles;
    u32 size;
    u32 base;
    u32 cap;
} GBlock;

static GBlock gb[LDT_ENTRIES];

static void set_tiles(int idx)
{
    GBlock *b = &gb[idx];
    int t;
    for (t = 0; t < b->tiles; t++) {
        u32 remain = b->size > (u32)t * 0x10000u ? b->size - (u32)t * 0x10000u : 0;
        u32 lim = remain >= 0x10000u ? 0x10000u : round16(remain ? remain : 1);
        if (lim > 0x10000u) lim = 0x10000u;
        sel_set(INDEX_SEL(idx + t), b->base + (u32)t * 0x10000u, lim - 1, b->code);
        g_ldt[idx + t].owner = (u16)INDEX_SEL(idx);
        if (b->tiles > 1) g_ldt[idx + t].flags |= DESC_HUGE;
    }
    g_ldt[idx].tiles = (u8)(b->tiles > 255 ? 255 : b->tiles);
}

void mem_init(void)
{
    g_mem = (u8 *)calloc(1, MEM_SIZE + 64);
    g_nfree = 0;
    free_insert(0, LIN_START, MEM_SIZE - LIN_START);
    memset(sel_used, 0, sizeof sel_used);
    sel_used[0] = 1;
    memset(gb, 0, sizeof gb);
}

static u16 make_handle(int idx)
{
    u16 sel = INDEX_SEL(idx);
    return (gb[idx].flags & G_MOVEABLE) ? (u16)(sel - 1) : sel;
}

u16 glob_alloc(u16 flags, u32 size, int kind, int code, u32 reserve)
{
    u32 tiles = (size + 0xFFFFu) >> 16;
    u32 cap;
    u32 base;
    u16 sel;
    int idx;
    if (tiles == 0) tiles = 1;
    if (size > 16u * 1024u * 1024u) return 0;
    cap = round16(size ? size : 16);
    if (reserve > cap) cap = round16(reserve);
    base = lin_alloc(cap);
    if (!base) return 0;
    sel = sel_alloc((int)tiles);
    if (!sel) { lin_free(base, cap); return 0; }
    idx = SEL_INDEX(sel);
    gb[idx].used = 1;
    gb[idx].kind = (u8)kind;
    gb[idx].code = (u8)code;
    gb[idx].flags = flags;
    gb[idx].lock = 0;
    gb[idx].tiles = (u16)tiles;
    gb[idx].size = size;
    gb[idx].base = base;
    gb[idx].cap = cap;
    set_tiles(idx);
    return make_handle(idx);
}

u16 sys_alloc(u32 size, int code)
{
    u16 h = glob_alloc(G_FIXED, size, GK_SYSTEM, code, 0);
    return h;
}

static int block_index(u16 h)
{
    int idx = SEL_INDEX(h);
    if (!h || idx == 0 || idx >= LDT_ENTRIES || !gb[idx].used) {
        /* maybe a tile selector of a huge block */
        if (h && idx > 0 && idx < LDT_ENTRIES && sel_used[idx] && g_ldt[idx].owner) {
            int o = SEL_INDEX(g_ldt[idx].owner);
            if (gb[o].used) return o;
        }
        return -1;
    }
    return idx;
}

u16 glob_sel(u16 h)
{
    int idx = block_index(h);
    return idx < 0 ? 0 : INDEX_SEL(idx);
}

u16 glob_handle(u16 sel)
{
    int idx = block_index(sel);
    return idx < 0 ? 0 : make_handle(idx);
}

u16 glob_flags(u16 h)
{
    int idx = block_index(h);
    return idx < 0 ? 0 : gb[idx].flags;
}

u16 glob_free(u16 h)
{
    int idx = block_index(h);
    if (idx < 0) return h;
    lin_free(gb[idx].base, gb[idx].cap);
    sel_free(INDEX_SEL(idx), gb[idx].tiles);
    memset(&gb[idx], 0, sizeof(GBlock));
    cpu_refresh_segs();
    return 0;
}

u32 glob_size(u16 h)
{
    int idx = block_index(h);
    if (idx < 0) return 0;
    return round16(gb[idx].size);
}

u16 glob_lock(u16 h)
{
    int idx = block_index(h);
    if (idx < 0) return 0;
    if (gb[idx].lock < 0xFFFF) gb[idx].lock++;
    return INDEX_SEL(idx);
}

int glob_unlock(u16 h)
{
    int idx = block_index(h);
    if (idx < 0) return 0;
    if (gb[idx].lock) gb[idx].lock--;
    return gb[idx].lock != 0;
}

u16 glob_realloc(u16 h, u32 size, u16 flags)
{
    int idx = block_index(h);
    GBlock *b;
    u32 newtiles;
    if (idx < 0) return 0;
    b = &gb[idx];
    if (flags & G_MODIFY) {
        if (flags & G_MOVEABLE) b->flags |= G_MOVEABLE;
        b->flags = (u16)((b->flags & ~G_DISCARDABLE) | (flags & G_DISCARDABLE));
        return make_handle(idx);
    }
    if (size == 0) size = 16;
    newtiles = (size + 0xFFFFu) >> 16;
    if (newtiles == 0) newtiles = 1;

    /* make sure we have enough selectors */
    if (newtiles > b->tiles) {
        u32 t;
        int ok = 1;
        for (t = b->tiles; t < newtiles; t++) {
            if (idx + (int)t >= LDT_ENTRIES || sel_used[idx + t]) { ok = 0; break; }
        }
        if (ok) {
            for (t = b->tiles; t < newtiles; t++) sel_used[idx + t] = 1;
        } else {
            /* move the block to a new selector range */
            u16 nsel = sel_alloc((int)newtiles);
            int nidx;
            if (!nsel) return 0;
            nidx = SEL_INDEX(nsel);
            gb[nidx] = *b;
            sel_free(INDEX_SEL(idx), b->tiles);
            memset(b, 0, sizeof(GBlock));
            idx = nidx;
            b = &gb[idx];
        }
    } else if (newtiles < b->tiles) {
        sel_free(INDEX_SEL(idx + (int)newtiles), b->tiles - (int)newtiles);
    }

    if (size > b->cap) {
        if (!lin_grow(b->base, b->cap, size)) {
            u32 ncap = round16(size + size / 4);
            u32 nbase = lin_alloc(ncap);
            if (!nbase) return 0;
            memcpy(g_mem + nbase, g_mem + b->base, b->size < size ? b->size : size);
            lin_free(b->base, b->cap);
            b->base = nbase;
            b->cap = ncap;
        } else {
            b->cap = round16(size);
        }
    }
    if (size > b->size) memset(g_mem + b->base + b->size, 0, size - b->size);
    b->size = size;
    b->tiles = (u16)newtiles;
    set_tiles(idx);
    cpu_refresh_segs();
    return make_handle(idx);
}

/* ---- local heap ------------------------------------------------------------- */

typedef struct {
    u16 off;     /* data offset */
    u16 size;    /* requested size */
    u16 cap;     /* usable capacity */
    u16 hent;    /* handle entry offset (moveable), 0 for fixed */
    u16 flags;
} LBlk;

static struct {
    u16 sel;
    u16 start, end;
    LBlk *b;
    int n, cap;
    u16 *fh;          /* free handle entries */
    int nfh, capfh;
} lh;

#define LH_HDR 4      /* arena header gap before each block */

u16 local_heap_sel(void) { return lh.sel; }

void local_init(u16 sel, u16 start, u16 end)
{
    lh.sel = sel;
    lh.start = (u16)((start + 3) & ~3);
    lh.end = end;
    lh.n = 0;
    lh.nfh = 0;
}

static u16 r4(u32 v) { return (u16)((v + 3) & ~3u); }

static int lblk_find_off(u16 off)
{
    int lo = 0, hi = lh.n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (lh.b[mid].off == off) return mid;
        if (lh.b[mid].off < off) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static int lblk_find_handle(u16 h)
{
    int i;
    if ((h & 3) == 2) {
        u16 addr = rd16(lin(lh.sel, h));
        if (addr) {
            i = lblk_find_off(addr);
            if (i >= 0 && lh.b[i].hent == h) return i;
        }
        for (i = 0; i < lh.n; i++) if (lh.b[i].hent == h) return i;
        return -1;
    }
    return lblk_find_off(h);
}

static int heap_grow(u32 needed_end)
{
    u16 dg = lh.sel;
    u32 newsize;
    if (needed_end > 0xFFF0u) return 0;
    newsize = (needed_end + 0x3FFu) & ~0x3FFu;     /* grow in 1K steps */
    if (newsize > 0xFFF0u) newsize = 0xFFF0u;
    if (newsize < needed_end) return 0;
    if (!glob_realloc(glob_handle(dg), newsize, 0)) return 0;
    lh.end = (u16)newsize;
    return 1;
}

/* find space for cap bytes; returns data offset or 0 */
static u16 lspace(u16 cap, int *insert_at)
{
    u32 prev_end = lh.start;
    int i;
    for (i = 0; i <= lh.n; i++) {
        u32 next = (i < lh.n) ? (u32)lh.b[i].off - LH_HDR : lh.end;
        u32 dataoff = prev_end + LH_HDR;
        if (dataoff + cap <= next) { *insert_at = i; return (u16)dataoff; }
        if (i < lh.n) prev_end = (u32)lh.b[i].off + lh.b[i].cap;
    }
    /* grow the heap at the end */
    {
        u32 dataoff = prev_end + LH_HDR;
        if (heap_grow(dataoff + cap + 16)) { *insert_at = lh.n; return (u16)dataoff; }
    }
    return 0;
}

static u16 lalloc_raw(u16 size, u16 flags, u16 hent)
{
    int at;
    u16 cap = r4(size ? size : 2);
    u16 off = lspace(cap, &at);
    if (!off) return 0;
    if (lh.n == lh.cap) {
        lh.cap = lh.cap ? lh.cap * 2 : 128;
        lh.b = realloc(lh.b, lh.cap * sizeof(LBlk));
    }
    memmove(&lh.b[at + 1], &lh.b[at], (lh.n - at) * sizeof(LBlk));
    lh.b[at].off = off;
    lh.b[at].size = size;
    lh.b[at].cap = cap;
    lh.b[at].hent = hent;
    lh.b[at].flags = flags;
    lh.n++;
    memset(ptr16(lh.sel, off), 0, cap);
    /* arena header: mimic Windows (prev pointer | flags) */
    wr16(lin(lh.sel, (u16)(off - 4)), (u16)(off - 4));
    wr16(lin(lh.sel, (u16)(off - 2)), (u16)(off + cap));
    return off;
}

static u16 new_handle_entry(void)
{
    if (!lh.nfh) {
        /* allocate a new handle table: WORD count + 32 entries */
        int k;
        u16 t = lalloc_raw(2 + 32 * 4, L_FIXED, 0);
        if (!t) return 0;
        wr16(lin(lh.sel, t), 32);
        for (k = 31; k >= 0; k--) {
            u16 e = (u16)(t + 2 + k * 4);
            wr16(lin(lh.sel, e), 0);
            wr8(lin(lh.sel, (u16)(e + 2)), 0xFF);   /* free marker */
            wr8(lin(lh.sel, (u16)(e + 3)), 0);
            if (lh.nfh == lh.capfh) {
                lh.capfh = lh.capfh ? lh.capfh * 2 : 64;
                lh.fh = realloc(lh.fh, lh.capfh * sizeof(u16));
            }
            lh.fh[lh.nfh++] = e;
        }
    }
    return lh.fh[--lh.nfh];
}

static void free_handle_entry(u16 e)
{
    wr16(lin(lh.sel, e), 0);
    wr8(lin(lh.sel, (u16)(e + 2)), 0xFF);
    wr8(lin(lh.sel, (u16)(e + 3)), 0);
    if (lh.nfh == lh.capfh) {
        lh.capfh = lh.capfh ? lh.capfh * 2 : 64;
        lh.fh = realloc(lh.fh, lh.capfh * sizeof(u16));
    }
    lh.fh[lh.nfh++] = e;
}

u16 local_alloc(u16 flags, u16 size)
{
    if (!lh.sel) return 0;
    if (flags & L_MOVEABLE) {
        u16 e = new_handle_entry();
        u16 off;
        if (!e) return 0;
        if (size == 0) {
            /* discarded block */
            wr16(lin(lh.sel, e), 0);
            wr8(lin(lh.sel, (u16)(e + 2)), (u8)(flags >> 8));
            wr8(lin(lh.sel, (u16)(e + 3)), 0);
            return e;
        }
        off = lalloc_raw(size, flags, e);
        if (!off) { free_handle_entry(e); return 0; }
        wr16(lin(lh.sel, e), off);
        wr8(lin(lh.sel, (u16)(e + 2)), (u8)(flags >> 8));
        wr8(lin(lh.sel, (u16)(e + 3)), 0);
        return e;
    }
    return lalloc_raw(size, flags, 0);
}

u16 local_free(u16 h)
{
    int i;
    if (!h) return 0;
    if ((h & 3) == 2 && rd16(lin(lh.sel, h)) == 0) {
        /* discarded moveable block */
        if (rd8(lin(lh.sel, (u16)(h + 2))) != 0xFF) free_handle_entry(h);
        return 0;
    }
    i = lblk_find_handle(h);
    if (i < 0) return h;
    if (lh.b[i].hent) free_handle_entry(lh.b[i].hent);
    memmove(&lh.b[i], &lh.b[i + 1], (lh.n - i - 1) * sizeof(LBlk));
    lh.n--;
    return 0;
}

u16 local_lock(u16 h)
{
    if (!h) return 0;
    if ((h & 3) == 2) {
        u32 e = lin(lh.sel, h);
        u16 addr = rd16(e);
        u8 lk = rd8(e + 3);
        if (!addr) return 0;
        if (lk < 0xFF) wr8(e + 3, (u8)(lk + 1));
        return addr;
    }
    return h;
}

int local_unlock(u16 h)
{
    if ((h & 3) == 2) {
        u32 e = lin(lh.sel, h);
        u8 lk = rd8(e + 3);
        if (lk) { lk--; wr8(e + 3, lk); }
        return lk;
    }
    return 0;
}

u16 local_size(u16 h)
{
    int i = lblk_find_handle(h);
    if (i < 0) return 0;
    return lh.b[i].cap;
}

u16 local_realloc(u16 h, u16 size, u16 flags)
{
    int i;
    LBlk *b;
    if (flags & L_MODIFY) {
        if ((h & 3) == 2) wr8(lin(lh.sel, (u16)(h + 2)), (u8)(flags >> 8));
        return h;
    }
    if ((h & 3) == 2 && rd16(lin(lh.sel, h)) == 0) {
        /* re-allocating a discarded block */
        u16 off;
        if (!size) return h;
        off = lalloc_raw(size, L_MOVEABLE, h);
        if (!off) return 0;
        wr16(lin(lh.sel, h), off);
        return h;
    }
    i = lblk_find_handle(h);
    if (i < 0) return 0;
    b = &lh.b[i];
    if (size == 0) {
        if (b->hent) {
            /* discard */
            u16 e = b->hent;
            memmove(&lh.b[i], &lh.b[i + 1], (lh.n - i - 1) * sizeof(LBlk));
            lh.n--;
            wr16(lin(lh.sel, e), 0);
            return e;
        }
        size = 2;
    }
    if (r4(size) <= b->cap) {
        if (size > b->size && (flags & L_ZEROINIT))
            memset(ptr16(lh.sel, (u16)(b->off + b->size)), 0, b->cap - b->size);
        b->size = size;
        return h;
    }
    /* grow in place? */
    {
        u32 limit = (i + 1 < lh.n) ? (u32)lh.b[i + 1].off - LH_HDR : lh.end;
        u32 need = (u32)b->off + r4(size);
        if (need <= limit || (i + 1 == lh.n && heap_grow(need + 16))) {
            memset(ptr16(lh.sel, (u16)(b->off + b->cap)), 0, r4(size) - b->cap);
            b->cap = r4(size);
            b->size = size;
            wr16(lin(lh.sel, (u16)(b->off - 2)), (u16)(b->off + b->cap));
            return h;
        }
    }
    /* move */
    if (b->hent || (flags & L_MOVEABLE)) {
        u16 oldoff = b->off, oldcap = b->cap, hent = b->hent, bfl = b->flags;
        u16 noff;
        u8 *tmp = (u8 *)malloc(oldcap ? oldcap : 4);
        if (!tmp) return 0;
        memcpy(tmp, ptr16(lh.sel, oldoff), oldcap);
        /* remove old first so its space can be reused */
        memmove(&lh.b[i], &lh.b[i + 1], (lh.n - i - 1) * sizeof(LBlk));
        lh.n--;
        noff = lalloc_raw(size, bfl, hent);
        if (!noff) {
            /* restore */
            u16 back = lalloc_raw(oldcap, bfl, hent);
            if (back) memcpy(ptr16(lh.sel, back), tmp, oldcap);
            if (hent) wr16(lin(lh.sel, hent), back);
            free(tmp);
            return 0;
        }
        memcpy(ptr16(lh.sel, noff), tmp, oldcap);
        free(tmp);
        if (hent) { wr16(lin(lh.sel, hent), noff); return hent; }
        return noff;
    }
    return 0;
}

/* ---- misc -------------------------------------------------------------------- */

u32 stack_alloc(u16 size)
{
    size = (u16)((size + 1) & ~1);
    SP -= size;
    return ((u32)SS << 16) | SP;
}

size_t strlen16(u32 segptr)
{
    const char *p = (const char *)ptrseg(segptr);
    return p ? strlen(p) : 0;
}
