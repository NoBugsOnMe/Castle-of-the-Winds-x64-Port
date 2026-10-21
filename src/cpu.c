/*
 * cpu.c - x86 interpreter (8086/186/286 + common 386/486 integer instructions)
 *         running in a Windows 3.x-style protected-mode environment.
 */
#include "cpu.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

CPU g_cpu;
u8 *g_mem;
Descriptor g_ldt[LDT_ENTRIES];
u32 g_trace[TRACE_LEN];
unsigned g_trace_pos;
u16 g_thunk_sel;

static u8 parity_tab[256];
static u32 seg_size[6];   /* limit+1 of the cached segment, 0 for null */

/* ------------------------------------------------------------------------ */

void cpu_init(void)
{
    int i, j;
    for (i = 0; i < 256; i++) {
        int bits = 0;
        for (j = 0; j < 8; j++) bits += (i >> j) & 1;
        parity_tab[i] = (bits & 1) ? 0 : F_PF;
    }
    memset(&g_cpu, 0, sizeof(g_cpu));
    g_cpu.flags = 0x0202;
    g_cpu.seg_ovr = S_NONE;
    memset(seg_size, 0, sizeof(seg_size));
}

static const char *seg_names[6] = { "ES", "CS", "SS", "DS", "FS", "GS" };

void cpu_dump_state(char *buf, size_t len)
{
    size_t n = 0;
    unsigned i;
    n += snprintf(buf + n, len - n,
        "CS:IP=%04X:%04X (start %04X)\r\nAX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X\r\n"
        "DS=%04X ES=%04X SS=%04X FS=%04X GS=%04X FL=%04X\r\n",
        g_cpu.seg[S_CS], g_cpu.ip & 0xFFFF, g_cpu.start_ip & 0xFFFF,
        AX, BX, CX, DX, SI, DI, BP, SP,
        g_cpu.seg[S_DS], g_cpu.seg[S_ES], g_cpu.seg[S_SS], g_cpu.seg[S_FS], g_cpu.seg[S_GS],
        g_cpu.flags & 0xFFFF);
    n += snprintf(buf + n, len - n, "bytes:");
    for (i = 0; i < 12 && n < len; i++)
        n += snprintf(buf + n, len - n, " %02X", rd8(g_cpu.base[S_CS] + ((g_cpu.start_ip + i) & 0xFFFF)));
    n += snprintf(buf + n, len - n, "\r\nrecent:");
    for (i = 0; i < 24 && n < len; i++) {
        u32 t = g_trace[(g_trace_pos - 24 + i) & (TRACE_LEN - 1)];
        n += snprintf(buf + n, len - n, " %04X:%04X", t >> 16, t & 0xFFFF);
    }
    if (n < len) snprintf(buf + n, len - n, "\r\n");
}

static void fault(const char *what, int s, u32 off)
{
    if (s >= 0 && s < 6)
        cpu_fatal("%s (%s=%04X offset %X)", what, seg_names[s], g_cpu.seg[s], off);
    else
        cpu_fatal("%s", what);
}

int cpu_load_seg(int s, u16 sel)
{
    if ((sel & 0xFFFC) == 0) {
        if (s == S_CS || s == S_SS) { fault("General protection fault: null CS/SS", s, 0); return 0; }
        g_cpu.seg[s] = sel;
        g_cpu.base[s] = 0;
        g_cpu.limit[s] = 0;
        seg_size[s] = 0;
        return 1;
    }
    {
        Descriptor *d = &g_ldt[SEL_INDEX(sel)];
        if (!(d->flags & DESC_PRESENT)) {
            char msg[64];
            snprintf(msg, sizeof msg, "General protection fault: invalid selector %04X", sel);
            fault(msg, -1, 0);
            return 0;
        }
        g_cpu.seg[s] = sel;
        g_cpu.base[s] = d->base;
        g_cpu.limit[s] = d->limit;
        seg_size[s] = d->limit + 1;
    }
    return 1;
}

/* reload cached descriptors (after a segment was moved / resized) */
void cpu_refresh_segs(void)
{
    int s;
    for (s = 0; s < 6; s++) {
        u16 sel = g_cpu.seg[s];
        if ((sel & 0xFFFC) == 0) continue;
        if (g_ldt[SEL_INDEX(sel)].flags & DESC_PRESENT) {
            g_cpu.base[s] = g_ldt[SEL_INDEX(sel)].base;
            g_cpu.limit[s] = g_ldt[SEL_INDEX(sel)].limit;
            seg_size[s] = g_cpu.limit[s] + 1;
        } else {
            /* segment was freed while loaded: behave like null */
            g_cpu.base[s] = 0; g_cpu.limit[s] = 0; seg_size[s] = 0;
            if (s != S_CS && s != S_SS) g_cpu.seg[s] = 0;
        }
    }
}

/* ---- memory access through segments ------------------------------------- */

static inline u32 ea_chk(int s, u32 off, u32 width)
{
    if ((u64)off + width > seg_size[s]) {
        fault(seg_size[s] ? "General protection fault: segment limit exceeded"
                          : "General protection fault: access through null selector", s, off);
    }
    return g_cpu.base[s] + off;
}

static inline u8  srd8 (int s, u32 off) { return rd8 (ea_chk(s, off, 1)); }
static inline u16 srd16(int s, u32 off) { return rd16(ea_chk(s, off, 2)); }
static inline u32 srd32(int s, u32 off) { return rd32(ea_chk(s, off, 4)); }
static inline void swr8 (int s, u32 off, u8  v) { wr8 (ea_chk(s, off, 1), v); }
static inline void swr16(int s, u32 off, u16 v) { wr16(ea_chk(s, off, 2), v); }
static inline void swr32(int s, u32 off, u32 v) { wr32(ea_chk(s, off, 4), v); }

/* ---- instruction fetch ---------------------------------------------------- */

static inline u8 fetch8(void)
{
    u8 b = rd8(g_cpu.base[S_CS] + (g_cpu.ip & 0xFFFF));
    g_cpu.ip = (g_cpu.ip + 1) & 0xFFFF;
    return b;
}
static inline u16 fetch16(void)
{
    u16 v = rd16(g_cpu.base[S_CS] + (g_cpu.ip & 0xFFFF));
    g_cpu.ip = (g_cpu.ip + 2) & 0xFFFF;
    return v;
}
static inline u32 fetch32(void)
{
    u32 v = rd32(g_cpu.base[S_CS] + (g_cpu.ip & 0xFFFF));
    g_cpu.ip = (g_cpu.ip + 4) & 0xFFFF;
    return v;
}

/* ---- stack ---------------------------------------------------------------- */

void push16(u16 v)
{
    SP -= 2;
    swr16(S_SS, SP, v);
}
u16 pop16(void)
{
    u16 v = srd16(S_SS, SP);
    SP += 2;
    return v;
}
void push32(u32 v)
{
    SP -= 4;
    swr32(S_SS, SP, v);
}
u32 pop32(void)
{
    u32 v = srd32(S_SS, SP);
    SP += 4;
    return v;
}

/* ---- registers ------------------------------------------------------------ */

static inline u8 *reg8p(int i) { return &((u8 *)&g_cpu.r[i & 3])[i >> 2]; }
static inline u8  getr8(int i) { return *reg8p(i); }
static inline void setr8(int i, u8 v) { *reg8p(i) = v; }
static inline u16 getr16(int i) { return (u16)g_cpu.r[i]; }
static inline void setr16(int i, u16 v) { *(u16 *)&g_cpu.r[i] = v; }

/* ---- ModRM ---------------------------------------------------------------- */

typedef struct {
    int mod, reg, rm;
    int seg;
    u32 off;
} ModRM;

static void decode_modrm(ModRM *m)
{
    u8 b = fetch8();
    m->mod = b >> 6;
    m->reg = (b >> 3) & 7;
    m->rm = b & 7;
    if (m->mod == 3) return;
    if (!g_cpu.adsz32) {
        u16 off = 0;
        int seg = S_DS;
        switch (m->rm) {
        case 0: off = BX + SI; break;
        case 1: off = BX + DI; break;
        case 2: off = BP + SI; seg = S_SS; break;
        case 3: off = BP + DI; seg = S_SS; break;
        case 4: off = SI; break;
        case 5: off = DI; break;
        case 6:
            if (m->mod == 0) off = fetch16();
            else { off = BP; seg = S_SS; }
            break;
        case 7: off = BX; break;
        }
        if (m->mod == 1) off = (u16)(off + (s8)fetch8());
        else if (m->mod == 2) off = (u16)(off + fetch16());
        m->off = off;
        m->seg = g_cpu.seg_ovr != S_NONE ? g_cpu.seg_ovr : seg;
    } else {
        u32 off = 0;
        int seg = S_DS;
        if (m->rm == 4) {
            u8 sib = fetch8();
            int sc = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
            if (base == 5 && m->mod == 0) off = fetch32();
            else {
                off = g_cpu.r[base];
                if (base == R_SP || base == R_BP) seg = S_SS;
            }
            if (idx != 4) off += g_cpu.r[idx] << sc;
        } else if (m->rm == 5 && m->mod == 0) {
            off = fetch32();
        } else {
            off = g_cpu.r[m->rm];
            if (m->rm == R_BP) seg = S_SS;
        }
        if (m->mod == 1) off += (u32)(s32)(s8)fetch8();
        else if (m->mod == 2) off += fetch32();
        m->off = off;
        m->seg = g_cpu.seg_ovr != S_NONE ? g_cpu.seg_ovr : seg;
    }
}

static inline u8 rm_rd8(ModRM *m) { return m->mod == 3 ? getr8(m->rm) : srd8(m->seg, m->off); }
static inline u16 rm_rd16(ModRM *m) { return m->mod == 3 ? getr16(m->rm) : srd16(m->seg, m->off); }
static inline u32 rm_rd32(ModRM *m) { return m->mod == 3 ? g_cpu.r[m->rm] : srd32(m->seg, m->off); }
static inline void rm_wr8(ModRM *m, u8 v) { if (m->mod == 3) setr8(m->rm, v); else swr8(m->seg, m->off, v); }
static inline void rm_wr16(ModRM *m, u16 v) { if (m->mod == 3) setr16(m->rm, v); else swr16(m->seg, m->off, v); }
static inline void rm_wr32(ModRM *m, u32 v) { if (m->mod == 3) g_cpu.r[m->rm] = v; else swr32(m->seg, m->off, v); }

/* ---- flags & ALU ---------------------------------------------------------- */

#define FL g_cpu.flags

static inline u32 szmask(int sz) { return sz == 8 ? 0xFFu : sz == 16 ? 0xFFFFu : 0xFFFFFFFFu; }
static inline u32 szsign(int sz) { return sz == 8 ? 0x80u : sz == 16 ? 0x8000u : 0x80000000u; }

static inline void set_szp(u32 r, int sz)
{
    r &= szmask(sz);
    if (r == 0) FL |= F_ZF;
    if (r & szsign(sz)) FL |= F_SF;
    FL |= parity_tab[r & 0xFF];
}

static inline u32 op_add(u32 a, u32 b, u32 cin, int sz)
{
    u32 m = szmask(sz);
    u64 r64 = (u64)(a & m) + (u64)(b & m) + cin;
    u32 r = (u32)r64 & m;
    FL &= ~F_ARITH;
    if (r64 > m) FL |= F_CF;
    if (((a ^ r) & (b ^ r)) & szsign(sz)) FL |= F_OF;
    if ((a ^ b ^ r) & 0x10) FL |= F_AF;
    set_szp(r, sz);
    return r;
}

static inline u32 op_sub(u32 a, u32 b, u32 cin, int sz)
{
    u32 m = szmask(sz);
    u32 r = (a - b - cin) & m;
    FL &= ~F_ARITH;
    if ((u64)(a & m) < (u64)(b & m) + cin) FL |= F_CF;
    if (((a ^ b) & (a ^ r)) & szsign(sz)) FL |= F_OF;
    if ((a ^ b ^ r) & 0x10) FL |= F_AF;
    set_szp(r, sz);
    return r;
}

static inline u32 op_logic(u32 r, int sz)
{
    r &= szmask(sz);
    FL &= ~F_ARITH;
    set_szp(r, sz);
    return r;
}

static u32 alu(int op, u32 a, u32 b, int sz)
{
    switch (op) {
    case 0: return op_add(a, b, 0, sz);
    case 1: return op_logic(a | b, sz);
    case 2: return op_add(a, b, FL & F_CF ? 1 : 0, sz);
    case 3: return op_sub(a, b, FL & F_CF ? 1 : 0, sz);
    case 4: return op_logic(a & b, sz);
    case 5: return op_sub(a, b, 0, sz);
    case 6: return op_logic(a ^ b, sz);
    default: op_sub(a, b, 0, sz); return a;
    }
}

static inline u32 op_inc(u32 a, int sz)
{
    u32 cf = FL & F_CF;
    u32 r = op_add(a, 1, 0, sz);
    FL = (FL & ~F_CF) | cf;
    return r;
}
static inline u32 op_dec(u32 a, int sz)
{
    u32 cf = FL & F_CF;
    u32 r = op_sub(a, 1, 0, sz);
    FL = (FL & ~F_CF) | cf;
    return r;
}

/* shifts and rotates; count already masked to 5 bits */
static u32 shift_op(int op, u32 v, unsigned cnt, int sz)
{
    u32 m = szmask(sz), sign = szsign(sz);
    u32 r = v & m;
    unsigned i;
    if (cnt == 0) return r;
    switch (op) {
    case 0: /* ROL */
        cnt %= sz;
        if (cnt) r = ((r << cnt) | (r >> (sz - cnt))) & m;
        FL = (FL & ~F_CF) | (r & 1 ? F_CF : 0);
        FL = (FL & ~F_OF) | ((((r & sign) ? 1 : 0) ^ (r & 1)) ? F_OF : 0);
        return r;
    case 1: /* ROR */
        cnt %= sz;
        if (cnt) r = ((r >> cnt) | (r << (sz - cnt))) & m;
        FL = (FL & ~F_CF) | (r & sign ? F_CF : 0);
        FL = (FL & ~F_OF) | ((((r & sign) ? 1 : 0) ^ ((r & (sign >> 1)) ? 1 : 0)) ? F_OF : 0);
        return r;
    case 2: /* RCL */
        if (sz == 8) cnt %= 9; else if (sz == 16) cnt %= 17;
        for (i = 0; i < cnt; i++) {
            u32 cf = (r & sign) ? 1 : 0;
            r = ((r << 1) | (FL & F_CF ? 1 : 0)) & m;
            FL = (FL & ~F_CF) | (cf ? F_CF : 0);
        }
        FL = (FL & ~F_OF) | ((((r & sign) ? 1 : 0) ^ (FL & F_CF ? 1 : 0)) ? F_OF : 0);
        return r;
    case 3: /* RCR */
        if (sz == 8) cnt %= 9; else if (sz == 16) cnt %= 17;
        for (i = 0; i < cnt; i++) {
            u32 cf = r & 1;
            r = (r >> 1) | ((FL & F_CF) ? sign : 0);
            FL = (FL & ~F_CF) | (cf ? F_CF : 0);
        }
        FL = (FL & ~F_OF) | ((((r & sign) ? 1 : 0) ^ ((r & (sign >> 1)) ? 1 : 0)) ? F_OF : 0);
        return r;
    case 4: case 6: { /* SHL/SAL */
        u32 cf;
        if (cnt <= (unsigned)sz) cf = ((u64)r << (cnt - 1)) & sign ? 1 : 0;
        else cf = 0;
        r = (u32)(((u64)r << cnt) & m);
        FL &= ~F_ARITH;
        if (cf) FL |= F_CF;
        if ((((r & sign) ? 1 : 0) ^ cf)) FL |= F_OF;
        set_szp(r, sz);
        return r;
    }
    case 5: { /* SHR */
        u32 cf = (cnt <= (unsigned)sz) ? (u32)(((u64)r >> (cnt - 1)) & 1) : 0;
        u32 orig = r;
        r = (u32)((u64)r >> cnt) & m;
        FL &= ~F_ARITH;
        if (cf) FL |= F_CF;
        if (orig & sign) FL |= F_OF;
        set_szp(r, sz);
        return r;
    }
    case 7: { /* SAR */
        s64 sv = (sz == 8) ? (s64)(s8)r : (sz == 16) ? (s64)(s16)r : (s64)(s32)r;
        u32 cf;
        if (cnt > (unsigned)sz) cnt = sz;
        cf = (u32)((sv >> (cnt - 1)) & 1);
        r = (u32)(sv >> cnt) & m;
        FL &= ~F_ARITH;
        if (cf) FL |= F_CF;
        set_szp(r, sz);
        return r;
    }
    }
    return r;
}

/* ---- conditions ----------------------------------------------------------- */

static inline int cond(int c)
{
    int r;
    switch (c >> 1) {
    case 0: r = (FL & F_OF) != 0; break;
    case 1: r = (FL & F_CF) != 0; break;
    case 2: r = (FL & F_ZF) != 0; break;
    case 3: r = (FL & (F_CF | F_ZF)) != 0; break;
    case 4: r = (FL & F_SF) != 0; break;
    case 5: r = (FL & F_PF) != 0; break;
    case 6: r = ((FL & F_SF) != 0) != ((FL & F_OF) != 0); break;
    default: r = (FL & F_ZF) || (((FL & F_SF) != 0) != ((FL & F_OF) != 0)); break;
    }
    return (c & 1) ? !r : r;
}

/* ---- control transfer ------------------------------------------------------- */

static void far_jump(u16 sel, u32 off)
{
    if (!cpu_load_seg(S_CS, sel)) return;
    g_cpu.ip = off & 0xFFFF;
    if (g_cpu.ip >= seg_size[S_CS]) fault("General protection fault: jump beyond code segment limit", S_CS, g_cpu.ip);
}

static void do_int(u8 vec)
{
    cpu_interrupt(vec);
}

static void set_flags16(u16 v)
{
    /* keep IOPL, and the reserved bits; allow NT and the arithmetic/control flags */
    u32 keep = FL & 0xFFFF0000u;
    u32 mask = F_CF | F_PF | F_AF | F_ZF | F_SF | F_TF | F_IF | F_DF | F_OF | F_NT;
    FL = keep | (FL & ~mask & 0xFFFF) | (v & mask) | 2;
    FL &= ~F_TF; /* never single-step */
}

/* ---- division --------------------------------------------------------------- */

static void div_error(void)
{
    fault("Divide by zero or overflow", -1, 0);
}

/* ---- the interpreter -------------------------------------------------------- */

static void exec_0f(void);
static void exec_string(u8 op);

#define OPSZ (g_cpu.opsz32 ? 32 : 16)

static void step(void)
{
    u8 op;
    ModRM m;

    g_cpu.start_ip = g_cpu.ip;
    g_trace[g_trace_pos++ & (TRACE_LEN - 1)] = ((u32)g_cpu.seg[S_CS] << 16) | (g_cpu.ip & 0xFFFF);
    g_cpu.seg_ovr = S_NONE;
    g_cpu.opsz32 = 0;
    g_cpu.adsz32 = 0;
    g_cpu.rep = 0;

    for (;;) {
        op = fetch8();
        switch (op) {
        case 0x26: g_cpu.seg_ovr = S_ES; continue;
        case 0x2E: g_cpu.seg_ovr = S_CS; continue;
        case 0x36: g_cpu.seg_ovr = S_SS; continue;
        case 0x3E: g_cpu.seg_ovr = S_DS; continue;
        case 0x64: g_cpu.seg_ovr = S_FS; continue;
        case 0x65: g_cpu.seg_ovr = S_GS; continue;
        case 0x66: g_cpu.opsz32 = 1; continue;
        case 0x67: g_cpu.adsz32 = 1; continue;
        case 0xF0: continue; /* LOCK */
        case 0xF2: case 0xF3: g_cpu.rep = op; continue;
        }
        break;
    }
    g_cpu.icount++;

    switch (op) {
    /* ---- ALU group: ADD OR ADC SBB AND SUB XOR CMP ---- */
    case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38: {
        int aop = op >> 3;
        decode_modrm(&m);
        {
            u8 r = (u8)alu(aop, rm_rd8(&m), getr8(m.reg), 8);
            if (aop != 7) rm_wr8(&m, r);
        }
        break;
    }
    case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x31: case 0x39: {
        int aop = op >> 3;
        decode_modrm(&m);
        if (g_cpu.opsz32) {
            u32 r = alu(aop, rm_rd32(&m), g_cpu.r[m.reg], 32);
            if (aop != 7) rm_wr32(&m, r);
        } else {
            u16 r = (u16)alu(aop, rm_rd16(&m), getr16(m.reg), 16);
            if (aop != 7) rm_wr16(&m, r);
        }
        break;
    }
    case 0x02: case 0x0A: case 0x12: case 0x1A: case 0x22: case 0x2A: case 0x32: case 0x3A: {
        int aop = op >> 3;
        decode_modrm(&m);
        {
            u8 r = (u8)alu(aop, getr8(m.reg), rm_rd8(&m), 8);
            if (aop != 7) setr8(m.reg, r);
        }
        break;
    }
    case 0x03: case 0x0B: case 0x13: case 0x1B: case 0x23: case 0x2B: case 0x33: case 0x3B: {
        int aop = op >> 3;
        decode_modrm(&m);
        if (g_cpu.opsz32) {
            u32 r = alu(aop, g_cpu.r[m.reg], rm_rd32(&m), 32);
            if (aop != 7) g_cpu.r[m.reg] = r;
        } else {
            u16 r = (u16)alu(aop, getr16(m.reg), rm_rd16(&m), 16);
            if (aop != 7) setr16(m.reg, r);
        }
        break;
    }
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: {
        int aop = op >> 3;
        u8 r = (u8)alu(aop, AL, fetch8(), 8);
        if (aop != 7) AL = r;
        break;
    }
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: {
        int aop = op >> 3;
        if (g_cpu.opsz32) {
            u32 r = alu(aop, EAX, fetch32(), 32);
            if (aop != 7) EAX = r;
        } else {
            u16 r = (u16)alu(aop, AX, fetch16(), 16);
            if (aop != 7) AX = r;
        }
        break;
    }

    /* ---- segment push/pop ---- */
    case 0x06: if (g_cpu.opsz32) push32(g_cpu.seg[S_ES]); else push16(g_cpu.seg[S_ES]); break;
    case 0x07: cpu_load_seg(S_ES, g_cpu.opsz32 ? (u16)pop32() : pop16()); break;
    case 0x0E: if (g_cpu.opsz32) push32(g_cpu.seg[S_CS]); else push16(g_cpu.seg[S_CS]); break;
    case 0x16: if (g_cpu.opsz32) push32(g_cpu.seg[S_SS]); else push16(g_cpu.seg[S_SS]); break;
    case 0x17: cpu_load_seg(S_SS, g_cpu.opsz32 ? (u16)pop32() : pop16()); break;
    case 0x1E: if (g_cpu.opsz32) push32(g_cpu.seg[S_DS]); else push16(g_cpu.seg[S_DS]); break;
    case 0x1F: cpu_load_seg(S_DS, g_cpu.opsz32 ? (u16)pop32() : pop16()); break;
    case 0x0F: exec_0f(); break;

    /* ---- BCD ---- */
    case 0x27: { /* DAA */
        u8 al = AL, oldal = al; u32 oldcf = FL & F_CF;
        FL &= ~F_CF;
        if ((al & 0x0F) > 9 || (FL & F_AF)) { al += 6; FL |= F_AF; if (oldcf || oldal > 0xF9) FL |= F_CF; }
        else FL &= ~F_AF;
        if (oldal > 0x99 || oldcf) { al += 0x60; FL |= F_CF; }
        AL = al;
        FL &= ~(F_SF | F_ZF | F_PF | F_OF);
        set_szp(al, 8);
        break;
    }
    case 0x2F: { /* DAS */
        u8 al = AL, oldal = al; u32 oldcf = FL & F_CF;
        FL &= ~F_CF;
        if ((al & 0x0F) > 9 || (FL & F_AF)) { al -= 6; FL |= F_AF; if (oldcf || oldal < 6) FL |= F_CF; }
        else FL &= ~F_AF;
        if (oldal > 0x99 || oldcf) { al -= 0x60; FL |= F_CF; }
        AL = al;
        FL &= ~(F_SF | F_ZF | F_PF | F_OF);
        set_szp(al, 8);
        break;
    }
    case 0x37: /* AAA */
        if ((AL & 0x0F) > 9 || (FL & F_AF)) { AX = AX + 0x106; FL |= F_AF | F_CF; }
        else FL &= ~(F_AF | F_CF);
        AL &= 0x0F;
        break;
    case 0x3F: /* AAS */
        if ((AL & 0x0F) > 9 || (FL & F_AF)) { AX = AX - 6; AH -= 1; FL |= F_AF | F_CF; }
        else FL &= ~(F_AF | F_CF);
        AL &= 0x0F;
        break;

    /* ---- INC/DEC/PUSH/POP reg ---- */
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        if (g_cpu.opsz32) g_cpu.r[op & 7] = op_inc(g_cpu.r[op & 7], 32);
        else setr16(op & 7, (u16)op_inc(getr16(op & 7), 16));
        break;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        if (g_cpu.opsz32) g_cpu.r[op & 7] = op_dec(g_cpu.r[op & 7], 32);
        else setr16(op & 7, (u16)op_dec(getr16(op & 7), 16));
        break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        if (g_cpu.opsz32) push32(g_cpu.r[op & 7]);
        else push16(getr16(op & 7));   /* PUSH SP pushes the old value on 286+ */
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        if (g_cpu.opsz32) { u32 v = pop32(); g_cpu.r[op & 7] = v; }
        else { u16 v = pop16(); setr16(op & 7, v); }
        break;

    case 0x60: /* PUSHA */
        if (g_cpu.opsz32) {
            u32 t = ESP;
            push32(EAX); push32(ECX); push32(EDX); push32(EBX);
            push32(t); push32(EBP); push32(ESI); push32(EDI);
        } else {
            u16 t = SP;
            push16(AX); push16(CX); push16(DX); push16(BX);
            push16(t); push16(BP); push16(SI); push16(DI);
        }
        break;
    case 0x61: /* POPA */
        if (g_cpu.opsz32) {
            EDI = pop32(); ESI = pop32(); EBP = pop32(); pop32();
            EBX = pop32(); EDX = pop32(); ECX = pop32(); EAX = pop32();
        } else {
            DI = pop16(); SI = pop16(); BP = pop16(); pop16();
            BX = pop16(); DX = pop16(); CX = pop16(); AX = pop16();
        }
        break;
    case 0x62: /* BOUND */
        decode_modrm(&m);
        if (g_cpu.opsz32) {
            s32 idx = (s32)g_cpu.r[m.reg];
            s32 lo = (s32)srd32(m.seg, m.off), hi = (s32)srd32(m.seg, m.off + 4);
            if (idx < lo || idx > hi) do_int(5);
        } else {
            s16 idx = (s16)getr16(m.reg);
            s16 lo = (s16)srd16(m.seg, m.off), hi = (s16)srd16(m.seg, (u16)(m.off + 2));
            if (idx < lo || idx > hi) do_int(5);
        }
        break;
    case 0x63: { /* ARPL */
        u16 d, s;
        decode_modrm(&m);
        d = rm_rd16(&m); s = getr16(m.reg);
        if ((d & 3) < (s & 3)) { rm_wr16(&m, (u16)((d & ~3) | (s & 3))); FL |= F_ZF; }
        else FL &= ~F_ZF;
        break;
    }
    case 0x68:
        if (g_cpu.opsz32) push32(fetch32()); else push16(fetch16());
        break;
    case 0x6A:
        if (g_cpu.opsz32) push32((u32)(s32)(s8)fetch8()); else push16((u16)(s16)(s8)fetch8());
        break;
    case 0x69: case 0x6B: { /* IMUL Gv,Ev,Iv/Ib */
        decode_modrm(&m);
        if (g_cpu.opsz32) {
            s64 a = (s32)rm_rd32(&m);
            s64 b = (op == 0x69) ? (s32)fetch32() : (s8)fetch8();
            s64 r = a * b;
            g_cpu.r[m.reg] = (u32)r;
            FL &= ~(F_CF | F_OF);
            if (r != (s32)r) FL |= F_CF | F_OF;
        } else {
            s32 a = (s16)rm_rd16(&m);
            s32 b = (op == 0x69) ? (s16)fetch16() : (s8)fetch8();
            s32 r = a * b;
            setr16(m.reg, (u16)r);
            FL &= ~(F_CF | F_OF);
            if (r != (s16)r) FL |= F_CF | F_OF;
        }
        break;
    }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        exec_string(op);
        break;

    /* ---- Jcc short ---- */
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        s8 d = (s8)fetch8();
        if (cond(op & 15)) g_cpu.ip = (g_cpu.ip + d) & 0xFFFF;
        break;
    }

    /* ---- group 1 immediate ---- */
    case 0x80: case 0x82: {
        decode_modrm(&m);
        {
            u8 a = rm_rd8(&m);
            u8 r = (u8)alu(m.reg, a, fetch8(), 8);
            if (m.reg != 7) rm_wr8(&m, r);
        }
        break;
    }
    case 0x81: case 0x83: {
        decode_modrm(&m);
        if (g_cpu.opsz32) {
            u32 a = rm_rd32(&m);
            u32 b = (op == 0x81) ? fetch32() : (u32)(s32)(s8)fetch8();
            u32 r = alu(m.reg, a, b, 32);
            if (m.reg != 7) rm_wr32(&m, r);
        } else {
            u16 a = rm_rd16(&m);
            u16 b = (op == 0x81) ? fetch16() : (u16)(s16)(s8)fetch8();
            u16 r = (u16)alu(m.reg, a, b, 16);
            if (m.reg != 7) rm_wr16(&m, r);
        }
        break;
    }
    case 0x84:
        decode_modrm(&m);
        op_logic(rm_rd8(&m) & getr8(m.reg), 8);
        break;
    case 0x85:
        decode_modrm(&m);
        if (g_cpu.opsz32) op_logic(rm_rd32(&m) & g_cpu.r[m.reg], 32);
        else op_logic(rm_rd16(&m) & getr16(m.reg), 16);
        break;
    case 0x86: {
        u8 t;
        decode_modrm(&m);
        t = rm_rd8(&m);
        rm_wr8(&m, getr8(m.reg));
        setr8(m.reg, t);
        break;
    }
    case 0x87:
        decode_modrm(&m);
        if (g_cpu.opsz32) { u32 t = rm_rd32(&m); rm_wr32(&m, g_cpu.r[m.reg]); g_cpu.r[m.reg] = t; }
        else { u16 t = rm_rd16(&m); rm_wr16(&m, getr16(m.reg)); setr16(m.reg, t); }
        break;
    case 0x88: decode_modrm(&m); rm_wr8(&m, getr8(m.reg)); break;
    case 0x89:
        decode_modrm(&m);
        if (g_cpu.opsz32) rm_wr32(&m, g_cpu.r[m.reg]); else rm_wr16(&m, getr16(m.reg));
        break;
    case 0x8A: decode_modrm(&m); setr8(m.reg, rm_rd8(&m)); break;
    case 0x8B:
        decode_modrm(&m);
        if (g_cpu.opsz32) g_cpu.r[m.reg] = rm_rd32(&m); else setr16(m.reg, rm_rd16(&m));
        break;
    case 0x8C:
        decode_modrm(&m);
        if (m.reg > 5) fault("Invalid opcode (mov from bad segment register)", -1, 0);
        if (m.mod == 3 && g_cpu.opsz32) g_cpu.r[m.rm] = g_cpu.seg[m.reg];
        else rm_wr16(&m, g_cpu.seg[m.reg]);
        break;
    case 0x8D:
        decode_modrm(&m);
        if (m.mod == 3) fault("Invalid opcode (LEA with register operand)", -1, 0);
        if (g_cpu.opsz32) g_cpu.r[m.reg] = m.off; else setr16(m.reg, (u16)m.off);
        break;
    case 0x8E:
        decode_modrm(&m);
        if (m.reg == S_CS || m.reg > 5) fault("Invalid opcode (mov to CS)", -1, 0);
        cpu_load_seg(m.reg, rm_rd16(&m));
        break;
    case 0x8F: {
        /* POP Ev: the effective address uses SP after the pop */
        if (g_cpu.opsz32) {
            u32 v = pop32();
            decode_modrm(&m);
            rm_wr32(&m, v);
        } else {
            u16 v = pop16();
            decode_modrm(&m);
            rm_wr16(&m, v);
        }
        break;
    }
    case 0x90: break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        if (g_cpu.opsz32) { u32 t = EAX; EAX = g_cpu.r[op & 7]; g_cpu.r[op & 7] = t; }
        else { u16 t = AX; AX = getr16(op & 7); setr16(op & 7, t); }
        break;
    case 0x98:
        if (g_cpu.opsz32) EAX = (u32)(s32)(s16)AX; else AX = (u16)(s16)(s8)AL;
        break;
    case 0x99:
        if (g_cpu.opsz32) EDX = (EAX & 0x80000000u) ? 0xFFFFFFFFu : 0;
        else DX = (AX & 0x8000) ? 0xFFFF : 0;
        break;
    case 0x9A: { /* CALL far */
        u32 off = g_cpu.opsz32 ? fetch32() : fetch16();
        u16 sel = fetch16();
        if (g_cpu.opsz32) { push32(g_cpu.seg[S_CS]); push32(g_cpu.ip); }
        else { push16(g_cpu.seg[S_CS]); push16((u16)g_cpu.ip); }
        far_jump(sel, off);
        break;
    }
    case 0x9B: break; /* WAIT */
    case 0x9C:
        if (g_cpu.opsz32) push32(FL & 0x00FCFFFFu); else push16((u16)FL);
        break;
    case 0x9D:
        if (g_cpu.opsz32) { u32 v = pop32(); set_flags16((u16)v); FL = (FL & 0xFFFF) | (v & 0x00240000u); }
        else set_flags16(pop16());
        break;
    case 0x9E: /* SAHF */
        FL = (FL & ~(F_SF | F_ZF | F_AF | F_PF | F_CF)) | (AH & (F_SF | F_ZF | F_AF | F_PF | F_CF));
        break;
    case 0x9F: /* LAHF */
        AH = (u8)((FL & (F_SF | F_ZF | F_AF | F_PF | F_CF)) | 2);
        break;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        u32 off = g_cpu.adsz32 ? fetch32() : fetch16();
        int s = g_cpu.seg_ovr != S_NONE ? g_cpu.seg_ovr : S_DS;
        switch (op) {
        case 0xA0: AL = srd8(s, off); break;
        case 0xA1: if (g_cpu.opsz32) EAX = srd32(s, off); else AX = srd16(s, off); break;
        case 0xA2: swr8(s, off, AL); break;
        case 0xA3: if (g_cpu.opsz32) swr32(s, off, EAX); else swr16(s, off, AX); break;
        }
        break;
    }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        exec_string(op);
        break;
    case 0xA8: op_logic(AL & fetch8(), 8); break;
    case 0xA9:
        if (g_cpu.opsz32) op_logic(EAX & fetch32(), 32); else op_logic(AX & fetch16(), 16);
        break;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        setr8(op & 7, fetch8());
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        if (g_cpu.opsz32) g_cpu.r[op & 7] = fetch32(); else setr16(op & 7, fetch16());
        break;

    /* ---- shifts ---- */
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        unsigned cnt;
        decode_modrm(&m);
        if (op == 0xC0 || op == 0xC1) cnt = fetch8();
        else if (op == 0xD0 || op == 0xD1) cnt = 1;
        else cnt = CL;
        cnt &= 31;
        if (!(op & 1)) {
            u8 v = rm_rd8(&m);
            if (cnt) rm_wr8(&m, (u8)shift_op(m.reg, v, cnt, 8));
        } else if (g_cpu.opsz32) {
            u32 v = rm_rd32(&m);
            if (cnt) rm_wr32(&m, shift_op(m.reg, v, cnt, 32));
        } else {
            u16 v = rm_rd16(&m);
            if (cnt) rm_wr16(&m, (u16)shift_op(m.reg, v, cnt, 16));
        }
        break;
    }
    case 0xC2: { u16 n = fetch16(); g_cpu.ip = g_cpu.opsz32 ? pop32() & 0xFFFF : pop16(); SP += n; break; }
    case 0xC3: g_cpu.ip = g_cpu.opsz32 ? pop32() & 0xFFFF : pop16(); break;
    case 0xC4: case 0xC5: { /* LES / LDS */
        decode_modrm(&m);
        if (m.mod == 3) fault("Invalid opcode (LES/LDS register)", -1, 0);
        if (g_cpu.opsz32) {
            u32 off = srd32(m.seg, m.off);
            u16 sel = srd16(m.seg, m.off + 4);
            if (cpu_load_seg(op == 0xC4 ? S_ES : S_DS, sel)) g_cpu.r[m.reg] = off;
        } else {
            u16 off = srd16(m.seg, m.off);
            u16 sel = srd16(m.seg, (u16)(m.off + 2));
            if (cpu_load_seg(op == 0xC4 ? S_ES : S_DS, sel)) setr16(m.reg, off);
        }
        break;
    }
    case 0xC6: decode_modrm(&m); rm_wr8(&m, fetch8()); break;
    case 0xC7:
        decode_modrm(&m);
        if (g_cpu.opsz32) rm_wr32(&m, fetch32()); else rm_wr16(&m, fetch16());
        break;
    case 0xC8: { /* ENTER */
        u16 size = fetch16();
        u8 level = fetch8() & 31;
        u16 frame;
        push16(BP);
        frame = SP;
        if (level > 0) {
            int i;
            for (i = 1; i < level; i++) { BP -= 2; push16(srd16(S_SS, BP)); }
            push16(frame);
        }
        BP = frame;
        SP -= size;
        break;
    }
    case 0xC9: /* LEAVE */
        SP = BP;
        BP = pop16();
        break;
    case 0xCA: case 0xCB: { /* RETF */
        u16 n = (op == 0xCA) ? fetch16() : 0;
        u32 off; u16 sel;
        if (g_cpu.opsz32) { off = pop32(); sel = (u16)pop32(); }
        else { off = pop16(); sel = pop16(); }
        SP += n;
        far_jump(sel, off);
        break;
    }
    case 0xCC: do_int(3); break;
    case 0xCD: do_int(fetch8()); break;
    case 0xCE: if (FL & F_OF) do_int(4); break;
    case 0xCF: { /* IRET */
        u16 ip = pop16(), sel = pop16(), fl = pop16();
        set_flags16(fl);
        far_jump(sel, ip);
        break;
    }
    case 0xD4: { /* AAM */
        u8 base = fetch8();
        if (!base) { div_error(); break; }
        AH = AL / base;
        AL = AL % base;
        FL &= ~(F_SF | F_ZF | F_PF);
        set_szp(AL, 8);
        break;
    }
    case 0xD5: { /* AAD */
        u8 base = fetch8();
        AL = (u8)(AL + AH * base);
        AH = 0;
        FL &= ~(F_SF | F_ZF | F_PF);
        set_szp(AL, 8);
        break;
    }
    case 0xD6: AL = (FL & F_CF) ? 0xFF : 0; break; /* SALC */
    case 0xD7: { /* XLAT */
        int s = g_cpu.seg_ovr != S_NONE ? g_cpu.seg_ovr : S_DS;
        AL = srd8(s, (u16)(BX + AL));
        break;
    }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        fault("Floating point instruction (no FPU emulation)", -1, 0);
        break;
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: {
        s8 d = (s8)fetch8();
        int take;
        if (op == 0xE3) take = g_cpu.adsz32 ? (ECX == 0) : (CX == 0);
        else {
            if (g_cpu.adsz32) { ECX--; take = ECX != 0; }
            else { CX--; take = CX != 0; }
            if (op == 0xE0) take = take && !(FL & F_ZF);
            else if (op == 0xE1) take = take && (FL & F_ZF);
        }
        if (take) g_cpu.ip = (g_cpu.ip + d) & 0xFFFF;
        break;
    }
    case 0xE4: fetch8(); AL = 0xFF; break;
    case 0xE5: fetch8(); if (g_cpu.opsz32) EAX = 0xFFFFFFFFu; else AX = 0xFFFF; break;
    case 0xE6: case 0xE7: fetch8(); break;
    case 0xEC: AL = 0xFF; break;
    case 0xED: if (g_cpu.opsz32) EAX = 0xFFFFFFFFu; else AX = 0xFFFF; break;
    case 0xEE: case 0xEF: break;
    case 0xE8: {
        if (g_cpu.opsz32) { s32 d = (s32)fetch32(); push32(g_cpu.ip); g_cpu.ip = (g_cpu.ip + d) & 0xFFFF; }
        else { s16 d = (s16)fetch16(); push16((u16)g_cpu.ip); g_cpu.ip = (g_cpu.ip + d) & 0xFFFF; }
        break;
    }
    case 0xE9: {
        if (g_cpu.opsz32) { s32 d = (s32)fetch32(); g_cpu.ip = (g_cpu.ip + d) & 0xFFFF; }
        else { s16 d = (s16)fetch16(); g_cpu.ip = (g_cpu.ip + d) & 0xFFFF; }
        break;
    }
    case 0xEA: {
        u32 off = g_cpu.opsz32 ? fetch32() : fetch16();
        u16 sel = fetch16();
        far_jump(sel, off);
        break;
    }
    case 0xEB: { s8 d = (s8)fetch8(); g_cpu.ip = (g_cpu.ip + d) & 0xFFFF; break; }
    case 0xF1: { /* emulator escape: native thunk */
        u16 id = fetch16();
        native_dispatch(id);
        break;
    }
    case 0xF4: fault("Privileged instruction (HLT)", -1, 0); break;
    case 0xF5: FL ^= F_CF; break;
    case 0xF6: {
        decode_modrm(&m);
        switch (m.reg) {
        case 0: case 1: op_logic(rm_rd8(&m) & fetch8(), 8); break;
        case 2: rm_wr8(&m, (u8)~rm_rd8(&m)); break;
        case 3: { u8 v = rm_rd8(&m); u8 r = (u8)op_sub(0, v, 0, 8); rm_wr8(&m, r); if (v) FL |= F_CF; else FL &= ~F_CF; break; }
        case 4: { u16 r = (u16)AL * rm_rd8(&m); AX = r; FL &= ~(F_CF | F_OF); if (AH) FL |= F_CF | F_OF; break; }
        case 5: { s16 r = (s16)(s8)AL * (s8)rm_rd8(&m); AX = (u16)r; FL &= ~(F_CF | F_OF); if (r != (s8)r) FL |= F_CF | F_OF; break; }
        case 6: {
            u8 d = rm_rd8(&m);
            u16 q;
            if (!d) { div_error(); break; }
            q = AX / d;
            if (q > 0xFF) { div_error(); break; }
            AH = (u8)(AX % d);
            AL = (u8)q;
            break;
        }
        case 7: {
            s8 d = (s8)rm_rd8(&m);
            s16 n = (s16)AX, q;
            if (!d) { div_error(); break; }
            if (n == -32768 && d == -1) { div_error(); break; }
            q = n / d;
            if (q > 127 || q < -128) { div_error(); break; }
            AH = (u8)(s8)(n % d);
            AL = (u8)(s8)q;
            break;
        }
        }
        break;
    }
    case 0xF7: {
        decode_modrm(&m);
        if (g_cpu.opsz32) {
            switch (m.reg) {
            case 0: case 1: op_logic(rm_rd32(&m) & fetch32(), 32); break;
            case 2: rm_wr32(&m, ~rm_rd32(&m)); break;
            case 3: { u32 v = rm_rd32(&m); rm_wr32(&m, op_sub(0, v, 0, 32)); if (v) FL |= F_CF; else FL &= ~F_CF; break; }
            case 4: { u64 r = (u64)EAX * rm_rd32(&m); EAX = (u32)r; EDX = (u32)(r >> 32); FL &= ~(F_CF | F_OF); if (EDX) FL |= F_CF | F_OF; break; }
            case 5: { s64 r = (s64)(s32)EAX * (s32)rm_rd32(&m); EAX = (u32)r; EDX = (u32)((u64)r >> 32); FL &= ~(F_CF | F_OF); if (r != (s32)r) FL |= F_CF | F_OF; break; }
            case 6: {
                u32 d = rm_rd32(&m);
                u64 n = ((u64)EDX << 32) | EAX, q;
                if (!d) { div_error(); break; }
                q = n / d;
                if (q > 0xFFFFFFFFu) { div_error(); break; }
                EDX = (u32)(n % d); EAX = (u32)q;
                break;
            }
            case 7: {
                s32 d = (s32)rm_rd32(&m);
                s64 n = (s64)(((u64)EDX << 32) | EAX), q;
                if (!d) { div_error(); break; }
                if (d == -1 && n == INT64_MIN) { div_error(); break; }
                q = n / d;
                if (q > 0x7FFFFFFF || q < -0x7FFFFFFF - 1) { div_error(); break; }
                EDX = (u32)(s32)(n % d); EAX = (u32)(s32)q;
                break;
            }
            }
        } else {
            switch (m.reg) {
            case 0: case 1: op_logic(rm_rd16(&m) & fetch16(), 16); break;
            case 2: rm_wr16(&m, (u16)~rm_rd16(&m)); break;
            case 3: { u16 v = rm_rd16(&m); rm_wr16(&m, (u16)op_sub(0, v, 0, 16)); if (v) FL |= F_CF; else FL &= ~F_CF; break; }
            case 4: { u32 r = (u32)AX * rm_rd16(&m); AX = (u16)r; DX = (u16)(r >> 16); FL &= ~(F_CF | F_OF); if (DX) FL |= F_CF | F_OF; break; }
            case 5: { s32 r = (s32)(s16)AX * (s16)rm_rd16(&m); AX = (u16)r; DX = (u16)((u32)r >> 16); FL &= ~(F_CF | F_OF); if (r != (s16)r) FL |= F_CF | F_OF; break; }
            case 6: {
                u16 d = rm_rd16(&m);
                u32 n = ((u32)DX << 16) | AX, q;
                if (!d) { div_error(); break; }
                q = n / d;
                if (q > 0xFFFF) { div_error(); break; }
                DX = (u16)(n % d); AX = (u16)q;
                break;
            }
            case 7: {
                s16 d = (s16)rm_rd16(&m);
                s32 n = (s32)(((u32)DX << 16) | AX), q;
                if (!d) { div_error(); break; }
                if (d == -1 && n == INT32_MIN) { div_error(); break; }
                q = n / d;
                if (q > 32767 || q < -32768) { div_error(); break; }
                DX = (u16)(s16)(n % d); AX = (u16)(s16)q;
                break;
            }
            }
        }
        break;
    }
    case 0xF8: FL &= ~F_CF; break;
    case 0xF9: FL |= F_CF; break;
    case 0xFA: FL &= ~F_IF; break;
    case 0xFB: FL |= F_IF; break;
    case 0xFC: FL &= ~F_DF; break;
    case 0xFD: FL |= F_DF; break;
    case 0xFE:
        decode_modrm(&m);
        if (m.reg == 0) rm_wr8(&m, (u8)op_inc(rm_rd8(&m), 8));
        else if (m.reg == 1) rm_wr8(&m, (u8)op_dec(rm_rd8(&m), 8));
        else fault("Invalid opcode (FE group)", -1, 0);
        break;
    case 0xFF:
        decode_modrm(&m);
        switch (m.reg) {
        case 0:
            if (g_cpu.opsz32) rm_wr32(&m, op_inc(rm_rd32(&m), 32));
            else rm_wr16(&m, (u16)op_inc(rm_rd16(&m), 16));
            break;
        case 1:
            if (g_cpu.opsz32) rm_wr32(&m, op_dec(rm_rd32(&m), 32));
            else rm_wr16(&m, (u16)op_dec(rm_rd16(&m), 16));
            break;
        case 2: { /* CALL near indirect */
            u32 t = g_cpu.opsz32 ? rm_rd32(&m) : rm_rd16(&m);
            if (g_cpu.opsz32) push32(g_cpu.ip); else push16((u16)g_cpu.ip);
            g_cpu.ip = t & 0xFFFF;
            break;
        }
        case 3: { /* CALL far indirect */
            u32 off; u16 sel;
            if (m.mod == 3) { fault("Invalid opcode (far call register)", -1, 0); break; }
            if (g_cpu.opsz32) { off = srd32(m.seg, m.off); sel = srd16(m.seg, m.off + 4); }
            else { off = srd16(m.seg, m.off); sel = srd16(m.seg, (u16)(m.off + 2)); }
            if (g_cpu.opsz32) { push32(g_cpu.seg[S_CS]); push32(g_cpu.ip); }
            else { push16(g_cpu.seg[S_CS]); push16((u16)g_cpu.ip); }
            far_jump(sel, off);
            break;
        }
        case 4: { /* JMP near indirect */
            u32 t = g_cpu.opsz32 ? rm_rd32(&m) : rm_rd16(&m);
            g_cpu.ip = t & 0xFFFF;
            break;
        }
        case 5: { /* JMP far indirect */
            u32 off; u16 sel;
            if (m.mod == 3) { fault("Invalid opcode (far jmp register)", -1, 0); break; }
            if (g_cpu.opsz32) { off = srd32(m.seg, m.off); sel = srd16(m.seg, m.off + 4); }
            else { off = srd16(m.seg, m.off); sel = srd16(m.seg, (u16)(m.off + 2)); }
            far_jump(sel, off);
            break;
        }
        case 6:
            if (g_cpu.opsz32) push32(rm_rd32(&m)); else push16(rm_rd16(&m));
            break;
        default:
            fault("Invalid opcode (FF group)", -1, 0);
        }
        break;
    default: {
        char msg[64];
        snprintf(msg, sizeof msg, "Invalid opcode %02X", op);
        fault(msg, -1, 0);
    }
    }
}

/* ---- string instructions ---- */

static void exec_string(u8 op)
{
    int src = g_cpu.seg_ovr != S_NONE ? g_cpu.seg_ovr : S_DS;
    int sz = (op & 1) ? (g_cpu.opsz32 ? 4 : 2) : 1;
    int d = (FL & F_DF) ? -sz : sz;
    int a32 = g_cpu.adsz32;
    int rep = g_cpu.rep;
    u32 count;

#define SI_ (a32 ? ESI : (u32)SI)
#define DI_ (a32 ? EDI : (u32)DI)
#define ADV_SI() do { if (a32) ESI += d; else SI = (u16)(SI + d); } while (0)
#define ADV_DI() do { if (a32) EDI += d; else DI = (u16)(DI + d); } while (0)
#define CNT_ (a32 ? ECX : (u32)CX)
#define SET_CNT(v) do { if (a32) ECX = (v); else CX = (u16)(v); } while (0)

    count = rep ? CNT_ : 1;
    if (rep && count == 0) return;

    switch (op) {
    case 0xA4: case 0xA5: /* MOVS */
        while (count) {
            if (sz == 1) swr8(S_ES, DI_, srd8(src, SI_));
            else if (sz == 2) swr16(S_ES, DI_, srd16(src, SI_));
            else swr32(S_ES, DI_, srd32(src, SI_));
            ADV_SI(); ADV_DI();
            count--;
        }
        if (rep) SET_CNT(0);
        break;
    case 0xAA: case 0xAB: /* STOS */
        while (count) {
            if (sz == 1) swr8(S_ES, DI_, AL);
            else if (sz == 2) swr16(S_ES, DI_, AX);
            else swr32(S_ES, DI_, EAX);
            ADV_DI();
            count--;
        }
        if (rep) SET_CNT(0);
        break;
    case 0xAC: case 0xAD: /* LODS */
        while (count) {
            if (sz == 1) AL = srd8(src, SI_);
            else if (sz == 2) AX = srd16(src, SI_);
            else EAX = srd32(src, SI_);
            ADV_SI();
            count--;
        }
        if (rep) SET_CNT(0);
        break;
    case 0xA6: case 0xA7: /* CMPS */
        while (count) {
            u32 a, b;
            if (sz == 1) { a = srd8(src, SI_); b = srd8(S_ES, DI_); op_sub(a, b, 0, 8); }
            else if (sz == 2) { a = srd16(src, SI_); b = srd16(S_ES, DI_); op_sub(a, b, 0, 16); }
            else { a = srd32(src, SI_); b = srd32(S_ES, DI_); op_sub(a, b, 0, 32); }
            ADV_SI(); ADV_DI();
            count--;
            if (rep) {
                SET_CNT(count);
                if (rep == 0xF3 && !(FL & F_ZF)) break;
                if (rep == 0xF2 && (FL & F_ZF)) break;
            }
        }
        break;
    case 0xAE: case 0xAF: /* SCAS */
        while (count) {
            if (sz == 1) op_sub(AL, srd8(S_ES, DI_), 0, 8);
            else if (sz == 2) op_sub(AX, srd16(S_ES, DI_), 0, 16);
            else op_sub(EAX, srd32(S_ES, DI_), 0, 32);
            ADV_DI();
            count--;
            if (rep) {
                SET_CNT(count);
                if (rep == 0xF3 && !(FL & F_ZF)) break;
                if (rep == 0xF2 && (FL & F_ZF)) break;
            }
        }
        break;
    case 0x6C: case 0x6D: /* INS */
        while (count) {
            if (sz == 1) swr8(S_ES, DI_, 0xFF); else if (sz == 2) swr16(S_ES, DI_, 0xFFFF); else swr32(S_ES, DI_, 0xFFFFFFFFu);
            ADV_DI();
            count--;
        }
        if (rep) SET_CNT(0);
        break;
    case 0x6E: case 0x6F: /* OUTS */
        while (count) { ADV_SI(); count--; }
        if (rep) SET_CNT(0);
        break;
    }
#undef SI_
#undef DI_
#undef ADV_SI
#undef ADV_DI
#undef CNT_
#undef SET_CNT
}

/* ---- two-byte opcodes ---- */

static u32 bitop(int which, u32 v, unsigned bit, int sz)
{
    u32 mask = 1u << (bit & (sz - 1));
    if (v & mask) FL |= F_CF; else FL &= ~F_CF;
    switch (which) {
    case 1: return v | mask;   /* BTS */
    case 2: return v & ~mask;  /* BTR */
    case 3: return v ^ mask;   /* BTC */
    }
    return v;
}

static void exec_0f(void)
{
    u8 op = fetch8();
    ModRM m;
    switch (op) {
    case 0x00: { /* SLDT/STR/LLDT/LTR/VERR/VERW */
        decode_modrm(&m);
        switch (m.reg) {
        case 0: rm_wr16(&m, 0); break;  /* SLDT: hide the LDT */
        case 1: rm_wr16(&m, 0); break;  /* STR */
        case 4: case 5: {
            u16 sel = rm_rd16(&m);
            if (sel_valid(sel) && (m.reg == 4 || (g_ldt[SEL_INDEX(sel)].flags & DESC_DATA))) FL |= F_ZF;
            else FL &= ~F_ZF;
            break;
        }
        default: fault("Privileged instruction (LLDT/LTR)", -1, 0);
        }
        break;
    }
    case 0x01: {
        decode_modrm(&m);
        if (m.reg == 4) rm_wr16(&m, 0xFFF1); /* SMSW: PE set */
        else if (m.reg == 0 || m.reg == 1) { /* SGDT/SIDT: write zeros */
            swr16(m.seg, m.off, 0); swr32(m.seg, m.off + 2, 0);
        } else fault("Privileged instruction (0F 01)", -1, 0);
        break;
    }
    case 0x02: case 0x03: { /* LAR / LSL */
        u16 sel;
        decode_modrm(&m);
        sel = rm_rd16(&m);
        if (sel_valid(sel)) {
            Descriptor *dsc = &g_ldt[SEL_INDEX(sel)];
            u32 v;
            if (op == 0x02) v = (dsc->flags & DESC_CODE) ? 0x00FB00 : 0x00F300;
            else v = dsc->limit;
            if (g_cpu.opsz32) g_cpu.r[m.reg] = v; else setr16(m.reg, (u16)v);
            FL |= F_ZF;
        } else FL &= ~F_ZF;
        break;
    }
    case 0x06: break; /* CLTS */
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
        s32 d = g_cpu.opsz32 ? (s32)fetch32() : (s16)fetch16();
        if (cond(op & 15)) g_cpu.ip = (g_cpu.ip + d) & 0xFFFF;
        break;
    }
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        decode_modrm(&m);
        rm_wr8(&m, cond(op & 15) ? 1 : 0);
        break;
    case 0xA0: if (g_cpu.opsz32) push32(g_cpu.seg[S_FS]); else push16(g_cpu.seg[S_FS]); break;
    case 0xA1: cpu_load_seg(S_FS, g_cpu.opsz32 ? (u16)pop32() : pop16()); break;
    case 0xA8: if (g_cpu.opsz32) push32(g_cpu.seg[S_GS]); else push16(g_cpu.seg[S_GS]); break;
    case 0xA9: cpu_load_seg(S_GS, g_cpu.opsz32 ? (u16)pop32() : pop16()); break;
    case 0xA2: /* CPUID: report a 486 */
        if (EAX == 0) { EAX = 1; EBX = 0x756E6547; EDX = 0x49656E69; ECX = 0x6C65746E; }
        else { EAX = 0x0400; EBX = 0; ECX = 0; EDX = 0; }
        break;
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: { /* BT/BTS/BTR/BTC r/m, reg */
        int which = op == 0xA3 ? 0 : op == 0xAB ? 1 : op == 0xB3 ? 2 : 3;
        decode_modrm(&m);
        if (g_cpu.opsz32) {
            s32 bit = (s32)g_cpu.r[m.reg];
            if (m.mod != 3) { m.off += (u32)((bit >> 5) * 4); bit &= 31; }
            { u32 v = rm_rd32(&m); u32 r = bitop(which, v, (unsigned)bit, 32); if (which) rm_wr32(&m, r); }
        } else {
            s16 bit = (s16)getr16(m.reg);
            if (m.mod != 3) { m.off = (u16)(m.off + (bit >> 4) * 2); bit &= 15; }
            { u16 v = rm_rd16(&m); u16 r = (u16)bitop(which, v, (unsigned)bit, 16); if (which) rm_wr16(&m, r); }
        }
        break;
    }
    case 0xBA: { /* BT group imm8 */
        u8 bit;
        decode_modrm(&m);
        bit = fetch8();
        if (m.reg < 4) { fault("Invalid opcode (0F BA)", -1, 0); break; }
        if (g_cpu.opsz32) { u32 v = rm_rd32(&m); u32 r = bitop(m.reg - 4, v, bit, 32); if (m.reg != 4) rm_wr32(&m, r); }
        else { u16 v = rm_rd16(&m); u16 r = (u16)bitop(m.reg - 4, v, bit, 16); if (m.reg != 4) rm_wr16(&m, r); }
        break;
    }
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: { /* SHLD / SHRD */
        unsigned cnt;
        decode_modrm(&m);
        cnt = (op == 0xA4 || op == 0xAC) ? fetch8() : CL;
        cnt &= 31;
        if (!cnt) break;
        if (g_cpu.opsz32) {
            u32 d = rm_rd32(&m), s = g_cpu.r[m.reg], r;
            u32 cf;
            if (op < 0xAC) { r = (d << cnt) | (s >> (32 - cnt)); cf = (d >> (32 - cnt)) & 1; }
            else { r = (d >> cnt) | (s << (32 - cnt)); cf = (d >> (cnt - 1)) & 1; }
            FL &= ~F_ARITH; if (cf) FL |= F_CF;
            if ((r ^ d) & 0x80000000u) FL |= F_OF;
            set_szp(r, 32);
            rm_wr32(&m, r);
        } else {
            u32 d = rm_rd16(&m), s = getr16(m.reg), r, cf;
            u32 wide;
            if (cnt > 16) cnt = 16; /* undefined; clamp */
            if (op < 0xAC) {
                wide = (d << 16) | s;
                r = (u16)((wide << cnt) >> 16);
                cf = (d >> (16 - cnt)) & 1;
            } else {
                wide = (s << 16) | d;
                r = (u16)(wide >> cnt);
                cf = (d >> (cnt - 1)) & 1;
            }
            FL &= ~F_ARITH; if (cf) FL |= F_CF;
            if ((r ^ d) & 0x8000) FL |= F_OF;
            set_szp(r, 16);
            rm_wr16(&m, (u16)r);
        }
        break;
    }
    case 0xAF: /* IMUL Gv, Ev */
        decode_modrm(&m);
        if (g_cpu.opsz32) {
            s64 r = (s64)(s32)g_cpu.r[m.reg] * (s32)rm_rd32(&m);
            g_cpu.r[m.reg] = (u32)r;
            FL &= ~(F_CF | F_OF); if (r != (s32)r) FL |= F_CF | F_OF;
        } else {
            s32 r = (s32)(s16)getr16(m.reg) * (s16)rm_rd16(&m);
            setr16(m.reg, (u16)r);
            FL &= ~(F_CF | F_OF); if (r != (s16)r) FL |= F_CF | F_OF;
        }
        break;
    case 0xB2: case 0xB4: case 0xB5: { /* LSS / LFS / LGS */
        int s = op == 0xB2 ? S_SS : op == 0xB4 ? S_FS : S_GS;
        decode_modrm(&m);
        if (g_cpu.opsz32) {
            u32 off = srd32(m.seg, m.off); u16 sel = srd16(m.seg, m.off + 4);
            if (cpu_load_seg(s, sel)) g_cpu.r[m.reg] = off;
        } else {
            u16 off = srd16(m.seg, m.off); u16 sel = srd16(m.seg, (u16)(m.off + 2));
            if (cpu_load_seg(s, sel)) setr16(m.reg, off);
        }
        break;
    }
    case 0xB6: decode_modrm(&m); if (g_cpu.opsz32) g_cpu.r[m.reg] = rm_rd8(&m); else setr16(m.reg, rm_rd8(&m)); break;
    case 0xB7: decode_modrm(&m); if (g_cpu.opsz32) g_cpu.r[m.reg] = rm_rd16(&m); else setr16(m.reg, rm_rd16(&m)); break;
    case 0xBE: decode_modrm(&m); if (g_cpu.opsz32) g_cpu.r[m.reg] = (u32)(s32)(s8)rm_rd8(&m); else setr16(m.reg, (u16)(s16)(s8)rm_rd8(&m)); break;
    case 0xBF: decode_modrm(&m); if (g_cpu.opsz32) g_cpu.r[m.reg] = (u32)(s32)(s16)rm_rd16(&m); else setr16(m.reg, rm_rd16(&m)); break;
    case 0xBC: case 0xBD: { /* BSF / BSR */
        u32 v;
        int i;
        decode_modrm(&m);
        v = g_cpu.opsz32 ? rm_rd32(&m) : rm_rd16(&m);
        if (!v) { FL |= F_ZF; break; }
        FL &= ~F_ZF;
        if (op == 0xBC) { for (i = 0; !(v & (1u << i)); i++) ; }
        else { for (i = 31; !(v & (1u << i)); i--) ; }
        if (g_cpu.opsz32) g_cpu.r[m.reg] = (u32)i; else setr16(m.reg, (u16)i);
        break;
    }
    case 0xC0: { /* XADD Eb,Gb */
        u8 a, b;
        decode_modrm(&m);
        a = rm_rd8(&m); b = getr8(m.reg);
        rm_wr8(&m, (u8)op_add(a, b, 0, 8));
        setr8(m.reg, a);
        break;
    }
    case 0xC1:
        decode_modrm(&m);
        if (g_cpu.opsz32) { u32 a = rm_rd32(&m), b = g_cpu.r[m.reg]; rm_wr32(&m, op_add(a, b, 0, 32)); g_cpu.r[m.reg] = a; }
        else { u16 a = rm_rd16(&m), b = getr16(m.reg); rm_wr16(&m, (u16)op_add(a, b, 0, 16)); setr16(m.reg, a); }
        break;
    case 0xB0: { /* CMPXCHG Eb,Gb */
        u8 v;
        decode_modrm(&m);
        v = rm_rd8(&m);
        op_sub(AL, v, 0, 8);
        if (FL & F_ZF) rm_wr8(&m, getr8(m.reg)); else AL = v;
        break;
    }
    case 0xB1:
        decode_modrm(&m);
        if (g_cpu.opsz32) {
            u32 v = rm_rd32(&m);
            op_sub(EAX, v, 0, 32);
            if (FL & F_ZF) rm_wr32(&m, g_cpu.r[m.reg]); else EAX = v;
        } else {
            u16 v = rm_rd16(&m);
            op_sub(AX, v, 0, 16);
            if (FL & F_ZF) rm_wr16(&m, getr16(m.reg)); else AX = v;
        }
        break;
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        g_cpu.r[op & 7] = __builtin_bswap32(g_cpu.r[op & 7]);
        break;
    default: {
        char msg[64];
        snprintf(msg, sizeof msg, "Invalid opcode 0F %02X", op);
        fault(msg, -1, 0);
    }
    }
}

void cpu_run(void)
{
    g_cpu.depth++;
    while (!g_cpu.stop) {
        step();
        if (!(g_cpu.icount & 0x3FFFF)) cpu_periodic();
    }
    g_cpu.stop = 0;
    g_cpu.depth--;
}

/* exposed for unit tests */
u32 cpu_test_alu(int op, u32 a, u32 b, int sz, u32 *flags)
{
    u32 r;
    FL = *flags;
    r = alu(op, a, b, sz);
    *flags = FL;
    return r;
}
u32 cpu_test_shift(int op, u32 a, unsigned cnt, int sz, u32 *flags)
{
    u32 r;
    FL = *flags;
    r = shift_op(op, a, cnt & 31, sz);
    *flags = FL;
    return r;
}
u32 cpu_test_incdec(int dec, u32 a, int sz, u32 *flags)
{
    u32 r;
    FL = *flags;
    r = dec ? op_dec(a, sz) : op_inc(a, sz);
    *flags = FL;
    return r;
}
