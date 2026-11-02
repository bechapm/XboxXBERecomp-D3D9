#pragma once
// A picture wider than the console's, for a game that widens its own view (app::GameInfo::hor_plus, app::HorPlus).
// Its 3D comes out as wide as the picture: the screen's targets are rendered at the picture's shape and the game's own
// camera is widened by the game. Its 2D is laid out in the console's 4:3 pixels and says nothing of what each piece is
// attached to, so the game names anchors (SetTwoDPlacement), the one thing a modern UI layout has that it lacks, and
// every position is worked out from the factor k = 1 / HorPlus: nothing is kept per shape or per resolution.
//
//   console 4:3, 640 wide        the wide picture
//   +----------------+   +--------------------------------------------+
//   |a      b       c|   |a'                b'                      c'|
//   +----------------+   +--------------------------------------------+
//   STRETCH  spans the picture, its shape stretched          (a backdrop)
//   CENTRE   in proportion, in a 4:3 box in the middle       (b, a panel)
//   LEFT     in proportion, the 4:3 box at the left edge     (a, a corner)
//   RIGHT    in proportion, the 4:3 box at the right edge    (c, a corner)
//   SIDE     LEFT, RIGHT or CENTRE by the third of the screen each draw is in
//   EDGE     spans when the draw reaches the screen's left or right edge, else as AUTO
//   SPREAD   in proportion, each draw's middle as far across the picture as it was across the screen
//   FILL     CENTRE, but a draw across the whole width spans it (a fade, a filter, a frame), and with the
//            overflow on a piece flush with the screen's edge keeps that edge at the picture's, the rest in
//            proportion, as a nine-slice does (a scope's mask, a filter's rim)
//   AUTO     the renderer's guess: as FILL without the edges, for a game that names nothing
//
// A placement holds until changed, 3D draws go by their camera, and with a 4:3 picture nothing moves.
#include <atomic>
#include <cstdint>

#include "gpu/state.h"
#include "gpu/vertex.h"

namespace gpu {

enum : uint32_t {
    kPlaceAuto,
    kPlaceStretch,
    kPlaceCentre,
    kPlaceLeft,
    kPlaceRight,
    kPlaceSide,
    kPlaceEdge,
    kPlaceSpread,
    kPlaceFill,
    kPlaceLast = kPlaceFill,
    kPlaceMask = 0xFF,
    // or-ed in: 3D that is part of the 2D layout, a turning character in a menu's panel or a weapon on a page. A draw
    // through a camera narrower than the picture (by a tenth) was not made for it and is kept at its 4:3 shape,
    // centred; the world's camera is as wide as the picture and is left alone, so this needs no list of what is in a
    // menu. Program draws through the projection the game names (g_projection_register). Only while the game's UI
    // draws, since a split screen's narrow view is the world; a 3D backdrop placed STRETCH still spans.
    kPlacePanel3d = 0x100,
    // with kPlacePanel3d: such a model goes with its panel's anchor (LEFT, RIGHT) rather than the middle
    kPlacePanelAnchored = 0x200,
};

// Where the game's next 2D draws go, and the call site they come from (XBE_2D_TAGS reports each site's extent). The
// game's thread, from its hooks; it reaches the renderer with the draws that follow (State::place)
void SetTwoDPlacement(uint32_t placement, uint32_t tag);
// A piece reaching past the console's screen goes on past the old edge, as the console's screen edge was what cut it:
// CSS's overflow visible. Off, a placed draw stops at its 4:3 box (overflow hidden). A scissor narrower than the
// screen still holds. A game opts in, since what was off its screen can now be seen
void SetTwoDOverflow(bool on);

// the host width of the screen's targets over their width at the render scale: the picture's shape over the console
// screen's (gw x gh) for a game that widens its own view, else 1
float PictureWide(uint32_t gw, uint32_t gh);

// one draw's placement on a target the picture's shape (the backend's, on the render thread)
struct Placed {
    // clip x' = x * k + offset * w: squeezed by k about the anchor (offset = anchor * (1 - k), anchor in clip units)
    float k = 1, offset = 0;
    bool bleed = false;  // FILL's edges: a vertex at the screen's edge stays at the picture's (the shader's place.z)
    // the draw's scissor, from its viewport and scissor in console pixels, as host x on a target hw host pixels wide
    // whose console width is gw; free: a rectangle within 2 pixels of the screen's width is the whole picture
    bool free = false;
    bool On() const { return k != 1; }
    void Scissor(float x0, float x1, uint32_t gw, uint32_t hw, long& out0, long& out1) const;
};

// wide: the target's host width over its console width, divided by its height's (1: nothing to place). program: the
// draw goes through the title's vertex program
Placed Place(const State& s, const Assembled& a, bool program, uint32_t gw, uint32_t gh, float wide);
// once a frame from the backend's present, for XBE_2D_TAGS
void PlacementFrame();
// the place constant: 1 - k, offset, bleed, 0, all 0 for a draw left alone
inline void PlaceConstant(const Placed& p, float out[4]) {
    out[0] = 1 - p.k;
    out[1] = p.offset;
    out[2] = p.bleed ? 1.0f : 0.0f;
    out[3] = 0;
}
// x' = x * k + offset * w folded into a row-vector PROJECTION: out = m * placement
void PlaceMatrix(const Placed& p, const float* m, float* out);

}  // namespace gpu
