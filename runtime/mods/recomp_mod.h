/*
 * Mods: libraries a player drops beside the game, loaded when the game's
 * Direct3D device is made (mods.cpp). A mod changes or adds to a title
 * without being built into it -- an overlay, a debugging tool, support for
 * a D3D9 tool such as RTX Remix in one game, a fix someone wants to try --
 * so the game's own build stays the port. docs/mods.md is the modder's
 * guide; a change to this contract updates it in the same commit.
 *
 * Where: the folder `mods` beside the executable; every .dll in it, in name
 * order. RECOMP_MOD_LIBS=0 loads none; RECOMP_MOD_LIBS_DIR names another
 * folder. (Texture packs are not mods: they go in textures\replacement\.)
 *
 * What a mod exports: one C function,
 *
 *     int recomp_mod_init(const RecompModApi *api);
 *
 * called once, on the game's thread, as the device is first made (so before
 * the renderer exists, and before the game's first draw). It returns nonzero
 * to stay loaded; zero (not this title, say) and it is unloaded again. The
 * api it is handed lives for the whole run.
 *
 * This header is the whole contract, and stands alone: a mod is built
 * against a copy of it, not against the SDK. It is the same contract, field
 * for field, as the DirectXbox-Recomp toolkit's, so one mod build loads under
 * either. Fields are only ever added at the end; a mod checks `size` before
 * using one newer than it knows, and `version` names the newest set.
 *
 * This SDK makes no frame captures: `replaying` is always 0, the capture
 * hooks are accepted and never called, and capture_note does nothing.
 */
#ifndef RECOMP_MOD_H
#define RECOMP_MOD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECOMP_MOD_API_VERSION 10

/* ---- the Direct3D 9 renderer (renderer = "d3d9" in the settings file) ----
 *
 * A mod hears of each draw the D3D9 renderer makes, after the renderer has
 * set the device up for it and before it is drawn, and again after. In
 * between it may change the device's state for that one draw -- what it
 * changes it puts back in `after` -- or have the draw left out. Under the
 * other renderers none of this is called. */

/* What the renderer knows of a draw. Fields are only added at the end. */
typedef struct RecompModDraw {
    uint32_t size;                  /* sizeof(RecompModDraw) when made */
    void    *device;                /* the IDirect3DDevice9 */
    int      title;                 /* the title's own draw, not the toolkit's
                                       (its overlay, its display passes) */
    int      two_d;                 /* the title's draw is screen-space 2D: no
                                       projection, or an orthographic one */
    int      title_2d;              /* fixed function: the title's own 2D test
                                       took it for 2D (two_d is then set too) */
    float    clip_w;                /* then: its mean clip w, or 0 */
    int      offscreen;             /* drawn into a texture, not the back buffer */
    int      program;               /* through a vertex program */
    uint64_t program_id;            /* the program's bytecode hash, or 0 */
    uint32_t primitive;             /* D3DPRIMITIVETYPE */
    uint32_t primitives;
    void    *textures[8];           /* IDirect3DBaseTexture9 per stage, or NULL */
    uint64_t texture_ids[8];        /* what each holds: stable across a replay */
    uint32_t screen_copies;         /* bit per stage: a copy of the screen */
    int      depth_state;           /* a depth state is bound; then: */
    int      depth_enable, depth_write;
    uint32_t depth_func;            /* D3DCMPFUNC */
    int      projection_set;        /* fixed function: the title set a projection */
    float    projection[16];        /* fixed function: it, as the title gave it */
    float    world[16], view[16];   /* fixed function: as the device has them */
    int      program_projection_set; /* a program: the toolkit knows its projection; */
    float    program_projection[16];/* then: it, as a D3D row-vector PROJECTION
                                       (clip = p * it for a position p in the
                                       program's view space, with the toolkit's
                                       widescreen and zoom applied) */
    int      indexed;               /* drawn through an index buffer; if not: */
    uint32_t first_vertex;          /* its vertices in the stream the device has */
    uint32_t vertices;
    /* If indexed: the draw's indices, for this call only. Each is added to
     * base_vertex to give a vertex in that stream (GetStreamSource), read as
     * the device's vertex declaration says. */
    const void *indices;
    uint32_t index_bytes;           /* 2 or 4 */
    uint32_t index_count;
    int32_t  base_vertex;
    int      fixed_transform;       /* the program (`program` is set) is a plain
                                       transform D3D9's fixed function may draw
                                       (d3d9_fixed_transform): no vertex shader
                                       is bound, WORLD is its model-view and VIEW
                                       identity, and PROJECTION its projection.
                                       Its pixel shader is still bound: remove it
                                       in before and fixed function draws this,
                                       leave it and it stays a program. Or clear
                                       this in before: it stays a program either
                                       way (a toolkit older than this one draws it
                                       fixed if the pixel shader is gone) */
    /* version 8 (this SDK's): a 2D draw's (two_d) place on its target, in
       the target's console pixels, as the title's program or fixed function
       puts its vertices: left, top, right, bottom. 0 when it could not be
       found or the draw is not 2D. A box that stays put while the camera
       moves is the screen's (a HUD); one that moves with it is the world's
       (a sprite the title projects itself) */
    int      screen_box_set;
    float    screen_box[4];
    /* version 9: what the title's pixel stage does with texture 0 and the
     * vertex colour, as Direct3D 9's stage 0 would say it (D3DTOP, D3DTA): for a tool that
     * reads texture stage states instead of running shaders. The toolkit runs
     * a pixel shader's combiners on sample colours with the draw's constants,
     * or reads a fixed function stage 0 used alone. A half not known (several
     * textures or stages, a texture shader, specular, or a sum no one stage
     * makes) says texture times vertex colour. On a draw with fixed_transform
     * set, stage 0 is already this. */
    uint32_t stage_known;           /* 1: the colour half is known, 2: the alpha */
    uint32_t stage_color_op, stage_color_arg1, stage_color_arg2;
    uint32_t stage_alpha_op, stage_alpha_arg1, stage_alpha_arg2;
} RecompModDraw;

#define RECOMP_DRAW_AS_IS 0         /* before's answers */
#define RECOMP_DRAW_SKIP  1         /* left out: before changed nothing */
#define RECOMP_DRAW_DEFER 2         /* version 7: made at the frame's end instead, after the title's
                                       last draw and before it is shown, into the same target with
                                       the device as before left it; the afters are called now. No
                                       later mod's before sees it. A toolkit older than 7 takes it
                                       for AS_IS: check api->version */

/* Whether a plain transform may be drawn by fixed function (d3d9_fixed_transform),
 * by the depth state the title has set for it: depth_func is a D3DCMPFUNC. */
typedef int  (*RecompModFixedFn)(int depth_enable, int depth_write, uint32_t depth_func, void *user);
typedef int  (*RecompModBeforeDraw)(RecompModDraw *draw, void *user);
typedef void (*RecompModAfterDraw)(RecompModDraw *draw, void *user);
typedef void (*RecompModDeviceFn)(void *device, void *user);
/* what: RECOMP_CLEAR_* bits; whole: the whole surface, not rectangles. */
typedef void (*RecompModClearFn)(uint32_t what, float z, int whole, void *user);
#define RECOMP_CLEAR_COLOR   1u
#define RECOMP_CLEAR_DEPTH   2u
#define RECOMP_CLEAR_STENCIL 4u

/* The title is copying a rectangle of the picture into a texture, as a title
 * does to show it on something, or to blur it. Fields are only added at the
 * end. */
typedef struct RecompModCopy {
    uint32_t size;                  /* sizeof(RecompModCopy) when made */
    void    *device;                /* the IDirect3DDevice9 */
    void    *source;                /* IDirect3DSurface9: what will be read */
    void    *dest;                  /* IDirect3DSurface9: what will be written */
    int      rect;                  /* a rectangle of it, rather than all of it: */
    int32_t  left, top, right, bottom;
} RecompModCopy;

/* Returns a surface to read instead (IDirect3DSurface9, the rectangle in it
 * as in `source`), or NULL to leave it. The mod keeps its reference. */
typedef void *(*RecompModCopyFn)(const RecompModCopy *copy, void *user);

typedef struct RecompModApi {
    uint32_t version;               /* RECOMP_MOD_API_VERSION when made */
    uint32_t size;                  /* sizeof(RecompModApi) when made */

    /* ---- the title ---- */

    /* The title running: its certificate's title ID (0x4553000A for
     * TimeSplitters 2), and its name, UTF-8 (NULL if it has none). Which
     * release it is the mod reads from the certificate in guest memory
     * (the XBE header is at 0x00010000, as on the console). */
    uint32_t    (*title_id)(void);
    const char *(*title_name)(void);

    /* A host pointer to guest address va. Guest memory is one block, so
     * the pointer is good for as far as the title's own data runs. */
    void       *(*guest_pointer)(uint32_t va);

    /* fn(user) once a frame, after the title's frame is shown. Up to 16
     * across all mods; returns 0 when full. */
    int         (*on_frame)(void (*fn)(void *user), void *user);

    /* The title's vertex-program constants as they stand for the draw
     * being made, count registers from first, in the hardware's order (the
     * game's register + 96, as gpu::State::vsc holds them). Returns how many
     * were copied. */
    uint32_t    (*vertex_constants)(uint32_t first, uint32_t count, float (*out)[4]);

    /* A line in the game's log (stderr), prefixed "[MOD]". */
    void        (*log)(const char *line);

    /* ---- the Direct3D 9 renderer (see above) ---- */

    /* Each returns 0 if there is no room left (8 of each across mods). */
    int   (*d3d9_on_device)(RecompModDeviceFn fn, void *user);    /* made */
    int   (*d3d9_on_draw)(RecompModBeforeDraw before, RecompModAfterDraw after,
                          void *user);
    int   (*d3d9_on_clear)(RecompModClearFn fn, void *user);
    int   (*d3d9_on_present)(RecompModDeviceFn fn, void *user);   /* before Present */

    /* HLSL put in front of every vertex program, from init only (before
     * the first is compiled). It may define RHI_MOD_POSITION(p), which is
     * then run on each output position (float4, clip space); the
     * renderer's own rhi_depth_clamp (c254) is declared before it, and its
     * w is always 0. It may also define RHI_MOD_NORMAL(n): a program that
     * rotates a normal then writes it to a NORMAL output, and this is run on
     * it first (float3, in whatever space the program rotated it to), so a
     * tool reading vertex outputs gets a normal after the program's skinning.
     * Returns 0 if it does not fit. */
    int   (*d3d9_vertex_hlsl)(const char *hlsl);

    /* A render state as the renderer last set it: returns 0 if it has not
     * set it (the device's default stands), else fills value. */
    int   (*d3d9_render_state)(uint32_t state, uint32_t *value);

    /* The fixed-function PROJECTION for the draw in hand, with the
     * renderer's viewport correction folded in, as it sets the title's. */
    void  (*d3d9_set_projection)(const float m[16]);

    /* On: a CPU-filled texture whose level 0 changes after a draw used it
     * is made anew (a new IDirect3DTexture9, its texels copied over), for
     * a D3D9 runtime that knows a texture by its contents, read once. */
    void  (*d3d9_remake_written_textures)(int on);

    /* A transform (D3DTRANSFORMSTATETYPE) set as the title's own would be:
     * the renderer keeps it as the device's, for later draws too, until
     * the title or a mod sets it again, and from `before` it applies to
     * the draw in hand. A vertex program ignores these; a D3D9 runtime
     * that reads them (RTX Remix) does not. */
    void  (*d3d9_set_transform)(uint32_t type, const float m[16]);

    /* ---- frame captures and replays ---- */

    /* Nonzero inside d3d8_replay (see above). */
    int   (*replaying)(void);

    /* fn(user) just before each draw is written to a frame capture, while
     * one is being made. Up to 8 across mods; 0 when full. With the
     * title's calls deferred (RECOMP_HLE_D3D8_DEFER), draws are written
     * when the frame's queue is run, at its end: guest memory then shows
     * the end of the frame, not the moment of that draw. */
    int   (*capture_on_draw)(void (*fn)(void *user), void *user);

    /* While a capture is being made: bytes of the mod's own, stored at this
     * point of it under tag (a mod's own four characters, say). Does
     * nothing otherwise. */
    void  (*capture_note)(uint32_t tag, const void *data, uint32_t bytes);

    /* In a replay: fn for each note, at the point of the capture it was
     * stored at, before the draw that came after it. Up to 8; 0 when full. */
    int   (*replay_on_note)(void (*fn)(uint32_t tag, const void *data, uint32_t bytes,
                                   void *user), void *user);

    /* ---- the Direct3D 9 renderer, added later ---- */

    /* fn before each copy of the picture into a texture, in a run and in a
     * replay. Up to 8 across mods, the first answer standing; 0 when full. */
    int   (*d3d9_on_copy)(RecompModCopyFn fn, void *user);

    /* ---- version 4 ---- */

    /* The title's own vertex buffer the draw in hand reads its vertices
     * from, from a d3d9_on_draw before or after: returns the guest address
     * of its D3DVertexBuffer (0 when the draw's vertices came from no buffer
     * of the title's, or in a replay), and fills the guest address of the
     * buffer's data, its stride, and the first vertex the draw reads from it
     * (the base vertex, for an indexed draw). This is how a mod tells which
     * of the title's models a draw is: the title keeps its buffers. */
    uint32_t (*draw_vertex_buffer)(uint32_t *data, uint32_t *stride, uint32_t *first);

    /* ---- version 5 ---- */

    /* Have a vertex program that is only a plain transform drawn by D3D9's
     * fixed-function pipeline: a matrix or two, a colour and coordinates
     * passed on, nothing fixed function could not do. For a tool that reads
     * the fixed-function transforms and otherwise has to read each vertex
     * back out of the shader. Such a draw comes with fixed_transform set
     * (RecompModDraw). `keep`, if given, is asked first, and a 0 leaves that
     * draw a program. XBE_D3D9_FIXED_TRANSFORM has the last word. */
    void (*d3d9_fixed_transform)(int on, RecompModFixedFn keep, void *user);

    /* ---- version 6 (this SDK's; not in the DirectXbox-Recomp toolkit yet). Version 7 adds no
     * field: a before may answer RECOMP_DRAW_DEFER ---- */

    /* An overlay the mod knows of has the mouse, one with a cursor of its own
     * (a D3D9 tool's menu, say): on, the game lets go of it -- mouse look stops
     * holding it, its buttons and wheel stop pressing the game's controls --
     * and leaves the cursor to the overlay; off gives it back. Counted: pair
     * each on with an off. Any thread. */
    void (*pointer_elsewhere)(int on);

    /* ---- version 10 ---- */

    /* On: a model the game's own hooks can give whole is drawn as one
     * skinned mesh per texture by D3D9's fixed function -- indexed vertex
     * blending, D3DTS_WORLDMATRIX(0..255) each a bone's model-view, VIEW
     * identity, PROJECTION the title's with its program's screen lines
     * (c-38, c-37) folded in, one BLENDWEIGHT of 1 per vertex beside the
     * index -- rather than in the pieces of a few matrices its vertex
     * programs draw it in. For a tool that skins a fixed-function draw
     * itself and takes it as one object (RTX Remix, up to 256 bones, in what
     * it ray traces). A rasterizer's fixed function addresses matrices 0 to 8
     * only (MaxVertexBlendMatrixIndex 8 on D3D9 and on Remix's), so turn it
     * on only under such a tool. Needs the renderer on the game's thread (a
     * mod that hooks draws); these draws are not shown to d3d9_on_draw. */
    void (*d3d9_skinned_meshes)(int on);
} RecompModApi;

typedef int (*RecompModInitFn)(const RecompModApi *api);

/* Whether the toolkit's api has a field (one added after the mod's oldest
 * supported version): its size covers it. */
#define RECOMP_MOD_HAS(api, field) \
    ((api)->size >= offsetof(RecompModApi, field) + sizeof((api)->field))

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_MOD_H */
