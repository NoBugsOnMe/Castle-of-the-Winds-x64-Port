/*
 * native.c - glue between the CPU emulator and the native API layer.
 */
#include "win16.h"

int g_api_argsz;
int g_api_current = -1;
int g_log_enabled;
static FILE *g_logf;
static int g_napi;

HMap g_hwnd_map, g_gdi_map, g_menu_map, g_icon_map;

/* thunk segment layout: id*4 = F1 lo hi 90 */
#define STUB_FIRST 1024
#define STUB_COUNT 1024

static WNDPROC g_stub_procs[STUB_COUNT];
static int g_nstubs;

/* inline code hooks: "F1 lo hi" written over game code; the handler runs
   and execution continues after the escape (see patch.c) */
#define HOOK_FIRST 0x0C00
#define HOOK_COUNT 64
static void (*g_hooks[HOOK_COUNT])(void);
static int g_nhooks;

int code_hook(u16 sel, u16 off, int len, void (*fn)(void))
{
    u8 *p;
    int i, id;
    if (len < 3 || g_nhooks == HOOK_COUNT) return 0;
    id = HOOK_FIRST + g_nhooks;
    g_hooks[g_nhooks++] = fn;
    p = (u8 *)ptr16(sel, off);
    p[0] = 0xF1;
    p[1] = (u8)id;
    p[2] = (u8)(id >> 8);
    for (i = 3; i < len; i++) p[i] = 0x90;
    return 1;
}

static u16 g_inst_sel;               /* MakeProcInstance thunks */
static u8  g_inst_used[8192];

void dos_int21(void);
void cpu_refresh_segs(void);

/* ---- logging ----------------------------------------------------------- */

static void log_open(void)
{
    if (!g_logf && g_log_enabled) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\cotw-port.log", g_data_dir);
        g_logf = fopen(path, "w");
        if (!g_logf) g_logf = fopen("cotw-port.log", "w");
    }
}

void emu_log(const char *fmt, ...)
{
    va_list ap;
    if (!g_log_enabled) return;
    log_open();
    if (!g_logf) return;
    va_start(ap, fmt);
    vfprintf(g_logf, fmt, ap);
    va_end(ap);
    fputc('\n', g_logf);
    fflush(g_logf);
}

void emu_trace_api(const char *fmt, ...)
{
    va_list ap;
    if (g_log_enabled < 2) return;
    log_open();
    if (!g_logf) return;
    va_start(ap, fmt);
    vfprintf(g_logf, fmt, ap);
    va_end(ap);
    fputc('\n', g_logf);
    fflush(g_logf);
}

void emu_shutdown(int code)
{
    if (g_logf) { fflush(g_logf); fclose(g_logf); g_logf = NULL; }
    ExitProcess((UINT)code);
}

void cpu_fatal(const char *fmt, ...)
{
    static int in_fatal;
    char msg[512], state[1024], full[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (in_fatal) emu_shutdown(3);
    in_fatal = 1;
    cpu_dump_state(state, sizeof state);
    snprintf(full, sizeof full,
             "Castle of the Winds encountered an unrecoverable error in the emulated 16-bit code.\r\n\r\n"
             "%s\r\n\r\nLast API: %s\r\n\r\n%s",
             msg,
             g_api_current >= 0 ? g_api_table[g_api_current].name : "(none)",
             state);
    if (!g_log_enabled) g_log_enabled = 1;
    emu_log("FATAL: %s", full);
    MessageBoxA(NULL, full, "Castle of the Winds - Application Error", MB_OK | MB_ICONSTOP | MB_TASKMODAL);
    emu_shutdown(2);
}

/* ---- thunks --------------------------------------------------------------- */

static void put_thunk(u16 id)
{
    u8 *p = (u8 *)ptr16(g_thunk_sel, (u16)(id * 4));
    p[0] = 0xF1;
    p[1] = (u8)id;
    p[2] = (u8)(id >> 8);
    p[3] = 0x90;
}

void thunks_init(void)
{
    int i;
    u16 h;
    for (g_napi = 0; g_api_table[g_napi].name; g_napi++) ;
    h = glob_alloc(G_FIXED, 0x10000, GK_SYSTEM, 1, 0);
    g_thunk_sel = glob_sel(h);
    put_thunk(0);
    for (i = 0; i < g_napi; i++) put_thunk((u16)(i + 1));
    for (i = 0; i < STUB_COUNT; i++) put_thunk((u16)(STUB_FIRST + i));
    put_thunk(0xFFFC / 4);   /* unresolved import trap */
    h = glob_alloc(G_FIXED, 0x10000, GK_SYSTEM, 1, 0);
    g_inst_sel = glob_sel(h);
}

u16 thunk_offset_for(const char *module, u16 ordinal, int *is_equate, u16 *equate_value)
{
    int i;
    *is_equate = 0;
    if (!_stricmp(module, "KERNEL")) {
        switch (ordinal) {
        case 113: *is_equate = 1; *equate_value = 3; return 0;        /* __AHSHIFT */
        case 114: *is_equate = 1; *equate_value = 8; return 0;        /* __AHINCR */
        case 178: *is_equate = 1; *equate_value = 0x0429; return 0;   /* __WINFLAGS */
        }
    }
    for (i = 0; i < g_napi; i++)
        if (g_api_table[i].ordinal == ordinal && !_stricmp(g_api_table[i].module, module))
            return (u16)((i + 1) * 4);
    emu_log("unresolved import %s.%d", module, ordinal);
    /* point at the return marker; calling it will be reported */
    return 0xFFFC;
}

void api_return(int argsz)
{
    u16 ip = pop16();
    u16 cs = pop16();
    SP = (u16)(SP + argsz);
    cpu_load_seg(S_CS, cs);
    g_cpu.ip = ip;
}

static void native_stub_call(int k)
{
    /* (HWND16, UINT16, WPARAM16, LPARAM) pascal, called by 16-bit code */
    u16 ao = (u16)(SP + 4 + 10);
    u16 hwnd, msg, wp;
    u32 lp, r;
    ao -= 2; hwnd = rd16(g_cpu.base[S_SS] + ao);
    ao -= 2; msg = rd16(g_cpu.base[S_SS] + ao);
    ao -= 2; wp = rd16(g_cpu.base[S_SS] + ao);
    ao -= 4; lp = rd32(g_cpu.base[S_SS] + ao);
    r = send_to_32(g_stub_procs[k], HWND_32(hwnd), msg, wp, lp, 0);
    AX = (u16)r;
    DX = (u16)(r >> 16);
    api_return(10);
}

void native_dispatch(u16 id)
{
    if (id == 0) { g_cpu.stop = 1; return; }
    if (id <= g_napi) {
        int prev = g_api_current;
        g_api_current = id - 1;
        g_api_argsz = g_api_table[id - 1].argsize;
        if (g_log_enabled >= 3) {
            u16 rip = rd16(g_cpu.base[S_SS] + SP), rcs = rd16(g_cpu.base[S_SS] + (u16)(SP + 2));
            char args[160];
            int k, n = 0, words = g_api_argsz / 2;
            args[0] = 0;
            /* print parameters in declaration order (highest address first) */
            for (k = words - 1; k >= 0 && n < (int)sizeof args - 8; k--)
                n += snprintf(args + n, sizeof args - n, "%04X ", rd16(g_cpu.base[S_SS] + (u16)(SP + 4 + 2 * k)));
            emu_log("API %s.%s from %04X:%04X [%s]", g_api_table[id - 1].module, g_api_table[id - 1].name, rcs, rip, args);
        }
        g_api_table[id - 1].fn();
        g_api_current = prev;
        return;
    }
    if (id >= STUB_FIRST && id < STUB_FIRST + STUB_COUNT && g_stub_procs[id - STUB_FIRST]) {
        native_stub_call(id - STUB_FIRST);
        return;
    }
    if (id >= HOOK_FIRST && id < HOOK_FIRST + g_nhooks) {
        g_hooks[id - HOOK_FIRST]();
        return;
    }
    if (id == 0xFFFC / 4) cpu_fatal("Call to an unresolved import");
    cpu_fatal("Bad native thunk id %u", id);
}

u32 native_proc_stub(WNDPROC proc)
{
    int i;
    if (!proc) return 0;
    for (i = 0; i < g_nstubs; i++)
        if (g_stub_procs[i] == proc) return ((u32)g_thunk_sel << 16) | (u16)((STUB_FIRST + i) * 4);
    if (g_nstubs == STUB_COUNT) cpu_fatal("Too many native window procedure stubs");
    g_stub_procs[g_nstubs] = proc;
    i = g_nstubs++;
    return ((u32)g_thunk_sel << 16) | (u16)((STUB_FIRST + i) * 4);
}

WNDPROC stub_native_proc(u32 segptr)
{
    u16 off = (u16)segptr;
    int k;
    if ((u16)(segptr >> 16) != g_thunk_sel) return NULL;
    if (off % 4) return NULL;
    k = off / 4 - STUB_FIRST;
    if (k < 0 || k >= g_nstubs) return NULL;
    return g_stub_procs[k];
}

/* ---- MakeProcInstance ------------------------------------------------------- */

u32 make_proc_instance(u32 proc, u16 ds)
{
    int i;
    if (!proc) return 0;
    for (i = 0; i < 8192; i++) {
        if (!g_inst_used[i]) {
            u8 *p = (u8 *)ptr16(g_inst_sel, (u16)(i * 8));
            g_inst_used[i] = 1;
            p[0] = 0xB8; p[1] = (u8)ds; p[2] = (u8)(ds >> 8);       /* mov ax, ds */
            p[3] = 0xEA;                                             /* jmp far proc */
            p[4] = (u8)proc; p[5] = (u8)(proc >> 8);
            p[6] = (u8)(proc >> 16); p[7] = (u8)(proc >> 24);
            return ((u32)g_inst_sel << 16) | (u16)(i * 8);
        }
    }
    cpu_fatal("Out of MakeProcInstance thunks");
    return 0;
}

void free_proc_instance(u32 thunk)
{
    u16 off = (u16)thunk;
    if ((u16)(thunk >> 16) != g_inst_sel || (off & 7)) return;
    g_inst_used[off / 8] = 0;
}

/* ---- calling into 16-bit code -------------------------------------------------- */

static void restore_seg(int s, u16 sel)
{
    if ((sel & 0xFFFC) && !sel_valid(sel)) sel = 0;
    if (!(sel & 0xFFFC) && (s == S_CS || s == S_SS)) return;
    cpu_load_seg(s, sel);
}

u32 call16(u32 fn, u16 ax, int nwords, const u16 *words)
{
    u32 saved_r[8], saved_ip, saved_fl;
    u16 saved_seg[6];
    u32 result;
    int i;

    if (!fn) return 0;
    memcpy(saved_r, g_cpu.r, sizeof saved_r);
    memcpy(saved_seg, g_cpu.seg, sizeof saved_seg);
    saved_ip = g_cpu.ip;
    saved_fl = g_cpu.flags;

    for (i = 0; i < nwords; i++) push16(words[i]);
    push16(g_thunk_sel);
    push16(0);
    AX = ax;
    cpu_load_seg(S_DS, SS);
    cpu_load_seg(S_ES, SS);
    g_cpu.flags &= ~F_DF;
    if (!cpu_load_seg(S_CS, (u16)(fn >> 16))) return 0;
    g_cpu.ip = fn & 0xFFFF;

    cpu_run();

    result = AX | ((u32)DX << 16);
    memcpy(g_cpu.r, saved_r, sizeof saved_r);
    restore_seg(S_SS, saved_seg[S_SS]);
    restore_seg(S_CS, saved_seg[S_CS]);
    restore_seg(S_DS, saved_seg[S_DS]);
    restore_seg(S_ES, saved_seg[S_ES]);
    restore_seg(S_FS, saved_seg[S_FS]);
    restore_seg(S_GS, saved_seg[S_GS]);
    g_cpu.ip = saved_ip;
    g_cpu.flags = saved_fl;
    return result;
}

u32 call16_wndproc(u32 proc, u16 hinst, u16 hwnd, u16 msg, u16 wp, u32 lp)
{
    u16 w[5];
    w[0] = hwnd;
    w[1] = msg;
    w[2] = wp;
    w[3] = (u16)(lp >> 16);
    w[4] = (u16)lp;
    return call16(proc, hinst ? hinst : SS, 5, w);
}

/* ---- interrupts ------------------------------------------------------------------ */

/* called by the interpreter every 256K instructions: whatever the game drew
   on the zoom / dimming surfaces reaches the screen within ~0.1 s even while
   it runs a loop that makes no system calls (animations) */
void cpu_periodic(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last >= 15) {
        last = now;
        zoom_flush_drawing(100);
    }
}

void cpu_interrupt(u8 vec)
{
    switch (vec) {
    case 0x21:
        dos_int21();
        return;
    case 0x2F:
        if (AX == 0x1600) { AL = 3; AH = 10; }       /* enhanced mode Windows 3.10 */
        else if (AX == 0x1686) AX = 0;               /* running in protected mode */
        return;
    case 0x1A:
        /* the game times animations with the BIOS clock: show what it drew */
        zoom_flush_drawing(0);
        if (AH == 0) {
            SYSTEMTIME st;
            u32 ticks;
            GetLocalTime(&st);
            ticks = (u32)(((st.wHour * 3600u + st.wMinute * 60u + st.wSecond) * 1000u + st.wMilliseconds) / 54.9254);
            CX = (u16)(ticks >> 16);
            DX = (u16)ticks;
            AL = 0;
        }
        return;
    case 0x03:
        return;
    case 0x00:
        cpu_fatal("Integer divide by zero");
        return;
    case 0x04:
        cpu_fatal("Integer overflow (INTO)");
        return;
    case 0x05:
        cpu_fatal("Array bounds exceeded (BOUND)");
        return;
    case 0x31: /* DPMI: a few selector services */
        switch (AX) {
        case 0x0006: {
            u32 b = sel_valid(BX) ? sel_base(BX) : 0;
            CX = (u16)(b >> 16); DX = (u16)b;
            g_cpu.flags &= ~F_CF;
            return;
        }
        case 0x0400:
            AX = 0x005A; BX = 0x0005; CL = 4; DX = 0x0870;
            g_cpu.flags &= ~F_CF;
            return;
        }
        emu_log("unsupported DPMI call AX=%04X", AX);
        g_cpu.flags |= F_CF;
        return;
    default:
        emu_log("ignored INT %02Xh (AX=%04X) at %04X:%04X", vec, AX, CS, g_cpu.start_ip);
        g_cpu.flags |= F_CF;
        return;
    }
}

/* ---- handle maps ------------------------------------------------------------------ */

#define HMAP_FIRST 0x0040
#define HMAP_LAST  0xFFEF

static u32 hhash(void *h)
{
    u64 v = (u64)(ULONG_PTR)h;
    v ^= v >> 17;
    v *= 0x9E3779B97F4A7C15ull;
    return (u32)(v >> 40) & (HMAP_HASH - 1);
}

static int hmap_find_slot(HMap *m, void *h)
{
    u32 i = hhash(h);
    while (m->key[i]) {
        if (m->key[i] == h) return (int)i;
        i = (i + 1) & (HMAP_HASH - 1);
    }
    return -(int)i - 1;
}

static int handle_still_valid(HMap *m, void *h)
{
    switch (m->kind) {
    case 0: return IsWindow((HWND)h);
    case 1: return GetObjectType((HGDIOBJ)h) != 0;
    case 2: return IsMenu((HMENU)h);
    default: {
        ICONINFO ii;
        if (!GetIconInfo((HICON)h, &ii)) return 0;
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        return 1;
    }
    }
}

static void hmap_rehash_remove(HMap *m, int slot)
{
    /* backward-shift deletion for linear probing */
    u32 i = (u32)slot, j = i;
    m->key[i] = NULL;
    for (;;) {
        u32 k;
        j = (j + 1) & (HMAP_HASH - 1);
        if (!m->key[j]) break;
        k = hhash(m->key[j]);
        if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) {
            m->key[i] = m->key[j];
            m->val[i] = m->val[j];
            m->key[j] = NULL;
            i = j;
        }
    }
}

void hmap_remove(HMap *m, void *h)
{
    int s;
    if (!h) return;
    s = hmap_find_slot(m, h);
    if (s < 0) return;
    m->h[m->val[s]] = NULL;
    hmap_rehash_remove(m, s);
    m->count--;
}

static void hmap_gc(HMap *m)
{
    u32 id;
    int freed = 0;
    for (id = HMAP_FIRST; id <= HMAP_LAST; id++) {
        if (m->h[id] && !handle_still_valid(m, m->h[id])) {
            hmap_remove(m, m->h[id]);
            freed++;
        }
    }
    emu_log("handle map %d: collected %d stale handles", m->kind, freed);
}

u16 hmap16(HMap *m, void *h)
{
    int s;
    u32 tries;
    if (!h) return 0;
    s = hmap_find_slot(m, h);
    if (s >= 0) return m->val[s];
    if (m->count > (HMAP_LAST - HMAP_FIRST) - 64) hmap_gc(m);
    for (tries = 0; tries <= HMAP_LAST - HMAP_FIRST; tries++) {
        u16 id = m->next;
        m->next = (u16)(id >= HMAP_LAST ? HMAP_FIRST : id + 1);
        if (!m->h[id]) {
            m->h[id] = h;
            s = hmap_find_slot(m, h);
            m->key[-s - 1] = h;
            m->val[-s - 1] = id;
            m->count++;
            return id;
        }
    }
    cpu_fatal("Out of 16-bit handles (map %d)", m->kind);
    return 0;
}

void *hmap32(HMap *m, u16 id)
{
    if (!id) return NULL;
    return m->h[id];
}

void hmaps_init(void)
{
    g_hwnd_map.kind = 0; g_hwnd_map.next = HMAP_FIRST;
    g_gdi_map.kind = 1;  g_gdi_map.next = HMAP_FIRST;
    g_menu_map.kind = 2; g_menu_map.next = HMAP_FIRST;
    g_icon_map.kind = 3; g_icon_map.next = HMAP_FIRST;
}

/* ---- brush helper ---- */

HBRUSH brush_32(u16 h)
{
    if (h && h < HMAP_FIRST) return (HBRUSH)(ULONG_PTR)h;   /* COLOR_xxx + 1 */
    return (HBRUSH)HGDI_32(h);
}
