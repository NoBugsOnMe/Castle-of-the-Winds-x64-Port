/*
 * kernel.c - KERNEL.EXE functions used by Castle of the Winds.
 */
#include "win16.h"

extern u16 g_psp_sel;
u16 g_env_sel;
int g_cmdshow = SW_SHOWNORMAL;
void dos_int21(void);
int  dos_open(const char *name, u16 mode, int create, u16 attr);
int  dos_close(u16 fd);
const char *dos_path(const char *p, char *buf, int len);

#define WIN16_FLAGS 0x0429u   /* WF_PMODE | WF_CPU486 | WF_ENHANCED | WF_80x87 */

/* ---- helpers ------------------------------------------------------------- */

static const char *str16(u32 segptr)
{
    return segptr ? (const char *)ptrseg(segptr) : NULL;
}

/* map an INI file name given by the game to the game directory */
const char *map_ini_name(const char *name, char *buf, int buflen)
{
    if (!name) return NULL;
    if (strchr(name, '\\') || strchr(name, '/') || strchr(name, ':')) {
        snprintf(buf, buflen, "%s", name);
        return buf;
    }
    if (!_stricmp(name, "CASTLE.INI") && g_ini_path[0]) {
        snprintf(buf, buflen, "%s", g_ini_path);
        return buf;
    }
    snprintf(buf, buflen, "%s\\%s", g_data_dir, name);
    return buf;
}

/* ---- startup ---------------------------------------------------------------- */

void KERNEL_InitTask(void)
{
    u16 stacksize = BX, heapsize = CX;
    u32 dsbase = g_cpu.base[S_DS];
    u16 sp_caller = (u16)(SP + 4);
    u16 stacktop = (u16)((sp_caller > stacksize ? sp_caller - stacksize : 0) + 150);
    u16 seg_end = (u16)(glob_size(glob_handle(g_mod.dgroup)) > 0xFFF0 ? 0xFFF0 : glob_size(glob_handle(g_mod.dgroup)));
    u16 heap_start;
    u16 info;
    const char *cmd;

    /* INSTANCEDATA */
    wr16(dsbase + 0x00, 0);
    wr32(dsbase + 0x02, 0);
    wr16(dsbase + 0x08, 0);
    wr16(dsbase + 0x0A, stacktop);
    wr16(dsbase + 0x0C, sp_caller);
    wr16(dsbase + 0x0E, sp_caller);

    /* local heap: after static data and stack */
    heap_start = (u16)(seg_end - heapsize);
    if (heap_start < sp_caller) heap_start = sp_caller;
    if (heapsize) {
        local_init(g_mod.dgroup, heap_start, seg_end);
        info = local_alloc(L_FIXED, 0x2A);
        if (info) {
            u32 p = lin(g_mod.dgroup, info);
            wr16(p + 0x04, 4);
            wr16(p + 0x06, heap_start);
            wr16(p + 0x0A, seg_end - 4);
            wr16(p + 0x18, 0x20);
            wr16(p + 0x24, 0x200);
            wr16(p + 0x26, heapsize);
            wr16(p + 0x28, 0x484C);  /* 'LH' */
            wr16(dsbase + 0x06, info);
        }
    }
    emu_log("InitTask: stack %04X heap %04X..%04X stacktop %04X", sp_caller, heap_start, seg_end, stacktop);

    AX = 1;
    CX = stacktop;
    DX = (u16)g_cmdshow;
    SI = 0;
    DI = g_mod.hInstance;
    cpu_load_seg(S_ES, g_psp_sel);
    cmd = (const char *)ptr16(g_psp_sel, 0x80);
    if (!cmd[0]) BX = 0x80;
    else {
        int i = 1;
        while (cmd[i] == ' ' || cmd[i] == '\t') i++;
        BX = (u16)(0x80 + i);
    }
    api_return(0);
}

void KERNEL_WaitEvent(void) { ARGS(); (void)A16(); RET16(0); }

void KERNEL_GetVersion(void)
{
    ARGS();
    RET32(0x06160A03u);   /* Windows 3.10 on MS-DOS 6.22 */
}

void KERNEL_GetWinFlags(void) { ARGS(); RET32(WIN16_FLAGS); }

void KERNEL_FatalExit(void)
{
    ARGS();
    MessageBoxA(NULL, "The application requested a fatal exit.", "Castle of the Winds", MB_OK | MB_ICONSTOP);
    (void)_argsz;
    emu_shutdown(1);
}

void KERNEL_FatalAppExit(void)
{
    ARGS();
    u16 action = A16();
    const char *msg = str16(A32());
    (void)action;
    MessageBoxA(NULL, msg ? msg : "Fatal application exit", "Castle of the Winds", MB_OK | MB_ICONSTOP | MB_TASKMODAL);
    emu_shutdown(1);
}

void KERNEL_DOS3Call(void)
{
    dos_int21();
    api_return(0);
}

/* ---- local heap ------------------------------------------------------------------ */

void KERNEL_LocalAlloc(void)
{
    ARGS();
    u16 flags = A16(), size = A16();
    u16 h = local_alloc(flags, size);
    emu_trace_api("LocalAlloc(%04X, %u) = %04X", flags, size, h);
    RET16(h);
}

void KERNEL_LocalReAlloc(void)
{
    ARGS();
    u16 h = A16(), size = A16(), flags = A16();
    u16 r = local_realloc(h, size, flags);
    emu_trace_api("LocalReAlloc(%04X, %u, %04X) = %04X", h, size, flags, r);
    RET16(r);
}

void KERNEL_LocalFree(void) { ARGS(); u16 h = A16(); RET16(local_free(h)); }

void KERNEL_LocalLock(void)
{
    ARGS();
    u16 h = A16();
    u16 p = local_lock(h);
    DX = p ? local_heap_sel() : 0;
    AX = p;
    RET0();
}

void KERNEL_LocalUnlock(void) { ARGS(); u16 h = A16(); RET16(local_unlock(h)); }
void KERNEL_LocalSize(void) { ARGS(); u16 h = A16(); RET16(local_size(h)); }

/* ---- global heap ------------------------------------------------------------------ */

void KERNEL_GlobalAlloc(void)
{
    ARGS();
    u16 flags = A16();
    u32 size = A32();
    u16 h = glob_alloc(flags, size, GK_APP, 0, 0);
    emu_trace_api("GlobalAlloc(%04X, %u) = %04X", flags, size, h);
    RET16(h);
}

void KERNEL_GlobalReAlloc(void)
{
    ARGS();
    u16 h = A16();
    u32 size = A32();
    u16 flags = A16();
    u16 r = glob_realloc(h, size, flags);
    emu_trace_api("GlobalReAlloc(%04X, %u, %04X) = %04X", h, size, flags, r);
    RET16(r);
}

void KERNEL_GlobalFree(void)
{
    ARGS();
    u16 h = A16();
    emu_trace_api("GlobalFree(%04X)", h);
    RET16(glob_free(h));
}

void KERNEL_GlobalLock(void)
{
    ARGS();
    u16 h = A16();
    u16 sel = glob_lock(h);
    RET32(sel ? ((u32)sel << 16) : 0);
}

void KERNEL_GlobalUnlock(void) { ARGS(); u16 h = A16(); RET16(glob_unlock(h)); }
void KERNEL_GlobalSize(void) { ARGS(); u16 h = A16(); RET32(glob_size(h)); }

void KERNEL_LockSegment(void)
{
    ARGS();
    u16 seg = A16();
    if (seg == 0xFFFF) seg = DS;
    RET16(glob_handle(seg));
}

void KERNEL_UnlockSegment(void) { ARGS(); (void)A16(); RET16(0); }

void KERNEL_GlobalCompact(void)
{
    ARGS();
    (void)A32();
    RET32(glob_free_space());
}

/* ---- modules ---------------------------------------------------------------------- */

void KERNEL_GetModuleHandle(void)
{
    ARGS();
    u32 name = A32();
    u16 r = 0;
    if ((name >> 16) == 0) {
        if (ne_is_module_handle((u16)name)) r = g_mod.hModule;
    } else {
        char tmp[64];
        const char *s = str16(name), *b;
        size_t n;
        b = strrchr(s, '\\'); b = b ? b + 1 : s;
        snprintf(tmp, sizeof tmp, "%s", b);
        n = strlen(tmp);
        if (n > 4 && !_stricmp(tmp + n - 4, ".EXE")) tmp[n - 4] = 0;
        if (!_stricmp(tmp, g_mod.name)) r = g_mod.hModule;
    }
    RET16(r);
}

void get_module_file_name(char *out, int len)
{
    /* The game derives the location of its high-score file from its own
       path, so report the (writable) data directory here. */
    char sp[MAX_PATH];
    const char *dir = g_data_dir;
    if (GetShortPathNameA(g_data_dir, sp, sizeof sp)) dir = sp;
    snprintf(out, len, "%s\\%s", dir, g_exe_name);
}

void KERNEL_GetModuleFileName(void)
{
    ARGS();
    u16 h = A16();
    u32 buf = A32();
    s16 size = AS16();
    char path[MAX_PATH];
    int n;
    (void)h;
    get_module_file_name(path, sizeof path);
    if (size <= 0) { RET16(0); return; }
    n = (int)strlen(path);
    if (n >= size) n = size - 1;
    memcpy(ptrseg(buf), path, n);
    ((char *)ptrseg(buf))[n] = 0;
    RET16(n);
}

void KERNEL_MakeProcInstance(void)
{
    ARGS();
    u32 proc = A32();
    u16 hinst = A16();
    (void)hinst;
    RET32(make_proc_instance(proc, g_mod.dgroup));
}

void KERNEL_FreeProcInstance(void)
{
    ARGS();
    u32 proc = A32();
    free_proc_instance(proc);
    RET0();
}

/* ---- resources -------------------------------------------------------------------- */

void KERNEL_FindResource(void)
{
    ARGS();
    u16 hinst = A16();
    u32 name = A32(), type = A32();
    int idx = ne_find_resource(type, name);
    (void)hinst;
    emu_trace_api("FindResource -> %d", idx);
    RET16(idx < 0 ? 0 : idx + 1);
}

static int res_by_handle(u16 h)
{
    int i;
    u16 sel = glob_sel(h);
    if (!sel) return -1;
    for (i = 0; i < g_mod.nres; i++)
        if (g_mod.res[i].loaded && glob_sel(g_mod.res[i].loaded) == sel) return i;
    return -1;
}

u16 load_resource16(int idx)
{
    NERes *r;
    const u8 *data;
    u32 len = 0;
    u16 h;
    if (idx < 0 || idx >= g_mod.nres) return 0;
    r = &g_mod.res[idx];
    if (r->loaded) { r->refcount++; return r->loaded; }
    data = ne_res_data(idx, &len);
    if (!data) return 0;
    h = glob_alloc(G_MOVEABLE | G_DISCARDABLE, r->length ? r->length : len, GK_RESOURCE, 0, 0);
    if (!h) return 0;
    memcpy(ptr16(glob_sel(h), 0), data, len);
    r->loaded = h;
    r->refcount = 1;
    return h;
}

void KERNEL_LoadResource(void)
{
    ARGS();
    u16 hinst = A16(), hres = A16();
    (void)hinst;
    RET16(load_resource16((int)hres - 1));
}

void KERNEL_LockResource(void)
{
    ARGS();
    u16 h = A16();
    u16 sel = glob_lock(h);
    RET32(sel ? ((u32)sel << 16) : 0);
}

void KERNEL_FreeResource(void)
{
    ARGS();
    u16 h = A16();
    int i = res_by_handle(h);
    if (i >= 0) {
        NERes *r = &g_mod.res[i];
        if (r->refcount) r->refcount--;
        if (!r->refcount) { glob_free(r->loaded); r->loaded = 0; }
        RET16(0);
        return;
    }
    RET16(glob_free(h) ? 1 : 0);
}

/* ---- files -------------------------------------------------------------------------- */

#define OF_PARSE_   0x0100
#define OF_DELETE_  0x0200
#define OF_CREATE_  0x1000
#define OF_EXIST_   0x4000
#define OF_REOPEN_  0x8000

static void short_full_path(const char *in, char *out, int len)
{
    char full[MAX_PATH], sp[MAX_PATH];
    if (!GetFullPathNameA(in, sizeof full, full, NULL)) snprintf(full, sizeof full, "%s", in);
    if (GetShortPathNameA(full, sp, sizeof sp)) snprintf(out, len, "%s", sp);
    else snprintf(out, len, "%s", full);
}

void KERNEL_OpenFile(void)
{
    ARGS();
    u32 pname = A32(), pofs = A32();
    u16 style = A16();
    OFSTRUCT16 *of = (OFSTRUCT16 *)ptrseg(pofs);
    char name[MAX_PATH], path[MAX_PATH], full[MAX_PATH];
    int fd;
    if (style & OF_REOPEN_) snprintf(name, sizeof name, "%s", of->szPathName);
    else snprintf(name, sizeof name, "%s", str16(pname) ? str16(pname) : "");
    dos_path(name, path, sizeof path);

    /* search: as given (relative to the current directory), then the game directory */
    if (!(style & (OF_CREATE_ | OF_PARSE_)) && !strchr(path, '\\') && !strchr(path, ':')) {
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
            char alt[MAX_PATH];
            snprintf(alt, sizeof alt, "%s\\%s", g_game_dir, path);
            if (GetFileAttributesA(alt) != INVALID_FILE_ATTRIBUTES) snprintf(path, sizeof path, "%s", alt);
        }
    }
    short_full_path(path, full, sizeof full);
    if (of) {
        of->cBytes = sizeof(OFSTRUCT16);
        of->fFixedDisk = 1;
        of->nErrCode = 0;
        snprintf(of->szPathName, sizeof of->szPathName, "%s", full);
    }
    emu_log("OpenFile('%s', %04X) -> %s", name, style, full);
    if (style & OF_PARSE_) { RET16(0); return; }
    if (style & OF_DELETE_) {
        if (DeleteFileA(path)) { RET16(1); }
        else { if (of) of->nErrCode = 2; RET16(0xFFFF); }
        return;
    }
    if (style & OF_CREATE_) fd = dos_open(path, style & 0x73, 1, 0);
    else fd = dos_open(path, style & 0x73, 0, 0);
    if (fd < 0) {
        if (of) of->nErrCode = (u16)-fd;
        RET16(0xFFFF);
        return;
    }
    if (style & OF_EXIST_) { dos_close((u16)fd); RET16(1); return; }
    if (of && (style & OF_CREATE_)) short_full_path(path, of->szPathName, sizeof of->szPathName);
    RET16(fd);
}

void KERNEL_GetTempFileName(void)
{
    ARGS();
    u16 drive = A16();
    const char *prefix = str16(A32());
    u16 unique = A16();
    u32 buf = A32();
    char dir[MAX_PATH], sp[MAX_PATH], out[MAX_PATH];
    UINT r;
    (void)drive;
    if (!GetTempPathA(sizeof dir, dir)) snprintf(dir, sizeof dir, "%s", g_data_dir);
    if (GetShortPathNameA(dir, sp, sizeof sp)) snprintf(dir, sizeof dir, "%s", sp);
    if (strlen(dir) > 100) snprintf(dir, sizeof dir, "%s", g_data_dir);
    r = GetTempFileNameA(dir, prefix ? prefix : "TMP", unique, out);
    if (!r) { RET16(0); return; }
    snprintf((char *)ptrseg(buf), 144, "%s", out);
    emu_log("GetTempFileName -> %s", out);
    RET16(r);
}

void KERNEL_lstrcpy(void)
{
    ARGS();
    u32 dst = A32(), src = A32();
    strcpy((char *)ptrseg(dst), (const char *)ptrseg(src));
    RET32(dst);
}

/* ---- profile ------------------------------------------------------------------------ */

void KERNEL_GetPrivateProfileInt(void)
{
    ARGS();
    const char *sec = str16(A32()), *key = str16(A32());
    s16 def = AS16();
    const char *file = str16(A32());
    char ini[MAX_PATH];
    UINT r = GetPrivateProfileIntA(sec, key, def, map_ini_name(file, ini, sizeof ini));
    RET16(r);
}

void KERNEL_GetPrivateProfileString(void)
{
    ARGS();
    const char *sec = str16(A32()), *key = str16(A32()), *def = str16(A32());
    u32 buf = A32();
    u16 size = A16();
    const char *file = str16(A32());
    char ini[MAX_PATH];
    DWORD r = GetPrivateProfileStringA(sec, key, def ? def : "", (char *)ptrseg(buf), size, map_ini_name(file, ini, sizeof ini));
    RET16(r);
}

void KERNEL_WritePrivateProfileString(void)
{
    ARGS();
    const char *sec = str16(A32()), *key = str16(A32()), *val = str16(A32()), *file = str16(A32());
    char ini[MAX_PATH];
    BOOL r = WritePrivateProfileStringA(sec, key, val, map_ini_name(file, ini, sizeof ini));
    RET16(r ? 1 : 0);
}

void KERNEL_GetDOSEnvironment(void)
{
    ARGS();
    RET32((u32)g_env_sel << 16);
}

/* ---- process setup (called from main) ------------------------------------------------ */

void kernel_setup_process(const char *cmdtail)
{
    u8 *psp, *env;
    char exe[MAX_PATH], tmp[MAX_PATH], sp[MAX_PATH];
    size_t n, pos = 0;
    int i;

    g_psp_sel = sys_alloc(0x100, 0);
    g_env_sel = sys_alloc(0x800, 0);
    psp = (u8 *)ptr16(g_psp_sel, 0);
    psp[0] = 0xCD; psp[1] = 0x20;
    psp[2] = 0xFF; psp[3] = 0x9F;
    psp[0x2C] = (u8)g_env_sel; psp[0x2D] = (u8)(g_env_sel >> 8);
    psp[0x32] = 20;
    for (i = 0; i < 20; i++) psp[0x18 + i] = (u8)(i < 5 ? i : 0xFF);
    n = strlen(cmdtail);
    if (n > 125) n = 125;
    psp[0x80] = (u8)n;
    memcpy(psp + 0x81, cmdtail, n);
    psp[0x81 + n] = 0x0D;

    env = (u8 *)ptr16(g_env_sel, 0);
    if (GetTempPathA(sizeof tmp, tmp)) {
        size_t l;
        if (GetShortPathNameA(tmp, sp, sizeof sp)) snprintf(tmp, sizeof tmp, "%s", sp);
        l = strlen(tmp);
        if (l > 3 && tmp[l - 1] == '\\') tmp[l - 1] = 0;
    } else snprintf(tmp, sizeof tmp, "C:\\TEMP");
    pos += sprintf((char *)env + pos, "COMSPEC=C:\\COMMAND.COM") + 1;
    pos += sprintf((char *)env + pos, "PATH=C:\\WINDOWS;C:\\WINDOWS\\SYSTEM") + 1;
    pos += sprintf((char *)env + pos, "TEMP=%s", tmp) + 1;
    pos += sprintf((char *)env + pos, "TMP=%s", tmp) + 1;
    pos += sprintf((char *)env + pos, "windir=C:\\WINDOWS") + 1;
    env[pos++] = 0;
    env[pos++] = 1;
    env[pos++] = 0;
    get_module_file_name(exe, sizeof exe);
    snprintf((char *)env + pos, 0x800 - pos, "%s", exe);
}
