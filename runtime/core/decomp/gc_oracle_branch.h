// gc_oracle_branch.h: how the decompilation oracle reads a call instruction of a function being compared (gc_oracle.cpp
// call_hook) and recognizes the register save/restore helpers of the ABI. Plain values only (no CPU, no game), so
// that it can be tested on hand-written instruction words (tests/decomp/test_call_sites.py).
//
// PowerPC branches read here (PowerPC User Instruction Set Architecture, Book I, "Branch Processor"; cross-checked on
// the game's code, notes/decomp.md D1.1): b/bl (opcode 18, LI, AA, LK), bc/bcl (opcode 16, BO, BI, BD, AA, LK),
// bclr/bclrl and bcctr/bcctrl (opcode 19, extended opcodes 16 and 528, BO, BI, LK). BO bit 0x04 clear: CTR is
// decremented and tested (BO bit 0x02: branch if CTR = 0); bcctr never decrements CTR (that form is invalid). BO bit
// 0x10 set: the condition is not tested; otherwise CR bit BI must equal BO bit 0x08.
#ifndef GC_ORACLE_BRANCH_H
#define GC_ORACLE_BRANCH_H

#include <cstdint>

namespace gc::oracle {

using branch_u32 = std::uint32_t;

enum class branch_kind : std::uint8_t {
    none,      // not a branch the oracle handles (or a return, bclr without link)
    not_taken, // a conditional branch whose condition fails: the instruction falls through by itself
    internal,  // a branch without link to an address inside the function
    call,      // a branch with link (bl, bcl, bclrl, bcctrl): the function goes on after it
    tail,      // a branch without link out of the function (b, bc, bcctr): it leaves for another one
};

struct branch_decision {
    branch_kind kind = branch_kind::none;
    branch_u32 target = 0;    // branch target (immediate, LR or CTR, low two bits cleared)
    branch_u32 ctr_after = 0; // CTR once the instruction ran (decremented by a bc/bclr whose BO says so)
};

// Reads the instruction `word` at `address` of the function [start, start + size), with the registers CR, CTR and LR
// it would see.
inline branch_decision decode_branch(branch_u32 word, branch_u32 address, branch_u32 start, branch_u32 size,
                                     branch_u32 cr, branch_u32 ctr, branch_u32 lr) noexcept {
    branch_decision d;
    d.ctr_after = ctr;
    const branch_u32 op = word >> 26, xo = (word >> 1) & 0x3FFu, lk = word & 1u;
    const bool to_lr = op == 19 && xo == 16, to_ctr = op == 19 && xo == 528;
    if (op != 18 && op != 16 && !to_lr && !to_ctr)
        return d;
    if (to_lr && !lk)
        return d; // a return (blr, bclr): not a call
    if (op != 18) {
        const branch_u32 bo = (word >> 21) & 31u, bi = (word >> 16) & 31u;
        const bool decrement = !(bo & 4u) && !to_ctr;
        if (decrement)
            d.ctr_after = ctr - 1u;
        const bool ctr_ok = !decrement || ((d.ctr_after != 0) != ((bo & 2u) != 0));
        const bool cond_ok = (bo & 16u) || (((cr >> (31u - bi)) & 1u) == ((bo >> 3) & 1u));
        if (!(ctr_ok && cond_ok)) {
            d.kind = branch_kind::not_taken;
            return d;
        }
    }
    if (op == 18 || op == 16) {
        std::int32_t off = op == 18 ? static_cast<std::int32_t>(word & 0x03FFFFFCu)
                                    : static_cast<std::int32_t>(word & 0xFFFCu);
        if (op == 18 && (off & 0x02000000))
            off -= 0x04000000;
        if (op == 16 && (off & 0x8000))
            off -= 0x10000;
        d.target = (word & 2u) ? static_cast<branch_u32>(off) : address + static_cast<branch_u32>(off);
        if (!lk && d.target - start < size) {
            d.kind = branch_kind::internal;
            return d;
        }
    } else {
        d.target = (to_lr ? lr : ctr) & ~3u;
    }
    d.kind = lk ? branch_kind::call : branch_kind::tail;
    return d;
}

// True if the oracle records the branch `d` as a call of the function being compared (gc_oracle.cpp call_hook): a taken
// call or tail branch whose target is not a register save/restore helper (`helper_end`: register_helper_end of the
// target; such a branch runs inline, with link or not).
inline bool is_recorded_call(const branch_decision& d, branch_u32 helper_end) noexcept {
    return (d.kind == branch_kind::call || d.kind == branch_kind::tail) && helper_end == 0;
}

// End (address after its blr) of the register save/restore helper of the ABI at `target` (_savegpr_N, _restgpr_N,
// _savefpr_N, _restfpr_N, and the forms that also return from their caller), 0 if `target` is not one; `read_word`
// gives the instruction word at an address. Recognized from its code: up to 18 stores or loads of r14..r31 / f14..f31
// relative to r11 (stw, lwz, stfd, lfd), then, for a restore that also returns from its caller, lwz r0,4(r11) /
// mtlr r0 / mr r1,r11, and a blr. Such a branch, with or without link, is part of the caller's prologue or epilogue:
// the oracle runs it inline (the decompiled C++ has no such call).
template <class ReadWord> branch_u32 register_helper_end(branch_u32 target, ReadWord read_word) {
    branch_u32 a = target;
    unsigned ladder = 0;
    for (unsigned i = 0; i < 24; i++, a += 4) {
        const branch_u32 w = read_word(a);
        const branch_u32 op = w >> 26, rt = (w >> 21) & 31u, ra = (w >> 16) & 31u;
        if ((op == 36 || op == 32 || op == 54 || op == 50) && ra == 11 && rt >= 14) {
            ladder++;
            continue;
        }
        if (ladder && (w == 0x800B0004u || w == 0x7C0803A6u || w == 0x7D615B78u)) // lwz r0,4(r11); mtlr r0; mr r1,r11
            continue;
        if (ladder && w == 0x4E800020u) // blr
            return a + 4;
        break;
    }
    return 0;
}

} // namespace gc::oracle

#endif // GC_ORACLE_BRANCH_H
