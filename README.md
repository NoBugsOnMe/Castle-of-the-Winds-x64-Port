# Castle of the Winds — Windows 10/11 port

An unofficial port that lets the original *Castle of the Winds* (Part 1: *A
Question of Vengeance*, Part 2: *Lifthransir's Bane*; © 1989–1993 SaadaSoft /
Rick Saada, freeware) run natively on 64-bit Windows 10 and 11 — no DOSBox,
no Windows 3.1 virtual machine.

**Playing:** download `CastleOfTheWinds-Win10-x64.zip` from the
[Releases](../../releases) page, unzip it and run `CASTLE1.EXE` or
`CASTLE2.EXE`. The included `README.txt` describes the additions: zoom for
large screens, pixel-art upscaling, dark mode, player and enemy health bars,
stats in the title bar, weapon class / armor value in the item popups, and a
built-in help viewer.

## How it works

The build produces `CASTLE1.EXE` and `CASTLE2.EXE`, native 64-bit Windows
programs that run the original, unmodified 16-bit Windows 3.1 executables.

Each output EXE embeds the original NE executable and a converted copy of
its WinHelp file, and contains:

| Part | Files | What it does |
|---|---|---|
| 16-bit CPU | `src/cpu.c`, `src/cpu.h` | Interpreter for 16-bit protected-mode x86 code (instructions up to the 386) with eagerly computed flags. `tests/test_alu.c` compares the ALU flag results with the host CPU. |
| Memory | `src/mem.c`, `src/mem.h` | 64 MB linear space, LDT-style selectors, Win16 global heap (moveable handles, huge blocks with `__AHINCR` tiling), local heaps and the task stack. |
| Loader | `src/ne.c`, `src/ne.h` | NE loader: segments, relocation chains, imports by ordinal, entry table, resources, MULTIPLEDATA prolog patching. |
| Thunks | `src/native.c`, `gen/apitab.c` | Each imported Win16 function is an `F1 xx xx` escape into C; callbacks back into 16-bit code (window procedures, dialog procedures) run nested CPU loops. Handle maps translate 16-bit HWND/HGDIOBJ/HMENU/HICON values to native handles. The same escape also implements in-line code hooks (`code_hook`). |
| KERNEL / DOS | `src/kernel.c`, `src/dos.c` | Task start-up (InitTask, PSP, environment), memory, resources, profile (INI) functions, `OpenFile`, and the INT 21h file/date/time services the C runtime uses. |
| USER | `src/user.c`, `src/msg.c`, `src/res.c` | Window classes, window procedures, message translation between Win16 and Win32 layouts (WM_COMMAND packing, CTLCOLOR, structure conversion, control message renumbering), subclassed native controls, dialog template conversion (to `DLGTEMPLATEEX`), menus, icons, cursors. |
| GDI | `src/gdi.c` | Drawing, DIB conversion (colour tables snapped to the 16 VGA colours, as on the original displays), metafiles, palettes. |
| COMMDLG | `src/commdlg.c` | Open/Save dialogs on the Unicode API; results converted to paths the 16-bit code can store (8.3 fallback). |
| Help | `src/help.c`, `tools/hlp.py`, `tools/hlp2bin.py` | WinHelp 3.x files are decompressed and converted at build time (`help*.bin`); a small built-in viewer shows them (contents, jumps, popups, pictures, search, browse buttons). It follows the zoom factor and dark mode. |
| Zoom | `src/zoom.c` | Integer scaling of the whole game (see below). |
| Upscaling | `src/scale.c` | Edge-aware pixel art magnification (MMPX) used instead of the plain stretch when zoomed (see below). |
| Dark mode | `src/dark.c` | Colour remapping of the game's drawing, dark title bars, menus, scroll bars, buttons and child-window captions (see below). |
| Options | `src/settings.c` | The *View* menu (*Zoom*, *Upscaling*, *Dark Mode*, *Combat Bars*, *Enemy Health Bars*, *Stats in Title Bar*) added to the main window's menu bars, in front of *Help*, and the same items in its system menu; settings in `CASTLE.INI` `[Port]`. |
| Health bars, title stats | `src/hud.c` | Hit point and mana bars above the player while taking damage, hit point bars above wounded monsters in view, and the player's statistics in the main window's title (see below). |
| Message boxes | `src/msgbox.c` | Message boxes drawn at the zoom factor and in dark colours (the real `MessageBox` is used at 1x in light mode). |
| Game patches | `src/patch.c` | Additions to the game code, located by byte signatures so they apply to both games: the object popup (right mouse button on an object) gains a *Weapon Class / damage* or *Armor Value* line. The addresses of the game variables the port reads (player position, map view origin, hit points, mana, level, experience, game clock, the level's monster list) and the game's monster visibility test are found the same way, from the code that uses them. |
| Start-up | `src/main.c`, `src/crt.c` | Locates the embedded image, chooses the data folder (EXE folder, or `%APPDATA%\Castle of the Winds` if that is read-only), and runs the task. |

### Zoom

The game is laid out for 640x480–1024x768 screens. With a zoom factor *S*
(1–6; *Auto* picks the largest factor that leaves at least 1024x700 logical
pixels, e.g. 2 on 2560x1440 or 5120x1440, 3 on 3840x2160), the 16-bit code
sees a screen *S* times smaller than the real one:

* Window frames, captions and menu bars keep their real size; client areas
  are *S* times larger than the game thinks. All coordinates crossing the
  boundary (window positions and sizes, mouse messages, `WM_SIZE`,
  `WINDOWPOS`, `MINMAXINFO`, `GetSystemMetrics`, cursor position, scrolling)
  are converted (`zoom_win_to_real` / `zoom_win_to_logical`).
* Each game window draws into an off-screen surface of its logical size;
  changed areas are scaled up with `StretchBlt` (`COLORONCOLOR`, so pixel art
  stays sharp) when the game releases the DC, ends painting, polls messages
  or reads the clock (the game times its animations, such as spell bolts,
  with busy loops on the DOS clock); as a fallback, anything drawn outside
  `WM_PAINT` is shown after at most 0.1 s. Owner-drawn items and dialogs
  painted by Windows use temporary surfaces.
* Native controls and dialog boxes get TrueType stand-ins of the game's
  bitmap fonts at *S* times the size (System → Arial Bold, Helv → Microsoft
  Sans Serif, Tms Rmn → Times New Roman, Courier → Courier New); cursors and
  icons are scaled.
* The factor can be changed while playing; all windows are re-laid out.

The EXEs are DPI-unaware on purpose: on displays with Windows scaling above
100 % Windows enlarges the window as a whole, and *Auto* works from the
scaled (logical) screen size.

### Upscaling

With `Upscaling=Sharp` (the default) or `Soft`, the surfaces are enlarged
with MMPX ("MMPX Style-Preserving Pixel Art Magnification", M. McGuire and
M. Gagiu, JCGT 2021) instead of `StretchBlt`: each pixel's sub-pixels copy
neighbouring colours where the rules detect diagonal edges, curves and
corners, so outlines become smoother while every output colour is one of
the original pixels (palette, dithering and 1-pixel lines are kept).

* *Sharp*: doubling to the next power of two at or above *S*; for 3x, 5x
  and 6x the result is reduced by picking pixels, so colours stay exact.
* *Soft*: one more doubling, then an area-averaging reduction, which
  anti-aliases the smoothed edges.
* Updates stay incremental: a changed area is scaled together with the
  margin of source pixels the rules look at (3–6 pixels), and the pixels
  around it whose result depends on it are redrawn as well; after
  `ScrollWindow` the edge pixels that lost their neighbours are redone.
  The result is identical to scaling the whole window at once.
* Large areas are split into bands scaled on worker threads (one per
  additional CPU core); the game's cursors and icons and the help viewer's
  pictures go through the same scaler.

### Dark mode

`DarkMode=Auto` follows the Windows *app mode* (light/dark) setting and
reacts when it changes. In dark mode:

* Colours the game sets (text, background, pens, brushes, metafile records
  such as the paper doll) are mapped per device context: light backgrounds
  become dark and dark text light, keeping the hue (`dark_bg`, `dark_fg`).
  The map windows only have white darkened, so the artwork is unchanged.
* *Dim the Map* (`DimMap=1`, `DimMapBrightness=60`) additionally scales the
  map's bright colours down when the surfaces are scaled up.
* Title bars use `DWMWA_USE_IMMERSIVE_DARK_MODE`; popup menus, scroll bars and
  the menu bar use the (undocumented) uxtheme dark-mode functions and the
  `WM_UAHDRAWMENU*` messages, on Windows 10 1809 and later. Push buttons,
  check boxes and radio buttons, and the captions of the inventory's child
  windows are painted by the port.

### Health bars and title bar stats

Whenever the game polls for messages, `hud_poll` reads the player's state
(`game_read_player`: the variables located in `patch.c`) and the monsters
of the current level (`game_read_monsters`).

* A drop in hit points while the game clock does not go backwards counts as
  a hit. Until 300 clock units (30 game seconds, about 12 moves) have passed
  without another one, a 30x7 bar (black frame, hit points in bright/dark
  red, mana in bright/dark blue) is shown above the player's tile, or below
  it in the top row. The next-level experience is computed with the formula
  of the game's own "next level at" routine, whose variables are found with
  the others (the results match the *Character* dialog).
* Monsters: the level's monsters are a local block (count, capacity, local
  handles of the 38-byte monster blocks) whose handle is in a global
  variable, found from the routine that lets monsters regain hit points
  (+2 hit points, +4 maximum; +0x16/+0x17 row/column, +0x0B bit 0 "drawn on
  the map"). A monster below its maximum gets a 30x4 hit point bar while
  the game has drawn it and the game's own visibility test (in the view,
  lit or within that monster kind's sight distance, in line of sight, or
  monsters detected) still passes. The test is a 16-bit routine of the
  game, run with `call16`, and only when a turn has passed or a monster or
  the view has changed.
* The bars are drawn on the screen DC after the map window's surface has been
  copied there (`hud_overlay`, called from the surface flush and from
  `EndPaint`), never into the surface itself; while they are enabled the map
  window always has a surface, also at 1x. Moving or hiding them re-copies
  the old rectangle from the surface (`zoom_redraw_logical`), and before
  `ScrollWindow` moves the map they are taken off the screen.
* The main window's title is the game's own title (which the game changes,
  e.g. to show the money and weight in the inventory) followed by the
  statistics; `GetWindowText` returns the game's title to the game.

## Building

Tested on Linux (Ubuntu 24.04) with clang/lld 18 cross-compiling for
`x86_64-pc-windows-msvc`, using Wine 11.19's headers and its `.spec` export
lists for the import libraries (only `kernel32`, `user32`, `gdi32`,
`comdlg32`, `shell32`, `advapi32` and `msvcrt` are imported; `uxtheme` and
`dwmapi` are loaded at run time when present).

Requirements:

* `clang`, `lld` (`lld-link`), `llvm-rc`, `llvm-dlltool`, `python3`, GNU `make`
* a Wine source tree (`WINE_SRC`) and a configured Wine build tree
  (`WINE_BUILD`, for generated headers such as `wtypes.h`)
* the original game files `CASTLE1.EXE`, `CASTLE2.EXE`, `CASTLE1.HLP`,
  `CASTLE2.HLP` (`GAME_DIR`)

```
make WINE_SRC=/path/to/wine WINE_BUILD=/path/to/wine-build GAME_DIR=/path/to/CotW
# -> build/x64/CASTLE1.EXE, build/x64/CASTLE2.EXE
```

(The paths can also be put in a `config.mk` next to the Makefile; see
`config.mk.example`. The program icons are extracted from the original EXEs
during the build.)

`gen/apitab.c` is generated from the games' import tables:
`WINE_SRC=/path/to/wine python3 tools/genapi.py CASTLE1.EXE CASTLE2.EXE`.

CPU self-test (on an x86-64 host):

```
gcc -O1 -mno-red-zone -fno-strict-aliasing -Isrc tests/test_alu.c src/cpu.c -o test_alu && ./test_alu
```

## Debugging

Set `COTW_LOG=1` (events such as window creation, file access, local heap
allocations, menu changes, zoom and dark-mode decisions, popup patch
activity) or `COTW_LOG=3` (additionally every Win16 API call with its caller
and arguments) before starting a game; the log is written to
`cotw-port.log` in the data folder. Unrecoverable emulation errors always
write the log and show the CPU state.

With `COTW_DSDUMP=1` set, F12 writes a snapshot of the game's data segment
(`dsdumpNN.bin`, 64 KB) to the data folder; comparing snapshots taken
before and after an event is how the game variables used by the port were
found (the monster list, map cells of `[terrain, flags, occupant]` at
`0x0CE2`/`0x0CF2 + 3*(row*64+column)`, and so on).

`tools/` also contains the analysis helpers used during development:
`nedump.py` (NE structures), `disasm.py` (annotated disassembly with import
names), `resdump.py`, `hlp.py` (WinHelp reader), and the Wine/Xvfb test
harness:

* `run.sh GAME [LOGLEVEL] [WAIT]` starts a game under Wine on a virtual
  desktop (`DESK=WxH`, default 1280x1024) on display `:99`;
* `newgame_z.sh`, `newgame2.sh` (part 2, past the spell and story screens),
  `loadgame_z.sh GAME LOGLEVEL [FILE]` drive the start-up dialogs by control
  name at any desktop size and zoom; `tomine.sh` walks from the part 1 hamlet
  to the mine;
* `shot.sh NAME [crop]` takes a screenshot, `xctl.c` injects mouse and
  keyboard input (XTest), `win/wininfo.c` + `wininfo.sh` dump the window
  tree, `wclick.py` clicks a window by class and caption.

## License

The port's own code is released under the MIT License (see `LICENSE`).
*Castle of the Winds* itself — the game programs, help files and artwork that
the built EXEs embed — remains © 1989–1993 Rick Saada / SaadaSoft and is not
covered by that license. This project is not affiliated with or endorsed by
Rick Saada or SaadaSoft.
