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

/* Diagnostic (GCRT_FORCE_ARG="ADDRESS:rN=VALUE[;...]", hexadecimal for the address): on each entry into the
 * function at ADDRESS, register rN receives VALUE, then the original function runs. Used to make two runs
 * comparable when the game depends on a clock value (e.g. random generator seed taken from OSGetTick: a build
 * that executes fewer instructions at startup gets another seed). Without a compiled hook (game.toml
 * [hle.hooks]), only the calls that go through the chassis are seen. */
#define FORCE_ARG_MAX 8
static struct {
    u32 address;
    u32 reg;
    u32 value;
} s_force[FORCE_ARG_MAX];
static unsigned s_force_count;

static bool hle_force_arg(GcrtCpu* g, u32 address) {
    for (unsigned i = 0; i < s_force_count; i++)
        if (s_force[i].address == address)
            g->state.gpr[s_force[i].reg] = s_force[i].value;
    return false; /* the original code runs afterwards */
}

static void install_force_args(GcrtCpu* g) {
    const char* spec = getenv("GCRT_FORCE_ARG");
    if (!spec || !spec[0])
        return;
    char buf[256];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (char* item = strtok(buf, ";,"); item && s_force_count < FORCE_ARG_MAX; item = strtok(NULL, ";,")) {
        unsigned long address = 0, reg = 0, value = 0;
        char* p = NULL;
        address = strtoul(item, &p, 16);
        if (!p || *p != ':' || (p[1] != 'r' && p[1] != 'R'))
            continue;
        reg = strtoul(p + 2, &p, 10);
        if (!p || *p != '=' || reg > 31)
            continue;
        value = strtoul(p + 1, NULL, 0);
        s_force[s_force_count].address = (u32)address;
        s_force[s_force_count].reg = (u32)reg;
        s_force[s_force_count].value = (u32)value;
        s_force_count++;
        gcrt_hle_register(g, (u32)address, "GCRT_FORCE_ARG", hle_force_arg);
        fprintf(stderr, "gcrt hle: GCRT_FORCE_ARG: r%lu = 0x%08lX on entry to %08lX\n", reg, value, address);
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
