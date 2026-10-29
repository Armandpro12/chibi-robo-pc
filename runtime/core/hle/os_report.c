/* Game log: replacements of OSReport, vprintf, OSPanic and PPCHalt (recognized by signature).
 *
 * OSReport(const char* fmt, ...): in a retail build, often an empty function (it saves its arguments then
 * returns); other empty debug functions have exactly the same code (DBPrintf, the DSP debug printf,
 * MWTRACE(level, fmt, ...) of the Metrowerks debugger) and are therefore recognized with the same signature. The
 * replacement prints only if r3 points to a plausible string in RAM (MWTRACE, whose r3 is a level, is thus
 * ignored) then returns like the original: nothing else is modified.
 * vprintf(const char* fmt, va_list) (MSL library): writes to the game's standard output; replaced by equivalent
 * formatting sent to the log; returns the number of characters.
 * OSPanic(const char* file, int line, const char* fmt, ...): prints the message and the stack, then halts the
 * game (PPCHalt on the console); here, the runtime stops with GCRT_STOP_GUEST_HALT.
 * PPCHalt(): infinite loop on the console; here, the runtime stops with the same reason. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "hle/gcrt_hle.h"
#include "platform/trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void return_to_caller(GcrtCpu* g) {
    g->state.pc = g->state.lr;
}

static bool hle_report(GcrtCpu* g, u32 address) {
    (void)address;
    if (gcrt_guest_plausible_text(g, g->state.gpr[3])) {
        char text[4096];
        GcrtGuestArgs args;
        gcrt_guest_args_from_registers(&args, g, 1);
        size_t n = gcrt_guest_vformat(&args, g->state.gpr[3], text, sizeof(text));
        gcrt_hle_guest_output(text, n);
    }
    return_to_caller(g);
    return true;
}

static bool hle_vprintf(GcrtCpu* g, u32 address) {
    (void)address;
    char text[4096];
    GcrtGuestArgs args;
    gcrt_guest_args_from_va_list(&args, g, g->state.gpr[4]);
    size_t n = gcrt_guest_vformat(&args, g->state.gpr[3], text, sizeof(text));
    gcrt_hle_guest_output(text, n);
    g->state.gpr[3] = (u32)n;
    return_to_caller(g);
    return true;
}

static void print_guest_stack(GcrtCpu* g, FILE* out) {
    CPUState* cpu = &g->state;
    u32 sp = cpu->gpr[1];
    fprintf(out, "  stack (address, back chain, saved lr):\n");
    for (int i = 0; i < 16 && sp >= GC_RAM_BASE && sp - GC_RAM_BASE + 8u <= cpu->ram_size && sp != 0xFFFFFFFFu; i++) {
        u32 next = mem_read32(cpu, sp);
        u32 lr = (next >= GC_RAM_BASE && next - GC_RAM_BASE + 8u <= cpu->ram_size) ? mem_read32(cpu, next + 4u) : 0;
        fprintf(out, "    %08X  %08X  %08X\n", sp, next, lr);
        sp = next;
    }
}

static bool hle_panic(GcrtCpu* g, u32 address) {
    (void)address;
    CPUState* cpu = &g->state;
    char file[256], text[4096];
    gcrt_guest_string(g, cpu->gpr[3], file, sizeof(file));
    GcrtGuestArgs args;
    gcrt_guest_args_from_registers(&args, g, 3);
    gcrt_guest_vformat(&args, cpu->gpr[5], text, sizeof(text));
    fflush(stdout);
    fprintf(stderr, "\ngcrt: game OSPanic (%s, line %d, called from %08X): %s\n", file, (int)cpu->gpr[4],
            cpu->lr, text);
    print_guest_stack(g, stderr);
    char line[4400];
    int n = snprintf(line, sizeof(line), "OSPanic: %s:%d: %s\n", file, (int)cpu->gpr[4], text);
    if (n > 0)
        gcrt_hle_guest_output(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    g->stop = GCRT_STOP_GUEST_HALT;
    return_to_caller(g);
    return true;
}

static bool hle_halt(GcrtCpu* g, u32 address) {
    fflush(stdout);
    fprintf(stderr, "\ngcrt: PPCHalt (%08X) called from %08X: the game halts\n", address, g->state.lr);
    print_guest_stack(g, stderr);
    g->stop = GCRT_STOP_GUEST_HALT;
    return_to_caller(g);
    return true;
}

/* Diagnostic and tooling (GCRT_FORCE_ARG="ITEM[;ITEM...]", at most FORCE_ARG_MAX items): on entry into a function,
 * before its original code runs, a register or a 32-bit word of guest memory receives a value. Item syntax:
 *   ADDRESS:TARGET=VALUE[#COUNT]   TARGET receives VALUE
 *   ADDRESS:TARGET&=VALUE[#COUNT]  TARGET receives TARGET & VALUE (e.g. clear one flag: &=0xFFFFDFFF)
 *   ADDRESS:TARGET|=VALUE[#COUNT]  TARGET receives TARGET | VALUE (set flags)
 * with TARGET = rN (register), [rN+OFFSET] (the big-endian word at rN+OFFSET, rN read on entry; "-" also
 * accepted) or [ABSOLUTE] (the big-endian word at a fixed guest address).
 * ADDRESS, OFFSET and ABSOLUTE are hexadecimal ("0x" optional); VALUE is decimal, or hexadecimal with "0x";
 * #COUNT applies the item to the first COUNT entries only (default: every entry). ADDRESS@RETURN instead of
 * ADDRESS: only the entries whose return address (link register) is RETURN (hexadecimal), i.e. the calls made from
 * the call instruction at RETURN - 4; the other entries are neither changed nor counted.
 * Uses: make two runs comparable when the game depends on a clock value (random seed taken from OSGetTick: a
 * build that executes fewer instructions at startup gets another seed), or change one value once (e.g. a field
 * of the game's data just before the game reads it), the game's own code doing the rest. The addresses are
 * specific to each game and version: they come from the caller (tools/run.py and games/<game>/game.toml), never
 * from the core. Without a compiled hook (game.toml [hle.hooks]), only the calls that go through the chassis are
 * seen. Traces: one stderr line per application of a counted item; every application with GCRT_TRACE=native. */
#define FORCE_ARG_MAX 8
enum { FORCE_REG, FORCE_MEM_REG, FORCE_MEM_ABS };
enum { FORCE_SET, FORCE_AND, FORCE_OR };
static struct {
    u32 address; /* function entry */
    u32 caller;  /* return address required on entry, 0 = any */
    u32 kind;
    u32 reg;
    u32 offset; /* FORCE_MEM_REG: added to rN (modulo 2^32); FORCE_MEM_ABS: the guest address */
    u32 op; /* FORCE_SET, FORCE_AND, FORCE_OR */
    u32 value;
    u32 limit; /* 0 = every entry */
    u32 hits;
} s_force[FORCE_ARG_MAX];
static unsigned s_force_count;

static void describe_force_target(unsigned i, char* out, size_t size) {
    switch (s_force[i].kind) {
    case FORCE_REG:
        snprintf(out, size, "r%u", (unsigned)s_force[i].reg);
        break;
    case FORCE_MEM_REG:
        snprintf(out, size, "[r%u+0x%X]", (unsigned)s_force[i].reg, (unsigned)s_force[i].offset);
        break;
    default:
        snprintf(out, size, "[0x%08X]", (unsigned)s_force[i].offset);
        break;
    }
}

static bool hle_force_arg(GcrtCpu* g, u32 address) {
    for (unsigned i = 0; i < s_force_count; i++) {
        if (s_force[i].address != address)
            continue;
        if (s_force[i].caller && g->state.lr != s_force[i].caller)
            continue;
        if (s_force[i].limit && s_force[i].hits >= s_force[i].limit)
            continue;
        s_force[i].hits++;
        u32 target = 0, before, after;
        if (s_force[i].kind == FORCE_REG) {
            before = g->state.gpr[s_force[i].reg];
        } else {
            target = s_force[i].kind == FORCE_MEM_REG ? g->state.gpr[s_force[i].reg] + s_force[i].offset
                                                      : s_force[i].offset;
            before = mem_read32(&g->state, target);
        }
        after = s_force[i].op == FORCE_AND  ? before & s_force[i].value
                : s_force[i].op == FORCE_OR ? before | s_force[i].value
                                            : s_force[i].value;
        if (s_force[i].kind == FORCE_REG)
            g->state.gpr[s_force[i].reg] = after;
        else
            mem_write32(&g->state, target, after);
        if (s_force[i].limit || GCRT_TRACE_ON(GCRT_TRACE_NATIVE)) {
            char what[48];
            describe_force_target(i, what, sizeof(what));
            char where[24] = "";
            if (s_force[i].kind != FORCE_REG)
                snprintf(where, sizeof(where), " (0x%08X)", (unsigned)target);
            char count[24];
            if (s_force[i].limit)
                snprintf(count, sizeof(count), "%u/%u", (unsigned)s_force[i].hits, (unsigned)s_force[i].limit);
            else
                snprintf(count, sizeof(count), "%u", (unsigned)s_force[i].hits);
            char line[192];
            snprintf(line, sizeof(line),
                     "GCRT_FORCE_ARG: entry %s into %08X (called from %08X): %s%s 0x%08X -> 0x%08X\n", count,
                     (unsigned)address, (unsigned)g->state.lr, what, where, (unsigned)before, (unsigned)after);
            if (s_force[i].limit)
                fprintf(stderr, "gcrt hle: %s", line);
            GCRT_TRACE(GCRT_TRACE_NATIVE, "%s", line);
        }
    }
    return false; /* the original code runs afterwards */
}

/* Parses one item (see above); false if it is malformed. */
static bool parse_force_item(const char* item, unsigned slot) {
    char* p = NULL;
    const unsigned long address = strtoul(item, &p, 16);
    if (!p || p == item)
        return false;
    unsigned long caller = 0;
    if (*p == '@') {
        char* begin = p + 1;
        caller = strtoul(begin, &p, 16);
        if (p == begin || caller == 0)
            return false;
    }
    if (*p != ':')
        return false;
    p++;
    u32 kind, reg = 0, offset = 0;
    if (*p == 'r' || *p == 'R') {
        kind = FORCE_REG;
        reg = (u32)strtoul(p + 1, &p, 10);
    } else if (*p == '[') {
        p++;
        if (*p == 'r' || *p == 'R') {
            kind = FORCE_MEM_REG;
            reg = (u32)strtoul(p + 1, &p, 10);
            if (*p == '+' || *p == '-') {
                const char sign = *p;
                const u32 magnitude = (u32)strtoul(p + 1, &p, 16);
                offset = sign == '-' ? 0u - magnitude : magnitude;
            }
        } else {
            kind = FORCE_MEM_ABS;
            char* start = p;
            offset = (u32)strtoul(p, &p, 16);
            if (p == start)
                return false;
        }
        if (*p != ']')
            return false;
        p++;
    } else {
        return false;
    }
    u32 op = FORCE_SET;
    if (*p == '&' || *p == '|') {
        op = *p == '&' ? FORCE_AND : FORCE_OR;
        p++;
    }
    if (reg > 31 || *p != '=')
        return false;
    char* start = p + 1;
    const unsigned long value = strtoul(start, &p, 0);
    if (p == start)
        return false;
    unsigned long limit = 0;
    if (*p == '#') {
        start = p + 1;
        limit = strtoul(start, &p, 10);
        if (p == start || limit == 0)
            return false;
    }
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p)
        return false;
    s_force[slot].address = (u32)address;
    s_force[slot].caller = (u32)caller;
    s_force[slot].kind = kind;
    s_force[slot].reg = reg;
    s_force[slot].offset = offset;
    s_force[slot].op = op;
    s_force[slot].value = (u32)value;
    s_force[slot].limit = (u32)limit;
    s_force[slot].hits = 0;
    return true;
}

static void install_force_args(GcrtCpu* g) {
    const char* spec = getenv("GCRT_FORCE_ARG");
    if (!spec || !spec[0])
        return;
    char buf[512];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (char* item = strtok(buf, ";,"); item; item = strtok(NULL, ";,")) {
        while (*item == ' ' || *item == '\t')
            item++;
        if (!*item)
            continue;
        if (s_force_count >= FORCE_ARG_MAX) {
            fprintf(stderr, "gcrt hle: GCRT_FORCE_ARG: more than %d items, \"%s\" and the next ones ignored\n",
                    FORCE_ARG_MAX, item);
            break;
        }
        if (!parse_force_item(item, s_force_count)) {
            fprintf(stderr, "gcrt hle: GCRT_FORCE_ARG: invalid item \"%s\" ignored (expected ADDRESS:rN=VALUE, "
                            "ADDRESS:[rN+OFFSET]=VALUE or ADDRESS:[ABSOLUTE]=VALUE, &= or |= instead of =, optionally ADDRESS@RETURN "
                            "and #COUNT)\n",
                    item);
            continue;
        }
        const unsigned i = s_force_count;
        /* gcrt_hle_register refuses an address that another function already holds (error line above): the item
         * would never act, so it is neither kept nor announced as active. Several items at one address share the same
         * hook (same function: accepted). */
        if (!gcrt_hle_register(g, s_force[i].address, "GCRT_FORCE_ARG", hle_force_arg)) {
            fprintf(stderr, "gcrt hle: GCRT_FORCE_ARG: ERROR: item \"%s\" NOT active: no hook at the entry %08X "
                            "(refused by gcrt_hle_register, error above)\n",
                    item, (unsigned)s_force[i].address);
            continue;
        }
        s_force_count++;
        char what[48], when[80] = "";
        describe_force_target(i, what, sizeof(what));
        if (s_force[i].limit && s_force[i].caller)
            snprintf(when, sizeof(when), " (first %u entries from return address %08X only)",
                     (unsigned)s_force[i].limit, (unsigned)s_force[i].caller);
        else if (s_force[i].limit)
            snprintf(when, sizeof(when), " (first %u entries only)", (unsigned)s_force[i].limit);
        else if (s_force[i].caller)
            snprintf(when, sizeof(when), " (entries from return address %08X only)", (unsigned)s_force[i].caller);
        static const char* const op_text[] = {"=", "&=", "|="};
        fprintf(stderr, "gcrt hle: GCRT_FORCE_ARG: %s %s 0x%08X on entry to %08X%s\n", what,
                op_text[s_force[i].op], (unsigned)s_force[i].value, (unsigned)s_force[i].address, when);
    }
}

void gcrt_hle_install_defaults(GcrtCpu* g) {
    install_force_args(g);
    gcrt_hle_replace(g, "OSReport", hle_report);
    gcrt_hle_replace(g, "vprintf", hle_vprintf);
    gcrt_hle_replace(g, "OSPanic", hle_panic);
    gcrt_hle_replace(g, "PPCHalt", hle_halt);
    gcrt_hle_replace(g, "OSLink", gcrt_hle_oslink);
    gcrt_hle_replace(g, "OSUnlink", gcrt_hle_osunlink);
}
