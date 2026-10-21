/*
 * dos.c - INT 21h (DOS3Call) services used by the Win16 C runtime, plus the
 *         DOS file handle table shared with KERNEL.OpenFile.
 */
#include "win16.h"

#define MAX_DOS_FILES 256

static HANDLE g_files[MAX_DOS_FILES];
static u32 g_dta;                    /* segptr of the disk transfer area */
static u16 g_last_error;
static u32 g_vectors[256];
u16 g_psp_sel;

typedef struct {
    HANDLE h;
    u16 attr;
    char pattern[MAX_PATH];
} FindState;
static FindState g_find[32];

static void set_cf(int err)
{
    if (err) { g_cpu.flags |= F_CF; AX = (u16)err; g_last_error = (u16)err; }
    else g_cpu.flags &= ~F_CF;
}

static u16 dos_error(DWORD w)
{
    switch (w) {
    case ERROR_FILE_NOT_FOUND: return 2;
    case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: case ERROR_BAD_PATHNAME: case ERROR_INVALID_DRIVE: return 3;
    case ERROR_TOO_MANY_OPEN_FILES: return 4;
    case ERROR_ACCESS_DENIED: case ERROR_WRITE_PROTECT: return 5;
    case ERROR_INVALID_HANDLE: return 6;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return 8;
    case ERROR_NOT_SAME_DEVICE: return 17;
    case ERROR_NO_MORE_FILES: return 18;
    case ERROR_SHARING_VIOLATION: return 32;
    case ERROR_LOCK_VIOLATION: return 33;
    case ERROR_HANDLE_DISK_FULL: case ERROR_DISK_FULL: return 39;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return 80;
    case ERROR_DIR_NOT_EMPTY: return 5;
    }
    return 5;
}

void dos_init(void)
{
    g_files[0] = GetStdHandle(STD_INPUT_HANDLE);
    g_files[1] = GetStdHandle(STD_OUTPUT_HANDLE);
    g_files[2] = GetStdHandle(STD_ERROR_HANDLE);
    g_files[3] = INVALID_HANDLE_VALUE;
    g_files[4] = INVALID_HANDLE_VALUE;
    g_dta = ((u32)g_psp_sel << 16) | 0x80;
}

int dos_alloc_handle(HANDLE h)
{
    int i;
    for (i = 5; i < MAX_DOS_FILES; i++) {
        if (!g_files[i]) { g_files[i] = h; return i; }
    }
    return -1;
}

HANDLE dos_handle(u16 fd)
{
    if (fd >= MAX_DOS_FILES) return NULL;
    return g_files[fd];
}

int dos_close(u16 fd)
{
    if (fd < 5 || fd >= MAX_DOS_FILES || !g_files[fd]) return 6;
    CloseHandle(g_files[fd]);
    g_files[fd] = NULL;
    return 0;
}

/* convert a DOS path from the application to a native path */
const char *dos_path(const char *p, char *buf, int len)
{
    int i;
    snprintf(buf, len, "%s", p);
    for (i = 0; buf[i]; i++) if (buf[i] == '/') buf[i] = '\\';
    /* strip trailing spaces/dots DOS would ignore */
    for (i = (int)strlen(buf) - 1; i >= 0 && buf[i] == ' '; i--) buf[i] = 0;
    return buf;
}

/* open a file; mode = DOS access/share byte; returns DOS handle or -error */
int dos_open(const char *name, u16 mode, int create, u16 attr)
{
    char path[MAX_PATH];
    DWORD access, disp, flags = FILE_ATTRIBUTE_NORMAL;
    HANDLE h;
    int fd;
    dos_path(name, path, sizeof path);
    if (create) {
        access = GENERIC_READ | GENERIC_WRITE;
        disp = (create == 2) ? CREATE_NEW : CREATE_ALWAYS;
        if (attr & 1) flags = FILE_ATTRIBUTE_READONLY;
        if (attr & 2) flags |= FILE_ATTRIBUTE_HIDDEN;
        if (attr & 4) flags |= FILE_ATTRIBUTE_SYSTEM;
        if (attr & 0x20) flags |= FILE_ATTRIBUTE_ARCHIVE;
    } else {
        switch (mode & 7) {
        case 0: access = GENERIC_READ; break;
        case 1: access = GENERIC_WRITE; break;
        default: access = GENERIC_READ | GENERIC_WRITE; break;
        }
        disp = OPEN_EXISTING;
    }
    h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, disp, flags, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        emu_log("DOS open '%s' mode %x create %d failed: %lu", path, mode, create, e);
        return -(int)dos_error(e);
    }
    fd = dos_alloc_handle(h);
    if (fd < 0) { CloseHandle(h); return -4; }
    emu_log("DOS open '%s' -> %d", path, fd);
    return fd;
}

static void dos_time(FILETIME *ft, u16 *time, u16 *date)
{
    FILETIME lt;
    WORD d = 0, t = 0;
    FileTimeToLocalFileTime(ft, &lt);
    FileTimeToDosDateTime(&lt, &d, &t);
    *time = t;
    *date = d;
}

static int is_83(const char *s)
{
    const char *dot = strchr(s, '.');
    size_t n = strlen(s);
    if (strchr(s, ' ')) return 0;
    if (!dot) return n <= 8;
    if (strchr(dot + 1, '.')) return 0;
    return (size_t)(dot - s) <= 8 && strlen(dot + 1) <= 3 && dot != s;
}

static int find_fill(int slot)
{
    WIN32_FIND_DATAA fd;
    FindState *f = &g_find[slot];
    u8 *dta = (u8 *)ptrseg(g_dta);
    for (;;) {
        const char *name;
        u16 attr, t, d;
        if (f->h == INVALID_HANDLE_VALUE) return 18;
        if (!FindNextFileA(f->h, &fd)) {
            FindClose(f->h);
            f->h = INVALID_HANDLE_VALUE;
            return 18;
        }
        name = fd.cFileName;
        if (!is_83(name)) {
            if (fd.cAlternateFileName[0]) name = fd.cAlternateFileName;
            else continue;
        }
        attr = (u16)(fd.dwFileAttributes & 0x37);
        if ((attr & 0x10) && !(f->attr & 0x10)) continue;
        if ((attr & 0x06) && !(f->attr & 0x06)) continue;
        dos_time(&fd.ftLastWriteTime, &t, &d);
        memset(dta, 0, 43);
        dta[0] = (u8)slot;
        dta[1] = 0xC5;   /* signature */
        dta[21] = (u8)attr;
        dta[22] = (u8)t; dta[23] = (u8)(t >> 8);
        dta[24] = (u8)d; dta[25] = (u8)(d >> 8);
        dta[26] = (u8)fd.nFileSizeLow; dta[27] = (u8)(fd.nFileSizeLow >> 8);
        dta[28] = (u8)(fd.nFileSizeLow >> 16); dta[29] = (u8)(fd.nFileSizeLow >> 24);
        {
            int i;
            for (i = 0; name[i] && i < 12; i++) dta[30 + i] = (u8)toupper((unsigned char)name[i]);
            dta[30 + i] = 0;
        }
        return 0;
    }
}

static int find_first(const char *spec, u16 attr)
{
    int slot;
    WIN32_FIND_DATAA fd;
    char path[MAX_PATH];
    HANDLE h;
    for (slot = 0; slot < 32; slot++) if (!g_find[slot].h || g_find[slot].h == INVALID_HANDLE_VALUE) break;
    if (slot == 32) {
        /* recycle */
        for (slot = 0; slot < 32; slot++) { if (g_find[slot].h && g_find[slot].h != INVALID_HANDLE_VALUE) FindClose(g_find[slot].h); g_find[slot].h = NULL; }
        slot = 0;
    }
    dos_path(spec, path, sizeof path);
    h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE) return 18;
    /* rewind trick: we process the first entry through the same filter by re-running */
    FindClose(h);
    g_find[slot].h = FindFirstFileA(path, &fd);
    g_find[slot].attr = attr;
    snprintf(g_find[slot].pattern, sizeof g_find[slot].pattern, "%s", path);
    /* handle the first entry */
    {
        u8 *dta = (u8 *)ptrseg(g_dta);
        const char *name = fd.cFileName;
        u16 a = (u16)(fd.dwFileAttributes & 0x37), t, d;
        int ok = 1;
        if (!is_83(name)) { if (fd.cAlternateFileName[0]) name = fd.cAlternateFileName; else ok = 0; }
        if ((a & 0x10) && !(attr & 0x10)) ok = 0;
        if ((a & 0x06) && !(attr & 0x06)) ok = 0;
        if (!ok) return find_fill(slot);
        dos_time(&fd.ftLastWriteTime, &t, &d);
        memset(dta, 0, 43);
        dta[0] = (u8)slot;
        dta[1] = 0xC5;
        dta[21] = (u8)a;
        dta[22] = (u8)t; dta[23] = (u8)(t >> 8);
        dta[24] = (u8)d; dta[25] = (u8)(d >> 8);
        dta[26] = (u8)fd.nFileSizeLow; dta[27] = (u8)(fd.nFileSizeLow >> 8);
        dta[28] = (u8)(fd.nFileSizeLow >> 16); dta[29] = (u8)(fd.nFileSizeLow >> 24);
        {
            int i;
            for (i = 0; name[i] && i < 12; i++) dta[30 + i] = (u8)toupper((unsigned char)name[i]);
            dta[30 + i] = 0;
        }
    }
    return 0;
}

static void get_cwd_dos(char *out, int len)
{
    char cwd[MAX_PATH], sp[MAX_PATH];
    const char *p;
    GetCurrentDirectoryA(sizeof cwd, cwd);
    if (GetShortPathNameA(cwd, sp, sizeof sp)) p = sp; else p = cwd;
    snprintf(out, len, "%s", p);
}

void dos_int21(void)
{
    u8 fn = AH;
    switch (fn) {
    case 0x02: case 0x06: case 0x09: /* console output: ignore */
        break;
    case 0x0E: { /* select disk */
        char root[4] = "A:\\";
        root[0] = (char)('A' + DL);
        SetCurrentDirectoryA(root);
        AL = 26;
        break;
    }
    case 0x19: { /* get current drive */
        char cwd[MAX_PATH];
        GetCurrentDirectoryA(sizeof cwd, cwd);
        AL = (u8)(toupper((unsigned char)cwd[0]) - 'A');
        break;
    }
    case 0x1A: g_dta = ((u32)DS << 16) | DX; break;
    case 0x25: g_vectors[AL] = ((u32)DS << 16) | DX; break;
    case 0x35: { u32 v = g_vectors[AL]; cpu_load_seg(S_ES, (u16)(v >> 16)); BX = (u16)v; break; }
    case 0x2A: {
        SYSTEMTIME st;
        zoom_flush_drawing(0);
        GetLocalTime(&st);
        CX = st.wYear; DH = (u8)st.wMonth; DL = (u8)st.wDay; AL = (u8)st.wDayOfWeek;
        break;
    }
    case 0x2B: AL = 0; break;
    case 0x2C: {
        SYSTEMTIME st;
        /* the game times its animations (spell bolts, ...) with busy loops on
           the clock: show what it has drawn so far on the off-screen surfaces */
        zoom_flush_drawing(0);
        GetLocalTime(&st);
        CH = (u8)st.wHour; CL = (u8)st.wMinute; DH = (u8)st.wSecond; DL = (u8)(st.wMilliseconds / 10);
        break;
    }
    case 0x2D: AL = 0; break;
    case 0x2F: cpu_load_seg(S_ES, (u16)(g_dta >> 16)); BX = (u16)g_dta; break;
    case 0x30: AL = 6; AH = 22; BX = 0; CX = 0; break;
    case 0x33:
        if (AL == 0) DL = 0;
        else if (AL == 5) DL = 3;
        else if (AL == 6) { BX = 0x3205; DX = 0; }
        break;
    case 0x36: {
        char root[4] = "C:\\";
        DWORD spc, bps, freec, total;
        if (DL) root[0] = (char)('A' + DL - 1);
        else { char cwd[MAX_PATH]; GetCurrentDirectoryA(sizeof cwd, cwd); root[0] = cwd[0]; }
        if (GetDiskFreeSpaceA(root, &spc, &bps, &freec, &total)) {
            /* express in units that fit 16-bit fields (report up to ~2GB) */
            u32 clsz = spc * bps, fr, tot;
            u64 fb = (u64)freec * clsz, tb = (u64)total * clsz;
            u32 unit = 32768;
            fr = (u32)(fb / unit); tot = (u32)(tb / unit);
            if (fr > 0xFFFE) fr = 0xFFFE;
            if (tot > 0xFFFE) tot = 0xFFFE;
            AX = 64; CX = 512; BX = (u16)fr; DX = (u16)tot;
        } else AX = 0xFFFF;
        break;
    }
    case 0x38: { /* country info */
        u8 *p = (u8 *)ptr16(DS, DX);
        memset(p, 0, 34);
        p[2] = '$'; p[7] = ','; p[9] = '.'; p[11] = '-'; p[13] = ':'; p[17] = 0; p[22] = ',';
        BX = 1;
        set_cf(0);
        break;
    }
    case 0x39: case 0x3A: case 0x3B: {
        char path[MAX_PATH];
        BOOL ok;
        dos_path((const char *)ptr16(DS, DX), path, sizeof path);
        if (fn == 0x39) ok = CreateDirectoryA(path, NULL);
        else if (fn == 0x3A) ok = RemoveDirectoryA(path);
        else ok = SetCurrentDirectoryA(path);
        set_cf(ok ? 0 : dos_error(GetLastError()));
        break;
    }
    case 0x3C: case 0x5B: {
        int r = dos_open((const char *)ptr16(DS, DX), 2, fn == 0x5B ? 2 : 1, CX);
        if (r < 0) set_cf(-r); else { AX = (u16)r; set_cf(0); }
        break;
    }
    case 0x3D: {
        int r = dos_open((const char *)ptr16(DS, DX), AL, 0, 0);
        if (r < 0) set_cf(-r); else { AX = (u16)r; set_cf(0); }
        break;
    }
    case 0x3E: {
        int e = dos_close(BX);
        if (BX < 5) e = 0;
        set_cf(e);
        break;
    }
    case 0x3F: {
        HANDLE h = dos_handle(BX);
        DWORD got = 0;
        if (!h || h == INVALID_HANDLE_VALUE) { if (BX < 5) { AX = 0; set_cf(0); } else set_cf(6); break; }
        if (!ReadFile(h, ptr16(DS, DX), CX, &got, NULL)) { set_cf(dos_error(GetLastError())); break; }
        AX = (u16)got;
        set_cf(0);
        break;
    }
    case 0x40: {
        HANDLE h = dos_handle(BX);
        DWORD put = 0;
        if (BX >= 1 && BX <= 4 && (!h || h == INVALID_HANDLE_VALUE || BX >= 3)) {
            /* console output of a GUI program: log it */
            char tmp[512];
            int n = CX < 511 ? CX : 511;
            memcpy(tmp, ptr16(DS, DX), n);
            tmp[n] = 0;
            emu_log("[stdout] %s", tmp);
            AX = CX;
            set_cf(0);
            break;
        }
        if (!h || h == INVALID_HANDLE_VALUE) { set_cf(6); break; }
        if (CX == 0) {
            if (!SetEndOfFile(h)) { set_cf(dos_error(GetLastError())); break; }
            AX = 0; set_cf(0); break;
        }
        if (!WriteFile(h, ptr16(DS, DX), CX, &put, NULL)) { set_cf(dos_error(GetLastError())); break; }
        AX = (u16)put;
        set_cf(0);
        break;
    }
    case 0x41: {
        char path[MAX_PATH];
        dos_path((const char *)ptr16(DS, DX), path, sizeof path);
        set_cf(DeleteFileA(path) ? 0 : dos_error(GetLastError()));
        break;
    }
    case 0x42: {
        HANDLE h = dos_handle(BX);
        LARGE_INTEGER d, n;
        DWORD how = AL == 0 ? FILE_BEGIN : AL == 1 ? FILE_CURRENT : FILE_END;
        if (!h || h == INVALID_HANDLE_VALUE) { if (BX < 5) { AX = 0; DX = 0; set_cf(0); } else set_cf(6); break; }
        d.QuadPart = (s32)(((u32)CX << 16) | DX);
        if (!SetFilePointerEx(h, d, &n, how)) { set_cf(dos_error(GetLastError())); break; }
        AX = (u16)n.QuadPart;
        DX = (u16)(n.QuadPart >> 16);
        set_cf(0);
        break;
    }
    case 0x43: {
        char path[MAX_PATH];
        dos_path((const char *)ptr16(DS, DX), path, sizeof path);
        if (AL == 0) {
            DWORD a = GetFileAttributesA(path);
            if (a == INVALID_FILE_ATTRIBUTES) { set_cf(dos_error(GetLastError())); break; }
            CX = (u16)(a & 0x37);
            AX = CX;
            set_cf(0);
        } else {
            DWORD a = CX & 0x27;
            if (!a) a = FILE_ATTRIBUTE_NORMAL;
            set_cf(SetFileAttributesA(path, a) ? 0 : dos_error(GetLastError()));
        }
        break;
    }
    case 0x44:
        switch (AL) {
        case 0x00: {
            HANDLE h = dos_handle(BX);
            if (BX < 3) { DX = 0x80D3; AX = DX; set_cf(0); break; }
            if (!h || h == INVALID_HANDLE_VALUE) { set_cf(6); break; }
            {
                char cwd[MAX_PATH];
                GetCurrentDirectoryA(sizeof cwd, cwd);
                DX = (u16)((toupper((unsigned char)cwd[0]) - 'A') & 0x1F);
            }
            AX = DX;
            set_cf(0);
            break;
        }
        case 0x01: set_cf(0); break;
        case 0x07: AL = 0xFF; set_cf(0); break;
        case 0x08: AX = 1; set_cf(0); break;   /* fixed */
        case 0x09: DX = 0; set_cf(0); break;   /* local */
        case 0x0A: DX = 0; set_cf(0); break;
        case 0x0E: AL = 0; set_cf(0); break;
        default: set_cf(1);
        }
        break;
    case 0x45: case 0x46: {
        HANDLE h = dos_handle(BX), d;
        int fd;
        if (!h || h == INVALID_HANDLE_VALUE) { set_cf(6); break; }
        if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS)) { set_cf(6); break; }
        if (fn == 0x46) {
            if (CX < 5 || CX >= MAX_DOS_FILES) { CloseHandle(d); set_cf(6); break; }
            if (g_files[CX]) CloseHandle(g_files[CX]);
            g_files[CX] = d;
            set_cf(0);
            break;
        }
        fd = dos_alloc_handle(d);
        if (fd < 0) { CloseHandle(d); set_cf(4); break; }
        AX = (u16)fd;
        set_cf(0);
        break;
    }
    case 0x47: {
        char cwd[MAX_PATH];
        char *out = (char *)ptr16(DS, SI);
        const char *p;
        get_cwd_dos(cwd, sizeof cwd);
        p = cwd;
        if (p[0] && p[1] == ':') p += 2;
        if (*p == '\\') p++;
        snprintf(out, 64, "%s", p);
        set_cf(0);
        AX = 0x0100;
        break;
    }
    case 0x48: set_cf(8); BX = 0; break;
    case 0x49: set_cf(0); break;
    case 0x4A: set_cf(8); break;
    case 0x4C:
        emu_log("program exit (code %d)", AL);
        emu_shutdown(AL);
        break;
    case 0x4D: AX = 0; break;
    case 0x4E: {
        int e = find_first((const char *)ptr16(DS, DX), CX);
        set_cf(e);
        if (!e) AX = 0;
        break;
    }
    case 0x4F: {
        u8 *dta = (u8 *)ptrseg(g_dta);
        int slot = dta[0];
        int e = (slot < 32 && dta[1] == 0xC5) ? find_fill(slot) : 18;
        set_cf(e);
        if (!e) AX = 0;
        break;
    }
    case 0x50: break;
    case 0x51: case 0x62: BX = g_psp_sel; break;
    case 0x54: AL = 0; break;
    case 0x56: {
        char a[MAX_PATH], b[MAX_PATH];
        dos_path((const char *)ptr16(DS, DX), a, sizeof a);
        dos_path((const char *)ptr16(ES, DI), b, sizeof b);
        set_cf(MoveFileA(a, b) ? 0 : dos_error(GetLastError()));
        break;
    }
    case 0x57: {
        HANDLE h = dos_handle(BX);
        FILETIME ft;
        if (!h || h == INVALID_HANDLE_VALUE) { set_cf(6); break; }
        if (AL == 0) {
            u16 t, d;
            if (!GetFileTime(h, NULL, NULL, &ft)) { set_cf(6); break; }
            dos_time(&ft, &t, &d);
            CX = t; DX = d;
            set_cf(0);
        } else {
            FILETIME lt;
            DosDateTimeToFileTime(DX, CX, &lt);
            LocalFileTimeToFileTime(&lt, &ft);
            SetFileTime(h, NULL, NULL, &ft);
            set_cf(0);
        }
        break;
    }
    case 0x59: AX = g_last_error; BH = 1; BL = 4; CH = 1; break;
    case 0x5A: {
        char *dir = (char *)ptr16(DS, DX);
        char path[MAX_PATH];
        int i, fd = -1;
        size_t n = strlen(dir);
        for (i = 0; i < 1000 && fd < 0; i++) {
            snprintf(path, sizeof path, "%s%s%04X%04X", dir, (n && dir[n - 1] != '\\') ? "\\" : "",
                     (unsigned)(GetTickCount() & 0xFFFF), (unsigned)i);
            fd = dos_open(path, 2, 2, CX);
            if (fd < 0 && fd != -80) break;
        }
        if (fd < 0) { set_cf(-fd); break; }
        strcpy(dir, path);
        AX = (u16)fd;
        set_cf(0);
        break;
    }
    case 0x60: {
        char path[MAX_PATH], full[MAX_PATH];
        dos_path((const char *)ptr16(DS, SI), path, sizeof path);
        if (!GetFullPathNameA(path, sizeof full, full, NULL)) { set_cf(3); break; }
        snprintf((char *)ptr16(ES, DI), 128, "%s", full);
        set_cf(0);
        break;
    }
    case 0x67: set_cf(0); break;
    case 0x68: case 0x6A: {
        HANDLE h = dos_handle(BX);
        if (h && h != INVALID_HANDLE_VALUE && BX >= 5) FlushFileBuffers(h);
        set_cf(0);
        break;
    }
    case 0x6C: {
        const char *name = (const char *)ptr16(DS, SI);
        u16 mode = BX, action = DX;
        int r;
        char path[MAX_PATH];
        DWORD a;
        dos_path(name, path, sizeof path);
        a = GetFileAttributesA(path);
        if (a != INVALID_FILE_ATTRIBUTES) {
            if ((action & 0x0F) == 0) { set_cf(80); break; }
            if ((action & 0x0F) == 2) { r = dos_open(name, mode, 1, CX); CX = 3; }
            else { r = dos_open(name, mode, 0, 0); CX = 1; }
        } else {
            if (!(action & 0x10)) { set_cf(2); break; }
            r = dos_open(name, mode, 1, CX);
            CX = 2;
        }
        if (r < 0) set_cf(-r); else { AX = (u16)r; set_cf(0); }
        break;
    }
    case 0x71: AX = 0x7100; g_cpu.flags |= F_CF; break;
    default:
        emu_log("unsupported INT 21h function AX=%04X", AX);
        set_cf(1);
        break;
    }
}
