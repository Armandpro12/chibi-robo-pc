/* Hardware register access counter (milestone 10, "no emulated hardware" criterion).
 *
 * The counting itself is in cpu_core.c (default bus, CPU boundary): every access that is neither in RAM
 * nor in the locked cache goes through it (no emulated hardware at all since step 10d). This file classifies the
 * addresses by subsystem and exports the result (readable report, JSON "gcrt-hwcount/1").
 * Block map: gcrt_cpu.h, "Hardware register access counter". Generic GameCube. */
#include "cpu/gcrt_cpu.h"

#include <string.h>

static const char* const k_names[GCRT_HW_KINDS] = {
    "CP", "PE", "VI", "PI", "MI", "DSP", "AR", "DI", "SI", "EXI", "AI", "WGPIPE", "EFB", "OTHER", "WGPIPE_OUTSIDE",
};

const char* gcrt_hw_kind_name(int kind) {
    return (kind >= 0 && kind < GCRT_HW_KINDS) ? k_names[kind] : "?";
}

int gcrt_hw_kind_of(u32 address) {
    u32 block = address & 0xFFFF0000u;
    if (block == 0xCC000000u || block == 0x0C000000u) {
        u32 off = address & 0xFFFFu;
        switch (off >> 12) {
        case 0x0: return GCRT_HW_CP;
        case 0x1: return GCRT_HW_PE;
        case 0x2: return GCRT_HW_VI;
        case 0x3: return GCRT_HW_PI;
        case 0x4: return GCRT_HW_MI;
        case 0x5: return (off >= 0x5010u && off < 0x5030u) ? GCRT_HW_AR : GCRT_HW_DSP;
        case 0x6:
            if (off < 0x6400u)
                return GCRT_HW_DI;
            if (off < 0x6800u)
                return GCRT_HW_SI;
            if (off < 0x6C00u)
                return GCRT_HW_EXI;
            return GCRT_HW_AI;
        case 0x8: return GCRT_HW_WGPIPE;
        default: return GCRT_HW_OTHER;
        }
    }
    if ((address & 0xFF000000u) == 0xC8000000u || (address & 0xFF000000u) == 0x08000000u)
        return GCRT_HW_EFB;
    return -1;
}

void gcrt_hw_count_add(GcrtCpu* g, int kind, u32 address, bool write) {
    if (kind < 0 || kind >= GCRT_HW_KINDS)
        return;
    GcrtHwCount* c = &g->hw_count[kind];
    if (c->reads + c->writes == 0) {
        c->first_pc = g->state.pc;
        c->first_lr = g->state.lr;
        c->first_address = address;
        c->first_write = write;
    }
    if (write)
        c->writes++;
    else
        c->reads++;
}

/* Total of the accesses: WGPIPE_OUTSIDE is a subset of WGPIPE (writes to the pipe outside an open draw), so it is
 * not added a second time. */
static bool counted_in_total(int kind) { return kind != GCRT_HW_WGPIPE_OUTSIDE; }

u64 gcrt_hw_count_total(const GcrtCpu* g) {
    u64 total = 0;
    for (int k = 0; k < GCRT_HW_KINDS; k++)
        if (counted_in_total(k))
            total += g->hw_count[k].reads + g->hw_count[k].writes;
    return total;
}

void gcrt_hw_count_mark_boot(GcrtCpu* g) {
    memcpy(g->hw_count_boot, g->hw_count, sizeof(g->hw_count));
    memset(g->hw_count, 0, sizeof(g->hw_count));
    g->hw_count_boot_marked = true;
}

void gcrt_hw_count_report(const GcrtCpu* g, FILE* out) {
    const u64 total = gcrt_hw_count_total(g);
    fprintf(out, "hardware accesses (0xCC00xxxx, EFB, other): %llu total\n", (unsigned long long)total);
    for (int k = 0; k < GCRT_HW_KINDS; k++) {
        const GcrtHwCount* c = &g->hw_count[k];
        if (c->reads + c->writes == 0)
            continue;
        fprintf(out, "  %-6s reads %12llu  writes %12llu  first: %s %08X at pc=%08X (lr=%08X)\n", k_names[k],
                (unsigned long long)c->reads, (unsigned long long)c->writes, c->first_write ? "write" : "read",
                c->first_address, c->first_pc, c->first_lr);
    }
    if (g->hw_count_boot_marked) {
        bool any = false;
        for (int k = 0; k < GCRT_HW_KINDS; k++) {
            const GcrtHwCount* c = &g->hw_count_boot[k];
            if (c->reads + c->writes == 0)
                continue;
            fprintf(out, "%s %s=%llu/%llu (pc=%08X)", any ? "" : "  during boot (not counted above):",
                    k_names[k], (unsigned long long)c->reads, (unsigned long long)c->writes, c->first_pc);
            any = true;
        }
        if (any)
            fputc('\n', out);
    }
}

void gcrt_hw_count_json(const GcrtCpu* g, FILE* out) {
    u64 reads = 0, writes = 0;
    for (int k = 0; k < GCRT_HW_KINDS; k++) {
        if (!counted_in_total(k))
            continue; /* subset of WGPIPE */
        reads += g->hw_count[k].reads;
        writes += g->hw_count[k].writes;
    }
    fprintf(out, "{\"format\":\"gcrt-hwcount/1\",\"total\":{\"reads\":%llu,\"writes\":%llu},\"subsystems\":{",
            (unsigned long long)reads, (unsigned long long)writes);
    for (int k = 0; k < GCRT_HW_KINDS; k++) {
        const GcrtHwCount* c = &g->hw_count[k];
        fprintf(out, "%s\"%s\":{\"reads\":%llu,\"writes\":%llu", k ? "," : "", k_names[k],
                (unsigned long long)c->reads, (unsigned long long)c->writes);
        if (c->reads + c->writes)
            fprintf(out, ",\"first\":{\"access\":\"%s\",\"address\":\"0x%08X\",\"pc\":\"0x%08X\",\"lr\":\"0x%08X\"}",
                    c->first_write ? "write" : "read", c->first_address, c->first_pc, c->first_lr);
        else
            fprintf(out, ",\"first\":null");
        fputc('}', out);
    }
    fputs("},\"boot\":{", out);
    for (int k = 0; k < GCRT_HW_KINDS; k++) {
        const GcrtHwCount* c = &g->hw_count_boot[k];
        fprintf(out, "%s\"%s\":{\"reads\":%llu,\"writes\":%llu}", k ? "," : "", k_names[k],
                (unsigned long long)c->reads, (unsigned long long)c->writes);
    }
    fputs("}}", out);
}

const char* gcrt_hw_count_summary(const GcrtCpu* g, char* buf, size_t size) {
    size_t used = 0;
    if (size)
        buf[0] = '\0';
    for (int k = 0; k < GCRT_HW_KINDS && used + 1 < size; k++) {
        const GcrtHwCount* c = &g->hw_count[k];
        if (c->reads + c->writes == 0)
            continue;
        int n = snprintf(buf + used, size - used, "%s%s=%llu/%llu", used ? " " : "", k_names[k],
                         (unsigned long long)c->reads, (unsigned long long)c->writes);
        if (n < 0)
            break;
        used += (size_t)n < size - used ? (size_t)n : size - used - 1;
    }
    if (size && used == 0)
        snprintf(buf, size, "none");
    return buf;
}
