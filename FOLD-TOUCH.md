# FTL on a foldable: touch-first Hyperspace (`touch-ui` branch)

This branch of [Hyperspace](https://github.com/FTL-Hyperspace/FTL-Hyperspace) makes *FTL: Faster Than Light* play like
a native tablet game on tall, nearly square screens, such as the inner screen of a Galaxy Z Fold. The game runs
under [Winlator](https://github.com/brunodev85/winlator), a Wine-based Windows emulator for Android. Everything
here is code in the Hyperspace DLL. Nothing is patched in the game files.

> **You need your own copy of FTL.** This repository contains no game files, and no ready-to-install Android
> package is distributed: such a package would include FTL's copyrighted data. Buy FTL (Steam, GOG or Humble)
> and build or download the DLL below.

## What it does

**Full-screen layout for a tall canvas** (`src/features/fold-layout/FoldLayout.cpp`)
- The game renders 1:1 on a canvas that matches the screen's shape instead of letterboxing 1280x720. The HUD is
  split into regions that are moved and enlarged for fingers.
- The top bar is about 1.4x. The crew list is 2x and shrinks to stay clear of the reactor as your crew grows.
- The systems and subsystems share one large bottom row. When it is too wide, it scrolls with a swipe instead of
  shrinking, with a scroll bar and soft edges. The open/close-all doors buttons sit at its start, so they show
  without scrolling.
- The weapons are at least 1.5x at the bottom right. The drones sit above them, with their labels on the boxes.
- The enemy window is enlarged and always ends above the weapons. Event boxes, the star map, stores and the ship
  screen are scaled to fit, keeping your resources visible.
- Tooltips are placed above the finger and kept on the screen. Desktop-only buttons are hidden.

**Touch controls** (`src/features/touch-ui/`, plus input mapping in `FoldLayout.cpp`)
- A tap is a click on release. Long-press or a two-finger tap is a right click.
- Pinch zooms the ships. A two-finger drag pans them.
- Tap-to-move: with crew selected, tapping a room sends them there, and then they are deselected.
- A large in-game Pause button sits in the top-right corner. The wrench opens the game menu.
- Several weapons at once: with one weapon armed, tap others to add them (tap again to drop). One tap or drag on
  an enemy room then aims them all. Beams take their drag across the ship.
- Double-tapping a target sets that weapon to autofire. Beams aim by dragging. Star-map taps snap to the nearest
  beacon. A swipe on the bottom row scrolls it and never changes power.

## Getting the DLL

- **Download a build:** open the **Actions** tab of this repository, pick a successful run of
  *Build Windows release (fast)* on `touch-ui`, and download the `hyperspace-binary` artifact (`Hyperspace.dll`).
  Artifacts need a GitHub login and expire after a while.
- **Build it yourself:** follow Hyperspace's [Building on Windows](../../wiki/Building-on-Windows) or
  [Building on Linux](../../wiki/Building-on-Linux) guide on this branch. The fold and touch features are part of
  the normal Windows build.

## Installing

1. Install Hyperspace on your own copy of FTL **1.6.9 for Windows** by following the
   [official guide](https://ftl-hyperspace.github.io/FTL-Hyperspace/en/windows/). This applies `Hyperspace.ftl`
   with Slipstream and puts `Hyperspace.dll` next to `FTLGame.exe`. Work on a copy of the game folder, not your
   Steam install.
2. Replace that `Hyperspace.dll` with the one from this branch.
3. Copy the game folder to your Android device and run `FTLGame.exe` in Winlator. Set the environment variables
   below in the container settings. Make the Wine desktop resolution match the screen's shape (the inner screen's
   own resolution, or a smaller one with the same proportions) so the canvas is tall.

## Settings (environment variables)

| Variable | Effect |
|---|---|
| `FTL_FOLD_LAYOUT=1` | Renders on the full, screen-shaped canvas ("borders" mode) and turns the layout on. The layout is active by default whenever the canvas is tall enough; `0` turns it off. |
| `FTL_HIDE_CURSOR=1` | Hides the game's mouse cursor (tooltips still show). Use it on a touch screen. |
| `FTL_TAP_TO_MOVE=0` | Turns tap-to-move and the multi-weapon selection off (back to the original click behaviour). |
| `FTL_FOLD_HUD_SCALE=1.6` | Forces the top HUD's scale instead of fitting it to the screen. |
| `FTL_FOLD_BOTTOM_SCALE=2.0` | Forces the bottom row's scale (default 2.2, or smaller if it does not fit). |
| `FTL_TOUCH_UI=1` | Experimental: feeds the left mouse button to FTL's own iPad touch handlers instead. |

The layout only rearranges the HUD on a tall canvas: scaled to 1280 pixels wide, it must be at least about 1020
pixels tall (150 extra above and below FTL's 720). On an ordinary 16:9 monitor the game looks and plays as usual.

## Testing without a phone

`src/features/fold-layout/FoldTest.cpp` is a scripted test harness. With `FTL_FOLD_TEST=<script file>` (and
`FTL_FOLD_TEST_CANVAS=1280x1154` to force a Fold-shaped window), the game reads timed steps and logs `PASS`/`FAIL`
lines and screenshots to `FTL_HS.log`. Steps cover taps, drags, long presses, keys, wheel, console commands and
expectations about game state, with touch points given in layout regions (for example `bl 162 682` for a system
box).

`FoldCheck.cpp` checks every frame for HUD drawn off the screen, HUD groups overlapping each other, and boxes cut in
two by the scrolling row. It reports anything that lasts more than a few frames. `FTL_FOLD_TEST_SAVES=<folder>`
keeps the test's saves away from your own.

## Credits

Built on [FTL: Hyperspace](https://github.com/FTL-Hyperspace/FTL-Hyperspace) by the Hyperspace team, and run on
Android with [Winlator](https://github.com/brunodev85/winlator). *FTL: Faster Than Light* is by Subset Games.
Same license as Hyperspace (see `LICENSE.md`).
