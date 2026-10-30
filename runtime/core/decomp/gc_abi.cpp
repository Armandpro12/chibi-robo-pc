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
    u64 fpu_declined = 0;    // entries declined because MSR[FP] = 0 (registration_uses_fpu)
    // Guest calls made while this function was on the C++ stack, its prefix unknown (no usable graph, or no call site
    // of its original matched the call): its cycles before the call were left to the end of its call (issue #68).
    u64 prefix_unknown = 0;
};

GcrtCpu* s_cpu = nullptr;
std::vector<entry_state> s_entries;  // one per registration
std::vector<u32> s_slots;            // open addressing: index + 1 into s_entries, 0 = empty
unsigned s_crosscheck_limit = 0;     // GCRT_DECOMP_CROSSCHECK: cross-checks per function (0: off)
bool s_report_registered = false;
u64 s_cost_charged_cycles = 0;  // guest cycles charged by charge_guest_cost (cycle clock)
u64 s_cost_uncharged_calls = 0; // calls whose cost was not charged: real-time clock (GCRT_PACING=host)
bool s_path_error_reported = false; // first path error printed (gc_path.h)
// Calls into guest code (issue #68, gc_abi.h): cycles charged before nested guest runs, as the prefixes of the
// decompiled functions on the C++ stack (also counted in s_cost_charged_cycles: same total) and as the pending cycles of
// the generated block that reached the gateway (cycles of recompiled code, not a guest cost); guest calls made by
// decompiled code, and frames whose prefix was unknown at one of them.
u64 s_prefix_charged_cycles = 0;
u64 s_pending_charged_cycles = 0;
u64 s_guest_calls_from_decomp = 0;
u64 s_prefix_unknown = 0;
bool s_prefix_unknown_reported = false;
// GCRT_DECOMP_TEST_LATE_CHARGE=1 (test only, read by install): the behaviour before issue #68 was fixed, every cycle
// charged after the body (no pending cycles nor prefix charged before guest calls, nor in the oracle's call hooks),
// so that a test can show what the fix changes.
bool s_late_charge = false;

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
    detail::guest_msr = &g->state.msr;
    // Guest RAM for gc_mem.h: the host buffer of the CPU, with its mirrors (gc_mem.h). The size includes the area of
    // the RELs placed after the 24 MiB (tools/build.py, host_ram_size).
    gc::init_guest_memory(g->state.ram, g->state.ram_size);
    gc::set_memory_fault_handler(&memory_fault);
    gc::set_guest_store_hook(&guest_store);
    // Issue #52: a nested run that holds a due, unmasked interrupt for more than one guest second (a callee waiting
    // for an event with MSR[EE] = 0) stops with GCRT_STOP_HELD_INTERRUPT and a message, instead of running until its
    // block budget (hle/native_irq.c). Production and oracle builds alike.
    gcrt_guest_call_set_held_irq_limit(GCRT_TB_HZ);
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
        // Refused when a native replacement already holds the address (tools/build.py refuses that statically; this
        // is the run-time guard): the decompiled function would not run, so the executable must not start.
        if (!gcrt_hle_register(g, r.address, r.record->name, r.record->entry)) {
            std::fprintf(stderr, "gc_abi: decompiled function %s (%08X) could not be registered: stopping\n",
                         r.record->name, static_cast<unsigned>(r.address));
            std::fflush(stderr);
            std::abort();
        }
    }
    // Guest cost of each function (registration::guest_cost_256ths, gc_abi.h "Guest time"), kept in the function's own
    // state (export_record::cost) so that a call charges it without a lookup. GCRT_DECOMP_GUEST_COST=0, read in every
    // build: nothing is charged (the cycle counter runs as with a gateway without guest costs; diagnostic of a timing
    // difference). It changes nothing else, and nothing at all with the real-time clock (GCRT_PACING=host); the default
    // pacing of normal play runs on the cycle clock, so there it does change the guest time (gc_abi.h "Guest time").
    const char* cost_env = std::getenv("GCRT_DECOMP_GUEST_COST");
    const bool costs_off = cost_env && cost_env[0] == '0' && cost_env[1] == 0;
    unsigned costed = 0, with_path = 0, mean_only = 0;
    for (unsigned i = 0; i < count; i++)
        if (table[i].record->cost)
            *table[i].record->cost = guest_cost_state{};
    for (unsigned i = 0; i < count; i++) {
        guest_cost_state* cost = table[i].record->cost;
        if (!cost)
            continue;
        const u32 wanted = costs_off ? 0u : table[i].guest_cost_256ths;
        if (cost->per_call_256ths && cost->per_call_256ths != wanted)
            std::fprintf(stderr, "gc_abi: %s registered twice with different guest costs: the last one is used\n",
                         table[i].record->name);
        cost->per_call_256ths = wanted;
        cost->remainder_256ths = 0;
        cost->registered = true;
        // Cost by path (gc_path.h): the graph of the original, unless the costs are off or the decompiled code does
        // not mark the original's branches (then the mean, a debt the message below counts).
        cost->address = table[i].address;
        // A graph without conditional branch (a straight-line original with call sites: its call table gives the
        // prefixes of its calls, issue #68) needs no marks: its walk is complete from the entry.
        const bool marked = (table[i].record->flags & flag_path_marked) ||
                            (table[i].path && table[i].path->node_count == 0);
        cost->path = costs_off || !marked ? nullptr : table[i].path;
        if (table[i].path && !marked)
            mean_only++;
        costed += wanted != 0 || cost->path != nullptr;
        with_path += cost->path != nullptr;
    }
    detail::guest_cost_enabled = costed != 0;
    const char* late = std::getenv("GCRT_DECOMP_TEST_LATE_CHARGE");
    s_late_charge = late && late[0] == '1';
    detail::test_late_charge = s_late_charge;
    if (s_late_charge)
        std::fprintf(stderr, "gc_abi: GCRT_DECOMP_TEST_LATE_CHARGE=1 (test only): no cycles charged before guest calls "
                             "(the behaviour before the fix of issue #68)\n");
    const char* cc = std::getenv("GCRT_DECOMP_CROSSCHECK");
    s_crosscheck_limit = cc && cc[0] ? static_cast<unsigned>(std::strtoul(cc, nullptr, 0)) : 0u;
    detail::crosscheck_enabled = s_crosscheck_limit != 0;
    unsigned with_fpu = 0;
    for (unsigned i = 0; i < count; i++)
        with_fpu += (table[i].flags & registration_uses_fpu) != 0;
    std::fprintf(stderr, "gc_abi: %u decompiled functions registered (%u whose original uses the FPU: declined while "
                         "MSR[FP] = 0)%s\n",
                 count, with_fpu,
                 detail::crosscheck_enabled ? " (cross-check with the recompiled originals on: diagnostic, the guest "
                                              "cycles of the cross-checked calls are counted twice)"
                                            : "");
    // The clock is not known yet here (the platform's pacing may switch to the cycle clock later): the message says
    // when the costs apply; the exit report (DECOMP guest_cost) says what was charged.
    std::fprintf(stderr, "gc_abi: guest cost of %u of them charged to the cycle clock, only while the CPU runs on it "
                         "(default pacing, bench; nothing with the real-time clock, GCRT_PACING=host), %u by the path "
                         "of their original, %u path-dependent charged their mean (code without path marks)%s\n",
                 costed, with_path, mean_only, costs_off ? " (GCRT_DECOMP_GUEST_COST=0: costs off)" : "");
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
        // calls = runs of the decompiled version (call_count, after the declines); fpu_declined = entries left to the
        // original because MSR[FP] = 0 (registration_uses_fpu), counted in hook/chassis too.
        std::fprintf(out, "DECOMP %-32s %08X calls=%llu hook=%llu chassis=%llu fpu=%u fpu_declined=%llu",
                     e.reg->record->name, static_cast<unsigned>(e.reg->address),
                     static_cast<unsigned long long>(calls), static_cast<unsigned long long>(via_hook),
                     static_cast<unsigned long long>(dispatched - via_hook),
                     (e.reg->flags & registration_uses_fpu) ? 1u : 0u,
                     static_cast<unsigned long long>(e.fpu_declined));
        if (s_crosscheck_limit)
            std::fprintf(out, " crosscheck=%u mismatches=%u", e.checks, e.mismatches);
        if (const guest_cost_state* c = e.reg->record->cost; c && c->path)
            std::fprintf(out, " path_walks=%llu path_errors=%llu", static_cast<unsigned long long>(c->path_walks),
                         static_cast<unsigned long long>(c->path_errors));
        if (e.prefix_unknown)
            std::fprintf(out, " prefix_unknown=%llu", static_cast<unsigned long long>(e.prefix_unknown));
        std::fputc('\n', out);
    }
    u64 dispatch_calls = 0, dispatch_misses = 0;
    gcrt_hle_dispatch_counts(&dispatch_calls, &dispatch_misses);
    std::fprintf(out, "DECOMP total functions=%u calls=%llu hle_dispatch=%llu hle_dispatch_misses=%llu\n",
                 static_cast<unsigned>(s_entries.size()), static_cast<unsigned long long>(total),
                 static_cast<unsigned long long>(dispatch_calls), static_cast<unsigned long long>(dispatch_misses));
    // Guest cost actually charged (cycle clock) and calls left uncharged (real-time clock: the cost was inert).
    std::fprintf(out, "DECOMP guest_cost charged_cycles=%llu uncharged_calls=%llu\n",
                 static_cast<unsigned long long>(s_cost_charged_cycles),
                 static_cast<unsigned long long>(s_cost_uncharged_calls));
    // Cost by path (gc_path.h): calls charged by the walk of their original's graph, and path errors (walks that did
    // not end on a return of the original: the table's mean was charged; 0 expected).
    u64 path_functions = 0, path_walks = 0, path_errors = 0;
    for (const entry_state& e : s_entries)
        if (const guest_cost_state* c = e.reg->record->cost; c && c->path) {
            path_functions++;
            path_walks += c->path_walks;
            path_errors += c->path_errors;
        }
    std::fprintf(out, "DECOMP path_cost functions=%llu walks=%llu errors=%llu\n",
                 static_cast<unsigned long long>(path_functions), static_cast<unsigned long long>(path_walks),
                 static_cast<unsigned long long>(path_errors));
    // Calls into guest code (issue #68): guest calls made by decompiled code, cycles charged before them (prefixes of
    // the decompiled callers, pending cycles of the generated block that reached the gateway), and frames whose
    // prefix was unknown at such a call (0 expected; the build refuses a function without the graph it needs).
    std::fprintf(out, "DECOMP guest_calls calls=%llu prefix_cycles=%llu pending_cycles=%llu prefix_unknown=%llu%s\n",
                 static_cast<unsigned long long>(s_guest_calls_from_decomp),
                 static_cast<unsigned long long>(s_prefix_charged_cycles),
                 static_cast<unsigned long long>(s_pending_charged_cycles),
                 static_cast<unsigned long long>(s_prefix_unknown),
                 s_late_charge ? " (GCRT_DECOMP_TEST_LATE_CHARGE=1: nothing charged before the calls)" : "");
}

namespace detail {

u32 bypass_address = 0;
bool crosscheck_enabled = false;
bool guest_cost_enabled = false;
bool test_late_charge = false;
path_walk* current_path = nullptr;

namespace {
// charge_guest_cost of the mean, less `already` cycles charged before guest calls (a walk with a graph whose end was
// a path error: the mean is charged, the part already charged taken out).
void charge_mean_less(GcrtCpu* g, guest_cost_state& cost, u64 already) noexcept {
    if (!already) {
        charge_guest_cost(g, cost);
        return;
    }
    if (!g || g->clock != GCRT_CLOCK_CYCLES)
        return;
    const u64 total = static_cast<u64>(cost.remainder_256ths) + cost.per_call_256ths;
    cost.remainder_256ths = static_cast<u32>(total & 0xFFu);
    const u64 whole = total >> 8;
    if (whole > already) {
        gcrt_cpu_add_cycles(g, whole - already);
        s_cost_charged_cycles += whole - already;
    }
}
} // namespace

void charge_guest_cost(GcrtCpu* g, guest_cost_state& cost, const path_scope& scope) noexcept {
    const path_walk& w = scope.walk;
    if (!w.graph || !g || g->clock != GCRT_CLOCK_CYCLES) {
        // Constant cost, or the real-time clock (nothing charged there). Nothing was charged before guest calls: a
        // walk without graph has no prefix, and none is charged off the cycle clock.
        charge_guest_cost(g, cost);
        return;
    }
    if (path_complete(w)) {
        cost.path_walks++;
        // The cycles of the path, less those already charged before guest calls (issue #68): same total.
        const u64 rest = w.cycles > w.charged ? w.cycles - w.charged : 0;
        if (rest)
            gcrt_cpu_add_cycles(g, rest);
        s_cost_charged_cycles += rest;
        return;
    }
    // The decompiled code marked fewer or more outcomes than the original's path has: its markers do not follow the
    // original's branches (gc_path.h). The table's mean is charged; the oracle shows such a call as a path mismatch.
    cost.path_errors++;
    if (!s_path_error_reported) {
        s_path_error_reported = true;
        const char* name = "?";
        for (const entry_state& e : s_entries)
            if (e.reg->record->cost == &cost)
                name = e.reg->record->name;
        std::fprintf(stderr, "gc_abi: PATH ERROR in %s: %u outcomes marked, walk %s (the mean cost is charged; "
                             "first error only, all counted in DECOMP path_cost)\n", name, w.steps,
                     w.error ? "had already reached a return" : "did not reach a return");
    }
    charge_mean_less(g, cost, w.charged);
}

void charge_pending_at_entry(GcrtCpu* g) noexcept {
    if (s_late_charge || !g)
        return;
    if (gcrt_hle_dispatch_from_hook())
        charge_pending_cycles(g);
    else
        g->state.downcount = 0; // the previous block's value, already charged by the chassis
}

void charge_pending_cycles(GcrtCpu* g) noexcept {
    if (s_late_charge || !g)
        return;
    const s64 pending = g->state.downcount;
    if (pending < 0) {
        gcrt_cpu_add_cycles(g, static_cast<u64>(-pending));
        s_pending_charged_cycles += static_cast<u64>(-pending);
    }
    g->state.downcount = 0;
}

bool match_call_site(path_walk& w, u32 target) noexcept {
    w.call_site = path_no_call;
    if (!w.graph)
        return false;
    u32 best = path_no_call;
    for (u32 i = 0; i < w.graph->call_count; i++) {
        const path_call& c = w.graph->calls[i];
        if (c.next != w.node || (c.target != target && c.target != 0))
            continue;
        if (w.call_after != path_no_call && c.after >= w.call_after)
            continue; // before the last call matched on this edge
        if (best == path_no_call || c.after > w.graph->calls[best].after)
            best = i;
    }
    if (best == path_no_call)
        return false;
    w.call_site = best;
    w.call_after = w.graph->calls[best].after;
    return true;
}

bool call_prefix(path_walk& w, u32 target, u64& prefix) noexcept {
    if (!match_call_site(w, target))
        return false;
    const u32 after = w.graph->calls[w.call_site].after;
    prefix = w.cycles >= after ? w.cycles - after : 0;
    return true;
}

namespace {
// Before a nested guest run from decompiled code (run_guest_call): the prefix of each decompiled function on the C++
// stack, from the innermost (whose call site is the guest call to `target`) to the outermost (whose call site is the
// plain C++ call matched when the next one started, path_scope), less what each already charged (gc_abi.h "Calls
// into guest code"). Only on the cycle clock with guest costs on, as the end of each call charges.
void charge_before_guest_call(GcrtCpu* g, u32 target) noexcept {
    s_guest_calls_from_decomp++;
    if (s_late_charge || !guest_cost_enabled || !g || g->clock != GCRT_CLOCK_CYCLES)
        return;
    u64 amount = 0;
    u32 t = target;
    for (path_walk* w = current_path; w; t = w->address, w = w->outer) {
        u64 prefix = 0;
        bool known;
        if (w == current_path) {
            known = call_prefix(*w, t, prefix);
        } else {
            known = w->graph && w->call_site != path_no_call;
            if (known) {
                const u32 after = w->graph->calls[w->call_site].after;
                prefix = w->cycles >= after ? w->cycles - after : 0;
            }
        }
        if (!known) {
            s_prefix_unknown++;
            entry_state* e = lookup(w->address);
            if (e)
                e->prefix_unknown++;
            if (!s_prefix_unknown_reported) {
                s_prefix_unknown_reported = true;
                std::fprintf(stderr, "gc_abi: PREFIX UNKNOWN in %s (%08X) at its call to %08X: %s; its cycles before the "
                                     "call are charged at its end (first one only, all counted in DECOMP guest_calls)\n",
                             e ? e->reg->record->name : "?", static_cast<unsigned>(w->address),
                             static_cast<unsigned>(t),
                             w->graph ? "no call site of its original matches" : "no graph of its original");
            }
            continue;
        }
        if (prefix > w->charged) {
            amount += prefix - w->charged;
            w->charged = prefix;
        }
    }
    if (amount) {
        gcrt_cpu_add_cycles(g, amount);
        s_cost_charged_cycles += amount;
        s_prefix_charged_cycles += amount;
    }
}
} // namespace

void charge_guest_cost(GcrtCpu* g, guest_cost_state& cost) noexcept {
    // Real-time clock (GCRT_PACING=host): the cycle counter is not time, nothing to keep in step and nothing written.
    // The default pacing of normal play and the bench both run on the cycle clock: the cost is charged there.
    if (!g)
        return;
    if (g->clock != GCRT_CLOCK_CYCLES) {
        s_cost_uncharged_calls++;
        return;
    }
    const u64 total = static_cast<u64>(cost.remainder_256ths) + cost.per_call_256ths;
    cost.remainder_256ths = static_cast<u32>(total & 0xFFu);
    if (total >> 8) {
        gcrt_cpu_add_cycles(g, total >> 8);
        s_cost_charged_cycles += total >> 8;
    }
}
const u32* guest_msr = nullptr;

bool decline_without_fpu(u32 address) noexcept {
    entry_state* e = lookup(address);
    if (!e || !(e->reg->flags & registration_uses_fpu))
        return false;
    e->fpu_declined++;
#if defined(GC_DECOMP_ORACLE)
    oracle_declined_fpu(address);
#endif
    return true;
}

bool registered_uses_fpu(u32 address) noexcept {
    const entry_state* e = lookup(address);
    return e && (e->reg->flags & registration_uses_fpu);
}

GcrtCpu* current_cpu() noexcept {
    return s_cpu;
}

#if defined(GC_DECOMP_ORACLE)
u64 guest_calls = 0;
#endif

#if defined(GC_DECOMP_ORACLE)
bool oracle_replaying() noexcept {
    return oracle_is_replaying();
}

bool call_function(GcrtCpu* g, u32 address, u32 argument_gprs, u32 argument_fprs) noexcept {
    if (oracle_replay_call(g, address, argument_gprs, argument_fprs))
        return true;
    return run_guest_call(g, address);
}
#endif

u32 address_of_function(void (*direct)()) noexcept {
    for (const entry_state& e : s_entries)
        if (e.reg->record->direct == direct)
            return e.reg->address;
    return 0;
}

namespace {
// Nested run of the guest function at `address` for decompiled code. interruptible: the callee runs as plain guest
// code (events delivered at its block boundaries with the caller's MSR[EE], the MSR[EE] it leaves kept): a call of
// decompiled code into recompiled code (issue #65, option B). Otherwise MSR[EE] = 0 during the call and restored
// afterwards: the oracle's run of an original under comparison, the cross-check's second run of an original.
bool nested_guest_run(GcrtCpu* g, u32 address, bool interruptible) noexcept {
    // Cycles of the generated block that reached the gateway (DolRecomp: `downcount -= n` at the start of each block,
    // the chassis charges -downcount when the block returns; charge_pending_at_entry left 0 or the real pending). The nested run sets downcount for each of
    // its own blocks and charges them. The outer block's pending cycles are charged before the nested run (issue #68:
    // the recompiled game charges them at the callee's first block boundary), so its downcount is 0 after it; under
    // GCRT_DECOMP_TEST_LATE_CHARGE=1 they are kept pending and charged when the outer block returns, as before.
    s64 outer_downcount = g->state.downcount;
#if defined(GC_DECOMP_ORACLE)
    // Measurement of issue #68 (oracle builds only): the cycles still pending in the outer block (0 when the gateway
    // was reached from the chassis, or after charge_pending_cycles at the entry of the decompiled function).
    GCRT_TIMELINE("nested pending run %08X %lld", static_cast<unsigned>(address),
                  static_cast<long long>(-outer_downcount));
#endif
    if (!s_late_charge) {
        charge_pending_cycles(g);
        outer_downcount = 0;
    }
    // The guest code starts a new chain of walks (gc_path.h): a decompiled function it reaches is not called by the
    // decompiled functions on the C++ stack here, whose prefixes are charged already.
    path_walk* const outer_walk = current_path;
    current_path = nullptr;
    const bool returned =
        interruptible ? gcrt_guest_call_interruptible(g, address) : gcrt_guest_call_prepared(g, address);
    current_path = outer_walk;
    g->state.downcount = outer_downcount;
    if (!returned)
        std::fprintf(stderr, "gc_abi: the guest function %08X called by decompiled code did not return\n",
                     static_cast<unsigned>(address));
    return returned;
}
} // namespace

bool run_guest(GcrtCpu* g, u32 address) noexcept {
#if defined(GC_DECOMP_ORACLE)
    guest_calls++; // the oracle tells its own runs of the original from guest calls made by decompiled code
#endif
    return nested_guest_run(g, address, true);
}

bool run_guest_call(GcrtCpu* g, u32 address) noexcept {
    charge_before_guest_call(g, address);
    return run_guest(g, address);
}

bool run_guest_masked(GcrtCpu* g, u32 address) noexcept {
#if defined(GC_DECOMP_ORACLE)
    guest_calls++;
#endif
    return nested_guest_run(g, address, false);
}

void stop_after_no_return(u32 address) noexcept {
    GcrtCpu* g = s_cpu;
    if (!g || g->stop != GCRT_STOP_NONE)
        return; // the nested run stopped the game itself (OSPanic, PPCHalt...)
    std::fprintf(stderr, "gc_abi: the guest function %08X, declared without return, returned to decompiled code: "
                         "the game stops\n", static_cast<unsigned>(address));
    g->stop = GCRT_STOP_GUEST_HALT;
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
    const bool returned = run_guest_masked(g, address); // second run of the original: no event delivered in it
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
