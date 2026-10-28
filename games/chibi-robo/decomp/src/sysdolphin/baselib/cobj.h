// HSD camera objects (sysdolphin): HSD_CObj, a camera with its viewport, scissor box and eye-position and interest
// world objects.
//
// Names and layout from cbr_decomp's cobj.h (https://github.com/eavpsp/cbr_decomp, folder
// decomp_dtk_main/src/sysdolphin/baselib, CC0 1.0); doldecomp/melee's sysdolphin uses the same names (names only,
// see the project rules). The offsets that decompiled functions use are checked against this game's code (static
// asserts below, oracle); the fields between `interest` and `projection_type` keep cbr_decomp's names and types
// [hypothesis: not used by decompiled code yet].
#ifndef CHIBI_DECOMP_SYSDOLPHIN_BASELIB_COBJ_H
#define CHIBI_DECOMP_SYSDOLPHIN_BASELIB_COBJ_H

#include "object.h"
#include "wobj.h"

#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::f32;
using gc::ptr;
using gc::u16;
using gc::u32;
using gc::u8;

struct HSD_CObj {
    HSD_Obj parent;
    be<u32> flags;
    struct Viewport {
        be<f32> left;
        be<f32> right;
        be<f32> top;
        be<f32> bottom;
    } viewport;
    struct Scissor {
        be<u16> left;
        be<u16> right;
        be<u16> top;
        be<u16> bottom;
    } scissor;
    ptr<HSD_WObj> eyepos;
    ptr<HSD_WObj> interest;
    union Orientation {
        be<f32> roll;
        Vec3 up;
    } u;
    be<f32> near_plane; // `near` and `far` in cbr_decomp (reserved words on some hosts)
    be<f32> far_plane;
    union ProjectionParam {
        struct Perspective {
            be<f32> fov;
            be<f32> aspect;
        } perspective;
        struct Box {
            be<f32> top;
            be<f32> bottom;
            be<f32> left;
            be<f32> right;
        } frustum, ortho;
    } projection_param;
    be<u8> projection_type; // one of the projection_* values below
};

// Projection types (PROJ_PERSPECTIVE, PROJ_FRUSTUM and PROJ_ORTHO in cbr_decomp's cobj.h).
inline constexpr u8 projection_perspective = 1;
inline constexpr u8 projection_frustum = 2;
inline constexpr u8 projection_ortho = 3;

// Bit 30 of the camera's flags: its matrix must be computed again [hypothesis on the meaning: name of the bit's
// setter in cbr_decomp, HSD_CObjSetMtxDirty].
inline constexpr u32 cobj_mtx_dirty = 1u << 30;

#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(HSD_CObj, flags) == 0x08);
static_assert(offsetof(HSD_CObj, viewport) == 0x0C && sizeof(HSD_CObj::Viewport) == 0x10);
static_assert(offsetof(HSD_CObj, scissor) == 0x1C && sizeof(HSD_CObj::Scissor) == 8);
static_assert(offsetof(HSD_CObj, eyepos) == 0x24);
static_assert(offsetof(HSD_CObj, interest) == 0x28);
static_assert(offsetof(HSD_CObj, projection_type) == 0x50);
#endif

} // namespace hsd

#endif // CHIBI_DECOMP_SYSDOLPHIN_BASELIB_COBJ_H
