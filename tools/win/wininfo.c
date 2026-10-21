#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void dump(HWND h, int depth)
{
    char cls[64], txt[64];
    RECT r;
    HWND c;
    GetClassNameA(h, cls, sizeof cls);
    GetWindowTextA(h, txt, sizeof txt);
    GetWindowRect(h, &r);
    printf("%*s%p [%s] '%s' style=%08lx ex=%08lx vis=%d en=%d (%ld,%ld)-(%ld,%ld) id=%ld\n", depth * 2, "", (void *)h, cls, txt,
           (unsigned long)GetWindowLongA(h, GWL_STYLE), (unsigned long)GetWindowLongA(h, GWL_EXSTYLE), IsWindowVisible(h), IsWindowEnabled(h),
           (long)r.left, (long)r.top, (long)r.right, (long)r.bottom, (long)GetWindowLongPtrA(h, GWLP_ID));
    for (c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) dump(c, depth + 1);
}
static BOOL CALLBACK top(HWND h, LPARAM lp)
{
    char cls[64];
    GetClassNameA(h, cls, sizeof cls);
    if (lp || IsWindowVisible(h)) dump(h, 0);
    return TRUE;
}
int main(int argc, char **argv)
{
    HDESK d = OpenDesktopA("cotw", 0, FALSE, GENERIC_ALL);
    printf("desktop %p\n", (void *)d);
    if (d) SetThreadDesktop(d);
    {
        char *cmd = GetCommandLineA(), *p;
        if ((p = strstr(cmd, " -h ")) != NULL) {
            HWND h = (HWND)(ULONG_PTR)strtoul(p + 4, &p, 16);
            int x = strtol(p, &p, 10), y = strtol(p, &p, 10);
            LRESULT r = SendMessageA(h, WM_NCHITTEST, 0, MAKELPARAM(x, y));
            RECT ur = { 0 };
            BOOL u = GetUpdateRect(h, &ur, FALSE);
            printf("NCHITTEST(%p, %d,%d) = %ld; update=%d (%ld,%ld)-(%ld,%ld)\n", (void *)h, x, y, (long)r, u, (long)ur.left, (long)ur.top, (long)ur.right, (long)ur.bottom);
            return 0;
        }
        if ((p = strstr(cmd, " -r ")) != NULL) {
            HWND h = (HWND)(ULONG_PTR)strtoul(p + 4, &p, 16);
            printf("redraw %p -> %d\n", (void *)h, RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW));
            return 0;
        }
        if ((p = strstr(cmd, " -p ")) != NULL) {
            int x = strtol(p + 4, &p, 10), y = strtol(p, &p, 10);
            POINT pt = { x, y };
            HWND h = WindowFromPoint(pt);
            char cls[64] = "";
            GetClassNameA(h, cls, sizeof cls);
            printf("WindowFromPoint(%d,%d) = %p [%s]\n", x, y, (void *)h, cls);
            return 0;
        }
    }
    EnumWindows(top, argc > 1);
    return 0;
}
