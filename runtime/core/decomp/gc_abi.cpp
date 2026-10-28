// Gateway between recompiled guest code and decompiled C++ functions: registry, installation, nested guest calls,
// call report and cross-check diagnostic (gc_abi.h). Generic: no game here.
#include "decomp/gc_abi.h"
#include "decomp/gc_abi_c.h"

// C headers: DolRecomp's cpu.h (included by gcrt_cpu.h before its own extern "C") declares the write journal, a C
// symbol, without extern "C" of its own.
extern "C" {
#include "cpu/gcrt_cpu.h"
}
#include "hle/gcrt_hle.h"
#include "hle/gcrt_native.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace gc::abi {

namespace {

struct entry_state {
    const registration* reg = nullptr;
    unsigned checks = 0;     // cross-checks done
    unsigned mismatches = 0; // cross-checks whose result differed
};

GcrtCpu* s_cpu = nullptr;
std::vector<entry_state> s_entries;  // one per registration
std::vector<u32> s_slots;            // open addressing: index + 1 into s_entries, 0 = empty
unsigned s_crosscheck_limit = 0;     // GCRT_DECOMP_CROSSCHECK: cross-checks per function (0: off)
bool s_report_registered = false;

u32 slot_hash(u32 address, std::size_t mask) {
    return static_cast<u32>(((address >> 2) * 2654435761u) & mask);
}

entry_state* lookup(u32 address) noexcept {
    if (s_slots.empty())
        return nullptr;
    const std::size_t mask = s_slots.size() - 1;
    for (std::size_t i = slot_hash(address, mask), n = 0; n < s_slots.size(); i = (i + 1) & mask, n++) {
        const u32 v = s_slots[i];
        if (!v)
            return nullptr;
        if (s_entries[v - 1].reg->address == address)
            return &s_entries[v - 1];
    }
    return nullptr;
}

void report_at_exit() {
    report(stdout);
    std::fflush(stdout);
}

// Store hook of gc_mem.h: a store of decompiled code into guest RAM has the side effects of a store of recompiled
// code (DolRecomp cpu.c mem_write8/16/32/64, in that order, before the bytes change): it clears the lwarx
// reservation when it falls in the reserved 32-byte block (same test as cpu.c clear_matching_reservation, both
// mirrors seen as one), then calls the write journal with the RAM offset and the size. Without it, the idle detection
// of a looping OSYieldThread (hle/native_os.c, spin_journal) would not see the stores of decompiled functions, and a
// stwcx. after such a store would succeed where the console's fails.
void guest_store(std::uint32_t address, std::uint32_t size) noexcept {
    CPUState* c = &s_cpu->state;
    if (c->reserve_valid && (((c->reserve_addr & ~0x40000000u) ^ (address & ~0x40000000u)) & ~31u) == 0)
        c->reserve_valid = false;
    if (g_mem_write_journal)
        g_mem_write_journal(address - 0x80000000u, size, g_mem_write_journal_user);
}

void memory_fault(const char* reason, std::uint32_t address, std::uint32_t size) {
    std::fprintf(stderr, "gc_abi: guest memory fault in decompiled code: %s (address %08X, %u bytes)\n", reason,
                 static_cast<unsigned>(address), static_cast<unsigned>(size));
    std::fflush(stderr);
    std::abort();
}

} // namespace

registers registers_of(GcrtCpu* g) noexcept {
    CPUState* c = &g->state;
    return registers{c->gpr, c->fpr, c->ps1, &c->lr, &c->pc};
}

void install(GcrtCpu* g, const registration* table, unsigned count) {
    s_cpu = g;
    // Guest RAM for gc_mem.h: the host buffer of the CPU, with its mirrors (gc_mem.h). The size includes the area of
    // the RELs placed after the 24 MiB (tools/build.py, host_ram_size).
    gc::init_guest_memory(g->state.ram, g->state.ram_size);
    gc::set_memory_fault_handler(&memory_fault);
    gc::set_guest_store_hook(&guest_store);
    s_entries.assign(count, entry_state{});
    std::size_t size = 16;
    while (size < 2u * count)
        size <<= 1;
    s_slots.assign(size, 0);
    for (unsigned i = 0; i < count; i++) {
        const registration& r = table[i];
        s_entries[i].reg = &r;
        if (lookup(r.address)) {
            std::fprintf(stderr, "gc_abi: two decompiled functions at %08X (%s): the second is ignored\n",
                         static_cast<unsigned>(r.address), r.record->name);
            continue;
        }
        const std::size_t mask = s_slots.size() - 1;
        std::size_t k = slot_hash(r.address, mask);
        while (s_slots[k])
            k = (k + 1) & mask;
        s_slots[k] = i + 1;
        gcrt_hle_register(g, r.address, r.record->name, r.record->entry);
    }
    const char* cc = std::getenv("GCRT_DECOMP_CROSSCHECK");
    s_crosscheck_limit = cc && cc[0] ? static_cast<unsigned>(std::strtoul(cc, nullptr, 0)) : 0u;
    detail::crosscheck_enabled = s_crosscheck_limit != 0;
    std::fprintf(stderr, "gc_abi: %u decompiled functions registered%s\n", count,
                 detail::crosscheck_enabled ? " (cross-check with the recompiled originals on)" : "");
    if (!s_report_registered) {
        s_report_registered = true;
        std::atexit(&report_at_exit);
    }
}

const export_record* find(u32 address) noexcept {
    const entry_state* e = lookup(address);
    return e ? e->reg->record : nullptr;
}

void report(std::FILE* out) {
    u64 total = 0;
    for (const entry_state& e : s_entries) {
        const u64 calls = *e.reg->record->calls;
        total += calls;
        // Path that reached the replacement: the hook compiled at the entry (direct `bl` calls) or the chassis.
        u64 dispatched = 0, via_hook = 0;
        gcrt_hle_calls_at(e.reg->address, &dispatched, &via_hook);
        std::fprintf(out, "DECOMP %-32s %08X calls=%llu hook=%llu chassis=%llu", e.reg->record->name,
                     static_cast<unsigned>(e.reg->address), static_cast<unsigned long long>(calls),
                     static_cast<unsigned long long>(via_hook),
                     static_cast<unsigned long long>(dispatched - via_hook));
        if (s_crosscheck_limit)
            std::fprintf(out, " crosscheck=%u mismatches=%u", e.checks, e.mismatches);
        std::fputc('\n', out);
    }
    u64 dispatch_calls = 0, dispatch_misses = 0;
    gcrt_hle_dispatch_counts(&dispatch_calls, &dispatch_misses);
    std::fprintf(out, "DECOMP total functions=%u calls=%llu hle_dispatch=%llu hle_dispatch_misses=%llu\n",
                 static_cast<unsigned>(s_entries.size()), static_cast<unsigned long long>(total),
                 static_cast<unsigned long long>(dispatch_calls), static_cast<unsigned long long>(dispatch_misses));
}

namespace detail {

u32 bypass_address = 0;
bool crosscheck_enabled = false;

GcrtCpu* current_cpu() noexcept {
    return s_cpu;
}

#if defined(GC_DECOMP_ORACLE)
u64 guest_calls = 0;
#endif

bool run_guest(GcrtCpu* g, u32 address) noexcept {
#if defined(GC_DECOMP_ORACLE)
    guest_calls++; // the oracle tells its own runs of the original from guest calls made by decompiled code
#endif
    const bool returned = gcrt_guest_call_prepared(g, address);
    if (!returned)
        std::fprintf(stderr, "gc_abi: the guest function %08X called by decompiled code did not return\n",
                     static_cast<unsigned>(address));
    return returned;
}

void first_call(u32 address) {
    const export_record* e = find(address);
    std::fprintf(stderr, "gc_abi: first call of %s (%08X)\n", e ? e->name : "?", static_cast<unsigned>(address));
}

void crosscheck(GcrtCpu* g, u32 address, const u32* saved_gpr, const f64* saved_fpr, const f64* saved_ps1) {
    entry_state* e = lookup(address);
    if (!e || !(e->reg->record->flags & flag_reads_only) || e->checks >= s_crosscheck_limit)
        return;
    const u8 kind = e->reg->record->result_kind;
    if (kind == result_memory)
        return; // structure results: the oracle compares memory (mission 4)
    CPUState* c = &g->state;
    const u32 mine_r3 = c->gpr[3], mine_r4 = c->gpr[4];
    const f64 mine_f1 = c->fpr[1], mine_ps1 = c->ps1[1];
    for (unsigned i = 3; i <= 10; i++)
        c->gpr[i] = saved_gpr[i];
    for (unsigned i = 1; i <= 8; i++) {
        c->fpr[i] = saved_fpr[i];
        c->ps1[i] = saved_ps1[i];
    }
    bypass_address = address;
    const bool returned = run_guest(g, address);
    bypass_address = 0;
    if (!returned)
        return;
    bool same = true;
    switch (kind) {
    case result_word:
        same = c->gpr[3] == mine_r3;
        break;
    case result_pair:
        same = c->gpr[3] == mine_r3 && c->gpr[4] == mine_r4;
        break;
    case result_f32:
        same = std::memcmp(&c->fpr[1], &mine_f1, sizeof(f64)) == 0 && std::memcmp(&c->ps1[1], &mine_ps1, sizeof(f64)) == 0;
        break;
    case result_f64:
        same = std::memcmp(&c->fpr[1], &mine_f1, sizeof(f64)) == 0;
        break;
    default:
        break;
    }
    e->checks++;
    if (!same) {
        if (e->mismatches++ < 8)
            std::fprintf(stderr,
                         "gc_abi: cross-check MISMATCH %s (%08X): recompiled r3=%08X r4=%08X, decompiled r3=%08X "
                         "r4=%08X (arguments r3=%08X r4=%08X r5=%08X)\n",
                         e->reg->record->name, static_cast<unsigned>(address), static_cast<unsigned>(c->gpr[3]),
                         static_cast<unsigned>(c->gpr[4]), static_cast<unsigned>(mine_r3),
                         static_cast<unsigned>(mine_r4), static_cast<unsigned>(saved_gpr[3]),
                         static_cast<unsigned>(saved_gpr[4]), static_cast<unsigned>(saved_gpr[5]));
    }
    // The game goes on with the result of the recompiled original (left in the registers by the nested run).
}

} // namespace detail

} // namespace gc::abi

extern "C" void gcrt_decomp_install_table(GcrtCpu* g, const void* table, unsigned count) {
    gc::abi::install(g, static_cast<const gc::abi::registration*>(table), count);
}
