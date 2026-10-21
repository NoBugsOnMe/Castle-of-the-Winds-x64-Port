Castle of the Winds for Windows 10 and 11
=========================================

Castle of the Winds (Part 1: A Question of Vengeance, Part 2: Lifthransir's
Bane) by Rick Saada / SaadaSoft, 1989-1993, freeware.

This package runs the original Windows 3.1 game programs on 64-bit Windows
10 and 11. Each EXE contains the original, unmodified game program, its help
file and a small built-in "Windows 3.1" for it to run on - nothing needs to
be installed.

  CASTLE1.EXE   Part 1: A Question of Vengeance
  CASTLE2.EXE   Part 2: Lifthransir's Bane
  README.txt    this file


Getting started
---------------
* Unzip the files into a folder of your choice, for example
  Documents\Castle of the Winds, and double-click CASTLE1.EXE or
  CASTLE2.EXE.
* The programs are not digitally signed, so Windows may say "Windows
  protected your PC". Click "More info", then "Run anyway".
* Saved games, high scores and settings (CASTLE.INI) are kept in the same
  folder as the EXEs. If that folder cannot be written to (for example
  under Program Files), they are kept in %APPDATA%\Castle of the Winds.
* The Help menu opens the game's original help file in a built-in viewer.


Zoom: a bigger game on large screens
------------------------------------
The game was made for screens of 640x480 to 1024x768. On larger screens the
port enlarges everything - map, windows, dialogs, text - by a whole-number
factor, so the pixel art stays sharp.

"Automatic" picks the largest factor that still leaves room for at least
1024x700 game pixels: 1x up to 1920x1080, 2x on 2560x1440 and 5120x1440,
3x on 3840x2160.

To change it, use the View menu in the menu bar (to the right of Window):
View -> Zoom -> Automatic, 1x, 2x, 3x or 4x. The change takes effect at
once and is remembered. (Everything in the View menu is also in the
window's system menu: click the small icon at the left end of the title
bar, or press Alt+Space.)

The size of each map is set by the game, so on very wide screens (21:9,
32:9) the smaller maps, such as the starting village, do not fill the
whole width.


Upscaling: smoother graphics when zoomed
----------------------------------------
View -> Upscaling (available at 2x and above):

  Original Pixels  every pixel simply enlarged, exactly as the original
  Sharp            edges and curves smoothed (the default)
  Soft             smoothed and anti-aliased

Sharp and Soft enlarge the game's own pictures with a pixel art scaler
(MMPX) instead of simply making each pixel bigger: outlines, diagonals and
round shapes - characters, monsters, roofs, the paper doll - come out
smoother, while the colours, shading patterns and fine lines of the
original art are kept. No new detail is invented; Sharp uses only the
original colours, Soft also blends the colours along the smoothed edges.
The game's mouse pointers and the pictures in the help file are upscaled
the same way.


Dark mode
---------
View -> Dark Mode:

  Same as Windows  follows Windows' "default app mode" (Settings ->
                   Personalization -> Colors)
  On / Off
  Dim the Map      in dark mode, also turns down the brightness of the map
                   (on by default)

Dark mode darkens the window backgrounds, text, dialog boxes, menus, title
bars and scroll bars. The map artwork itself is left as it is, apart from
white areas and the optional dimming. Dark title bars, menu bars and scroll
bars need Windows 10 version 1809 or later.


Health bars and stats in the title bar
--------------------------------------
When you take damage, two small bars appear just above your character on
the map: hit points (red) and mana (blue), drawn in the game's own colours.
The bright part is what you have left, the dark part what is missing. The
bars follow you around and disappear after about a dozen turns without
damage. They count game time, so they stay up while you stop to think.

Monsters you have wounded get a hit point bar of their own, just above
them, showing how much life they have left. It stays while you can see the
monster and it has not healed, and goes away when the monster dies. A
monster you cannot see (in the dark, out of your line of sight) never shows
a bar, so the bars give nothing away that the game would not show you.

The title bar shows your hit points, mana, level and experience, including
the experience still needed for the next level, for example:

  Castle of the Winds | HP 52/52 | Mana 27/27 | Level 7 | XP 1880/3800 (1920 to next level)

All three can be switched off in the View menu: Combat Bars, Enemy Health
Bars and Stats in Title Bar.


Weapon class and armor value
----------------------------
Hold down the right mouse button on an object (in the inventory or in a
shop) to see its information window. It now also shows:

  weapons    the weapon class and damage dice, for example
             "Weapon Class: 2 (damage 1d4)" for a dagger and
             "Weapon Class: 4 (damage 1d5+1)" for a spear
  armor      the armor value of body armor, shields, helmets, cloaks,
             boots, gauntlets and bracers, for example "Armor Value: 12"

These are the object's base values, the same numbers the game itself uses.
Magical bonuses of enchanted objects are not included; the game describes
those in its own lines below (once you know what the object does).


Settings in CASTLE.INI
----------------------
The View menu stores its settings in CASTLE.INI, section [Port]:

  Zoom=Auto             Auto, or a factor from 1 to 6
  Upscaling=Sharp       Original, Sharp or Soft
  DarkMode=Auto         Auto (same as Windows), On or Off
  DimMap=1              1 = dim the map in dark mode, 0 = don't
  DimMapBrightness=60   brightness of the dimmed map, in percent (30-100)
  CombatBars=1          1 = show the combat bars, 0 = don't
  EnemyBars=1           1 = show the enemy health bars, 0 = don't
  TitleStats=1          1 = show the stats in the title bar, 0 = don't


Notes
-----
* If Windows display scaling is above 100 %, Windows enlarges the game
  window as well; "Automatic" already takes that into account. If the game
  is too big or too small, pick another factor in the Zoom menu.
* For bug reports: set the environment variable COTW_LOG=1 before starting
  the game; it then writes a log file, cotw-port.log, next to the saved
  games.


Castle of the Winds (c) 1989-1993 Rick Saada / SaadaSoft, freeware.
