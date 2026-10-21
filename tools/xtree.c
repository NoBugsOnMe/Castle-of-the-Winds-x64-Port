#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
int main(void){
  Display *d = XOpenDisplay(NULL); Window root = DefaultRootWindow(d), r, p, *ch; unsigned n, i;
  XQueryTree(d, root, &r, &p, &ch, &n);
  for (i = 0; i < n; i++) { XWindowAttributes a; char *name = NULL; XGetWindowAttributes(d, ch[i], &a); XFetchName(d, ch[i], &name);
    printf("%lx %s %dx%d+%d+%d map=%d or=%d\n", ch[i], name ? name : "-", a.width, a.height, a.x, a.y, a.map_state, a.override_redirect); }
  return 0; }
