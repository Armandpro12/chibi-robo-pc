// HSD light objects (sysdolphin): HSD_LObj, the light masks and the active lights.
//
// Original unit: sysdolphin/baselib/lobj.c [hypothesis: names and place in the unit from doldecomp/melee (names only)
// and TeamOrre/xd-decomp (CC0), symbol supplement]. The code was written from the disassembly of both versions
// (identical in US and PAL, the addresses of the globals aside). Cross-check [hypothesis on the identity]: the
// functions have the logic and the fields of the functions of the same names in cbr_decomp's lobj.c
// (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main/src, CC0 1.0), whose statics give the names of the
// globals: lightmask_diffuse, lightmask_specular, lightmask_attnfunc and lightmask_alpha are declared in that order
// there and lie at consecutive small-data addresses here (the attenuation mask, read by US 801BD060, is not
// decompiled yet), then nb_active_lights and active_lights. In this game the index test of HSD_LObjGetActiveByIndex
// accepts 0 to 7 (cbr_decomp: 0 to GX_MAX_LIGHT - 2). Structure: lobj.h.
#include "lobj.h"

#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp/gc_path.h"
#include "decomp_globals.h"

// Cost by path (runtime/core/decomp/gc_path.h): gc::path::taken / falls_through mark, in the original's order, the
// outcome of each of its conditional branches (identical in US and PAL), so that each call is charged the cycles of the
// original's path; they return their condition unchanged.
namespace hsd {

using gc::s32;

// Masks of the GX lights (one bit per light) of the active lights that light the diffuse colour, the alpha and the
// specular highlights [hypothesis on the meaning: from the names], and the number of active lights.
constexpr gc::global<be<s32>, global_id::hsd_lobj_lightmask_diffuse> light_mask_diffuse{};
constexpr gc::global<be<s32>, global_id::hsd_lobj_lightmask_alpha> light_mask_alpha{};
constexpr gc::global<be<s32>, global_id::hsd_lobj_lightmask_specular> light_mask_specular{};
constexpr gc::global<be<s32>, global_id::hsd_lobj_nb_active_lights> active_light_count{};

// active_lights of lobj.c: the active lights, by index (eight slots).
inline constexpr s32 active_light_slots = 8;
constexpr gc::global<gc::array<ptr<HSD_LObj>, active_light_slots>, global_id::hsd_lobj_active_lights> active_lights{};

// Sets the light's colour. The colour is passed by value in C, which the PowerPC EABI turns into a pointer to a copy
// (r4); there is no test of a missing light (the original has none). The original moves the four bytes with one
// word load and one word store, so the colour is first copied into a local: the result is the original's even when
// the source overlaps the light's own colour (anomaly A19).
void HSD_LObjSetColor(ptr<HSD_LObj> lobj, ptr<const GXColor> color) {
    const GXColor value = *color;
    lobj->color = value;
}

s32 HSD_LObjGetLightMaskDiffuse() {
    return *light_mask_diffuse;
}

s32 HSD_LObjGetLightMaskAlpha() {
    return *light_mask_alpha;
}

s32 HSD_LObjGetLightMaskSpecular() {
    return *light_mask_specular;
}

s32 HSD_LObjGetNbActive() {
    return *active_light_count;
}

// The active light in slot `index`, null when the index is outside the eight slots (signed tests: a negative index is
// refused first).
ptr<HSD_LObj> HSD_LObjGetActiveByIndex(s32 index) {
    if (gc::path::taken(index < 0) || gc::path::taken(index >= active_light_slots))
        return nullptr;
    return (*active_lights)[static_cast<std::size_t>(index)];
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_lobj_set_color, hsd::HSD_LObjSetColor, gc::abi::flag_none);
GC_DECOMP_EXPORT(hsd_lobj_get_light_mask_diffuse, hsd::HSD_LObjGetLightMaskDiffuse, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(hsd_lobj_get_light_mask_alpha, hsd::HSD_LObjGetLightMaskAlpha, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(hsd_lobj_get_light_mask_specular, hsd::HSD_LObjGetLightMaskSpecular, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(hsd_lobj_get_nb_active, hsd::HSD_LObjGetNbActive, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(hsd_lobj_get_active_by_index, hsd::HSD_LObjGetActiveByIndex,
                 gc::abi::flag_reads_only | gc::abi::flag_path_marked);
