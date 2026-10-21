/* xctl - tiny X11 input injector (XTest) for driving test runs under Xvfb.
 * usage: xctl move X Y | click X Y | rclick X Y | dclick X Y | down/up X Y | rdown/rup X Y |
 *        key SPEC... (e.g. Return, ctrl+s, KP_8) | type TEXT | drag X1 Y1 X2 Y2 | sleep MS
 */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

Bool XTestFakeMotionEvent(Display *, int, int, int, unsigned long);
Bool XTestFakeButtonEvent(Display *, unsigned int, Bool, unsigned long);
Bool XTestFakeKeyEvent(Display *, unsigned int, Bool, unsigned long);

static Display *d;

static void sync_sleep(int ms) { XSync(d, False); usleep(ms * 1000); }

static void key_spec(const char *spec)
{
    char buf[128], *parts[8];
    int n = 0, i;
    KeyCode codes[8];
    snprintf(buf, sizeof buf, "%s", spec);
    parts[n++] = strtok(buf, "+");
    while (n < 8 && (parts[n] = strtok(NULL, "+"))) n++;
    for (i = 0; i < n; i++) {
        const char *p = parts[i];
        KeySym ks;
        if (!strcmp(p, "ctrl")) ks = XK_Control_L;
        else if (!strcmp(p, "shift")) ks = XK_Shift_L;
        else if (!strcmp(p, "alt")) ks = XK_Alt_L;
        else ks = XStringToKeysym(p);
        if (ks == NoSymbol) { fprintf(stderr, "unknown key %s\n", p); exit(1); }
        codes[i] = XKeysymToKeycode(d, ks);
    }
    for (i = 0; i < n; i++) { XTestFakeKeyEvent(d, codes[i], True, 0); sync_sleep(15); }
    for (i = n - 1; i >= 0; i--) { XTestFakeKeyEvent(d, codes[i], False, 0); sync_sleep(15); }
}

int main(int argc, char **argv)
{
    int i = 1;
    d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "cannot open display\n"); return 1; }
    while (i < argc) {
        const char *c = argv[i++];
        if (!strcmp(c, "move")) {
            XTestFakeMotionEvent(d, -1, atoi(argv[i]), atoi(argv[i + 1]), 0); i += 2; sync_sleep(30);
        } else if (!strcmp(c, "click") || !strcmp(c, "dclick") || !strcmp(c, "rclick")) {
            int x = atoi(argv[i]), y = atoi(argv[i + 1]), b = !strcmp(c, "rclick") ? 3 : 1, k, times = !strcmp(c, "dclick") ? 2 : 1;
            i += 2;
            XTestFakeMotionEvent(d, -1, x, y, 0); sync_sleep(40);
            for (k = 0; k < times; k++) {
                XTestFakeButtonEvent(d, b, True, 0); sync_sleep(30);
                XTestFakeButtonEvent(d, b, False, 0); sync_sleep(30);
            }
        } else if (!strcmp(c, "down") || !strcmp(c, "up") || !strcmp(c, "rdown") || !strcmp(c, "rup")) {
            XTestFakeMotionEvent(d, -1, atoi(argv[i]), atoi(argv[i + 1]), 0); i += 2; sync_sleep(30);
            XTestFakeButtonEvent(d, c[0] == 'r' ? 3 : 1, !strcmp(c, "down") || !strcmp(c, "rdown"), 0); sync_sleep(30);
        } else if (!strcmp(c, "drag")) {
            int x1 = atoi(argv[i]), y1 = atoi(argv[i + 1]), x2 = atoi(argv[i + 2]), y2 = atoi(argv[i + 3]), s;
            i += 4;
            XTestFakeMotionEvent(d, -1, x1, y1, 0); sync_sleep(50);
            XTestFakeButtonEvent(d, 1, True, 0); sync_sleep(80);
            for (s = 1; s <= 10; s++) { XTestFakeMotionEvent(d, -1, x1 + (x2 - x1) * s / 10, y1 + (y2 - y1) * s / 10, 0); sync_sleep(30); }
            XTestFakeButtonEvent(d, 1, False, 0); sync_sleep(50);
        } else if (!strcmp(c, "key")) {
            key_spec(argv[i++]); sync_sleep(30);
        } else if (!strcmp(c, "type")) {
            const char *t = argv[i++];
            for (; *t; t++) {
                char k[32];
                if (*t == ' ') strcpy(k, "space");
                else if (*t >= 'A' && *t <= 'Z') snprintf(k, sizeof k, "shift+%c", *t + 32);
                else if (*t == '.') strcpy(k, "period");
                else if (*t == '\\') strcpy(k, "backslash");
                else if (*t == ':') strcpy(k, "shift+semicolon");
                else if (*t == '-') strcpy(k, "minus");
                else { k[0] = *t; k[1] = 0; }
                key_spec(k);
            }
        } else if (!strcmp(c, "sleep")) {
            sync_sleep(atoi(argv[i++]));
        } else {
            fprintf(stderr, "unknown command %s\n", c);
            return 1;
        }
    }
    XSync(d, False);
    XCloseDisplay(d);
    return 0;
}
