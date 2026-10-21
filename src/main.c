/*
 * main.c - entry point of the Windows port of Castle of the Winds.
 *
 * The original 16-bit executable is embedded as resource "NEIMAGE" and is run
 * by the built-in 16-bit x86 emulator; its Win16 API calls are translated to
 * Win32.
 */
#include "win16.h"

HINSTANCE g_hinst32;
char g_game_dir[MAX_PATH];   /* where the port's EXE lives */
char g_data_dir[MAX_PATH];   /* where the game keeps its files (= g_game_dir unless read-only) */
char g_ini_path[MAX_PATH];
char g_exe_name[64] = "CASTLE.EXE";
extern int g_cmdshow;

void kernel_setup_process(const char *cmdtail);
void patches_apply(void);
void dos_init(void);

static void fatal_box(const char *msg)
{
    MessageBoxA(NULL, msg, "Castle of the Winds", MB_OK | MB_ICONSTOP);
}

static int dir_writable(const char *dir)
{
    char path[MAX_PATH];
    HANDLE h;
    snprintf(path, sizeof path, "%s\\~cotw%lu.tmp", dir, GetCurrentProcessId());
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}

/* directory of a native path, in a form the 16-bit game can use */
static int dir_for_win16(WCHAR *path, char *out, int outlen)
{
    WCHAR *slash = wcsrchr(path, L'\\');
    if (slash) *slash = 0;
    return path_for_win16(path, out, outlen);
}

static void setup_paths(void)
{
    WCHAR exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    /* 16-bit programs keep paths in 128-byte buffers; leave room for a file name */
    if (!n || n >= MAX_PATH * 2 || !dir_for_win16(exe, g_game_dir, 100)) {
        char a[MAX_PATH], *p;
        GetModuleFileNameA(NULL, a, sizeof a);
        p = strrchr(a, '\\');
        if (p) *p = 0;
        snprintf(g_game_dir, sizeof g_game_dir, "%s", a);
    }
    snprintf(g_data_dir, sizeof g_data_dir, "%s", g_game_dir);
    snprintf(g_ini_path, sizeof g_ini_path, "%s\\CASTLE.INI", g_game_dir);
    if (!dir_writable(g_game_dir)) {
        /* read-only install location (e.g. Program Files): the settings,
           high scores and default save folder live in %APPDATA% instead */
        WCHAR appdata[MAX_PATH * 2];
        char dir[MAX_PATH];
        DWORD k = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
        if (k && k < MAX_PATH) {
            lstrcatW(appdata, L"\\Castle of the Winds");
            CreateDirectoryW(appdata, NULL);
            lstrcatW(appdata, L"\\x");    /* dir_for_win16 strips the last component */
            if (dir_for_win16(appdata, dir, 100) && dir_writable(dir)) {
                static const char *const files[] = { "CASTLE.INI", "CASTLE1.SCR", "CASTLE2.SCR" };
                int i;
                snprintf(g_data_dir, sizeof g_data_dir, "%s", dir);
                snprintf(g_ini_path, sizeof g_ini_path, "%s\\CASTLE.INI", dir);
                /* carry over existing settings / scores on first use */
                for (i = 0; i < 3; i++) {
                    char src[MAX_PATH], dst[MAX_PATH];
                    snprintf(src, sizeof src, "%s\\%s", g_game_dir, files[i]);
                    snprintf(dst, sizeof dst, "%s\\%s", dir, files[i]);
                    CopyFileA(src, dst, TRUE);
                }
            }
        }
    }
    SetCurrentDirectoryA(g_data_dir);
}

static const u8 *load_image(const char *cmdline, u32 *len, char *name, int namelen)
{
    HRSRC r = FindResourceA(g_hinst32, "NEIMAGE", (LPCSTR)RT_RCDATA);
    const char *opt;
    if (r) {
        HGLOBAL g = LoadResource(g_hinst32, r);
        HRSRC rn = FindResourceA(g_hinst32, "NENAME", (LPCSTR)RT_RCDATA);
        *len = SizeofResource(g_hinst32, r);
        if (rn) {
            DWORD n = SizeofResource(g_hinst32, rn);
            const char *s = (const char *)LockResource(LoadResource(g_hinst32, rn));
            if (n >= (DWORD)namelen) n = namelen - 1;
            memcpy(name, s, n);
            name[n] = 0;
            while (n && (name[n - 1] == 0 || name[n - 1] == '\r' || name[n - 1] == '\n' || name[n - 1] == ' ')) name[--n] = 0;
        }
        return (const u8 *)LockResource(g);
    }
    /* development mode: --ne=path\to\CASTLE1.EXE */
    opt = strstr(cmdline, "--ne=");
    if (opt) {
        char path[MAX_PATH];
        int i = 0;
        FILE *f;
        u8 *buf;
        long sz;
        opt += 5;
        while (*opt && *opt != ' ' && i < MAX_PATH - 1) path[i++] = *opt++;
        path[i] = 0;
        f = fopen(path, "rb");
        if (!f) return NULL;
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = malloc(sz);
        if (fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
        fclose(f);
        *len = (u32)sz;
        {
            const char *b = strrchr(path, '\\');
            snprintf(name, namelen, "%s", b ? b + 1 : path);
            for (i = 0; name[i]; i++) name[i] = (char)toupper((unsigned char)name[i]);
        }
        return buf;
    }
    return NULL;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdline, int show)
{
    const u8 *img;
    u32 len = 0;
    char err[256], tail[256];
    const char *p;
    int i;
    (void)hPrev;

    g_hinst32 = hInst;
    g_cmdshow = show;
    {
        char v[16];
        if (GetEnvironmentVariableA("COTW_LOG", v, sizeof v)) g_log_enabled = atoi(v) ? atoi(v) : 1;
        if (strstr(cmdline, "--log")) g_log_enabled = strstr(cmdline, "--log=2") ? 2 : 1;
    }
    setup_paths();
    zoom_init();
    dark_init();
    hud_init();

    img = load_image(cmdline, &len, g_exe_name, sizeof g_exe_name);
    if (!img) {
        fatal_box("The game data (embedded 16-bit executable) could not be found.");
        return 1;
    }

    mem_init();
    cpu_init();
    hmaps_init();
    thunks_init();
    if (!ne_load(img, len, g_exe_name, err, sizeof err)) {
        char msg[400];
        snprintf(msg, sizeof msg, "Could not load %s:\n%s", g_exe_name, err);
        fatal_box(msg);
        return 1;
    }
    emu_log("loaded %s: %d segments, %d resources, entry %04X:%04X", g_exe_name, g_mod.nseg, g_mod.nres, g_mod.cs, g_mod.ip);
    patches_apply();

    /* command tail for the 16-bit program: drop our own options */
    tail[0] = 0;
    p = cmdline;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (!strncmp(p, "--", 2)) { while (*p && *p != ' ') p++; continue; }
        i = (int)strlen(tail);
        if (i < 120) {
            tail[i++] = ' ';
            while (*p && *p != ' ' && i < 125) tail[i++] = *p++;
            tail[i] = 0;
        }
        while (*p && *p != ' ') p++;
    }
    kernel_setup_process(tail);
    dos_init();

    /* initial register state of a Win16 task */
    {
        extern u16 g_psp_sel;
        u16 sp = g_mod.sp;
        if (!sp) {
            /* SP = end of static data + stack */
            const u8 *se = g_mod.img + g_mod.ne + (g_mod.img[g_mod.ne + 0x22] | (g_mod.img[g_mod.ne + 0x23] << 8)) + 8 * (g_mod.ss - 1);
            u32 minalloc = se[6] | (se[7] << 8);
            if (!minalloc) minalloc = 0x10000;
            sp = (u16)((minalloc + g_mod.stack) & ~1u);
        }
        cpu_load_seg(S_SS, g_mod.segsel[g_mod.ss - 1]);
        SP = sp;
        push16(g_thunk_sel);
        push16(0);
        cpu_load_seg(S_DS, g_mod.dgroup);
        cpu_load_seg(S_ES, g_psp_sel);
        BX = g_mod.stack;
        CX = g_mod.heap;
        DI = g_mod.hInstance;
        SI = 0;
        AX = 0;
        BP = 0;
        DX = 0;
        cpu_load_seg(S_CS, g_mod.segsel[g_mod.cs - 1]);
        g_cpu.ip = g_mod.ip;
    }
    cpu_run();
    emu_log("16-bit program returned");
    emu_shutdown(0);
    return 0;
}
