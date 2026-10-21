/*
 * crt.c - minimal C runtime glue (startup code, helpers) so the port only
 *         depends on system DLLs that ship with every Windows 10 install.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int _fltused = 0;

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int);

int cotw_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    int r;
    if (!n) return 0;
    r = _vsnprintf(buf, n, fmt, ap);
    if (r < 0 || (size_t)r >= n) {
        buf[n - 1] = 0;
        r = (int)n - 1;
    }
    return r;
}

int cotw_snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = cotw_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

void __cdecl WinMainCRTStartup(void)
{
    STARTUPINFOA si;
    char *cmd = GetCommandLineA();
    int show = SW_SHOWDEFAULT;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    GetStartupInfoA(&si);
    if (si.dwFlags & STARTF_USESHOWWINDOW) show = si.wShowWindow;
    /* skip the program name */
    if (*cmd == '"') {
        cmd++;
        while (*cmd && *cmd != '"') cmd++;
        if (*cmd == '"') cmd++;
    } else {
        while (*cmd && *cmd != ' ' && *cmd != '\t') cmd++;
    }
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    ExitProcess((UINT)WinMain(GetModuleHandleA(NULL), NULL, cmd, show));
}

#if defined(__x86_64__)
/* stack probe used by the compiler for large frames */
__asm__(".globl __chkstk\n"
        "__chkstk:\n"
        "    pushq %rcx\n"
        "    pushq %rax\n"
        "    cmpq $0x1000, %rax\n"
        "    leaq 24(%rsp), %rcx\n"
        "    jb 2f\n"
        "1:  subq $0x1000, %rcx\n"
        "    orq $0, (%rcx)\n"
        "    subq $0x1000, %rax\n"
        "    cmpq $0x1000, %rax\n"
        "    ja 1b\n"
        "2:  subq %rax, %rcx\n"
        "    orq $0, (%rcx)\n"
        "    popq %rax\n"
        "    popq %rcx\n"
        "    ret\n");
#endif
