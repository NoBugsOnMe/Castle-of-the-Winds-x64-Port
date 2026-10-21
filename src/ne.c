/*
 * ne.c - load a Win16 NE executable into emulated memory.
 */
#include "ne.h"
#include "mem.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

void emu_log(const char *fmt, ...);

NEModule g_mod;

static u16 g16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 g32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static void pstr(const u8 *p, char *out, int max)
{
    int n = p[0];
    if (n >= max) n = max - 1;
    memcpy(out, p + 1, n);
    out[n] = 0;
}

u32 ne_entry(int ordinal)
{
    NEEntry *e;
    if (ordinal <= 0 || ordinal > g_mod.nent) return 0;
    e = &g_mod.ent[ordinal];
    if (!e->seg || e->seg == 0xFE) return 0;
    return ((u32)g_mod.segsel[e->seg - 1] << 16) | e->off;
}

int ne_is_module_handle(u16 h)
{
    if (!h) return 0;
    if (SEL_INDEX(h) == SEL_INDEX(g_mod.hModule)) return 1;
    if (SEL_INDEX(h) == SEL_INDEX(g_mod.hInstance)) return 1;
    return 0;
}

static int load_entries(const u8 *b, u16 ne)
{
    u16 off = g16(b + ne + 4), len = g16(b + ne + 6);
    const u8 *p = b + ne + off, *end = p + len;
    int ord = 1;
    /* first pass: count */
    int count = 0;
    {
        const u8 *q = p;
        while (q < end && q[0]) {
            int cnt = q[0], typ = q[1];
            q += 2;
            count += cnt;
            if (typ == 0) continue;
            q += cnt * (typ == 0xFF ? 6 : 3);
        }
    }
    g_mod.nent = count;
    g_mod.ent = calloc(count + 2, sizeof(NEEntry));
    while (p < end && p[0]) {
        int cnt = p[0], typ = p[1], k;
        p += 2;
        if (typ == 0) { ord += cnt; continue; }
        for (k = 0; k < cnt; k++) {
            if (typ == 0xFF) {
                g_mod.ent[ord].flags = p[0];
                g_mod.ent[ord].seg = p[3];
                g_mod.ent[ord].off = g16(p + 4);
                p += 6;
            } else {
                g_mod.ent[ord].flags = p[0];
                g_mod.ent[ord].seg = (u8)typ;
                g_mod.ent[ord].off = g16(p + 1);
                p += 3;
            }
            ord++;
        }
    }
    return 1;
}

static void load_resources(const u8 *b, u16 ne)
{
    u16 roff = g16(b + ne + 0x24), rnoff = g16(b + ne + 0x26);
    const u8 *p, *base;
    u16 shift;
    int cap = 0;
    if (roff == rnoff) return;
    base = b + ne + roff;
    shift = g16(base);
    p = base + 2;
    while (g16(p)) {
        u16 tid = g16(p), cnt = g16(p + 2), k;
        p += 8;
        for (k = 0; k < cnt; k++) {
            NERes *r;
            u16 rid;
            if (g_mod.nres == cap) {
                cap = cap ? cap * 2 : 256;
                g_mod.res = realloc(g_mod.res, cap * sizeof(NERes));
            }
            r = &g_mod.res[g_mod.nres++];
            memset(r, 0, sizeof *r);
            if (tid & 0x8000) r->type_id = tid & 0x7FFF;
            else pstr(base + tid, r->type_name, sizeof r->type_name);
            r->offset = (u32)g16(p) << shift;
            r->length = (u32)g16(p + 2) << shift;
            r->flags = g16(p + 4);
            rid = g16(p + 6);
            if (rid & 0x8000) r->name_id = rid & 0x7FFF;
            else pstr(base + rid, r->name, sizeof r->name);
            p += 12;
        }
    }
}

static int name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

int ne_find_resource_c(u16 type_id, const char *type_name, u16 name_id, const char *name)
{
    int i;
    for (i = 0; i < g_mod.nres; i++) {
        NERes *r = &g_mod.res[i];
        if (type_id) { if (r->type_id != type_id) continue; }
        else { if (r->type_id || !name_eq(r->type_name, type_name)) continue; }
        if (name_id) { if (r->name_id != name_id) continue; }
        else { if (r->name_id || !name_eq(r->name, name)) continue; }
        return i;
    }
    return -1;
}

static void decode_resname(u32 v, u16 *id, char *buf, int max)
{
    *id = 0;
    buf[0] = 0;
    if ((v >> 16) == 0) { *id = (u16)v; return; }
    {
        const char *s = (const char *)ptrseg(v);
        if (s[0] == '#') { *id = (u16)atoi(s + 1); return; }
        strncpy(buf, s, max - 1);
        buf[max - 1] = 0;
    }
}

int ne_find_resource(u32 type, u32 name)
{
    u16 tid, nid;
    char tname[64], nname[64];
    decode_resname(type, &tid, tname, sizeof tname);
    decode_resname(name, &nid, nname, sizeof nname);
    return ne_find_resource_c(tid, tname, nid, nname);
}

const u8 *ne_res_data(int idx, u32 *len)
{
    NERes *r;
    if (idx < 0 || idx >= g_mod.nres) return NULL;
    r = &g_mod.res[idx];
    if (r->offset + r->length > g_mod.imglen) {
        /* the last resource may be padded beyond the end of the file */
        if (r->offset >= g_mod.imglen) return NULL;
        if (len) *len = g_mod.imglen - r->offset;
        return g_mod.img + r->offset;
    }
    if (len) *len = r->length;
    return g_mod.img + r->offset;
}

static void apply_relocs(int segno, char *err, int errlen)
{
    const u8 *b = g_mod.img;
    u16 ne = g_mod.ne;
    u16 segtab = g16(b + ne + 0x22);
    const u8 *se = b + ne + segtab + 8 * (segno - 1);
    u32 fileoff = (u32)g16(se) << g16(b + ne + 0x32);
    u32 flen = g16(se + 2);
    u16 fl = g16(se + 4);
    u16 modref = g16(b + ne + 0x28), impname = g16(b + ne + 0x2A);
    u16 sel = g_mod.segsel[segno - 1];
    const u8 *rp;
    u16 n, i;
    (void)err; (void)errlen;
    if (!(fl & 0x100) || !fileoff) return;
    if (!flen) flen = 0x10000;
    rp = b + fileoff + flen;
    n = g16(rp);
    rp += 2;
    for (i = 0; i < n; i++, rp += 8) {
        u8 stype = rp[0], rflags = rp[1];
        u16 soff = g16(rp + 2), t1 = g16(rp + 4), t2 = g16(rp + 6);
        u16 tsel = 0, toff = 0;
        int additive = rflags & 4;
        int is_const = 0;
        switch (rflags & 3) {
        case 0: /* internal reference */
            if ((t1 & 0xFF) == 0xFF) {
                NEEntry *e = (t2 <= g_mod.nent) ? &g_mod.ent[t2] : NULL;
                if (!e || !e->seg) { emu_log("bad internal entry ordinal %d", t2); continue; }
                if (e->seg == 0xFE) { is_const = 1; toff = e->off; }
                else { tsel = g_mod.segsel[e->seg - 1]; toff = e->off; }
            } else {
                tsel = g_mod.segsel[(t1 & 0xFF) - 1];
                toff = t2;
            }
            break;
        case 1: case 2: { /* import by ordinal / name */
            char modname[32];
            int is_equ = 0;
            u16 equ = 0, ord = t2;
            pstr(b + ne + impname + g16(b + ne + modref + 2 * (t1 - 1)), modname, sizeof modname);
            if ((rflags & 3) == 2) {
                char fname[64];
                pstr(b + ne + impname + t2, fname, sizeof fname);
                emu_log("import by name %s.%s not supported", modname, fname);
                ord = 0xFFFF;
            }
            toff = thunk_offset_for(modname, ord, &is_equ, &equ);
            if (is_equ) { is_const = 1; toff = equ; }
            else tsel = g_thunk_sel;
            break;
        }
        default:
            continue; /* OS fixups (floating point) */
        }
        if (is_const) tsel = 0;
        if (additive) {
            u32 a = lin(sel, soff);
            switch (stype & 0x7F) {
            case 0: wr8(a, (u8)(rd8(a) + toff)); break;
            case 2: wr16(a, (u16)(rd16(a) + tsel)); break;
            case 3: wr16(a, (u16)(rd16(a) + toff)); wr16(a + 2, tsel); break;
            case 5: wr16(a, (u16)(rd16(a) + toff)); break;
            default: emu_log("unsupported additive reloc type %d", stype);
            }
        } else {
            u16 o = soff;
            int guard = 0;
            while (o != 0xFFFF && guard++ < 10000) {
                u32 a = lin(sel, o);
                u16 next = rd16(a);
                switch (stype & 0x7F) {
                case 0: wr8(a, (u8)toff); break;
                case 2: wr16(a, is_const ? toff : tsel); break;
                case 3: wr16(a, toff); wr16(a + 2, tsel); break;
                case 5: wr16(a, toff); break;
                default: emu_log("unsupported reloc type %d", stype);
                }
                if ((stype & 0x7F) == 0) break;  /* byte fixups don't chain */
                o = next;
            }
        }
    }
}

static void patch_prologs(void)
{
    int ord;
    for (ord = 1; ord <= g_mod.nent; ord++) {
        NEEntry *e = &g_mod.ent[ord];
        u8 *f;
        if (!e->seg || e->seg == 0xFE) continue;
        if (g_mod.segflags[e->seg - 1] & 1) continue; /* data segment */
        f = (u8 *)ptr16(g_mod.segsel[e->seg - 1], e->off);
        if (f[2] != 0x90) continue;
        if (f[0] == 0x1E && f[1] == 0x58) { f[0] = 0x8C; f[1] = 0xD8; }  /* push ds;pop ax -> mov ax,ds */
        if (f[0] == 0x8C && f[1] == 0xD8) {
            if (e->flags & 2) {          /* shared data: mov ax, dgroup */
                f[0] = 0xB8;
                f[1] = (u8)g_mod.dgroup;
                f[2] = (u8)(g_mod.dgroup >> 8);
            } else if ((g_mod.flags & 2) && (e->flags & 1)) {
                f[0] = 0x90; f[1] = 0x90;  /* exported, multiple data: AX comes from the caller */
            }
        }
    }
}

int ne_load(const u8 *img, u32 len, const char *path, char *err, int errlen)
{
    const u8 *b = img;
    u16 ne, nseg, segtab, align;
    int i;

    memset(&g_mod, 0, sizeof g_mod);
    g_mod.img = img;
    g_mod.imglen = len;
    strncpy(g_mod.path, path, sizeof g_mod.path - 1);
    if (len < 0x40 || b[0] != 'M' || b[1] != 'Z') { snprintf(err, errlen, "not an MZ executable"); return 0; }
    ne = (u16)g32(b + 0x3C);
    if (ne + 0x40u > len || b[ne] != 'N' || b[ne + 1] != 'E') { snprintf(err, errlen, "not a Windows (NE) executable"); return 0; }
    g_mod.ne = ne;
    g_mod.flags = g16(b + ne + 0x0C);
    g_mod.autodata = g16(b + ne + 0x0E);
    g_mod.heap = g16(b + ne + 0x10);
    g_mod.stack = g16(b + ne + 0x12);
    g_mod.ip = g16(b + ne + 0x14);
    g_mod.cs = g16(b + ne + 0x16);
    g_mod.sp = g16(b + ne + 0x18);
    g_mod.ss = g16(b + ne + 0x1A);
    g_mod.expver = g16(b + ne + 0x3E);
    nseg = g16(b + ne + 0x1C);
    segtab = g16(b + ne + 0x22);
    align = g16(b + ne + 0x32);
    if (!(g_mod.flags & 0x8000) && g_mod.stack < 0x1400) g_mod.stack = 0x1400;
    pstr(b + ne + g16(b + ne + 0x26), g_mod.name, sizeof g_mod.name);
    g_mod.nseg = nseg;
    if (nseg > 255) { snprintf(err, errlen, "too many segments"); return 0; }

    load_entries(b, ne);
    load_resources(b, ne);

    /* allocate and load the segments */
    for (i = 0; i < nseg; i++) {
        const u8 *se = b + ne + segtab + 8 * i;
        u32 fileoff = (u32)g16(se) << align;
        u32 flen = g16(se + 2);
        u16 fl = g16(se + 4);
        u32 minalloc = g16(se + 6);
        u32 size, reserve = 0;
        u16 h;
        int code = !(fl & 1);
        if (!minalloc) minalloc = 0x10000;
        if (fileoff && !flen) flen = 0x10000;
        if (!fileoff) flen = 0;
        size = minalloc;
        if (flen > size) size = flen;
        if (i + 1 == g_mod.autodata) {
            size += g_mod.stack + g_mod.heap;
            if (size > 0xFFF0) size = 0xFFF0;
            reserve = 0x10000;
        }
        h = glob_alloc((u16)((fl & 0x10) ? G_MOVEABLE : G_FIXED), size, GK_SEGMENT, code, reserve);
        if (!h) { snprintf(err, errlen, "out of memory loading segment %d", i + 1); return 0; }
        g_mod.segsel[i] = glob_sel(h);
        g_mod.seglen[i] = (u16)(size > 0xFFFF ? 0xFFFF : size);
        g_mod.segflags[i] = fl;
        if (flen) {
            if (fileoff + flen > len) { snprintf(err, errlen, "truncated segment %d", i + 1); return 0; }
            memcpy(ptr16(g_mod.segsel[i], 0), b + fileoff, flen);
        }
    }
    if (g_mod.autodata) {
        g_mod.dgroup = g_mod.segsel[g_mod.autodata - 1];
        g_mod.hInstance = g_mod.dgroup;
    }
    /* fake module database */
    g_mod.hModule = sys_alloc(0x40, 0);
    memcpy(ptr16(g_mod.hModule, 0), b + ne, 0x40);

    for (i = 0; i < nseg; i++) apply_relocs(i + 1, err, errlen);
    patch_prologs();
    return 1;
}
