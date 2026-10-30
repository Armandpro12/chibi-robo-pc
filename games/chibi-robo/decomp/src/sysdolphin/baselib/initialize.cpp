// HSD library start-up and rendering passes (sysdolphin): the current pass, the end of a pass, and the GX caches to
// invalidate at the start of the next pass.
//
// Original unit: sysdolphin/baselib/initialize.c [hypothesis: the functions are around the start of a rendering pass
// (US 801CAF20, which stores the pass and calls GXSetPixelFmt, GXSetFieldMode, GXInvalidateVtxCache and
// GXInvalidateTexAll), in the order of the functions of initialize.c in cbr_decomp (https://github.com/eavpsp/
// cbr_decomp, folder decomp_dtk_main/src, CC0 1.0) and doldecomp/melee (names only), where the function after
// HSD_StartRender does nothing useful]. The code was written from the disassembly of both versions (identical in US
// and PAL, the addresses of the globals aside). HSD_GetCurrentRenderPass returns current_render_pass, as in
// cbr_decomp's initialize.c. The names HSD_EndRender and HSD_RequestGXInvalidate (and of its global) are this
// project's [hypothesis]: the reference projects have no such function or only an address-based name.
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp_globals.h"

namespace hsd {

using gc::be;
using gc::s32;
using gc::u32;

// Rendering pass (HSD_RenderPass of cbr_decomp's video.h), stored as a 32-bit word.
enum class HSD_RenderPass : s32 {
    screen = 0,
    top_half = 1,
    bottom_half = 2,
    offscreen = 3,
};

// current_render_pass of initialize.c: the pass being drawn, stored by the start of a pass.
constexpr gc::global<be<HSD_RenderPass>, global_id::hsd_current_render_pass> current_render_pass{};

// GX caches to invalidate at the start of the next rendering pass: bit 0 the vertex cache (GXInvalidateVtxCache), bit
// 1 all the textures (GXInvalidateTexAll); the start of a pass (US 801CAF7C-801CAFA8) calls them and clears the word
// [verified by reading the US disassembly]. The polygon code requests bit 0 (US 801AE934) and the texture code bit 1
// (US 801B13D4, after copying 0x20 bytes) [hypothesis on why].
inline constexpr u32 gx_invalidate_vertex_cache = 1u << 0;
inline constexpr u32 gx_invalidate_textures = 1u << 1;
constexpr gc::global<be<u32>, global_id::hsd_pending_gx_invalidation> pending_gx_invalidation{};

// The rendering pass being drawn.
HSD_RenderPass HSD_GetCurrentRenderPass() {
    return *current_render_pass;
}

// End of a rendering pass: nothing to do in this game (an empty function; two functions of the game call it, US
// 8000DFA8 and 80193F3C, from the call graph of the US symbols).
void HSD_EndRender() {}

// Adds the caches of `mask` (gx_invalidate_* bits) to those to invalidate at the start of the next pass.
void HSD_RequestGXInvalidate(u32 mask) {
    *pending_gx_invalidation |= mask;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_get_current_render_pass, hsd::HSD_GetCurrentRenderPass, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(hsd_end_render, hsd::HSD_EndRender, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(hsd_request_gx_invalidate, hsd::HSD_RequestGXInvalidate, gc::abi::flag_none);
