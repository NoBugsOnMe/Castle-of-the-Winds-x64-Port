/*
 * patch.c - small additions to the original game code.
 *
 * Code locations are found by searching the loaded code segments for byte
 * signatures, so the same patch works for CASTLE1.EXE and CASTLE2.EXE (the
 * two games share most of their code, at different addresses).
 *
 * Object information popup
 * ------------------------
 * Right-clicking an object shows a small window built by a routine that
 * fills a table of up to ten lines (local handles of Pascal strings plus line
 * heights) and returns the text block's size.  The patch hooks the end of
 * that routine and, for weapons and for armour of every kind (body armour,
 * shields, helmets, cloaks, boots, ...), inserts a line with the weapon class
 * and damage dice or the armour value.  The values are
 * read from the object itself (the base "damage dice" / "armour" effects the
 * game attaches to every such object), so they are exactly what the game
 * uses; enchantments stay hidden, as in the original.
 */
#include "win16.h"

int code_hook(u16 sel, u16 off, int len, void (*fn)(void));

/* ---- signature search ------------------------------------------------------ */

/* sig: bytes, mask: 'x' = must match, '?' = any */
static int sig_match(const u8 *p, const u8 *sig, const char *mask)
{
    int i;
    for (i = 0; mask[i]; i++)
        if (mask[i] == 'x' && p[i] != sig[i]) return 0;
    return 1;
}

/* finds the first match in any code segment; returns 1 and the location */
static int find_code(const u8 *sig, const char *mask, int *seg, u16 *off)
{
    int s, len = (int)strlen(mask);
    for (s = 0; s < g_mod.nseg; s++) {
        const u8 *base;
        int n, i;
        if (g_mod.segflags[s] & 1) continue;        /* data segment */
        base = (const u8 *)ptr16(g_mod.segsel[s], 0);
        n = g_mod.seglen[s];
        for (i = 0; i + len <= n; i++) {
            if (base[i] == sig[0] && sig_match(base + i, sig, mask)) {
                *seg = s;
                *off = (u16)i;
                return 1;
            }
        }
    }
    return 0;
}

static int find_in(int seg, u16 from, u16 span, const u8 *sig, const char *mask, u16 *off)
{
    const u8 *base = (const u8 *)ptr16(g_mod.segsel[seg], 0);
    int len = (int)strlen(mask), i, end = from + span;
    if (end > g_mod.seglen[seg]) end = g_mod.seglen[seg];
    for (i = from; i + len <= end; i++)
        if (sig_match(base + i, sig, mask)) { *off = (u16)i; return 1; }
    return 0;
}

/* ---- object popup ------------------------------------------------------------ */

static u16 g_popup_table;    /* DS offset of the 10-entry line table */
static u16 g_popup_font;     /* DS offset of the variable holding the font */

#define POPUP_LINES 10

/* the help file's "Weapon Class" of each weapon type (same in both games) */
static const u8 weapon_class[18] = { 0, 1, 2, 2, 3, 3, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12 };
/* base armour values from the games' object creation tables, used only if an
   object unexpectedly lacks its armour effect */
static const u8 armor_av[11] = { 0, 6, 12, 18, 24, 30, 36, 42, 48, 54, 52 };
static const u8 shield_av[13] = { 0, 3, 6, 7, 9, 10, 11, 12, 13, 15, 16, 18, 21 };
static const u8 helmet_av[9] = { 0, 3, 6, 9, 15, 9, 9, 9, 25 };

enum { CAT_WEAPON = 6, CAT_ARMOR = 7, CAT_SHIELD = 8, CAT_HELMET = 9 };
enum { EFF_DICE = 0x0D, EFF_BASE_AV = 0x38 };

static int object_info_line(u16 hobj, char *out, int outlen)
{
    u16 p = local_lock(hobj);
    u32 o;
    int cat, sub, neff, i, found = 0;
    int dcount = 0, ddie = 0, dbonus = 0, av = 0;
    if (!p) return 0;
    o = lin(local_heap_sel(), p);
    cat = rd8(o);
    sub = rd8(o + 1);
    neff = rd8(o + 0x0A) & 0x0F;
    if (g_log_enabled) {
        char b[256]; int n = 0;
        for (i = 0; i < neff && n < 200; i++) {
            u32 e = o + 0x0C + 10u * i;
            n += snprintf(b + n, sizeof b - n, " [%04X %04X %04X %04X %04X]", rd16(e), rd16(e + 2), rd16(e + 4), rd16(e + 6), rd16(e + 8));
        }
        b[n] = 0;
        emu_log("popup object cat %d sub %d neff %d:%s", cat, sub, neff, b);
    }
    for (i = 0; i < neff; i++) {
        u32 e = o + 0x0C + 10u * i;
        int type = rd16(e) & 0x3FF;
        if (cat == CAT_WEAPON && type == EFF_DICE && !found) {
            dbonus = (s16)rd16(e + 4);
            dcount = (s16)rd16(e + 6);
            ddie = (s16)rd16(e + 8);
            found = 1;
        } else if (cat != CAT_WEAPON && type == EFF_BASE_AV && !found) {
            av = (s16)rd16(e + 4);
            found = 1;
        }
    }
    local_unlock(hobj);

    switch (cat) {
    case CAT_WEAPON: {
        char dice[32];
        if (!found) return 0;
        if (dbonus) snprintf(dice, sizeof dice, "%dd%d%+d", dcount, ddie, dbonus);
        else snprintf(dice, sizeof dice, "%dd%d", dcount, ddie);
        if (sub < 18) snprintf(out, outlen, "Weapon Class: %d (damage %s)", weapon_class[sub], dice);
        else snprintf(out, outlen, "Damage: %s", dice);
        return 1;
    }
    case CAT_ARMOR:
        if (!found) { if (sub >= 11) return 0; av = armor_av[sub]; }
        break;
    case CAT_SHIELD:
        if (!found) { if (sub >= 13) return 0; av = shield_av[sub]; }
        break;
    case CAT_HELMET:
        if (!found) { if (sub >= 9) return 0; av = helmet_av[sub]; }
        break;
    default:
        /* cloaks, boots, gauntlets and the like carry the same armour effect */
        if (!found) return 0;
        break;
    }
    snprintf(out, outlen, "Armor Value: %d", av);
    return 1;
}

/* reads the Pascal string of a popup line */
static int line_text(u16 h, char *out, int outlen)
{
    u16 p = h ? local_lock(h) : 0;
    int n, i;
    if (!p) { out[0] = 0; return 0; }
    n = rd8(lin(local_heap_sel(), p));
    if (n >= outlen) n = outlen - 1;
    for (i = 0; i < n; i++) out[i] = (char)rd8(lin(local_heap_sel(), (u16)(p + 1 + i)));
    out[n] = 0;
    local_unlock(h);
    return n;
}

/* replaces "mov ax,[bp-0Ch] / mov dx,[bp-0Eh]" at the routine's common exit */
static void popup_hook(void)
{
    u32 frame = g_cpu.base[S_SS];
    u16 hobj = rd16(frame + (u16)(BP + 8));
    u16 width = rd16(frame + (u16)(BP - 0x0C));
    u16 height = rd16(frame + (u16)(BP - 0x0E));
    char text[96], buf[64];
    int count, i, at, len;

    if (width && hobj && object_info_line(hobj, text, sizeof text)) {
        u32 tab = lin(g_mod.dgroup, g_popup_table);
        u16 lh = 0, hstr, p;
        for (count = 0; count < POPUP_LINES && rd16(tab + 4u * count); count++) ;
        if (count > 0 && count < POPUP_LINES) {
            /* insert after the "Weight: ... Bulk ..." line */
            at = count;
            for (i = 0; i < count; i++) {
                line_text(rd16(tab + 4u * i), buf, sizeof buf);
                if (!strncmp(buf, "Weight", 6)) { at = i + 1; lh = rd16(tab + 4u * i + 2); break; }
            }
            if (!lh) lh = rd16(tab + 4u * (count - 1) + 2);
            len = (int)strlen(text);
            hstr = local_alloc(L_MOVEABLE | L_ZEROINIT, (u16)(len + 1));
            if (hstr && (p = local_lock(hstr)) != 0) {
                SIZE sz = { 0, 0 };
                HDC dc = GetDC(NULL);
                HGDIOBJ font = HGDI_32(rd16(lin(g_mod.dgroup, g_popup_font))), old = NULL;
                wr8(lin(local_heap_sel(), p), (u8)len);
                for (i = 0; i < len; i++) wr8(lin(local_heap_sel(), (u16)(p + 1 + i)), (u8)text[i]);
                local_unlock(hstr);
                if (font) old = SelectObject(dc, font);
                GetTextExtentPoint32A(dc, text, len, &sz);
                if (old) SelectObject(dc, old);
                ReleaseDC(NULL, dc);
                if (!lh) lh = (u16)sz.cy;
                /* recompute the table address: the allocation may have grown the heap */
                tab = lin(g_mod.dgroup, g_popup_table);
                for (i = count; i > at; i--) {
                    wr16(tab + 4u * i, rd16(tab + 4u * (i - 1)));
                    wr16(tab + 4u * i + 2, rd16(tab + 4u * (i - 1) + 2));
                }
                wr16(tab + 4u * at, hstr);
                wr16(tab + 4u * at + 2, lh);
                height = (u16)(height + lh);
                if (sz.cx > width) width = (u16)sz.cx;
                wr16(frame + (u16)(BP - 0x0C), width);
                wr16(frame + (u16)(BP - 0x0E), height);
                emu_log("popup: added '%s' (line %d of %d)", text, at, count + 1);
            } else if (hstr) {
                local_free(hstr);
            }
        }
    }
    AX = width;
    DX = height;
}

static void patch_object_popup(void)
{
    /* inc bp / push bp / mov bp,sp / push ds / mov ds,ax / sub sp,11Ch /
       push di / push si / sub ax,ax / mov [bp-0Eh],ax / mov [bp-0Ch],ax /
       mov word [bp-8], table / push word [bp+8] / call far LocalLock */
    static const u8 sig_start[] = {
        0x45, 0x55, 0x8B, 0xEC, 0x1E, 0x8E, 0xD8, 0x81, 0xEC, 0x1C, 0x01, 0x57, 0x56,
        0x2B, 0xC0, 0x89, 0x46, 0xF2, 0x89, 0x46, 0xF4, 0xC7, 0x46, 0xF8, 0, 0,
        0xFF, 0x76, 0x08, 0x9A
    };
    static const char mask_start[] = "xxxxxxxxxxxxxxxxxxxxxxxx??xxxx";
    /* mov ax,[bp-0Ch] / mov dx,[bp-0Eh] / jmp short +0Dh / nop / nop /
       push word [bp+8] / call far (LocalUnlock) */
    static const u8 sig_exit[] = { 0x8B, 0x46, 0xF4, 0x8B, 0x56, 0xF2, 0xEB, 0x0D, 0x90, 0x90, 0xFF, 0x76, 0x08, 0x9A };
    static const char mask_exit[] = "xxxxxxxxxxxxxx";
    int seg;
    u16 off, exit_off;
    const u8 *f;
    if (!find_code(sig_start, mask_start, &seg, &off)) { emu_log("patch: object popup routine not found"); return; }
    f = (const u8 *)ptr16(g_mod.segsel[seg], off);
    /* push ax / push word [font] / call far SelectObject at +38h */
    if (f[0x38] != 0x50 || f[0x39] != 0xFF || f[0x3A] != 0x36 || f[0x3D] != 0x9A) {
        emu_log("patch: object popup font reference not found");
        return;
    }
    if (!find_in(seg, off, 0x600, sig_exit, mask_exit, &exit_off)) { emu_log("patch: object popup exit not found"); return; }
    g_popup_table = (u16)(f[0x18] | (f[0x19] << 8));
    g_popup_font = (u16)(f[0x3B] | (f[0x3C] << 8));
    code_hook(g_mod.segsel[seg], exit_off, 6, popup_hook);
    emu_log("patch: object popup at seg%d:%04X (exit %04X), table %04X, font [%04X]",
            seg + 1, off, exit_off, g_popup_table, g_popup_font);
}

/* ---- entry ------------------------------------------------------------------- */

/* ---- the player's state, for the combat bars and the title bar (hud.c) ------ */

GameVars g_gv;

/* finds a pattern given as hex bytes with "??" wildcards; returns a pointer
   to the match in the loaded code, or NULL */
static const u8 *find_hex(const char *hex)
{
    u8 sig[256];
    char mask[257];
    int n = 0, seg;
    u16 off;
    const char *p = hex;
    while (*p && n < 256) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (p[0] == '?') { sig[n] = 0; mask[n] = '?'; }
        else { sig[n] = (u8)strtoul(p, NULL, 16); mask[n] = 'x'; }
        n++;
        p += 2;
    }
    mask[n] = 0;
    if (!find_code(sig, mask, &seg, &off)) return NULL;
    return (const u8 *)ptr16(g_mod.segsel[seg], off);
}

static u16 w16(const u8 *p, int o) { return (u16)(p[o] | (p[o + 1] << 8)); }

/* the far address (selector:offset) of a signature's first byte, 0 if not found */
static u32 find_hex_addr(const char *hex)
{
    const u8 *p = find_hex(hex);
    int s;
    if (!p) return 0;
    for (s = 0; s < g_mod.nseg; s++) {
        const u8 *base = (const u8 *)ptr16(g_mod.segsel[s], 0);
        if (p >= base && p < base + g_mod.seglen[s]) return ((u32)g_mod.segsel[s] << 16) | (u16)(p - base);
    }
    return 0;
}

/* the game's own test whether the player can see a monster: it is in the
   map window's view, and lit or within the distance at which that kind of
   monster can be seen, and in line of sight (or monsters are detected) */
static u32 g_mon_visible_fn;

/* The addresses differ between the two games; they are taken from the
   instructions that use them:
   - the map window's WM_SIZE handler centres the view on the player
     (player position, view origin, the map mode that selects which origin),
   - the Character dialog prints hit points, mana and experience,
   - the "next level at" routine (level, difficulty, a constant),
   - a routine that compares the game clock. */
static void find_game_vars(void)
{
    const u8 *v = find_hex("B1 05 8B 46 08 D3 E8 A3 ?? ?? 8B 46 06 D3 E8 A3 ?? ?? 80 3E ?? ?? 03 75 2F 99 2B C2 D1 F8 "
                           "8A 0E ?? ?? 2A ED 2B C8 79 02 2B C9 89 0E ?? ?? A1 ?? ?? 99 2B C2 D1 F8 8A 16 ?? ?? "
                           "2A F6 2B D0 79 02 2B D2 89 16 ?? ?? EB 2D 99 2B C2 D1 F8 8A 0E ?? ?? 2A ED 2B C8 79 02 "
                           "2B C9 89 0E ?? ?? A1 ?? ?? 99 2B C2 D1 F8 8A 16 ?? ?? 2A F6 2B D0 79 02 2B D2 89 16 ?? ??");
    const u8 *hp = find_hex("8D 86 5A FE 50 B9 5C 00 51 FF 36 ?? ?? 2B C9 51 FF 36 ?? ?? 51 51 9A");
    const u8 *mp = find_hex("8D 86 5A FE 50 B9 DA 00 51 FF 36 ?? ?? FF 36 ?? ?? 2B C9 51 51 51 9A");
    const u8 *xp = find_hex("B8 0A 00 50 8D 8E 5A FE 51 FF 36 ?? ?? FF 36 ?? ?? 9A ?? ?? ?? ?? 83 C4 08 FF 76 0E B8 70 00");
    const u8 *nx = find_hex("A1 ?? ?? 48 F7 26 ?? ?? 8B C8 05 1E 00 2B D2 8B D9 8A 0E ?? ?? 0A C9 74 08 D1 E0 D1 D2 "
                            "FE C9 75 F8 8D 4F 28 2B C1 83 DA 00");
    const u8 *tm = find_hex("A1 ?? ?? 8B 16 ?? ?? 39 56 08 72 0D 77 05 39 46 06 76 06 89 46 06 89 56 08");
    memset(&g_gv, 0, sizeof g_gv);
    if (!v || !hp || !mp || !xp || !nx) {
        emu_log("patch: player variables not found (%d %d %d %d %d)", !!v, !!hp, !!mp, !!xp, !!nx);
        return;
    }
    g_gv.mode = w16(v, 20);
    g_gv.px = w16(v, 32);  g_gv.py = w16(v, 56);
    g_gv.vx3 = w16(v, 44); g_gv.vy3 = w16(v, 68);
    g_gv.vx = w16(v, 91);  g_gv.vy = w16(v, 115);
    g_gv.hp = w16(hp, 11); g_gv.hpmax = w16(hp, 18);
    g_gv.mp = w16(mp, 11); g_gv.mpmax = w16(mp, 15);
    g_gv.xphi = w16(xp, 11); g_gv.xplo = w16(xp, 15);
    g_gv.diff = w16(nx, 1); g_gv.kconst = w16(nx, 6); g_gv.level = w16(nx, 19);
    if (tm) { g_gv.time = w16(tm, 1); g_gv.has_time = 1; }
    g_gv.ok = 1;
    /* The monsters of a level: a local block (count, capacity, handles of the
       monsters' blocks) whose handle is in a global variable.  Taken from the
       routine that lets the monsters regain hit points (+2 hit points, +4
       maximum), and checked against the one that notes which monsters are no
       longer in view (+0x0B bit 0 "drawn on the map", +0x16/+0x17 row/column). */
    {
        const u8 *rg = find_hex("83 3E ?? ?? 00 74 35 8B 36 ?? ?? 8B 04 8B C8 05 04 00 8B D8 8B F1 8B 3C D1 E7 03 CF "
                                "83 C1 04 8B D1 3B C8 76 17 8B 37 8B 3C 8B 34 8B 44 04 39 44 02 7D 03 FF 45 02");
        const u8 *vs = find_hex("F6 44 0B 01 74 28 8A 44 16 2A E4 50 8A 44 17 50 9A");
        if (rg && vs && w16(rg, 2) == w16(rg, 9)) {
            g_gv.mlist = w16(rg, 2);
            g_gv.has_mons = 1;
        }
        g_mon_visible_fn = find_hex_addr("1E 58 90 45 55 8B EC 1E 8E D8 83 EC 04 57 56 83 7E 06 00 75 13 FF 76 0A FF 76 08 "
                                         "0E E8 ?? ?? 0B C0 75 05 2B C0 EB 5B 90 83 3E ?? ?? 00 74 05 B8 01 00 EB 4E "
                                         "8B 76 08 8B 7E 0A B1 06 8B DF D3 E3 03 DE 8B C3 D1 E3 03 D8 F6 87 ?? ?? 04 75 22");
        emu_log("patch: monster list [%04X]%s, visibility test %04X:%04X", g_gv.mlist, g_gv.has_mons ? "" : " not found",
                (u16)(g_mon_visible_fn >> 16), (u16)g_mon_visible_fn);
    }
    emu_log("patch: player at [%04X],[%04X], view [%04X],[%04X] / [%04X],[%04X] (mode [%04X]), hp [%04X]/[%04X], "
            "mana [%04X]/[%04X], xp [%04X]:[%04X], level [%04X], difficulty [%04X]*[%04X], clock [%04X]",
            g_gv.px, g_gv.py, g_gv.vx, g_gv.vy, g_gv.vx3, g_gv.vy3, g_gv.mode, g_gv.hp, g_gv.hpmax,
            g_gv.mp, g_gv.mpmax, g_gv.xphi, g_gv.xplo, g_gv.level, g_gv.diff, g_gv.kconst, g_gv.time);
}

static u32 ds_lin(u16 off) { return lin(g_mod.dgroup, off); }

int game_read_player(PlayerState *st)
{
    int mode, d;
    long base;
    memset(st, 0, sizeof *st);
    if (!g_gv.ok) return 0;
    st->hp = (s16)rd16(ds_lin(g_gv.hp));
    st->hpmax = (s16)rd16(ds_lin(g_gv.hpmax));
    st->mp = (s16)rd16(ds_lin(g_gv.mp));
    st->mpmax = (s16)rd16(ds_lin(g_gv.mpmax));
    st->xp = (u32)rd16(ds_lin(g_gv.xplo)) | ((u32)rd16(ds_lin(g_gv.xphi)) << 16);
    st->level = rd8(ds_lin(g_gv.level));
    st->x = rd8(ds_lin(g_gv.px));
    st->y = rd8(ds_lin(g_gv.py));
    mode = rd8(ds_lin(g_gv.mode));
    st->mode = mode;
    st->vx = (s16)rd16(ds_lin(mode == 3 ? g_gv.vx3 : g_gv.vx));
    st->vy = (s16)rd16(ds_lin(mode == 3 ? g_gv.vy3 : g_gv.vy));
    st->clock = g_gv.has_time ? ((u32)rd16(ds_lin(g_gv.time)) | ((u32)rd16(ds_lin((u16)(g_gv.time + 2))) << 16)) : 0;
    /* the game's own "next level at" formula (see find_game_vars) */
    d = (s16)rd16(ds_lin(g_gv.diff)) - 1;
    base = (long)(u16)(d * (s16)rd16(ds_lin(g_gv.kconst)));
    st->next = ((u32)(u16)(base + 30) << (st->level & 31)) - (u32)(u16)(base + 40);
    st->in_game = st->hpmax > 0 && st->level > 0;
    return st->in_game;
}

/* the local heap block a moveable handle refers to (0 if discarded / invalid) */
static u16 local_ptr(u16 h)
{
    if (!h || h >= 0xFFF0) return 0;
    return rd16(ds_lin(h));
}

int game_read_monsters(MonsterState *m, int max)
{
    u16 list, n, i;
    int k = 0;
    if (!g_gv.has_mons) return 0;
    list = local_ptr(rd16(ds_lin(g_gv.mlist)));
    if (!list) return 0;
    n = rd16(ds_lin(list));
    if (n > 512) return 0;
    for (i = 0; i < n && k < max; i++) {
        u16 h = rd16(ds_lin((u16)(list + 4 + 2 * i)));
        u16 p = local_ptr(h);
        if (!p) continue;
        m[k].id = h;
        m[k].ptr = p;
        m[k].hp = (s16)rd16(ds_lin((u16)(p + 2)));
        m[k].hpmax = (s16)rd16(ds_lin((u16)(p + 4)));
        m[k].seen = rd8(ds_lin((u16)(p + 0x0B))) & 1;
        m[k].y = rd8(ds_lin((u16)(p + 0x16)));
        m[k].x = rd8(ds_lin((u16)(p + 0x17)));
        k++;
    }
    return k;
}

/* asks the game whether the player can see the monster now (the "drawn"
   flag is only brought up to date when the map is redrawn) */
int game_monster_visible(const MonsterState *m)
{
    u16 w[4];
    if (!g_mon_visible_fn) return m->seen;
    /* Pascal order: monster, row, column, "overview map" flag */
    w[0] = m->ptr;
    w[1] = (u16)m->y;
    w[2] = (u16)m->x;
    w[3] = 0;
    return (u16)call16(g_mon_visible_fn, g_mod.dgroup, 4, w) != 0;
}

void patches_apply(void)
{
    patch_object_popup();
    find_game_vars();
}

/* ---- debugging: DGROUP snapshots (COTW_DSDUMP set, F12) ---------------------------- */

int debug_dump_ds(void)
{
    static int n = -1;
    char path[MAX_PATH];
    FILE *f;
    if (n < 0) {
        char v[8];
        if (!GetEnvironmentVariableA("COTW_DSDUMP", v, sizeof v)) return 0;
        n = 0;
    }
    snprintf(path, sizeof path, "%s\\dsdump%02d.bin", g_data_dir, n++);
    f = fopen(path, "wb");
    if (!f) return 1;
    fwrite(ptr16(g_mod.dgroup, 0), 1, 65536, f);
    fclose(f);
    emu_log("DGROUP dumped to %s", path);
    return 1;
}
