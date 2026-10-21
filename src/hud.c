/*
 * hud.c - status additions while playing:
 *
 *  - Combat bars: when the player takes damage, two small bars (hit points
 *    and mana) appear just above the player on the map, in the game's own
 *    16-colour style, and go away again once there has been no damage for a
 *    while (measured on the game clock, so pausing to think does not hide
 *    them).  They are drawn over the map window's image after it has been
 *    copied to the screen - never into the game's own drawing - so the map
 *    underneath is restored exactly when they move or disappear.
 *  - Enemy bars: a hit point bar above each monster in view that has been
 *    wounded, until it is dead or has healed again.  The game itself notes
 *    which monsters it has drawn on the map, so only monsters the player can
 *    see get one.
 *  - The player's hit points, mana, level and experience (with the amount
 *    needed for the next level) are added to the main window's title.
 *
 * The values come from the game's variables (game_read_player and
 * game_read_monsters, patch.c) and are checked whenever the game polls for
 * messages.
 */
#include "win16.h"

int g_hud_bars = 1, g_hud_title = 1, g_hud_enemy = 1;

#define COMBAT_CLOCK 300          /* game clock units (1/10 s) the bars stay after a hit */
#define COMBAT_MS 10000           /* fallback without the game clock */

static HWND g_map;                /* the map window ("Dungeon" class) */
static HWND g_title_wnd;          /* the main window */
static char g_game_title[256];    /* the title the game gave it */
static char g_title_shown[512];
static int g_title_dirty = 1;

typedef struct { int vis; RECT r; int hp, hpmax, mp, mpmax; } Bars;
static Bars g_bars;               /* what is on the screen */

#define MAX_EBARS 64
typedef struct { RECT r; int hp, hpmax; } EBar;
static EBar g_ebar[MAX_EBARS];    /* enemy bars on the screen */
static int g_nebar;

static PlayerState g_last;        /* last state seen */
static int g_have_last;
static int g_hit;
static u32 g_hit_clock;
static DWORD g_hit_tick;

void hud_init(void)
{
    g_hud_bars = GetPrivateProfileIntA("Port", "CombatBars", 1, g_ini_path) != 0;
    g_hud_title = GetPrivateProfileIntA("Port", "TitleStats", 1, g_ini_path) != 0;
    g_hud_enemy = GetPrivateProfileIntA("Port", "EnemyBars", 1, g_ini_path) != 0;
}

static ATOM g_map_atom;

static void forget_bars(void) { memset(&g_bars, 0, sizeof g_bars); g_nebar = 0; }

void hud_set_map(HWND h) { g_map = h; forget_bars(); }
void hud_map_class(ATOM a) { g_map_atom = a; }

void hud_window_destroyed(HWND h)
{
    if (h == g_map) { g_map = NULL; forget_bars(); }
    if (h == g_title_wnd) g_title_wnd = NULL;
}

/* the map window keeps an off-screen image while bars are on, so that what
   is under them can be put back */
int hud_wants_surface(HWND h)
{
    if (!h || !g_map_atom) return 0;
    if (h != g_map) {
        if ((ATOM)GetClassWord(h, GCW_ATOM) != g_map_atom) return 0;
        hud_set_map(h);
    }
    return g_hud_bars || g_hud_enemy;
}

/* ---- title bar ---------------------------------------------------------------- */

void hud_set_main(HWND h)
{
    g_title_wnd = h;
    GetWindowTextA(h, g_game_title, sizeof g_game_title);
    g_title_shown[0] = 0;
    g_title_dirty = 1;
}

int hud_is_main(HWND h) { return h && h == g_title_wnd; }

/* the game sets the main window's title (it shows the money and weight in
   the inventory, for example) */
void hud_game_title(HWND h, const char *s)
{
    (void)h;
    snprintf(g_game_title, sizeof g_game_title, "%s", s ? s : "");
    g_title_dirty = 1;
    hud_poll();
}

/* what the game reads back: its own title */
const char *hud_game_title_text(void) { return g_game_title; }

static void update_title(const PlayerState *st)
{
    char t[512];
    if (!g_title_wnd) return;
    if (g_hud_title && st && st->in_game) {
        long left = (long)st->next - (long)st->xp;
        if (left < 0) left = 0;
        snprintf(t, sizeof t, "%s   |   HP %d/%d   |   Mana %d/%d   |   Level %d   |   XP %lu/%lu (%ld to next level)",
                 g_game_title, st->hp, st->hpmax, st->mp, st->mpmax, st->level,
                 (unsigned long)st->xp, (unsigned long)st->next, left);
    } else {
        snprintf(t, sizeof t, "%s", g_game_title);
    }
    if (strcmp(t, g_title_shown)) {
        snprintf(g_title_shown, sizeof g_title_shown, "%s", t);
        SetWindowTextA(g_title_wnd, t);
    }
}

void hud_settings_changed(void)
{
    g_title_dirty = 1;
    if (!g_hud_bars && g_bars.vis) {
        RECT r = g_bars.r;
        g_bars.vis = 0;
        if (g_map) zoom_redraw_logical(g_map, &r);
    }
    if (!g_hud_enemy && g_nebar) {
        int i, n = g_nebar;
        g_nebar = 0;
        for (i = 0; i < n && g_map; i++) zoom_redraw_logical(g_map, &g_ebar[i].r);
    }
    /* whether the map keeps an off-screen image may have changed: start it
       afresh from a full repaint */
    zoom_surfaces_reset();
    if (g_map) RedrawWindow(g_map, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    hud_poll();
}

/* ---- combat bars -------------------------------------------------------------------- */

/* the map window's size in game pixels; 0 if it is not shown */
static int map_size(int *lw, int *lh)
{
    RECT cr;
    if (!g_map || !IsWindowVisible(g_map) || !GetClientRect(g_map, &cr)) return 0;
    *lw = zceil(cr.right);
    *lh = zceil(cr.bottom);
    return 1;
}

/* where a bar h pixels high goes for the map square x,y: just above it, or
   below it in the top row; 0 if the square is not (completely) in view */
static int bar_rect(const PlayerState *st, int x, int y, int h, int lw, int lh, RECT *r)
{
    int tx = (x - st->vx) * 32, ty = (y - st->vy) * 32;
    if (tx < 0 || ty < 0 || tx + 32 > lw || ty + 32 > lh) return 0;
    r->left = tx + 1;
    r->right = tx + 31;
    r->top = ty - h;
    if (r->top < 0) r->top = ty + 32;
    r->bottom = r->top + h;
    return r->bottom <= lh;
}

static void bars_for(const PlayerState *st, Bars *b)
{
    int lw, lh;
    memset(b, 0, sizeof *b);
    if (!map_size(&lw, &lh)) return;
    if (!bar_rect(st, st->x, st->y, st->mpmax > 0 ? 7 : 4, lw, lh, &b->r)) return;
    b->vis = 1;
    b->hp = st->hp; b->hpmax = st->hpmax;
    b->mp = st->mp; b->mpmax = st->mpmax;
}

static u32 fnv(u32 h, u32 v)
{
    int i;
    for (i = 0; i < 4; i++, v >>= 8) h = (h ^ (v & 0xFF)) * 16777619u;
    return h;
}

/* wounded monsters the game has drawn on the map and the player can still
   see.  The game is only asked about visibility when something it depends
   on has changed (a turn has passed, the view has moved, a monster has moved
   or been hurt), since this runs whenever the game looks for messages. */
static int enemy_bars(const PlayerState *st, EBar *out)
{
    static MonsterState m[256];
    static EBar last[MAX_EBARS];
    static int last_n = -1;
    static u32 last_sig;
    int n, i, k = 0, lw, lh;
    u32 sig = 2166136261u;
    if (!map_size(&lw, &lh)) return 0;
    n = game_read_monsters(m, 256);
    sig = fnv(sig, st->clock);
    sig = fnv(sig, (u32)(st->x | st->y << 8 | lw << 16));
    sig = fnv(sig, (u32)(st->vx | st->vy << 16));
    sig = fnv(sig, (u32)(lh | n << 16));
    for (i = 0; i < n; i++) {
        sig = fnv(sig, (u32)(m[i].id | m[i].x << 16 | m[i].y << 24));
        sig = fnv(sig, (u32)((m[i].hp & 0x7FFF) | (m[i].hpmax & 0x7FFF) << 15 | (u32)m[i].seen << 31));
    }
    if (last_n >= 0 && sig == last_sig) {
        memcpy(out, last, last_n * sizeof *out);
        return last_n;
    }
    for (i = 0; i < n && k < MAX_EBARS; i++) {
        if (!m[i].seen || m[i].hpmax <= 0 || m[i].hp <= 0 || m[i].hp >= m[i].hpmax) continue;
        if (m[i].x == st->x && m[i].y == st->y) continue;
        if (!bar_rect(st, m[i].x, m[i].y, 4, lw, lh, &out[k].r)) continue;
        if (!game_monster_visible(&m[i])) continue;
        out[k].hp = m[i].hp;
        out[k].hpmax = m[i].hpmax;
        k++;
    }
    memcpy(last, out, k * sizeof *out);
    last_n = k;
    last_sig = sig;
    return k;
}

static int same_ebar(const EBar *a, const EBar *b) { return !memcmp(a, b, sizeof *a); }

/* puts the new set of enemy bars on the screen: the squares of bars that
   went away or changed are restored, the new ones drawn */
static void set_enemy_bars(const EBar *nb, int nn)
{
    EBar old[MAX_EBARS];
    int no = g_nebar, i, j;
    if (nn == no && !memcmp(nb, g_ebar, nn * sizeof *nb)) return;
    memcpy(old, g_ebar, no * sizeof *old);
    memcpy(g_ebar, nb, nn * sizeof *nb);
    g_nebar = nn;
    if (!g_map) return;
    for (i = 0; i < no; i++) {
        for (j = 0; j < nn && !same_ebar(&old[i], &nb[j]); j++) ;
        if (j == nn) zoom_redraw_logical(g_map, &old[i].r);
    }
    for (j = 0; j < nn; j++) {
        for (i = 0; i < no && !same_ebar(&old[i], &nb[j]); i++) ;
        if (i == no) zoom_redraw_logical(g_map, &nb[j].r);
    }
}

void hud_poll(void)
{
    PlayerState st;
    Bars nb;
    int in = game_read_player(&st);
    if (!in) {
        g_have_last = 0;
        g_hit = 0;
    } else {
        /* a hit: hit points went down, and the clock did not go backwards
           (that would be another game being loaded) */
        if (g_have_last && st.hp < g_last.hp && st.clock >= g_last.clock) {
            g_hit = 1;
            g_hit_clock = st.clock;
            g_hit_tick = GetTickCount();
        }
        if (g_hit) {
            int over = g_gv.has_time ? (st.clock < g_hit_clock || st.clock - g_hit_clock > COMBAT_CLOCK)
                                     : (GetTickCount() - g_hit_tick > COMBAT_MS);
            if (over) g_hit = 0;
        }
    }
    if (g_title_dirty || !g_have_last || memcmp(&st, &g_last, sizeof st)) {
        update_title(in ? &st : NULL);
        g_title_dirty = 0;
    }
    if (in) { g_last = st; g_have_last = 1; }

    {
        static EBar ne[MAX_EBARS];
        int nn = in && g_hud_enemy ? enemy_bars(&st, ne) : 0;
        set_enemy_bars(ne, nn);
    }

    memset(&nb, 0, sizeof nb);
    if (in && g_hud_bars && g_hit) bars_for(&st, &nb);
    if (memcmp(&nb, &g_bars, sizeof nb)) {
        Bars old = g_bars;
        g_bars = nb;
        if (g_map) {
            if (old.vis) zoom_redraw_logical(g_map, &old.r);
            if (nb.vis) zoom_redraw_logical(g_map, &nb.r);
        }
    }
}

/* the map window is about to be scrolled: take the bars off the screen first
   (they are drawn again where they belong at the next poll) */
int hud_take_bars(HWND h, RECT *r)
{
    int i, any = 0;
    RECT u;
    if (h != g_map) return 0;
    SetRectEmpty(&u);
    if (g_bars.vis) { u = g_bars.r; any = 1; }
    for (i = 0; i < g_nebar; i++) {
        if (any) UnionRect(&u, &u, &g_ebar[i].r);
        else u = g_ebar[i].r;
        any = 1;
    }
    if (!any) return 0;
    forget_bars();
    *r = u;
    return 1;
}

static void fill(HDC dc, int x0, int y0, int x1, int y1, COLORREF c)
{
    RECT r;
    HBRUSH b;
    int s = g_zoom;
    if (x1 <= x0 || y1 <= y0) return;
    SetRect(&r, x0 * s, y0 * s, x1 * s, y1 * s);
    b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

/* draws one bar's inside: full part / empty part */
static void bar(HDC dc, int x0, int x1, int y0, int y1, int v, int max, COLORREF full, COLORREF empty)
{
    int w = x1 - x0, f;
    if (max <= 0) max = 1;
    if (v < 0) v = 0;
    if (v > max) v = max;
    f = (int)(((long)w * v + max - 1) / max);        /* any life left shows */
    fill(dc, x0, y0, x0 + f, y1, full);
    fill(dc, x0 + f, y0, x1, y1, empty);
}

/* called after the map window's image was copied to the screen through dc */
void hud_overlay(HWND h, HDC dc)
{
    const RECT *r = &g_bars.r;
    int i;
    if (h != g_map || !dc) return;
    /* the game's palette: black frame, bright red / dark red, bright blue / dark blue */
    for (i = 0; i < g_nebar; i++) {
        const RECT *e = &g_ebar[i].r;
        fill(dc, e->left, e->top, e->right, e->bottom, RGB(0, 0, 0));
        bar(dc, e->left + 1, e->right - 1, e->top + 1, e->top + 3, g_ebar[i].hp, g_ebar[i].hpmax, RGB(255, 0, 0), RGB(128, 0, 0));
    }
    if (!g_bars.vis) return;
    fill(dc, r->left, r->top, r->right, r->bottom, RGB(0, 0, 0));
    bar(dc, r->left + 1, r->right - 1, r->top + 1, r->top + 3, g_bars.hp, g_bars.hpmax, RGB(255, 0, 0), RGB(128, 0, 0));
    if (g_bars.mpmax > 0)
        bar(dc, r->left + 1, r->right - 1, r->top + 4, r->top + 6, g_bars.mp, g_bars.mpmax, RGB(0, 0, 255), RGB(0, 0, 128));
}
