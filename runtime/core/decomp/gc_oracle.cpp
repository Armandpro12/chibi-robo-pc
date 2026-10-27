// Decompilation oracle (stage 2, D1.3): in-game differential comparison of each decompiled function with its
// recompiled original, on the real calls of a run. Built only into the oracle executable (CMake option
// GCRT_DECOMP_ORACLE, tools/build.py --oracle): this file and every call to it are absent otherwise. Generic: no game
// here (names, addresses and sizes come from the registration table that tools/build.py generates).
//
// One call of a decompiled function F (gc_abi.h entry<F>, recompiled -> decompiled):
//   1. R0 = guest registers at entry;
//   2. the recompiled original runs in a nested run of the chassis (gcrt_guest_call_prepared, the entry declines
//      its own address meanwhile), with DolRecomp's write journal attached (called before every RAM store of the
//      generated code, cpu.c mem_write*): W1 = bytes written, R1 = registers after it. Any block run outside
//      [F, F + size) during that run, or a nested call of another decompiled function, marks F "not a leaf";
//   3. W1 is undone, R0 restored, the decompiled version runs with the gc_mem.h observer installed (W2 = bytes
//      written, L2 = bytes read), R2 = registers after it;
//   4. comparison (gc_oracle_compare.h): result registers, registers the EABI preserves, bytes written (not the dead
//      stack under r1); FPSCR is not compared;
//   5. W2 is undone, W1 and R1 put back: the game goes on with the ORIGINAL's results, the oracle changes nothing
//      in the game's behavior (it only runs both versions);
//   6. the starting state (argument registers and the bytes the decompiled version read) is fingerprinted; distinct
//      ones are counted and the first ones stored as replayable cases (oracle_replay.cpp, tools/decomp_oracle.py).
// Functions that are not leaves (a call or a tail branch in their code, read once at the first call, or seen during
// a run), or whose code holds a store the journal does not see (dcbz, dcbz_l), are not compared: they run as their
// original (D2: calls recorded and replayed, notes/decomp.md D1.3).
//
// Environment: GCRT_DECOMP_ORACLE_OUT=<folder>: report file oracle-<version>-<pid>.jsonl there (cases,
// differences, one summary per function); without it, only the summary lines on stdout at exit ("ORACLE ...").
// GCRT_DECOMP_ORACLE_CASES=<n>: cases stored per function (default 64).
#include "decomp/gc_abi.h"
#include "decomp/gc_oracle_compare.h"

// C headers without their own extern "C" (DolRecomp's cpu.h): the write journal is a C symbol.
extern "C" {
#include "cpu/gcrt_cpu.h"
}

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define GC_ORACLE_GETPID _getpid
#else
#include <unistd.h>
#define GC_ORACLE_GETPID getpid
#endif

namespace gc::abi::detail {

namespace {

using oracle::byte_write;
using oracle::difference;
using oracle::reg_state;

enum leaf_state : u8 { leaf_unknown, leaf_yes, leaf_no, leaf_unsupported };

struct function_state {
    u32 address = 0;
    u32 size = 0;
    const char* name = "?";
    leaf_state leaf = leaf_unknown;
    char leaf_reason[96] = "";
    u64 calls = 0;
    u64 compared = 0;
    u64 mismatches = 0;    // compared calls with at least one difference
    u64 not_compared = 0;  // calls run as the original only (not a leaf, unsupported, original did not return)
    u64 stored = 0;        // cases written
    u64 printed = 0;       // differences printed on stderr
    std::unordered_set<u64> fingerprints;     // distinct starting states
    std::unordered_set<u64> bad_fingerprints; // distinct starting states with a difference
};

const char* s_version = "?";
#if defined(GC_DECOMP_ORACLE_NEGATIVE)
constexpr bool negative_build = true; // wrong variants registered (tools/build.py --oracle-negative)
#else
constexpr bool negative_build = false;
#endif
std::vector<function_state> s_functions;
std::FILE* s_out = nullptr;
std::string s_out_path;
u64 s_case_limit = 64;
unsigned s_depth = 0;              // > 0 while a comparison runs
function_state* s_running = nullptr; // function being compared
bool s_initialized = false;

// --- registers -----------------------------------------------------------------------------------------------------

struct cpu_snapshot {
    reg_state regs;
    u32 pc, msr, reserve_addr;
    bool reserve_valid;
};

cpu_snapshot take(const CPUState* c) {
    cpu_snapshot s{};
    std::memcpy(s.regs.gpr, c->gpr, sizeof(s.regs.gpr));
    std::memcpy(s.regs.fpr, c->fpr, sizeof(s.regs.fpr));
    std::memcpy(s.regs.ps1, c->ps1, sizeof(s.regs.ps1));
    s.regs.cr = c->cr;
    s.regs.xer = c->xer;
    s.regs.lr = c->lr;
    s.regs.ctr = c->ctr;
    s.regs.fpscr = c->fpscr;
    s.pc = c->pc;
    s.msr = c->msr;
    s.reserve_addr = c->reserve_addr;
    s.reserve_valid = c->reserve_valid;
    return s;
}

void put(CPUState* c, const cpu_snapshot& s) {
    std::memcpy(c->gpr, s.regs.gpr, sizeof(s.regs.gpr));
    std::memcpy(c->fpr, s.regs.fpr, sizeof(s.regs.fpr));
    std::memcpy(c->ps1, s.regs.ps1, sizeof(s.regs.ps1));
    c->cr = s.regs.cr;
    c->xer = s.regs.xer;
    c->lr = s.regs.lr;
    c->ctr = s.regs.ctr;
    c->fpscr = s.regs.fpscr;
    c->pc = s.pc;
    c->msr = s.msr;
    c->reserve_addr = s.reserve_addr;
    c->reserve_valid = s.reserve_valid;
}

// --- memory --------------------------------------------------------------------------------------------------------

CPUState* s_state = nullptr;

u8* ram_byte(u32 address) {
    const u32 offset = address & 0x3FFFFFFFu;
    return offset < s_state->ram_size ? s_state->ram + offset : nullptr;
}
u8 read_byte(u32 address) {
    const u8* p = ram_byte(address);
    return p ? *p : 0;
}
void write_byte(u32 address, u8 value) {
    if (u8* p = ram_byte(address))
        *p = value;
}

// DolRecomp's write journal (cpu.c: called with the RAM offset and size before the store). The journal another part
// of the runtime may have attached (hle/native_os.c, idle detection) keeps being called.
oracle::write_set s_original_writes;
PPCMemWriteJournal s_previous_journal = nullptr;
void* s_previous_user = nullptr;

void journal(u32 offset, u32 size, void* /*user*/) {
    if (s_previous_journal)
        s_previous_journal(offset, size, s_previous_user);
    u64 old_value = 0;
    for (u32 i = 0; i < size && i < 8; i++)
        old_value = (old_value << 8) | (offset + i < s_state->ram_size ? s_state->ram[offset + i] : 0u);
    s_original_writes.add(0x80000000u + offset, size < 8 ? size : 8, old_value);
}

// Observer of the decompiled version (gc_mem.h, GC_MEM_OBSERVER).
struct read_record {
    u32 address;
    u32 size;
    u64 value;
};
struct decompiled_observer final : gc::memory_observer {
    oracle::write_set writes;
    std::vector<read_record> reads;
    void on_read(std::uint32_t address, std::uint32_t size, std::uint64_t value) override {
        reads.push_back(read_record{oracle::canonical_address(address), size, value});
    }
    void on_write(std::uint32_t address, std::uint32_t size, std::uint64_t old_value, std::uint64_t) override {
        writes.add(address, size, old_value);
    }
};

// --- leaf check ----------------------------------------------------------------------------------------------------

// Reads the function's code once: calls (bl, bcl, bclrl, bcctrl), tail branches out of the function (b, bcctr) and
// stores the write journal does not see (dcbz, dcbz_l) make it not comparable here.
void check_code(function_state& f) {
    for (u32 a = f.address; a < f.address + f.size; a += 4) {
        const u32 w = (u32{read_byte(a)} << 24) | (u32{read_byte(a + 1)} << 16) | (u32{read_byte(a + 2)} << 8) |
                      read_byte(a + 3);
        const u32 op = w >> 26, xo = (w >> 1) & 0x3FFu, lk = w & 1u;
        const char* reason = nullptr;
        if ((op == 18 || op == 16) && lk)
            reason = "call (bl/bcl)";
        else if (op == 19 && (xo == 16 || xo == 528) && lk)
            reason = "call (bclrl/bcctrl)";
        else if (op == 19 && xo == 528)
            reason = "indirect tail branch (bcctr)";
        else if (op == 18) {
            s32 offset = static_cast<s32>(w & 0x03FFFFFCu);
            if (offset & 0x02000000)
                offset -= 0x04000000;
            const u32 target = (w & 2u) ? static_cast<u32>(offset) : a + static_cast<u32>(offset);
            if (target < f.address || target >= f.address + f.size)
                reason = "tail branch out of the function (b)";
        } else if ((op == 31 || op == 4) && xo == 1014) {
            f.leaf = leaf_unsupported;
            std::snprintf(f.leaf_reason, sizeof(f.leaf_reason), "dcbz at %08X: store not seen by the write journal",
                          static_cast<unsigned>(a));
            return;
        }
        if (reason) {
            f.leaf = leaf_no;
            std::snprintf(f.leaf_reason, sizeof(f.leaf_reason), "%s at %08X", reason, static_cast<unsigned>(a));
            return;
        }
    }
    f.leaf = leaf_yes;
}

function_state* find_function(u32 address) {
    for (function_state& f : s_functions)
        if (f.address == address)
            return &f;
    return nullptr;
}

// --- report --------------------------------------------------------------------------------------------------------

void print_regs(std::FILE* out, const char* key, const reg_state& r) {
    std::fprintf(out, "\"%s\":{\"gpr\":[", key);
    for (unsigned i = 0; i < 32; i++)
        std::fprintf(out, "%s\"%08X\"", i ? "," : "", static_cast<unsigned>(r.gpr[i]));
    std::fprintf(out, "],\"fpr\":[");
    for (unsigned i = 0; i < 32; i++)
        std::fprintf(out, "%s\"%016llX\"", i ? "," : "", static_cast<unsigned long long>(r.fpr[i]));
    std::fprintf(out, "],\"ps1\":[");
    for (unsigned i = 0; i < 32; i++)
        std::fprintf(out, "%s\"%016llX\"", i ? "," : "", static_cast<unsigned long long>(r.ps1[i]));
    std::fprintf(out, "],\"cr\":\"%08X\",\"xer\":\"%08X\",\"lr\":\"%08X\",\"ctr\":\"%08X\",\"fpscr\":\"%08X\"}",
                 static_cast<unsigned>(r.cr), static_cast<unsigned>(r.xer), static_cast<unsigned>(r.lr),
                 static_cast<unsigned>(r.ctr), static_cast<unsigned>(r.fpscr));
}

// Byte runs [address, "before hex", "after hex"] of a write set.
void print_writes(std::FILE* out, const char* key, const std::vector<byte_write>& bytes) {
    std::fprintf(out, "\"%s\":[", key);
    for (std::size_t i = 0; i < bytes.size();) {
        std::size_t j = i + 1;
        while (j < bytes.size() && bytes[j].address == bytes[j - 1].address + 1)
            j++;
        std::fprintf(out, "%s[\"%08X\",\"", i ? "," : "", static_cast<unsigned>(bytes[i].address));
        for (std::size_t k = i; k < j; k++)
            std::fprintf(out, "%02X", bytes[k].before);
        std::fprintf(out, "\",\"");
        for (std::size_t k = i; k < j; k++)
            std::fprintf(out, "%02X", bytes[k].after);
        std::fprintf(out, "\"]");
        i = j;
    }
    std::fputc(']', out);
}

void print_reads(std::FILE* out, const std::vector<read_record>& reads) {
    std::fprintf(out, "\"reads\":[");
    for (std::size_t i = 0; i < reads.size(); i++)
        std::fprintf(out, "%s[\"%08X\",%u,\"%0*llX\"]", i ? "," : "", static_cast<unsigned>(reads[i].address),
                     static_cast<unsigned>(reads[i].size), static_cast<int>(2 * reads[i].size),
                     static_cast<unsigned long long>(reads[i].value));
    std::fputc(']', out);
}

void print_case(const char* kind, const function_state& f, u64 fingerprint, u8 result_kind, const reg_state& r0,
                const std::vector<read_record>& reads, const reg_state& r1, const std::vector<byte_write>& w1,
                const reg_state* r2, const std::vector<byte_write>* w2, const std::vector<difference>* diffs) {
    if (!s_out)
        return;
    std::fprintf(s_out, "{\"kind\":\"%s\",\"function\":\"%s\",\"version\":\"%s\",\"address\":\"%08X\",\"size\":%u,"
                        "\"result_kind\":%u,\"fingerprint\":\"%016llX\",",
                 kind, f.name, s_version, static_cast<unsigned>(f.address), static_cast<unsigned>(f.size),
                 static_cast<unsigned>(result_kind), static_cast<unsigned long long>(fingerprint));
    print_regs(s_out, "entry", r0);
    std::fputc(',', s_out);
    print_reads(s_out, reads);
    std::fputc(',', s_out);
    print_regs(s_out, "original", r1);
    std::fputc(',', s_out);
    print_writes(s_out, "original_writes", w1);
    if (r2 && w2) {
        std::fputc(',', s_out);
        print_regs(s_out, "decompiled", *r2);
        std::fputc(',', s_out);
        print_writes(s_out, "decompiled_writes", *w2);
    }
    if (diffs) {
        std::fprintf(s_out, ",\"differences\":[");
        for (std::size_t i = 0; i < diffs->size(); i++) {
            char text[160];
            oracle::describe((*diffs)[i], text, sizeof(text));
            std::fprintf(s_out, "%s\"%s\"", i ? "," : "", text);
        }
        std::fputc(']', s_out);
    }
    std::fprintf(s_out, "}\n");
}

void report_at_exit() {
    for (const function_state& f : s_functions) {
        const char* leaf = f.leaf == leaf_yes ? "yes" : f.leaf == leaf_no ? "no" : f.leaf == leaf_unsupported
                                                                                   ? "unsupported"
                                                                                   : "unknown";
        std::fprintf(stdout,
                     "ORACLE %-32s %s %08X calls=%llu compared=%llu mismatches=%llu cases=%zu bad_cases=%zu "
                     "stored=%llu not_compared=%llu leaf=%s%s%s\n",
                     f.name, s_version, static_cast<unsigned>(f.address), static_cast<unsigned long long>(f.calls),
                     static_cast<unsigned long long>(f.compared), static_cast<unsigned long long>(f.mismatches),
                     f.fingerprints.size(), f.bad_fingerprints.size(), static_cast<unsigned long long>(f.stored),
                     static_cast<unsigned long long>(f.not_compared), leaf, f.leaf_reason[0] ? " reason=" : "",
                     f.leaf_reason);
        if (s_out)
            std::fprintf(s_out,
                         "{\"kind\":\"summary\",\"function\":\"%s\",\"version\":\"%s\",\"address\":\"%08X\","
                         "\"size\":%u,\"calls\":%llu,\"compared\":%llu,\"mismatches\":%llu,\"cases\":%zu,"
                         "\"bad_cases\":%zu,\"stored\":%llu,\"not_compared\":%llu,\"leaf\":\"%s\",\"reason\":\"%s\"",
                         f.name, s_version, static_cast<unsigned>(f.address), static_cast<unsigned>(f.size),
                         static_cast<unsigned long long>(f.calls), static_cast<unsigned long long>(f.compared),
                         static_cast<unsigned long long>(f.mismatches), f.fingerprints.size(),
                         f.bad_fingerprints.size(), static_cast<unsigned long long>(f.stored),
                         static_cast<unsigned long long>(f.not_compared), leaf, f.leaf_reason);
        if (s_out) {
            // Distinct starting states of this run (tools/decomp_oracle.py adds the runs up), at most 65536.
            std::size_t n = 0;
            std::fprintf(s_out, ",\"fingerprints\":[");
            for (u64 h : f.fingerprints) {
                if (n == 65536)
                    break;
                std::fprintf(s_out, "%s\"%016llX\"", n++ ? "," : "", static_cast<unsigned long long>(h));
            }
            std::fprintf(s_out, "],\"fingerprints_truncated\":%s}\n", n < f.fingerprints.size() ? "true" : "false");
        }
    }
    if (!s_out_path.empty())
        std::fprintf(stdout, "ORACLE file %s\n", s_out_path.c_str());
    std::fflush(stdout);
    if (s_out) {
        std::fclose(s_out);
        s_out = nullptr;
    }
}

void initialize() {
    s_initialized = true;
    if (const char* n = std::getenv("GCRT_DECOMP_ORACLE_CASES"); n && n[0])
        s_case_limit = std::strtoull(n, nullptr, 0);
    if (const char* dir = std::getenv("GCRT_DECOMP_ORACLE_OUT"); dir && dir[0]) {
        char name[96];
        std::snprintf(name, sizeof(name), "/oracle-%s-%d-%lld.jsonl", s_version, static_cast<int>(GC_ORACLE_GETPID()),
                      static_cast<long long>(std::time(nullptr)));
        s_out_path = std::string(dir) + name;
        s_out = std::fopen(s_out_path.c_str(), "w");
        if (!s_out)
            std::fprintf(stderr, "gc_oracle: cannot write %s: no report file\n", s_out_path.c_str());
        else
            std::fprintf(s_out, "{\"kind\":\"header\",\"version\":\"%s\",\"functions\":%zu,\"negative\":%s}\n",
                         s_version, s_functions.size(), negative_build ? "true" : "false");
    }
    std::fprintf(stderr, "gc_oracle: %zu decompiled functions compared with their recompiled originals (%s)%s%s\n",
                 s_functions.size(), s_version, s_out ? ", report " : "", s_out ? s_out_path.c_str() : "");
    std::atexit(&report_at_exit);
}

} // namespace

bool oracle_entry(GcrtCpu* g, u32 address, void (*invoke)(const registers&), const oracle_signature& signature) {
    if (s_depth) {
        // Another decompiled function reached during a comparison: the function being compared is not a leaf.
        if (s_running && s_running->leaf == leaf_yes) {
            s_running->leaf = leaf_no;
            std::snprintf(s_running->leaf_reason, sizeof(s_running->leaf_reason),
                          "called the decompiled function at %08X", static_cast<unsigned>(address));
        }
        return false;
    }
    if (!s_initialized)
        initialize();
    function_state* f = find_function(address);
    if (!f)
        return false;
    CPUState* c = &g->state;
    s_state = c;
    f->calls++;
    if (f->leaf == leaf_unknown)
        check_code(*f);
    if (f->leaf != leaf_yes) {
        // Not comparable: the original runs alone (the game keeps its results).
        f->not_compared++;
        bypass_address = address;
        run_guest(g, address);
        bypass_address = 0;
        return true;
    }

    // 1-2. Original, with the write journal.
    const cpu_snapshot r0 = take(c);
    s_original_writes.clear();
    s_previous_journal = g_mem_write_journal;
    s_previous_user = g_mem_write_journal_user;
    ppc_set_mem_write_journal(&journal, nullptr);
    const u32 ring_start = g->ring_pos;
    s_depth++;
    s_running = f;
    bypass_address = address;
    const bool returned = run_guest(g, address);
    bypass_address = 0;
    s_running = nullptr;
    s_depth--;
    ppc_set_mem_write_journal(s_previous_journal, s_previous_user);
    if (!returned) {
        f->not_compared++;
        return true;
    }
    const u32 ring_end = g->ring_pos;
    if (ring_end - ring_start > g->ring_mask + 1u) {
        f->leaf = leaf_no;
        std::snprintf(f->leaf_reason, sizeof(f->leaf_reason), "more blocks than the history holds (%u)",
                      static_cast<unsigned>(ring_end - ring_start));
    } else {
        for (u32 i = ring_start; i != ring_end; i++) {
            const u32 pc = g->ring[i & g->ring_mask];
            if (pc < f->address || pc >= f->address + f->size) {
                f->leaf = leaf_no;
                std::snprintf(f->leaf_reason, sizeof(f->leaf_reason), "ran the block %08X", static_cast<unsigned>(pc));
                break;
            }
        }
    }
    if (f->leaf != leaf_yes) {
        f->not_compared++;
        return true; // registers and memory are the original's
    }
    const cpu_snapshot r1 = take(c);
    s_original_writes.finish(&read_byte);
    const std::vector<byte_write> w1 = s_original_writes.bytes();

    // 3. Back to the starting state, then the decompiled version under the observer.
    for (const byte_write& b : w1)
        write_byte(b.address, b.before);
    put(c, r0);
    decompiled_observer observer;
    gc::memory_observer* previous = gc::set_memory_observer(&observer);
    s_depth++;
    s_running = f;
    invoke(registers_of(g));
    s_running = nullptr;
    s_depth--;
    gc::set_memory_observer(previous);
    const cpu_snapshot r2 = take(c);
    observer.writes.finish(&read_byte);
    const std::vector<byte_write>& w2 = observer.writes.bytes();

    // 4. Comparison.
    std::vector<difference> diffs;
    oracle::compare(r1.regs, r2.regs, signature.result_kind, r0.regs.gpr[1], w1, w2, diffs);
    f->compared++;

    // 6. Fingerprint of the starting state: argument registers and the bytes the decompiled version read.
    u64 h = oracle::fnv_offset;
    for (unsigned i = 0; i < 32; i++)
        if (signature.argument_gprs & (1u << i))
            h = oracle::fnv1a(h, &r0.regs.gpr[i], 4);
    for (unsigned i = 0; i < 32; i++)
        if (signature.argument_fprs & (1u << i))
            h = oracle::fnv1a(h, &r0.regs.fpr[i], 8);
    for (const read_record& r : observer.reads) {
        h = oracle::fnv1a(h, &r.address, 4);
        h = oracle::fnv1a(h, &r.size, 4);
        h = oracle::fnv1a(h, &r.value, 8);
    }
    const bool is_new = f->fingerprints.size() < (1u << 20) && f->fingerprints.insert(h).second;
    if (!diffs.empty()) {
        f->mismatches++;
        const bool new_bad = f->bad_fingerprints.insert(h).second;
        if (f->printed < 4) {
            f->printed++;
            std::fprintf(stderr, "gc_oracle: MISMATCH %s (%s %08X), entry r3=%08X r4=%08X r5=%08X f1=%016llX:",
                         f->name, s_version, static_cast<unsigned>(f->address), static_cast<unsigned>(r0.regs.gpr[3]),
                         static_cast<unsigned>(r0.regs.gpr[4]), static_cast<unsigned>(r0.regs.gpr[5]),
                         static_cast<unsigned long long>(r0.regs.fpr[1]));
            for (std::size_t i = 0; i < diffs.size() && i < 6; i++) {
                char text[160];
                oracle::describe(diffs[i], text, sizeof(text));
                std::fprintf(stderr, "%s %s", i ? ";" : "", text);
            }
            std::fprintf(stderr, "%s\n", diffs.size() > 6 ? "; ..." : "");
        }
        if (new_bad && f->bad_fingerprints.size() <= 16)
            print_case("mismatch", *f, h, signature.result_kind, r0.regs, observer.reads, r1.regs, w1, &r2.regs, &w2,
                       &diffs);
    } else if (is_new && f->stored < s_case_limit) {
        f->stored++;
        print_case("case", *f, h, signature.result_kind, r0.regs, observer.reads, r1.regs, w1, nullptr, nullptr,
                   nullptr);
    }

    // 5. The game goes on with the original's results.
    for (const byte_write& b : w2)
        write_byte(b.address, b.before);
    for (const byte_write& b : w1)
        write_byte(b.address, b.after);
    put(c, r1);
    return true;
}

} // namespace gc::abi::detail

#if defined(GC_DECOMP_ORACLE_NEGATIVE)
namespace gc::abi::testing {
void clobber_gpr(unsigned reg, u32 value) noexcept {
    if (GcrtCpu* g = detail::current_cpu(); g && reg < 32)
        g->state.gpr[reg] = value;
}
void clobber_cr(u32 value) noexcept {
    if (GcrtCpu* g = detail::current_cpu())
        g->state.cr = value;
}
} // namespace gc::abi::testing
#endif

// Called by the registration table that tools/build.py generates for an oracle build, before
// gcrt_decomp_install_table: the version and the size of each function (leaf check).
extern "C" void gcrt_decomp_oracle_configure(const char* version, const unsigned* addresses, const unsigned* sizes,
                                             const char* const* names, unsigned count) {
    using gc::abi::detail::s_functions;
    gc::abi::detail::s_version = version;
    s_functions.clear();
    s_functions.resize(count);
    for (unsigned i = 0; i < count; i++) {
        s_functions[i].address = addresses[i];
        s_functions[i].size = sizes[i];
        s_functions[i].name = names[i];
    }
}
