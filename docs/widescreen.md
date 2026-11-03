# Any picture shape

A game whose camera can be widened (`GameInfo::hor_plus`) is shown at any shape the player picks, `aspect_ratio` in the
settings file: `"4:3"`, `"16:10"`, `"16:9"`, `"21:9"`, `"32:9"` or `"screen"` (the output's, the default, held between 4:3
and 32:9). Any other game is shown as before, 16:9 on a widescreen output and 4:3 on any other.

The work is split three ways:

| Who | What |
| --- | --- |
| The SDK | renders the screen's targets at the picture's shape, places the 2D, shows the frame at that shape |
| The game | widens its camera by `app::HorPlus()`, names where each piece of its 2D is anchored (`gpu::SetTwoDPlacement`) |
| The player | picks the shape |

## The picture

**Figure 1.** One frame at 21:9. Nothing is stretched: the 3D sees more at the sides, the 2D keeps its 4:3 proportions.

```text
   the console's 4:3 screen, 640 x 480         the picture at 21:9, the same 480 lines tall
  +--------------------------+       +------------------------------------------------+
  |HP          [map]    AMMO|       |HP                    [map]                  AMMO|
  |                          |  -->  |   more of the world    +    more of the world   |
  |          (3D)            |       |                      (3D)                      |
  +--------------------------+       +------------------------------------------------+
```

Every target the size of the console's screen, colour and depth, is made `HorPlus()` times wider than the render scale
gives it (`gpu::PictureWide`); smaller targets (shadow maps, reflections) are left alone. The game keeps working in its
console's 640 x 480: its viewport, scissor and copies are scaled to the target across and down separately, so a copy
from the screen into a smaller texture is drawn across rather than copied. The frame is shown at the picture's shape and
letterboxed on an output of another shape.

A game's 3D gets wider by its own camera: `HorPlus()` is how many times wider than 4:3 the picture is, the factor its
horizontal field of view or its aspect argument is widened by. Without that the world is drawn at the picture's shape
but stretched.

## The 2D

A game's 2D is laid out in the console's pixels and says nothing of what each piece is attached to. A modern UI layout
has that, so the game names it before a group of draws: `gpu::SetTwoDPlacement(placement, tag)`, from its hooks on the
game thread. The placement holds until changed. The tag names the call site, for `XBE_2D_TAGS=1`.

**Figure 2.** The placements, `gpu/picture.h`.

```text
  console 4:3                      the wide picture
  +----------------+   +--------------------------------------------+
  |a      b       c|   |a'                b'                      c'|
  +----------------+   +--------------------------------------------+
```

| Placement | Where it goes | For |
| --- | --- | --- |
| `kPlaceStretch` | spans the picture, its shape stretched | a backdrop |
| `kPlaceCentre` | in proportion, in a 4:3 box in the middle (b) | a panel, a menu |
| `kPlaceLeft` | in proportion, the 4:3 box at the left edge (a) | a corner of the HUD |
| `kPlaceRight` | in proportion, the 4:3 box at the right edge (c) | a corner of the HUD |
| `kPlaceSide` | left, right or centre by the third of the screen each draw is in | a HUD drawn by one function |
| `kPlaceEdge` | spans when it reaches the screen's left or right edge, else as auto | bars across the screen |
| `kPlaceSpread` | in proportion, each draw's middle as far across the picture as it was across the screen | a row of icons |
| `kPlaceFill` | centre, but a draw across the whole width spans; with the overflow on, a piece flush with the screen's edge keeps that edge at the picture's | a fade, a filter, a scope's mask |
| `kPlaceAuto` | as fill without the edges | a game that names nothing |

Two flags go with a placement for 3D that is part of the 2D layout, a turning character on a menu's panel or a weapon on
a page: `kPlacePanel3d` keeps a draw through a camera narrower than the picture at its 4:3 shape, centred, and
`kPlacePanelAnchored` sends it with its panel's anchor instead. The world's camera is as wide as the picture and is left
alone, so this needs no list of what is in a menu. Program draws are told apart by the projection register the game
names (`gpu::g_projection_register`).

What counts as 2D: a draw of pre-transformed vertices, a fixed function draw through an orthographic projection, or a
program draw that does not read the projection register. Its extent is measured on the CPU (a program draw's positions
through the program itself); a draw reaching 98% of the screen's width spans the picture.

`gpu::SetTwoDOverflow(true)` lets a piece reaching past the console's screen go on past the old edge, as CSS's
`overflow: visible` does: the screen's edge was what cut it. Off, a placed draw stops at its 4:3 box. A scissor narrower
than the screen still holds. A game opts in, since what was off its screen can now be seen.

How it is drawn: the placement is a squeeze of clip space about the anchor, `x' = x k + offset w`, `k = 1 / HorPlus()`.
Shaders apply it at their end (constant `place`, register c247 under Shader Model 3); Direct3D 9's fixed function takes
it in its projection, so tools see the same transforms.

## Passes over the frame

`d3d::g_frame_passes` (`xdk/d3d8.h`) limits the game's full-screen passes over its own finished frame, the draws with
one of the swap chain's buffers at stage 0, to that many a frame: a soften, a glow or a colour grade a game can offer as
a setting. -1 (the default) draws every one, 0 none.

## Switches

| Switch | What it does |
| --- | --- |
| `aspect_ratio` | the picture's shape, settings file |
| `XBE_2D_TAGS=1` | every 300 frames, each tagged call site with its placement, its draws and the extent they reached |
