/*
 * commdlg.c - COMMDLG.DLL GetOpenFileName / GetSaveFileName.
 *
 * The dialogs run through the Unicode API; the chosen path is then turned
 * into something the 16-bit game can store: an ANSI path that fits its
 * buffer, falling back to the 8.3 short form for long paths or for folder
 * names with characters outside the ANSI code page.
 */
#include "win16.h"
#include <commdlg.h>

static const char *str16(u32 p) { return p ? (const char *)ptrseg(p) : NULL; }

/* ANSI string (possibly double-NUL terminated list) -> malloc'd wide copy */
static WCHAR *to_wide(const char *s, int multi)
{
    int n;
    WCHAR *w;
    if (!s) return NULL;
    if (multi) {
        const char *p = s;
        while (*p) p += strlen(p) + 1;
        n = (int)(p - s) + 1;
    } else {
        n = (int)strlen(s) + 1;
    }
    w = calloc((size_t)n + 1, sizeof(WCHAR));
    if (w) MultiByteToWideChar(CP_ACP, 0, s, n, w, n);
    return w;
}

/* wide -> ANSI without lossy best-fit mapping; 1 if exact and it fits */
static int narrow_exact(const WCHAR *w, char *out, int outlen)
{
    BOOL lossy = FALSE;
    int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, -1, out, outlen, NULL, &lossy);
    return n > 0 && !lossy;
}

/* Converts a native path to one the game can use (ANSI, < maxlen chars).
   The file itself need not exist (save dialog): then only the folder part
   is shortened. Returns 0 if no usable form exists. */
int path_for_win16(const WCHAR *path, char *out, int maxlen)
{
    WCHAR sp[MAX_PATH * 2], dir[MAX_PATH * 2], *slash;
    char tmp[MAX_PATH * 2];
    if (narrow_exact(path, tmp, sizeof tmp) && (int)strlen(tmp) < maxlen) { strcpy(out, tmp); return 1; }
    if (GetShortPathNameW(path, sp, MAX_PATH * 2) && narrow_exact(sp, tmp, sizeof tmp) && (int)strlen(tmp) < maxlen) {
        strcpy(out, tmp);
        return 1;
    }
    /* file does not exist yet: shorten the folder, keep the name */
    lstrcpynW(dir, path, MAX_PATH * 2);
    slash = wcsrchr(dir, L'\\');
    if (slash) {
        char name[MAX_PATH];
        *slash = 0;
        if (!narrow_exact(slash + 1, name, sizeof name)) return 0;
        if (GetShortPathNameW(dir, sp, MAX_PATH * 2) && narrow_exact(sp, tmp, sizeof tmp)) {
            char full[MAX_PATH * 3];
            snprintf(full, sizeof full, "%s\\%s", tmp, name);
            if ((int)strlen(full) < maxlen) { strcpy(out, full); return 1; }
        }
    }
    return 0;
}

static u16 do_file_dialog(u32 p, int save)
{
    OPENFILENAME16 *o = (OPENFILENAME16 *)ptrseg(p);
    OPENFILENAMEW ofn;
    WCHAR file[MAX_PATH * 2], title[MAX_PATH];
    WCHAR *filter, *custom = NULL, *initdir, *dlgtitle, *defext;
    char result[MAX_PATH * 2];
    BOOL ok;
    char *fname;
    int flen, maxlen;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = HWND_32(o->hwndOwner);
    ofn.hInstance = g_hinst32;
    filter = to_wide(str16(o->lpstrFilter), 1);
    ofn.lpstrFilter = filter;
    if (o->lpstrCustomFilter && o->nMaxCustFilter) {
        custom = calloc((size_t)o->nMaxCustFilter + 2, sizeof(WCHAR));
        if (custom) {
            MultiByteToWideChar(CP_ACP, 0, str16(o->lpstrCustomFilter), -1, custom, o->nMaxCustFilter);
            ofn.lpstrCustomFilter = custom;
            ofn.nMaxCustFilter = o->nMaxCustFilter;
        }
    }
    ofn.nFilterIndex = o->nFilterIndex;
    file[0] = 0;
    if (o->lpstrFile) MultiByteToWideChar(CP_ACP, 0, str16(o->lpstrFile), -1, file, MAX_PATH * 2);
    file[MAX_PATH * 2 - 1] = 0;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    title[0] = 0;
    ofn.lpstrFileTitle = title;
    ofn.nMaxFileTitle = MAX_PATH;
    initdir = to_wide(str16(o->lpstrInitialDir), 0);
    ofn.lpstrInitialDir = (initdir && initdir[0]) ? initdir : NULL;
    dlgtitle = to_wide(str16(o->lpstrTitle), 0);
    ofn.lpstrTitle = dlgtitle;
    ofn.Flags = (o->Flags & ~(OFN_ENABLEHOOK | OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE | OFN_SHOWHELP | OFN_ALLOWMULTISELECT))
                | OFN_EXPLORER | OFN_NOCHANGEDIR;
    defext = to_wide(str16(o->lpstrDefExt), 0);
    ofn.lpstrDefExt = defext;
    if (o->Flags & OFN_ENABLEHOOK) emu_log("file dialog hook ignored");

    for (;;) {
        ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
        if (!ok) {
            emu_log("file dialog cancelled/failed: %lu", CommDlgExtendedError());
            break;
        }
        maxlen = o->nMaxFile ? (int)o->nMaxFile : 128;
        if (maxlen > (int)sizeof result) maxlen = (int)sizeof result;
        if (path_for_win16(file, result, maxlen)) break;
        MessageBoxW(ofn.hwndOwner,
                    L"Castle of the Winds cannot use this file name or folder (the path is too long or "
                    L"contains characters it cannot handle).\nPlease choose another name or folder.",
                    L"Castle of the Winds", MB_OK | MB_ICONEXCLAMATION);
        ok = FALSE;
    }
    free(filter); free(custom); free(initdir); free(dlgtitle); free(defext);
    if (!ok) return 0;

    if (o->lpstrFile && o->nMaxFile) snprintf((char *)ptrseg(o->lpstrFile), o->nMaxFile, "%s", result);
    fname = strrchr(result, '\\');
    fname = fname ? fname + 1 : result;
    flen = (int)strlen(result);
    o->nFileOffset = (u16)(fname - result);
    {
        char *dot = strrchr(fname, '.');
        o->nFileExtension = (u16)(dot ? (dot - result) + 1 : flen);
    }
    if (o->lpstrFileTitle && o->nMaxFileTitle) snprintf((char *)ptrseg(o->lpstrFileTitle), o->nMaxFileTitle, "%s", fname);
    o->nFilterIndex = (u16)ofn.nFilterIndex;
    o->Flags = (o->Flags & ~(OFN_READONLY | OFN_EXTENSIONDIFFERENT)) | (ofn.Flags & (OFN_READONLY | OFN_EXTENSIONDIFFERENT));
    emu_log("file dialog -> %s", result);
    return 1;
}

void COMMDLG_GetOpenFileName(void)
{
    ARGS();
    u32 p = A32();
    RET16(do_file_dialog(p, 0));
}

void COMMDLG_GetSaveFileName(void)
{
    ARGS();
    u32 p = A32();
    RET16(do_file_dialog(p, 1));
}
