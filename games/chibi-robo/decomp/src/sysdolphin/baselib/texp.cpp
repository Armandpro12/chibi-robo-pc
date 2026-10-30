// HSD TEV expressions (sysdolphin): type of a node, reference counting, and assignment of constants to registers.
//
// Original unit: sysdolphin/baselib/texp.c [hypothesis: names and place in the unit from doldecomp/melee (names only)
// and TeamOrre/xd-decomp (CC0), which put HSD_TExpGetType and HSD_TExpRef next to the same neighbours; the three
// register assignment functions are the statics AssignColorReg, AssignColorKonst and AssignAlphaKonst of cbr_decomp's
// texp.c (CC0 1.0), whose logic, fields and lookup tables they have (symbol supplement, names from doldecomp/melee,
// names only); in this game their tables are read from the game's memory (globals table), never copied]. The code was
// written from the disassembly of both versions (identical in US and PAL). Cross-check [hypothesis on the identity]:
// both have the logic and the fields of the functions of the same names in cbr_decomp's texp.c
// (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main/src, CC0 1.0). Structure: texp.h.
#include "texp.h"

#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp/gc_path.h"
#include "decomp_globals.h"

namespace hsd {

// Cost by path (runtime/core/decomp/gc_path.h): gc::path::taken / falls_through mark, in the original's order, the
// outcome of each of its conditional branches (identical in US and PAL), so that each call is charged the cycles of the
// original's path; they return their condition unchanged.
// Kind of a node: zero for no node, texture or raster for the two marker pointers, otherwise the node's own type. (The
// original branches away on the "other" side of each test: falls_through.)
HSD_TExpType HSD_TExpGetType(ptr<const HSD_TExp> texp) {
    if (gc::path::falls_through(texp == nullptr))
        return HSD_TExpType::zero;
    if (gc::path::falls_through(is_texp_marker(texp, texp_marker_texture)))
        return HSD_TExpType::texture;
    if (gc::path::falls_through(is_texp_marker(texp, texp_marker_raster)))
        return HSD_TExpType::raster;
    return gc::load(texp->type);
}

// Counts one more reference to the node: to the colour or the alpha result of a TEV stage (`select`), or to a
// constant. Other kinds keep no count. The original has the call to HSD_TExpGetType inlined (its path marks count
// for this function then) and tests the kind in this order: constant, above constant (signed), tev.
void HSD_TExpRef(ptr<HSD_TExp> texp, u8 select) {
    const HSD_TExpType type = HSD_TExpGetType(texp);
    if (gc::path::taken(type == HSD_TExpType::constant)) {
        texp->cnst.ref += 1;
        return;
    }
    if (gc::path::taken(type > HSD_TExpType::constant))
        return; // a kind above constant: no count
    if (gc::path::taken(type != HSD_TExpType::tev))
        return; // zero, texture or raster: no count
    if (gc::path::falls_through(select == texp_select_colour))
        texp->tev.c_ref += 1;
    else
        texp->tev.a_ref += 1;
}

// Lookup tables of the register assignment functions (static arrays of cbr_decomp's texp.c; one 32-bit GX enumeration
// value per entry, of which the original stores the low byte). Read from the game's memory, by version:
//   a_in / c_in of AssignColorReg: the GX colour argument of the alpha (GX_CC_A0..) and of the colour (GX_CC_C0..) of
//   colour registers 4-7 (index: register - 4);
//   xsel / csel of AssignColorKonst: the GX constant colour selection of one component of constant register 0-3
//   ([register][component]) and of a whole constant register; sel of AssignAlphaKonst: the constant alpha selection
//   of one component ([register][component]).
using lookup_row = gc::array<be<u32>, 4>;
constexpr gc::global<lookup_row, global_id::hsd_texp_register_alpha_args> register_alpha_args{};
constexpr gc::global<lookup_row, global_id::hsd_texp_register_colour_args> register_colour_args{};
constexpr gc::global<gc::array<lookup_row, 4>, global_id::hsd_texp_konst_component_colour_sel>
    konst_component_colour_sel{};
constexpr gc::global<lookup_row, global_id::hsd_texp_konst_colour_sel> konst_colour_sel{};
constexpr gc::global<gc::array<lookup_row, 4>, global_id::hsd_texp_konst_component_alpha_sel>
    konst_component_alpha_sel{};

namespace {

u8 low_byte(const be<u32>& entry) {
    return static_cast<u8>(gc::load(entry) & 0xFFu);
}

// The constant node that feeds `input` (the original reads the input's node pointer once, at the start).
ptr<HSD_TECnst> constant_of(const HSD_TEArg& input) {
    return gc::address_of(input.exp->cnst);
}

} // namespace

// Feeds colour input `index` of the stage from a colour register (4-7): the one its constant is assigned to, or the
// first free one, which is then reserved in `res` (its alpha for a one-component constant, its colour otherwise).
// 0 on success, -1 if the constant is assigned to a constant register (0-3) or no colour register is free. Reads and
// writes in the original's order: an assigned register is read again from the node after the input's type is stored.
s32 AssignColorReg(ptr<HSD_TETev> tev, u32 index, ptr<HSD_TExpRes> res) {
    HSD_TEArg& input = tev->c_in[index];
    const ptr<HSD_TECnst> cnst = constant_of(input);
    const u8 assigned = gc::load(cnst->reg);
    if (gc::path::falls_through(assigned != texp_register_undefined)) {
        if (gc::path::falls_through(assigned < 4))
            return -1;
        input.type = texp_arg_immediate;
        if (gc::path::falls_through(gc::load(cnst->comp) == texp_input_x))
            input.arg = low_byte((*register_alpha_args)[gc::load(cnst->reg) - 4u]);
        else
            input.arg = low_byte((*register_colour_args)[gc::load(cnst->reg) - 4u]);
        return 0;
    }
    if (gc::path::falls_through(gc::load(cnst->comp) == texp_input_x)) {
        u32 reg = 4;
        do {
            if (gc::path::falls_through(gc::load(res->reg[reg].alpha) == 0)) {
                res->reg[reg].alpha = 1;
                cnst->reg = static_cast<u8>(reg);
                cnst->idx = 3;
                input.type = texp_arg_immediate;
                input.arg = low_byte((*register_alpha_args)[reg - 4u]);
                return 0;
            }
            ++reg;
        } while (gc::path::taken(reg < 8)); // original: a counted loop (bdnz), four turns
        return -1;
    }
    u32 reg = 4;
    do {
        if (gc::path::falls_through(gc::load(res->reg[reg].color) == 0)) {
            res->reg[reg].color = 3;
            cnst->reg = static_cast<u8>(reg);
            cnst->idx = 0;
            input.type = texp_arg_immediate;
            input.arg = low_byte((*register_colour_args)[reg - 4u]);
            return 0;
        }
        ++reg;
    } while (gc::path::taken(reg < 8));
    return -1;
}

// Feeds colour input `index` of the stage from a constant register (0-3) through the stage's constant colour
// selection: the register its constant is assigned to, or one reserved in `res`. A one-component constant takes the
// alpha of register 1-3 if one is free, otherwise the next free colour component of register 0-3; a colour constant
// takes the first register 0-3 whose colour is unused. 0 on success, -1 if the constant is assigned to a colour
// register (4-7) or nothing is free. Reads and writes in the original's order (the node's register and component are
// read again after they are stored).
s32 AssignColorKonst(ptr<HSD_TETev> tev, u32 index, ptr<HSD_TExpRes> res) {
    HSD_TEArg& input = tev->c_in[index];
    const ptr<HSD_TECnst> cnst = constant_of(input);
    const u8 assigned = gc::load(cnst->reg);
    if (gc::path::falls_through(assigned != texp_register_undefined)) {
        if (gc::path::falls_through(assigned >= 4))
            return -1;
        if (gc::path::falls_through(gc::load(cnst->comp) == texp_input_x))
            tev->kcsel = low_byte((*konst_component_colour_sel)[assigned][gc::load(cnst->idx)]);
        else
            tev->kcsel = low_byte((*konst_colour_sel)[assigned]);
        input.type = texp_arg_konst;
        input.arg = gx_cc_konst;
        return 0;
    }
    if (gc::path::falls_through(gc::load(cnst->comp) == texp_input_x)) {
        u32 reg = 1;
        do {
            if (gc::path::falls_through(gc::load(res->reg[reg].alpha) == 0)) {
                res->reg[reg].alpha = 1;
                cnst->reg = static_cast<u8>(reg);
                cnst->idx = 3;
                tev->kcsel = low_byte((*konst_component_colour_sel)[gc::load(cnst->reg)][gc::load(cnst->idx)]);
                input.type = texp_arg_konst;
                input.arg = gx_cc_konst;
                return 0;
            }
            ++reg;
        } while (gc::path::taken(reg < 4)); // three turns
        reg = 0;
        do {
            if (gc::path::falls_through(gc::load(res->reg[reg].color) < 3)) {
                cnst->reg = static_cast<u8>(reg);
                const u8 component = gc::load(res->reg[reg].color);
                res->reg[reg].color = static_cast<u8>(component + 1);
                cnst->idx = component;
                tev->kcsel = low_byte((*konst_component_colour_sel)[gc::load(cnst->reg)][gc::load(cnst->idx)]);
                input.type = texp_arg_konst;
                input.arg = gx_cc_konst;
                return 0;
            }
            ++reg;
        } while (gc::path::taken(reg < 4)); // four turns
        return -1;
    }
    u32 reg = 0;
    do {
        if (gc::path::falls_through(gc::load(res->reg[reg].color) == 0)) {
            res->reg[reg].color = 3;
            cnst->reg = static_cast<u8>(reg);
            cnst->idx = 0;
            tev->kcsel = low_byte((*konst_colour_sel)[gc::load(cnst->reg)]);
            input.type = texp_arg_konst;
            input.arg = gx_cc_konst;
            return 0;
        }
        ++reg;
    } while (gc::path::taken(reg < 4));
    return -1;
}

// Feeds alpha input `index` of the stage from one component of a constant register (0-3) through the stage's
// constant alpha selection: the register its constant is assigned to, or the alpha of a free register 1-3, or the
// next free colour component of register 0-3 (the constant's kind is not tested, unlike AssignColorKonst). 0 on
// success, -1 if the constant is assigned to a colour register (4-7) or nothing is free. Reads and writes in the
// original's order.
s32 AssignAlphaKonst(ptr<HSD_TETev> tev, u32 index, ptr<HSD_TExpRes> res) {
    HSD_TEArg& input = tev->a_in[index];
    const ptr<HSD_TECnst> cnst = constant_of(input);
    const u8 assigned = gc::load(cnst->reg);
    if (gc::path::falls_through(assigned != texp_register_undefined)) {
        if (gc::path::falls_through(assigned >= 4))
            return -1;
        tev->kasel = low_byte((*konst_component_alpha_sel)[assigned][gc::load(cnst->idx)]);
        input.type = texp_arg_konst;
        input.arg = gx_ca_konst;
        return 0;
    }
    u32 reg = 1;
    do {
        if (gc::path::falls_through(gc::load(res->reg[reg].alpha) == 0)) {
            res->reg[reg].alpha = 1;
            cnst->reg = static_cast<u8>(reg);
            cnst->idx = 3;
            tev->kasel = low_byte((*konst_component_alpha_sel)[gc::load(cnst->reg)][gc::load(cnst->idx)]);
            input.type = texp_arg_konst;
            input.arg = gx_ca_konst;
            return 0;
        }
        ++reg;
    } while (gc::path::taken(reg < 4));
    reg = 0;
    do {
        if (gc::path::falls_through(gc::load(res->reg[reg].color) < 3)) {
            cnst->reg = static_cast<u8>(reg);
            const u8 component = gc::load(res->reg[reg].color);
            res->reg[reg].color = static_cast<u8>(component + 1);
            cnst->idx = component;
            tev->kasel = low_byte((*konst_component_alpha_sel)[gc::load(cnst->reg)][gc::load(cnst->idx)]);
            input.type = texp_arg_konst;
            input.arg = gx_ca_konst;
            return 0;
        }
        ++reg;
    } while (gc::path::taken(reg < 4));
    return -1;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_texp_get_type, hsd::HSD_TExpGetType, gc::abi::flag_reads_only | gc::abi::flag_path_marked);
GC_DECOMP_EXPORT(hsd_texp_ref, hsd::HSD_TExpRef, gc::abi::flag_path_marked);
GC_DECOMP_EXPORT(hsd_texp_assign_color_reg, hsd::AssignColorReg, gc::abi::flag_path_marked);
GC_DECOMP_EXPORT(hsd_texp_assign_color_konst, hsd::AssignColorKonst, gc::abi::flag_path_marked);
GC_DECOMP_EXPORT(hsd_texp_assign_alpha_konst, hsd::AssignAlphaKonst, gc::abi::flag_path_marked);
