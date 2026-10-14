/* Recognized busy waits of the game: the host waits for the next event (generic GameCube, real-time pacing).
 *
 * A wait loop of the game (frame limiter that rereads the retrace counter, wait for a flag set by
 * a callback...) costs as much host time as real computation: every iteration is executed to make time advance.
 * When the game already runs at the host's maximum speed, that time is missing elsewhere (audio running dry, late
 * frames).
 *
 * Principle (like Dolphin's "idle skipping"): a short loop that only reads memory and
 * computes on registers, without writing anything and without any register carried from one iteration to the next,
 * can only exit if memory changes; and memory only changes through a timed event (interrupt or native
 * timer). On such a loop, the host therefore waits for the next deadline
 * (gcrt_cpu_wait_until: the host sleeps with the real-time clock, emulated time jumps with the cycle clock) instead
 * of executing the intermediate iterations. What the game observes is what it would have observed by spinning (to
 * within one iteration: the current iteration finishes with the values read before the wait).
 * Milestone 10b (end-of-life plan, notes/decisions.md): no more writes to g->cycles; the OS idle loop
 * (SelectThread) and OSYieldThread loops are waits of the native OS (hle/native_os.c); this module now only
 * handles the game's own waits, and only with real-time pacing (never with the free-running cycle clock
 * of the A/B comparisons; the native OS waits, for their part, belong to the OS and are always active).
 * [verified] before 10b on the A/B bench in forced fast-forward (gardien-6a-force, 2026-10-10); [hypothesis] for
 * real time (non-deterministic); known limitation: the simulated iteration starts from the beginning of the loop and
 * not from pc, so an entry in the middle of the loop with registers coming from outside may produce a wait (at most
 * 1 ms) whereas the code would have left the loop.
 * A loop or leaf that goes through an address handled by host_call (replaced function) is rejected.
 *
 * Recognition (on the chassis's first visit to an address, then remembered):
 *   - address in the DOL code (first recompiled module: static code; the RELs are not analyzed);
 *   - cycle of at most 24 instructions closed by a backward branch to its start (b or bc without
 *     CTR decrement, without link); conditional branches to the outside allowed;
 *   - instructions: loads lwz/lbz/lhz/lha (+ indexed forms), integer arithmetic and logic without XER,
 *     shifts and rotations, comparisons, and the call (bl) of a leaf function of at most 8 instructions made
 *     of the same instructions and ended by blr;
 *   - no register (GPR, CR field) both read before being written in the iteration and written in the iteration;
 *   - at least one load; a load base computed in the iteration (lis/li/addi/ori) must designate RAM;
 *   - at the time of the wait: one complete iteration, evaluated from the start of the loop with the current memory
 *     and registers (pure subset above, loads from RAM only), comes back to the start. Otherwise (the loop is about
 *     to exit, or is only a loop head being passed through), the code runs normally. */
#include "cpu/gcrt_cpu.h"

#include <stdlib.h>
#include <string.h>

#define IDLE_MAX_BODY 24
#define IDLE_MAX_LEAF 8
#define IDLE_MAX_LOADS 8
#define IDLE_MAX_LOOPS 128
/* Maximum wait: 1 emulated ms (the hardware deadlines are closer anyway). */
#define IDLE_MAX_SKIP_TICKS (GCRT_TB_HZ / 1000u)

typedef struct {
    u8 ra, rb; /* rb = 0xFF: D form (ra + d); ra = 0xFE: address known at analysis time (abs) */
    s16 d;
    u32 abs;
} IdleLoad;

typedef struct {
    u32 start, end; /* first and last instruction of the cycle */
    u32 nloads;
    IdleLoad loads[IDLE_MAX_LOADS];
    u64 skips;
} IdleLoop;

struct GcrtIdleState {
    u8* analyzed; /* one bit per RAM instruction */
    u8* idle;
    u32 ram_size;
    IdleLoop loops[IDLE_MAX_LOOPS];
    u32 nloops;
};

typedef struct {
    u64 written; /* bits 0-31: GPR; 32-39: CR fields; 40: LR */
    u64 rbw;     /* read before being written in the iteration */
    u32 nloads;
    IdleLoad loads[IDLE_MAX_LOADS];
    /* Constants known in the iteration (lis, li, addi, ori from constants): a load base computed
     * in the iteration must be known, to check at analysis time that it designates RAM. */
    u32 known;
    u32 val[32];
} Dep;

#define CRB(f) (32 + (f))
#define LRB 40

static void use(Dep* d, unsigned bit) {
    if (!((d->written >> bit) & 1u))
        d->rbw |= 1ull << bit;
}
static void def(Dep* d, unsigned bit) {
    d->written |= 1ull << bit;
    if (bit < 32)
        d->known &= ~(1u << bit); /* unknown value unless the caller provides it again (set_known) */
}

static bool ram_address(u32 ea) {
    /* Main RAM (24 MiB maximum) through the cached or uncached access; the rest (hardware registers
     * 0xCC..., locked cache, EFB) is rejected. */
    return (ea >= 0x80000000u && ea < 0x81800000u) || (ea >= 0xC0000000u && ea < 0xC1800000u);
}

static bool written_in_turn(const Dep* d, u32 r) { return r != 0 && ((d->written >> r) & 1u); }

static bool add_load(Dep* d, u32 ra, u32 rb, s16 disp) {
    if (d->nloads >= IDLE_MAX_LOADS)
        return false;
    IdleLoad* l = &d->loads[d->nloads];
    l->ra = (u8)ra;
    l->rb = (u8)rb;
    l->d = disp;
    l->abs = 0;
    const bool ra_turn = written_in_turn(d, ra), rb_turn = rb != 0xFF && written_in_turn(d, rb);
    if (ra_turn || rb_turn) {
        /* Base computed in the iteration: it must be a known constant, and designate RAM. */
        if ((ra_turn && !((d->known >> ra) & 1u)) || (rb_turn && !((d->known >> rb) & 1u)))
            return false;
        if ((ra && !ra_turn) || (rb != 0xFF && !rb_turn))
            return false; /* mix of constant / invariant register: rejected (rare) */
        const u32 ea = (ra ? d->val[ra] : 0u) + (rb == 0xFF ? (u32)(s32)disp : d->val[rb]);
        if (!ram_address(ea))
            return false; /* hardware register: waiting for a device, not for memory */
        l->ra = 0xFE;
        l->abs = ea;
    }
    d->nloads++;
    return true;
}

static void set_known(Dep* d, u32 r, u32 v) {
    d->known |= 1u << r;
    d->val[r] = v;
}


/* Instruction with no effect outside the registers: updates the dependencies and returns true. */
static bool pure_insn(u32 ins, Dep* d) {
    const u32 op = ins >> 26, rd = (ins >> 21) & 31u, ra = (ins >> 16) & 31u, rb = (ins >> 11) & 31u;
    const s16 imm = (s16)(ins & 0xFFFFu);
    switch (op) {
    case 32: case 34: case 40: case 42: /* lwz lbz lhz lha */
        if (ra)
            use(d, ra);
        if (!add_load(d, ra, 0xFF, imm))
            return false;
        def(d, rd);
        return true;
    case 14: case 15: { /* addi addis (li, lis) */
        const u32 k = op == 15 ? (u32)imm << 16 : (u32)(s32)imm;
        if (ra)
            use(d, ra);
        const bool known = ra == 0 || (written_in_turn(d, ra) && ((d->known >> ra) & 1u));
        const u32 v = ra == 0 ? k : d->val[ra] + k;
        def(d, rd);
        if (known)
            set_known(d, rd, v);
        return true;
    }
    case 7: /* mulli */
        use(d, ra);
        def(d, rd);
        return true;
    case 24: case 25: case 26: case 27: /* ori oris xori xoris: rS = rd field */
        use(d, rd);
        const bool known = op == 24 && written_in_turn(d, rd) && ((d->known >> rd) & 1u);
        const u32 v = d->val[rd] | (u32)(u16)imm;
        def(d, ra);
        if (known)
            set_known(d, ra, v);
        return true;
    case 28: case 29: /* andi. andis. */
        use(d, rd);
        def(d, ra);
        def(d, CRB(0));
        return true;
    case 21: /* rlwinm */
        use(d, rd);
        def(d, ra);
        if (ins & 1u)
            def(d, CRB(0));
        return true;
    case 20: /* rlwimi */
        use(d, rd);
        use(d, ra);
        def(d, ra);
        if (ins & 1u)
            def(d, CRB(0));
        return true;
    case 10: case 11: /* cmpli cmpi */
        use(d, ra);
        def(d, CRB((ins >> 23) & 7u));
        return true;
    case 31: {
        const u32 xo10 = (ins >> 1) & 0x3FFu, xo9 = (ins >> 1) & 0x1FFu;
        const bool rc = (ins & 1u) != 0;
        switch (xo10) {
        case 0: case 32: /* cmp cmpl */
            use(d, ra);
            use(d, rb);
            def(d, CRB((ins >> 23) & 7u));
            return true;
        case 28: case 60: case 444: case 412: case 316: case 124: case 476: case 284: case 24: case 536:
            /* and andc or orc xor nor nand eqv slw srw */
            use(d, rd);
            use(d, rb);
            def(d, ra);
            if (rc)
                def(d, CRB(0));
            return true;
        case 954: case 922: case 26: /* extsb extsh cntlzw */
            use(d, rd);
            def(d, ra);
            if (rc)
                def(d, CRB(0));
            return true;
        case 23: case 87: case 279: /* lwzx lbzx lhzx */
            if (ra)
                use(d, ra);
            use(d, rb);
            if (!add_load(d, ra, rb, 0))
                return false;
            def(d, rd);
            return true;
        default:
            break;
        }
        if ((ins >> 10) & 1u)
            return false; /* OE = 1: XER modified */
        switch (xo9) {
        case 266: case 40: case 235: case 491: case 459: case 75: case 11: /* add subf mullw divw divwu mulhw mulhwu */
            use(d, ra);
            use(d, rb);
            def(d, rd);
            if (rc)
                def(d, CRB(0));
            return true;
        case 104: /* neg */
            use(d, ra);
            def(d, rd);
            if (rc)
                def(d, CRB(0));
            return true;
        default:
            return false;
        }
    }
    default:
        return false;
    }
}

static bool fetch(const CPUState* cpu, u32 address, u32* ins) {
    const u32 off = address - GC_RAM_BASE;
    if (address < GC_RAM_BASE || off >= cpu->ram_size || off > cpu->ram_size - 4u || (off & 3u))
        return false;
    const u8* p = cpu->ram + off;
    *ins = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
    return true;
}

/* Leaf function called by bl: pure instructions then blr. */
static bool pure_leaf(const CPUState* cpu, u32 target, Dep* d) {
    for (u32 i = 0; i < IDLE_MAX_LEAF; i++) {
        u32 ins;
        if (!fetch(cpu, target + 4u * i, &ins))
            return false;
        if (ins == 0x4E800020u) { /* blr */
            use(d, LRB);
            return true;
        }
        if (!pure_insn(ins, d))
            return false;
    }
    return false;
}

static s32 bc_disp(u32 ins) { return (s32)(s16)(ins & 0xFFFCu); }
static s32 b_disp(u32 ins) {
    s32 li = (s32)(ins & 0x03FFFFFCu);
    return (li & 0x02000000) ? li - 0x04000000 : li;
}

/* Looks for the cycle that contains `pc` and validates it. Returns the loop index, or -1. */
static int analyze(GcrtCpu* g, u32 pc, u32* start_out, u32* end_out) {
    const CPUState* cpu = &g->state;
    u32 start = 0, end = 0;
    bool found = false;
    /* 1. Closing branch: walking forward from pc. */
    for (u32 i = 0, a = pc; i < IDLE_MAX_BODY && !found; i++, a += 4u) {
        u32 ins;
        if (!fetch(cpu, a, &ins))
            return -1;
        const u32 op = ins >> 26;
        if (op == 16) { /* bc */
            if ((ins & 3u) != 0 || !((ins >> 21) & 0x04u))
                return -1; /* link, absolute address or CTR decrement */
            const u32 target = a + (u32)bc_disp(ins);
            if (target <= pc && pc - target <= 4u * IDLE_MAX_BODY) {
                start = target, end = a, found = true;
            } else if (target <= a) {
                return -1; /* inner loop */
            }
        } else if (op == 18) { /* b, bl */
            if (ins & 2u)
                return -1;
            if (ins & 1u)
                continue; /* bl: the leaf is checked further down */
            const u32 target = a + (u32)b_disp(ins);
            if (target <= pc && pc - target <= 4u * IDLE_MAX_BODY)
                start = target, end = a, found = true;
            else
                return -1;
        } else if (op == 19 || op == 17) {
            return -1; /* blr, bctr, rfi, sc... */
        }
    }
    if (!found || end - start >= 4u * IDLE_MAX_BODY)
        return -1;
    /* 2. Validation of the cycle in execution order, from its start. */
    Dep d;
    memset(&d, 0, sizeof(d));
    for (u32 a = start; a <= end; a += 4u) {
        u32 ins;
        if (!fetch(cpu, a, &ins))
            return -1;
        const u32 op = ins >> 26;
        if (op == 16) {
            const u32 bo = (ins >> 21) & 31u, bi = (ins >> 16) & 31u;
            if ((ins & 3u) != 0 || !(bo & 0x04u))
                return -1;
            const u32 target = a + (u32)bc_disp(ins);
            if (a == end) {
                if (target != start)
                    return -1;
            } else if (!(target > end || target < start) || (bo & 0x10u)) {
                return -1; /* inner branch, or unconditional exit */
            }
            if (!(bo & 0x10u))
                use(&d, CRB(bi >> 2));
        } else if (op == 18) {
            if (ins & 2u)
                return -1;
            const u32 target = a + (u32)b_disp(ins);
            if (ins & 1u) {
                def(&d, LRB);
                if (!pure_leaf(cpu, target, &d))
                    return -1;
            } else if (!(a == end && target == start)) {
                return -1;
            }
        } else if (!pure_insn(ins, &d)) {
            return -1;
        }
    }
    /* 3. No value is carried from one iteration to the next through a register, and the exit can depend on memory
     *    (without a load, such a loop exits on the first iteration or never: it is not a wait). */
    if (d.rbw & d.written)
        return -1;
    if (d.nloads == 0)
        return -1;
    struct GcrtIdleState* s = g->idle;
    if (s->nloops >= IDLE_MAX_LOOPS)
        return -1;
    IdleLoop* l = &s->loops[s->nloops];
    memset(l, 0, sizeof(*l));
    l->start = start;
    l->end = end;
    l->nloads = d.nloads;
    memcpy(l->loads, d.loads, sizeof(d.loads));
    *start_out = start;
    *end_out = end;
    return (int)s->nloops++;
}

/* --------------------------------------------------------------------------------------------------------
 * Evaluation of one iteration (at skip time): will the loop really loop back with the current memory and
 * registers? Only the pure subset accepted by the analysis is evaluated; the slightest surprise
 * (address outside RAM, unrecognized instruction) answers "no": the code is then left to run normally. */

typedef struct {
    u32 r[32];
    u32 cr;
    u32 so; /* XER[SO], copied into the comparisons */
} Sim;

static void sim_set_crf(Sim* s, u32 f, u32 bits) {
    const u32 shift = 28u - 4u * f;
    s->cr = (s->cr & ~(0xFu << shift)) | ((bits & 0xFu) << shift);
}

static u32 cmp_bits_signed(s32 a, s32 b, u32 so) { return (a < b ? 8u : a > b ? 4u : 2u) | so; }
static u32 cmp_bits_unsigned(u32 a, u32 b, u32 so) { return (a < b ? 8u : a > b ? 4u : 2u) | so; }

static void sim_rc(Sim* s, u32 v) { sim_set_crf(s, 0, cmp_bits_signed((s32)v, 0, s->so)); }

static bool sim_load(const CPUState* cpu, u32 ea, u32 size, u32* out) {
    u32 off;
    if (ea >= 0x80000000u && ea - 0x80000000u < cpu->ram_size)
        off = ea - 0x80000000u;
    else if (ea >= 0xC0000000u && ea - 0xC0000000u < cpu->ram_size)
        off = ea - 0xC0000000u;
    else
        return false;
    if (off > cpu->ram_size - size)
        return false;
    const u8* p = cpu->ram + off;
    if (size == 4)
        *out = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
    else if (size == 2)
        *out = ((u32)p[0] << 8) | (u32)p[1];
    else
        *out = p[0];
    return true;
}

static u32 rotl32(u32 v, u32 n) { return n ? (v << n) | (v >> (32u - n)) : v; }
static u32 mask32(u32 mb, u32 me) {
    const u32 a = 0xFFFFFFFFu >> mb, b = me == 31u ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1u));
    return mb <= me ? (a & b) : (a | b);
}

/* Executes an instruction of the pure subset; false if unrecognized or if a load is outside RAM. */
static bool sim_insn(const CPUState* cpu, u32 ins, Sim* s) {
    const u32 op = ins >> 26, rd = (ins >> 21) & 31u, ra = (ins >> 16) & 31u, rb = (ins >> 11) & 31u;
    const s16 imm = (s16)(ins & 0xFFFFu);
    const u32 uimm = ins & 0xFFFFu;
    const u32 base = ra ? s->r[ra] : 0u;
    u32 v;
    switch (op) {
    case 32: case 34: case 40: case 42: {
        const u32 size = op == 32 ? 4u : op == 34 ? 1u : 2u;
        if (!sim_load(cpu, base + (u32)(s32)imm, size, &v))
            return false;
        s->r[rd] = op == 42 ? (u32)(s32)(s16)v : v;
        return true;
    }
    case 14: s->r[rd] = base + (u32)(s32)imm; return true;
    case 15: s->r[rd] = base + ((u32)uimm << 16); return true;
    case 7: s->r[rd] = s->r[ra] * (u32)(s32)imm; return true; /* low 32 bits of the product, no signed overflow in C */
    case 24: s->r[ra] = s->r[rd] | uimm; return true;
    case 25: s->r[ra] = s->r[rd] | (uimm << 16); return true;
    case 26: s->r[ra] = s->r[rd] ^ uimm; return true;
    case 27: s->r[ra] = s->r[rd] ^ (uimm << 16); return true;
    case 28: s->r[ra] = s->r[rd] & uimm; sim_rc(s, s->r[ra]); return true;
    case 29: s->r[ra] = s->r[rd] & (uimm << 16); sim_rc(s, s->r[ra]); return true;
    case 21: case 20: {
        const u32 sh = (ins >> 11) & 31u, mb = (ins >> 6) & 31u, me = (ins >> 1) & 31u;
        const u32 m = mask32(mb, me), r = rotl32(s->r[rd], sh);
        s->r[ra] = op == 21 ? (r & m) : ((r & m) | (s->r[ra] & ~m));
        if (ins & 1u)
            sim_rc(s, s->r[ra]);
        return true;
    }
    case 10: sim_set_crf(s, (ins >> 23) & 7u, cmp_bits_unsigned(s->r[ra], uimm, s->so)); return true;
    case 11: sim_set_crf(s, (ins >> 23) & 7u, cmp_bits_signed((s32)s->r[ra], (s32)imm, s->so)); return true;
    case 31: {
        const u32 xo10 = (ins >> 1) & 0x3FFu, xo9 = (ins >> 1) & 0x1FFu;
        const bool rc = (ins & 1u) != 0;
        const u32 a = s->r[ra], b = s->r[rb], rs = s->r[rd];
        switch (xo10) {
        case 0: sim_set_crf(s, (ins >> 23) & 7u, cmp_bits_signed((s32)a, (s32)b, s->so)); return true;
        case 32: sim_set_crf(s, (ins >> 23) & 7u, cmp_bits_unsigned(a, b, s->so)); return true;
        case 28: v = rs & b; break;
        case 60: v = rs & ~b; break;
        case 444: v = rs | b; break;
        case 412: v = rs | ~b; break;
        case 316: v = rs ^ b; break;
        case 124: v = ~(rs | b); break;
        case 476: v = ~(rs & b); break;
        case 284: v = ~(rs ^ b); break;
        case 24: v = (b & 0x20u) ? 0u : rs << (b & 31u); break;
        case 536: v = (b & 0x20u) ? 0u : rs >> (b & 31u); break;
        case 954: v = (u32)(s32)(s8)rs; break;
        case 922: v = (u32)(s32)(s16)rs; break;
        case 26: {
            u32 n = 0;
            while (n < 32u && !(rs & (0x80000000u >> n)))
                n++;
            v = n;
            break;
        }
        case 23: case 87: case 279: {
            const u32 size = xo10 == 23 ? 4u : xo10 == 87 ? 1u : 2u;
            if (!sim_load(cpu, base + b, size, &v))
                return false;
            s->r[rd] = v;
            return true;
        }
        default:
            goto xo_form;
        }
        s->r[ra] = v;
        if (rc)
            sim_rc(s, v);
        return true;
    xo_form:
        if ((ins >> 10) & 1u)
            return false;
        switch (xo9) {
        case 266: v = a + b; break;
        case 40: v = b - a; break;
        case 235: v = a * b; break; /* mullw: the low 32 bits do not depend on the sign */
        case 491:
            if (b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu))
                return false; /* undefined result on the console: do not predict */
            v = (u32)((s32)a / (s32)b);
            break;
        case 459:
            if (b == 0)
                return false;
            v = a / b;
            break;
        case 75: v = (u32)(((s64)(s32)a * (s64)(s32)b) >> 32); break;
        case 11: v = (u32)(((u64)a * (u64)b) >> 32); break;
        case 104: v = 0u - a; break; /* neg (no signed overflow in C) */
        default: return false;
        }
        s->r[rd] = v;
        if (rc)
            sim_rc(s, v);
        return true;
    }
    default:
        return false;
    }
}

static bool sim_cond(const Sim* s, u32 ins) {
    const u32 bo = (ins >> 21) & 31u, bi = (ins >> 16) & 31u;
    if (bo & 0x10u)
        return true;
    const u32 bit = (s->cr >> (31u - bi)) & 1u;
    return bit == ((bo >> 3) & 1u);
}

/* One iteration from the start of the loop: true if it comes back to the start (the loop waits for a change). */
static bool sim_turn_loops(const CPUState* cpu, const IdleLoop* l) {
    Sim s;
    memcpy(s.r, cpu->gpr, sizeof(s.r));
    s.cr = cpu->cr;
    s.so = (cpu->xer >> 31) & 1u;
    for (u32 a = l->start; a <= l->end; a += 4u) {
        u32 ins;
        if (!fetch(cpu, a, &ins))
            return false;
        const u32 op = ins >> 26;
        if (op == 16) {
            const bool taken = sim_cond(&s, ins);
            if (a == l->end)
                return taken; /* closing branch: to the start if taken, otherwise exit */
            if (taken)
                return false; /* exit */
        } else if (op == 18) {
            if (ins & 1u) { /* bl: the leaf */
                const u32 target = a + (u32)b_disp(ins);
                for (u32 i = 0;; i++) {
                    u32 li;
                    if (i >= IDLE_MAX_LEAF || !fetch(cpu, target + 4u * i, &li))
                        return false;
                    if (li == 0x4E800020u)
                        break;
                    if (!sim_insn(cpu, li, &s))
                        return false;
                }
            } else {
                return a == l->end; /* b to the start */
            }
        } else if (!sim_insn(cpu, ins, &s)) {
            return false;
        }
    }
    return false;
}

static bool bit_get(const u8* b, u32 i) { return (b[i >> 3] >> (i & 7u)) & 1u; }
static void bit_set(u8* b, u32 i) { b[i >> 3] |= (u8)(1u << (i & 7u)); }

bool gcrt_cpu_idle_skip_enable(GcrtCpu* g, bool on) {
    if (!on) {
        g->idle_skip = false;
        return true;
    }
    if (!g->idle) {
        struct GcrtIdleState* s = (struct GcrtIdleState*)calloc(1, sizeof(*s));
        if (!s)
            return false;
        s->ram_size = g->state.ram_size;
        const size_t bytes = (size_t)(s->ram_size / 4u + 7u) / 8u;
        s->analyzed = (u8*)calloc(bytes, 1);
        s->idle = (u8*)calloc(bytes, 1);
        if (!s->analyzed || !s->idle) {
            free(s->analyzed);
            free(s->idle);
            free(s);
            return false;
        }
        g->idle = s;
    }
    g->idle_skip = true;
    return true;
}

static bool ea_in_ram(const CPUState* cpu, u32 ea) {
    if (ea >= 0x80000000u && ea - 0x80000000u < cpu->ram_size)
        return true;
    return ea >= 0xC0000000u && ea - 0xC0000000u < cpu->ram_size;
}

/* Is the address handled by host_call (HLE/native replacement, hit counter)? Such a function does not
 * do what its guest code says: its effect cannot be predicted. Without a filter (GCRT_HOST_CALL_FILTER=0),
 * we do not know: cautious answer "yes". */
static bool host_call_marked(const GcrtCpu* g, u32 address) {
    if (!g->host_call)
        return false;
    if (!g->host_call_filter)
        return true;
    const u32 off = address - GC_RAM_BASE;
    if (address < GC_RAM_BASE || off >= g->state.ram_size)
        return true;
    const u32 i = off >> 2;
    return (g->host_call_filter[i >> 3] >> (i & 7u)) & 1u;
}

/* Does the loop, or a leaf function it calls, go through an address handled by host_call? Checked at
 * every attempt (and not once at analysis time): a replacement may be installed later (linked REL, HLE). */
static bool loop_touches_host_call(const GcrtCpu* g, const IdleLoop* l) {
    const CPUState* cpu = &g->state;
    for (u32 a = l->start; a <= l->end; a += 4u) {
        if (host_call_marked(g, a))
            return true;
        u32 ins;
        if (!fetch(cpu, a, &ins))
            return true;
        if ((ins >> 26) == 18 && (ins & 3u) == 1u) { /* bl: the leaf, up to its blr */
            const u32 target = a + (u32)b_disp(ins);
            for (u32 i = 0; i < IDLE_MAX_LEAF; i++) {
                u32 li;
                if (host_call_marked(g, target + 4u * i) || !fetch(cpu, target + 4u * i, &li))
                    return true;
                if (li == 0x4E800020u)
                    break;
            }
        }
    }
    return false;
}

bool gcrt_cpu_idle_try_skip(GcrtCpu* g, u32 pc) {
    struct GcrtIdleState* s = g->idle;
    const CPUState* cpu = &g->state;
    const u32 off = pc - GC_RAM_BASE;
    if (pc < GC_RAM_BASE || off >= s->ram_size)
        return false;
    const u32 idx = off >> 2;
    if (!bit_get(s->analyzed, idx)) {
        u32 start = 0, end = 0;
        const int li = analyze(g, pc, &start, &end);
        if (li < 0) {
            bit_set(s->analyzed, idx);
            return false;
        }
        for (u32 a = start; a <= end; a += 4u) {
            bit_set(s->analyzed, (a - GC_RAM_BASE) >> 2);
            bit_set(s->idle, (a - GC_RAM_BASE) >> 2);
        }
        g->idle_loops = s->nloops;
    }
    if (!bit_get(s->idle, idx))
        return false;
    IdleLoop* l = NULL;
    for (u32 i = 0; i < s->nloops; i++) {
        if (pc >= s->loops[i].start && pc <= s->loops[i].end) {
            l = &s->loops[i];
            break;
        }
    }
    if (!l)
        return false;
    if (loop_touches_host_call(g, l))
        return false;
    /* Will the loop really loop back with the current memory? (A loop head is also reached
     * when the code passes through it without waiting: no skip in that case.) */
    if (!sim_turn_loops(cpu, l))
        return false;
    /* Next deadline: anything that can modify memory or divert the flow. */
    const u64 now = g->ticks;
    u64 dl = g->native_timer_deadline;
    if (cpu->msr & GCRT_MSR_EE) {
        if (g->native_irq_deadline <= now)
            return false; /* an interrupt is already waiting to be delivered */
        if (g->native_irq_deadline < dl)
            dl = g->native_irq_deadline;
    }
    if (dl <= now || dl == UINT64_MAX)
        return false;
    if (dl - now > IDLE_MAX_SKIP_TICKS)
        dl = now + IDLE_MAX_SKIP_TICKS;
    gcrt_cpu_wait_until(g, dl);
    g->idle_skips++;
    g->idle_skipped_ticks += dl - now;
    l->skips++;
    return true;
}

void gcrt_cpu_idle_report(const GcrtCpu* g, FILE* out) {
    const struct GcrtIdleState* s = g->idle;
    if (!s || !out)
        return;
    fprintf(out, "game busy waits: %s, %llu host waits, %.2f emulated s waited, %u loops recognized",
            g->idle_skip ? "host wait active" : "inactive", (unsigned long long)g->idle_skips,
            (double)g->idle_skipped_ticks / (double)GCRT_TB_HZ, s->nloops);
    for (u32 i = 0; i < s->nloops && i < 40; i++)
        fprintf(out, "%s%08X-%08X (%llu)", i ? ", " : ": ", s->loops[i].start, s->loops[i].end,
                (unsigned long long)s->loops[i].skips);
    fprintf(out, "\n");
}
