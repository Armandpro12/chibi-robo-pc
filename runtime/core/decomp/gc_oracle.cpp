// Decompilation oracle (stage 2, D1.3): in-game differential comparison of each decompiled function with its
// recompiled original, on the real calls of a run and on hand-written synthetic cases. Built only into the oracle
// executable (CMake option GCRT_DECOMP_ORACLE, tools/build.py --oracle): this file and every call to it are absent
// otherwise. Generic: no game here (names, addresses, sizes and basic blocks come from the registration table that
// tools/build.py generates).
//
// One call of a decompiled function F (gc_abi.h entry<F>, recompiled -> decompiled):
//   1. R0 = guest registers at entry;
//   2. the recompiled original runs in a nested run of the chassis (gcrt_guest_call_prepared, the entry declines
//      its own address meanwhile), with DolRecomp's write journal attached (called before every RAM store of the
//      generated code, cpu.c mem_write*): W1 = bytes written, R1 = registers after it. Any block run outside
//      [F, F + size) during that run, or a nested call of another decompiled function, marks F "not a leaf". The
//      oracle build compiles a hook at the start of every basic block of F (tools/build.py): each one that runs is
//      recorded (block coverage of the original). A store outside RAM (hardware registers, write-gather pipe, locked
//      cache: everything cpu.c sends to external_write) makes F not comparable: it cannot be undone;
//   3. W1 is undone, R0 restored, the decompiled version runs with the gc_mem.h observer installed (W2 = bytes
//      written, L2 = bytes read), R2 = registers after it. If it calls guest code (run_guest), F is not comparable
//      as a leaf; the RAM writes of that guest code are journaled into W2 so that they are undone too;
//   4. comparison (gc_oracle_compare.h): result registers, registers the EABI preserves, bytes written (not the dead
//      stack under r1, GCRT_DECOMP_ORACLE_DEAD_STACK bytes, default 64 KiB); FPSCR is not compared (see
//      gc_oracle_compare.h);
//   5. W2 is undone, W1 and R1 put back: the game goes on with the ORIGINAL's results, the oracle changes nothing
//      in the game's behavior (it only runs both versions);
//   6. the starting state (argument registers and the bytes the decompiled version read) is fingerprinted; distinct
//      ones are counted and the first ones stored as replayable cases (oracle_replay.cpp, tools/decomp_oracle.py);
//      the distinct results of the original (r3, r3:r4 or the bits of f1) and the distinct sets of blocks run
//      ("paths") are counted too.
// Functions that are not leaves (a call or a tail branch in their code, read once at the first call, or seen during
// a run), or whose code holds a store the journal does not see (dcbz, dcbz_l), are not compared: they run as their
// original (D2: calls recorded and replayed, notes/decomp.md D1.3).
//
// Synthetic cases (GCRT_DECOMP_ORACLE_SYNTHETIC=<folder of .txt files>, format below): starting states written by
// hand to reach branches the scenarios do not reach. At the first comparable call of F, each case of F is injected
// into a scratch area of the dead stack (under the caller's r1, so no live data and nothing another thread reads),
// both versions run from it as above, and everything is put back afterwards (registers, scratch area, W1, W2): the
// game does not see them. They count apart from the real calls (synthetic_* in the summary) and are written as
// replayable cases with their label.
//   case <function> <label>     starts a case (one function, any label without spaces)
//   area <bytes>                size of the scratch area (default 0x100, at most 0x400), zero-filled first
//   rN = <value>                gpr N; fN = <hex bits> (fpr N), pN = <hex bits> (ps1 of fpr N)
//   mem <address> <1|2|4> <value>   big-endian store into the area before the run
//   end
// <value> and <address>: hexadecimal, or S, S+<hex>, S-<hex> with S the start of the scratch area. r1 is set by the
// oracle (0x100 bytes under the area, so the area is above the stack pointer and compared). '#' starts a comment.
//
// Limits (notes/decomp.md D1.3): writes outside RAM are detected and refused, not compared; the observer of gc_mem.h
// does not see plain u8 struct fields (gc_mem.h refuses the byte pointers, not the fields): a decompiled write to
// such a field is neither compared nor undone.
//
// Environment: GCRT_DECOMP_ORACLE_OUT=<folder>: report file oracle-<version>-<pid>-<time>.jsonl there (header,
// cases, differences, one summary per function, end); without it, only the summary lines on stdout at exit
// ("ORACLE ..."). Every case and difference line is flushed when written, so that a run cut short keeps them (its
// missing summaries and end line tell tools/decomp_oracle.py it did not finish). GCRT_DECOMP_ORACLE_CASES=<n>: cases
// stored per function (default 64).
#include "decomp/gc_abi.h"
#include "decomp/gc_oracle_compare.h"

// C headers without their own extern "C" (DolRecomp's cpu.h): the write journal is a C symbol.
extern "C" {
#include "cpu/gcrt_cpu.h"
#include "hle/gcrt_hle.h"
}

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

struct synthetic_case {
    std::string label;
    u32 area = 0x100;
    struct reg_set {
        char kind; // 'r', 'f', 'p'
        unsigned index;
        bool relative;
        s64 value; // relative: offset from S
    };
    std::vector<reg_set> regs;
    struct mem_set {
        u32 offset; // from S
        u32 size;
        bool relative;
        s64 value;
    };
    std::vector<mem_set> mem;
};

struct function_state {
    u32 address = 0;
    u32 size = 0;
    const char* name = "?";
    leaf_state leaf = leaf_unknown;
    char leaf_reason[112] = "";
    u64 calls = 0;
    u64 compared = 0;
    u64 mismatches = 0;    // compared calls with at least one difference
    u64 not_compared = 0;  // calls run as the original only (not a leaf, unsupported, original did not return)
    u64 stored = 0;        // cases written
    u64 printed = 0;       // differences printed on stderr
    std::unordered_set<u64> fingerprints;     // distinct starting states
    std::unordered_set<u64> bad_fingerprints; // distinct starting states with a difference
    // Basic blocks (tools/build.py): starts of the reachable ones (blocks[0] = entry), and of the unreachable ones.
    std::vector<u32> blocks, dead_blocks;
    std::vector<u64> block_hits, dead_block_hits; // compared calls (real and synthetic) that ran each block
    std::vector<u8> call_blocks, call_dead;       // blocks run by the current call of the original
    std::unordered_set<u64> paths;                 // distinct sets of blocks run
    std::set<std::pair<u64, u64>> results;         // distinct results of the original
    // Synthetic cases.
    std::vector<synthetic_case> synthetic;
    bool synthetic_done = false;
    u64 synthetic_compared = 0, synthetic_mismatches = 0, synthetic_not_compared = 0;
};

const char* s_version = "?";
const char* s_sources_hash = "";
#if defined(GC_DECOMP_ORACLE_NEGATIVE)
constexpr bool negative_build = true; // wrong variants registered (tools/build.py --oracle-negative)
#else
constexpr bool negative_build = false;
#endif
std::vector<function_state> s_functions;
std::unordered_map<u32, std::pair<u32, u32>> s_probes; // block start -> (function index, block index | 0x80000000 dead)
std::FILE* s_out = nullptr;
std::string s_out_path;
std::string s_synthetic_dir;
unsigned s_synthetic_loaded = 0;
std::vector<std::string> s_synthetic_errors;
u64 s_case_limit = 64;
u32 s_dead_stack = oracle::default_dead_stack;
unsigned s_depth = 0;                // > 0 while a comparison runs
function_state* s_running = nullptr; // function being compared
bool s_in_original = false;          // the original of s_running is running (block hooks record)
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

// DolRecomp's write journal (cpu.c: called with the RAM offset and size before the store), into *s_journal_target.
// The journal another part of the runtime may have attached (hle/native_os.c, idle detection) keeps being called.
oracle::write_set s_original_writes;
oracle::write_set* s_journal_target = &s_original_writes;
PPCMemWriteJournal s_previous_journal = nullptr;
void* s_previous_user = nullptr;

void journal(u32 offset, u32 size, void* /*user*/) {
    if (s_previous_journal)
        s_previous_journal(offset, size, s_previous_user);
    u64 old_value = 0;
    for (u32 i = 0; i < size && i < 8; i++)
        old_value = (old_value << 8) | (offset + i < s_state->ram_size ? s_state->ram[offset + i] : 0u);
    s_journal_target->add(0x80000000u + offset, size < 8 ? size : 8, old_value);
}

void attach_journal(oracle::write_set* target) {
    s_journal_target = target;
    s_previous_journal = g_mem_write_journal;
    s_previous_user = g_mem_write_journal_user;
    ppc_set_mem_write_journal(&journal, nullptr);
}
void detach_journal() {
    ppc_set_mem_write_journal(s_previous_journal, s_previous_user);
}

// Stores outside RAM during the original (everything cpu.c does not resolve to RAM goes to external_write, or to
// external_write32 for ecowx): counted, then passed on unchanged.
u64 s_external_writes = 0;
u32 s_first_external = 0;
PPCExternalWrite s_external_write = nullptr;
PPCExternalWrite32 s_external_write32 = nullptr;
void external_write(CPUState* cpu, u32 ea, u64 value, u8 size) {
    if (!s_external_writes++)
        s_first_external = ea;
    if (s_external_write)
        s_external_write(cpu, ea, value, size);
}
void external_write32(CPUState* cpu, u32 ea, u32 value, u8 rid) {
    if (!s_external_writes++)
        s_first_external = ea;
    if (s_external_write32)
        s_external_write32(cpu, ea, value, rid);
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
function_state* find_function(const std::string& name) {
    for (function_state& f : s_functions)
        if (name == f.name)
            return &f;
    return nullptr;
}

// Hook compiled at the start of a basic block of a decompiled function (tools/build.py, oracle builds): records that
// the block ran during the comparison of its function, then declines (the original goes on).
bool block_hook(GcrtCpu* /*g*/, u32 address) {
    if (!s_running || !s_in_original)
        return false;
    const auto it = s_probes.find(address);
    if (it == s_probes.end() || &s_functions[it->second.first] != s_running)
        return false;
    const u32 index = it->second.second;
    if (index & 0x80000000u)
        s_running->call_dead[index & 0x7FFFFFFFu] = 1;
    else
        s_running->call_blocks[index] = 1;
    return false;
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

void print_block_list(std::FILE* out, const char* key, const std::vector<u32>& blocks, const std::vector<u8>* only) {
    std::fprintf(out, "\"%s\":[", key);
    bool first = true;
    for (std::size_t i = 0; i < blocks.size(); i++)
        if (!only || (*only)[i]) {
            std::fprintf(out, "%s\"%08X\"", first ? "" : ",", static_cast<unsigned>(blocks[i]));
            first = false;
        }
    std::fputc(']', out);
}

void print_case(const char* kind, const function_state& f, u64 fingerprint, u8 result_kind, const reg_state& r0,
                const std::vector<read_record>& reads, const reg_state& r1, const std::vector<byte_write>& w1,
                const reg_state* r2, const std::vector<byte_write>* w2, const std::vector<difference>* diffs,
                const char* synthetic) {
    if (!s_out)
        return;
    std::fprintf(s_out, "{\"kind\":\"%s\",\"function\":\"%s\",\"version\":\"%s\",\"address\":\"%08X\",\"size\":%u,"
                        "\"result_kind\":%u,\"fingerprint\":\"%016llX\",\"dead_stack\":\"%08X\",",
                 kind, f.name, s_version, static_cast<unsigned>(f.address), static_cast<unsigned>(f.size),
                 static_cast<unsigned>(result_kind), static_cast<unsigned long long>(fingerprint),
                 static_cast<unsigned>(s_dead_stack));
    if (synthetic)
        std::fprintf(s_out, "\"synthetic\":\"%s\",", synthetic);
    print_block_list(s_out, "blocks", f.blocks, &f.call_blocks);
    std::fputc(',', s_out);
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
    std::fflush(s_out); // a run cut short keeps its cases and differences
}

void print_u64_list(std::FILE* out, const char* key, const std::vector<u64>& values) {
    std::fprintf(out, ",\"%s\":[", key);
    for (std::size_t i = 0; i < values.size(); i++)
        std::fprintf(out, "%s%llu", i ? "," : "", static_cast<unsigned long long>(values[i]));
    std::fputc(']', out);
}

void report_at_exit() {
    for (const function_state& f : s_functions) {
        const char* leaf = f.leaf == leaf_yes ? "yes" : f.leaf == leaf_no ? "no" : f.leaf == leaf_unsupported
                                                                                   ? "unsupported"
                                                                                   : "unknown";
        u64 covered = 0;
        for (u64 n : f.block_hits)
            covered += n != 0;
        std::fprintf(stdout,
                     "ORACLE %-32s %s %08X calls=%llu compared=%llu mismatches=%llu cases=%zu bad_cases=%zu "
                     "stored=%llu not_compared=%llu leaf=%s blocks=%llu/%zu results=%zu synthetic=%llu/%zu "
                     "synthetic_mismatches=%llu%s%s\n",
                     f.name, s_version, static_cast<unsigned>(f.address), static_cast<unsigned long long>(f.calls),
                     static_cast<unsigned long long>(f.compared), static_cast<unsigned long long>(f.mismatches),
                     f.fingerprints.size(), f.bad_fingerprints.size(), static_cast<unsigned long long>(f.stored),
                     static_cast<unsigned long long>(f.not_compared), leaf, static_cast<unsigned long long>(covered),
                     f.blocks.size(), f.results.size(), static_cast<unsigned long long>(f.synthetic_compared),
                     f.synthetic.size(), static_cast<unsigned long long>(f.synthetic_mismatches),
                     f.leaf_reason[0] ? " reason=" : "", f.leaf_reason);
        if (!s_out)
            continue;
        std::fprintf(s_out,
                     "{\"kind\":\"summary\",\"function\":\"%s\",\"version\":\"%s\",\"address\":\"%08X\","
                     "\"size\":%u,\"calls\":%llu,\"compared\":%llu,\"mismatches\":%llu,\"cases\":%zu,"
                     "\"bad_cases\":%zu,\"stored\":%llu,\"not_compared\":%llu,\"leaf\":\"%s\",\"reason\":\"%s\","
                     "\"synthetic_cases\":%zu,\"synthetic_compared\":%llu,\"synthetic_mismatches\":%llu,"
                     "\"synthetic_not_compared\":%llu,\"paths\":%zu,",
                     f.name, s_version, static_cast<unsigned>(f.address), static_cast<unsigned>(f.size),
                     static_cast<unsigned long long>(f.calls), static_cast<unsigned long long>(f.compared),
                     static_cast<unsigned long long>(f.mismatches), f.fingerprints.size(), f.bad_fingerprints.size(),
                     static_cast<unsigned long long>(f.stored), static_cast<unsigned long long>(f.not_compared), leaf,
                     f.leaf_reason, f.synthetic.size(), static_cast<unsigned long long>(f.synthetic_compared),
                     static_cast<unsigned long long>(f.synthetic_mismatches),
                     static_cast<unsigned long long>(f.synthetic_not_compared), f.paths.size());
        print_block_list(s_out, "blocks", f.blocks, nullptr);
        print_u64_list(s_out, "block_hits", f.block_hits);
        std::fputc(',', s_out);
        print_block_list(s_out, "dead_blocks", f.dead_blocks, nullptr);
        print_u64_list(s_out, "dead_block_hits", f.dead_block_hits);
        // Distinct results of the original (hex of the result registers), at most 64 listed.
        std::fprintf(s_out, ",\"results_count\":%zu,\"results\":[", f.results.size());
        std::size_t n = 0;
        for (const auto& [a, b] : f.results) {
            if (n == 64)
                break;
            std::fprintf(s_out, "%s\"%llX:%llX\"", n++ ? "," : "", static_cast<unsigned long long>(a),
                         static_cast<unsigned long long>(b));
        }
        // Distinct starting states of this run (tools/decomp_oracle.py adds the runs up), at most 65536.
        n = 0;
        std::fprintf(s_out, "],\"fingerprints\":[");
        for (u64 h : f.fingerprints) {
            if (n == 65536)
                break;
            std::fprintf(s_out, "%s\"%016llX\"", n++ ? "," : "", static_cast<unsigned long long>(h));
        }
        std::fprintf(s_out, "],\"fingerprints_truncated\":%s}\n", n < f.fingerprints.size() ? "true" : "false");
    }
    if (!s_out_path.empty())
        std::fprintf(stdout, "ORACLE file %s\n", s_out_path.c_str());
    std::fflush(stdout);
    if (s_out) {
        // Last line: the run reached its normal exit (tools/decomp_oracle.py refuses a file without it).
        std::fprintf(s_out, "{\"kind\":\"end\",\"functions\":%zu}\n", s_functions.size());
        std::fclose(s_out);
        s_out = nullptr;
    }
}

// --- synthetic cases -----------------------------------------------------------------------------------------------

// "S", "S+hex", "S-hex" or "hex".
bool parse_value(const std::string& text, bool& relative, s64& value) {
    if (text.empty())
        return false;
    char* end = nullptr;
    if (text[0] == 'S') {
        relative = true;
        if (text.size() == 1) {
            value = 0;
            return true;
        }
        if (text[1] != '+' && text[1] != '-')
            return false;
        const s64 v = static_cast<s64>(std::strtoull(text.c_str() + 2, &end, 16));
        value = text[1] == '-' ? -v : v;
        return end && *end == '\0';
    }
    relative = false;
    value = static_cast<s64>(std::strtoull(text.c_str(), &end, 16));
    return end && *end == '\0';
}

void load_synthetic_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        s_synthetic_errors.push_back(path + ": cannot read");
        return;
    }
    std::string line;
    unsigned number = 0;
    function_state* f = nullptr;
    synthetic_case c;
    auto error = [&](const char* what) {
        s_synthetic_errors.push_back(path + ":" + std::to_string(number) + ": " + what);
    };
    while (std::getline(in, line)) {
        number++;
        if (const auto hash = line.find('#'); hash != std::string::npos)
            line.resize(hash);
        std::istringstream ls(line);
        std::string tag;
        if (!(ls >> tag))
            continue;
        if (tag == "case") {
            std::string name;
            c = synthetic_case{};
            ls >> name >> c.label;
            f = find_function(name);
            if (!f)
                error("function not registered in this build");
            if (c.label.empty())
                error("case without a label");
        } else if (tag == "area") {
            std::string v;
            ls >> v;
            c.area = static_cast<u32>(std::strtoul(v.c_str(), nullptr, 16));
            if (c.area == 0 || c.area > 0x400 || (c.area & 3))
                error("area: 4 to 0x400 bytes, multiple of 4");
        } else if (tag == "mem") {
            std::string a, size, v;
            ls >> a >> size >> v;
            synthetic_case::mem_set m{};
            bool rel = false;
            s64 addr = 0;
            m.size = static_cast<u32>(std::strtoul(size.c_str(), nullptr, 0));
            if (!parse_value(a, rel, addr) || !rel || addr < 0 || (m.size != 1 && m.size != 2 && m.size != 4) ||
                !parse_value(v, m.relative, m.value)) {
                error("mem <S+offset> <1|2|4> <value>");
                continue;
            }
            m.offset = static_cast<u32>(addr);
            c.mem.push_back(m);
        } else if (tag == "end") {
            if (f) {
                bool ok = true;
                for (const auto& m : c.mem)
                    ok &= m.offset + m.size <= c.area;
                if (!ok)
                    error("mem outside the area");
                else {
                    f->synthetic.push_back(c);
                    s_synthetic_loaded++;
                }
            }
            f = nullptr;
        } else if (tag.size() >= 2 && (tag[0] == 'r' || tag[0] == 'f' || tag[0] == 'p')) {
            std::string eq, v;
            ls >> eq >> v;
            synthetic_case::reg_set r{};
            r.kind = tag[0];
            r.index = static_cast<unsigned>(std::strtoul(tag.c_str() + 1, nullptr, 10));
            if (eq != "=" || r.index >= 32 || (r.kind == 'r' && r.index == 1) || !parse_value(v, r.relative, r.value) ||
                (r.relative && r.kind != 'r')) {
                error("register: rN = <value> (not r1), fN = <hex bits>, pN = <hex bits>");
                continue;
            }
            c.regs.push_back(r);
        } else {
            error("unknown line");
        }
    }
}

// Every *.txt file of the folder, in name order.
void load_synthetic(const char* dir) {
    s_synthetic_dir = dir;
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(s_synthetic_dir, error))
        if (entry.is_regular_file() && entry.path().extension() == ".txt")
            files.push_back(entry.path());
    if (error) {
        s_synthetic_errors.push_back(s_synthetic_dir + ": cannot list the folder");
        return;
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files)
        load_synthetic_file(file.string());
}

// --- one comparison ------------------------------------------------------------------------------------------------

u64 result_key_hash(const std::vector<u8>& blocks) {
    return oracle::fnv1a(oracle::fnv_offset, blocks.data(), blocks.size());
}

std::pair<u64, u64> result_of(const reg_state& r, u8 kind) {
    switch (kind) {
    case oracle::result_word:
    case oracle::result_memory:
        return {r.gpr[3], 0};
    case oracle::result_pair:
        return {r.gpr[3], r.gpr[4]};
    case oracle::result_f32:
        return {r.fpr[1], r.ps1[1]};
    case oracle::result_f64:
        return {r.fpr[1], 0};
    default:
        return {0, 0};
    }
}

void mark_not_comparable(function_state& f, leaf_state state, const char* reason) {
    f.leaf = state;
    std::snprintf(f.leaf_reason, sizeof(f.leaf_reason), "%s", reason);
}

// Runs the original then the decompiled version from the current CPU state and compares them. `synthetic`: label of a
// synthetic case (everything is undone afterwards; the caller restores R0) or nullptr for a real call (the game keeps
// the original's results). True if compared.
bool compare_call(GcrtCpu* g, function_state& f, void (*invoke)(const registers&), const oracle_signature& signature,
                  const char* synthetic) {
    CPUState* c = &g->state;
    auto not_compared = [&]() {
        if (synthetic)
            f.synthetic_not_compared++;
        else
            f.not_compared++;
        return false;
    };

    // 1-2. Original, with the write journal, the block hooks and the stores outside RAM watched.
    const cpu_snapshot r0 = take(c);
    s_original_writes.clear();
    std::fill(f.call_blocks.begin(), f.call_blocks.end(), u8{0});
    std::fill(f.call_dead.begin(), f.call_dead.end(), u8{0});
    attach_journal(&s_original_writes);
    s_external_writes = 0;
    s_external_write = c->external_write;
    s_external_write32 = c->external_write32;
    c->external_write = &external_write;
    c->external_write32 = &external_write32;
    const u32 ring_start = g->ring_pos;
    s_depth++;
    s_running = &f;
    s_in_original = true;
    bypass_address = f.address;
    const bool returned = run_guest(g, f.address);
    bypass_address = 0;
    s_in_original = false;
    s_running = nullptr;
    s_depth--;
    c->external_write = s_external_write;
    c->external_write32 = s_external_write32;
    detach_journal();
    s_original_writes.finish(&read_byte);
    const std::vector<byte_write> w1 = s_original_writes.bytes();
    auto undo_original = [&]() {
        for (const byte_write& b : w1)
            write_byte(b.address, b.before);
    };
    if (!returned) {
        if (synthetic)
            undo_original();
        return not_compared();
    }
    if (s_external_writes) {
        // A store to a hardware register cannot be undone, and the decompiled version cannot be compared on it.
        char reason[112];
        std::snprintf(reason, sizeof(reason), "the original wrote outside RAM at %08X (%llu stores): not comparable",
                      static_cast<unsigned>(s_first_external), static_cast<unsigned long long>(s_external_writes));
        mark_not_comparable(f, leaf_unsupported, reason);
        if (synthetic)
            undo_original();
        return not_compared();
    }
    const u32 ring_end = g->ring_pos;
    if (ring_end - ring_start > g->ring_mask + 1u) {
        char reason[112];
        std::snprintf(reason, sizeof(reason), "more blocks than the history holds (%u)",
                      static_cast<unsigned>(ring_end - ring_start));
        mark_not_comparable(f, leaf_no, reason);
    } else {
        for (u32 i = ring_start; i != ring_end; i++) {
            const u32 pc = g->ring[i & g->ring_mask];
            if (pc < f.address || pc >= f.address + f.size) {
                char reason[112];
                std::snprintf(reason, sizeof(reason), "ran the block %08X", static_cast<unsigned>(pc));
                mark_not_comparable(f, leaf_no, reason);
                break;
            }
        }
    }
    if (f.leaf != leaf_yes) {
        if (synthetic)
            undo_original();
        return not_compared(); // real call: registers and memory are the original's
    }
    if (!f.call_blocks.empty())
        f.call_blocks[0] = 1; // the entry block ran (the original returned); it has no block hook of its own
    const cpu_snapshot r1 = take(c);

    // 3. Back to the starting state, then the decompiled version under the observer (and the journal, for guest code
    // it would call).
    undo_original();
    put(c, r0);
    decompiled_observer observer;
    gc::memory_observer* previous = gc::set_memory_observer(&observer);
    attach_journal(&observer.writes);
    const u64 guest_calls_before = guest_calls;
    s_depth++;
    s_running = &f;
    invoke(registers_of(g));
    s_running = nullptr;
    s_depth--;
    detach_journal();
    gc::set_memory_observer(previous);
    const cpu_snapshot r2 = take(c);
    observer.writes.finish(&read_byte);
    const std::vector<byte_write>& w2 = observer.writes.bytes();
    auto finish = [&]() {
        // 5. Undo the decompiled version; a real call goes on with the original's results.
        for (const byte_write& b : w2)
            write_byte(b.address, b.before);
        if (synthetic) {
            put(c, r0);
            return;
        }
        for (const byte_write& b : w1)
            write_byte(b.address, b.after);
        put(c, r1);
    };
    if (guest_calls != guest_calls_before || f.leaf != leaf_yes) {
        if (f.leaf == leaf_yes)
            mark_not_comparable(f, leaf_no, "the decompiled version called guest code: not comparable as a leaf");
        finish();
        return not_compared();
    }

    // 4. Comparison.
    std::vector<difference> diffs;
    oracle::compare(r1.regs, r2.regs, signature.result_kind, r0.regs.gpr[1], w1, w2, diffs, s_dead_stack);

    // Coverage and results (compared calls only).
    for (std::size_t i = 0; i < f.call_blocks.size(); i++)
        f.block_hits[i] += f.call_blocks[i];
    for (std::size_t i = 0; i < f.call_dead.size(); i++)
        f.dead_block_hits[i] += f.call_dead[i];
    if (f.paths.size() < (1u << 16))
        f.paths.insert(result_key_hash(f.call_blocks));
    if (f.results.size() < 4096)
        f.results.insert(result_of(r1.regs, signature.result_kind));

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
    bool is_new = false;
    if (synthetic)
        f.synthetic_compared++;
    else {
        f.compared++;
        is_new = f.fingerprints.size() < (1u << 20) && f.fingerprints.insert(h).second;
    }
    if (!diffs.empty()) {
        bool new_bad = true;
        if (synthetic)
            f.synthetic_mismatches++;
        else {
            f.mismatches++;
            new_bad = f.bad_fingerprints.insert(h).second;
        }
        if (f.printed < 4 || synthetic) {
            f.printed++;
            std::fprintf(stderr, "gc_oracle: MISMATCH %s (%s %08X)%s%s, entry r3=%08X r4=%08X r5=%08X f1=%016llX:",
                         f.name, s_version, static_cast<unsigned>(f.address), synthetic ? " synthetic " : "",
                         synthetic ? synthetic : "", static_cast<unsigned>(r0.regs.gpr[3]),
                         static_cast<unsigned>(r0.regs.gpr[4]), static_cast<unsigned>(r0.regs.gpr[5]),
                         static_cast<unsigned long long>(r0.regs.fpr[1]));
            for (std::size_t i = 0; i < diffs.size() && i < 6; i++) {
                char text[160];
                oracle::describe(diffs[i], text, sizeof(text));
                std::fprintf(stderr, "%s %s", i ? ";" : "", text);
            }
            std::fprintf(stderr, "%s\n", diffs.size() > 6 ? "; ..." : "");
        }
        if (new_bad && (synthetic || f.bad_fingerprints.size() <= 16))
            print_case("mismatch", f, h, signature.result_kind, r0.regs, observer.reads, r1.regs, w1, &r2.regs, &w2,
                       &diffs, synthetic);
    } else if (synthetic || (is_new && f.stored < s_case_limit)) {
        if (!synthetic)
            f.stored++;
        print_case("case", f, h, signature.result_kind, r0.regs, observer.reads, r1.regs, w1, nullptr, nullptr,
                   nullptr, synthetic);
    }
    finish();
    return true;
}

// Synthetic cases of `f`, from the current state (a real call of f, before it is handled): each one in a scratch area
// of the dead stack, everything put back afterwards.
void run_synthetic(GcrtCpu* g, function_state& f, void (*invoke)(const registers&),
                   const oracle_signature& signature) {
    f.synthetic_done = true;
    if (f.synthetic.empty())
        return;
    CPUState* c = &g->state;
    const cpu_snapshot saved = take(c);
    // Scratch area: 0x800 bytes under the caller's r1 (dead stack), the case's r1 0x100 bytes under it.
    const u32 base = (oracle::canonical_address(saved.regs.gpr[1]) - 0x800u) & ~0xFFu;
    const u32 save_from = base - 0x400u, save_size = 0x800u;
    std::vector<u8> saved_bytes(save_size);
    for (u32 i = 0; i < save_size; i++)
        saved_bytes[i] = read_byte(save_from + i);
    for (const synthetic_case& sc : f.synthetic) {
        for (u32 i = 0; i < sc.area; i++)
            write_byte(base + i, 0);
        for (const auto& m : sc.mem) {
            const u64 v = static_cast<u64>(m.relative ? static_cast<s64>(base) + m.value : m.value);
            for (u32 i = 0; i < m.size; i++)
                write_byte(base + m.offset + i, static_cast<u8>(v >> (8u * (m.size - 1u - i))));
        }
        put(c, saved);
        c->gpr[1] = base - 0x100u;
        for (const auto& r : sc.regs) {
            const u64 v = static_cast<u64>(r.relative ? static_cast<s64>(base) + r.value : r.value);
            if (r.kind == 'r')
                c->gpr[r.index] = static_cast<u32>(v);
            else if (r.kind == 'f')
                std::memcpy(&c->fpr[r.index], &v, sizeof(v));
            else
                std::memcpy(&c->ps1[r.index], &v, sizeof(v));
        }
        compare_call(g, f, invoke, signature, sc.label.c_str());
        for (u32 i = 0; i < save_size; i++)
            write_byte(save_from + i, saved_bytes[i]);
        put(c, saved);
        if (f.leaf != leaf_yes)
            break;
    }
}

void initialize() {
    s_initialized = true;
    if (const char* n = std::getenv("GCRT_DECOMP_ORACLE_CASES"); n && n[0])
        s_case_limit = std::strtoull(n, nullptr, 0);
    if (const char* n = std::getenv("GCRT_DECOMP_ORACLE_DEAD_STACK"); n && n[0])
        s_dead_stack = static_cast<u32>(std::strtoul(n, nullptr, 0));
    if (const char* dir = std::getenv("GCRT_DECOMP_ORACLE_SYNTHETIC"); dir && dir[0])
        load_synthetic(dir);
    for (const std::string& e : s_synthetic_errors)
        std::fprintf(stderr, "gc_oracle: synthetic cases: %s\n", e.c_str());
    if (const char* dir = std::getenv("GCRT_DECOMP_ORACLE_OUT"); dir && dir[0]) {
        char name[96];
        std::snprintf(name, sizeof(name), "/oracle-%s-%d-%lld.jsonl", s_version, static_cast<int>(GC_ORACLE_GETPID()),
                      static_cast<long long>(std::time(nullptr)));
        s_out_path = std::string(dir) + name;
        s_out = std::fopen(s_out_path.c_str(), "w");
        if (!s_out)
            std::fprintf(stderr, "gc_oracle: cannot write %s: no report file\n", s_out_path.c_str());
        else {
            std::fprintf(s_out,
                         "{\"kind\":\"header\",\"version\":\"%s\",\"functions\":%zu,\"negative\":%s,\"sources\":\"%s\","
                         "\"dead_stack\":\"%08X\",\"synthetic_cases\":%u,\"synthetic_errors\":%zu}\n",
                         s_version, s_functions.size(), negative_build ? "true" : "false", s_sources_hash,
                         static_cast<unsigned>(s_dead_stack), s_synthetic_loaded, s_synthetic_errors.size());
            std::fflush(s_out);
        }
    }
    std::fprintf(stderr, "gc_oracle: %zu decompiled functions compared with their recompiled originals (%s), %u "
                         "synthetic cases%s%s\n",
                 s_functions.size(), s_version, s_synthetic_loaded, s_out ? ", report " : "",
                 s_out ? s_out_path.c_str() : "");
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
    if (f->leaf == leaf_yes && !f->synthetic_done)
        run_synthetic(g, *f, invoke, signature);
    if (f->leaf != leaf_yes) {
        // Not comparable: the original runs alone (the game keeps its results).
        f->not_compared++;
        bypass_address = address;
        run_guest(g, address);
        bypass_address = 0;
        return true;
    }
    compare_call(g, *f, invoke, signature, nullptr);
    return true; // registers and memory are the original's
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
// gcrt_decomp_install_table: the version, the size and the basic blocks of each function (leaf check, block
// coverage), and the hash of the sources the executable was built from (header of the report file). A hook declining
// at once is registered at the start of every block but the entry (compiled by DolRecomp, tools/build.py).
extern "C" void gcrt_decomp_oracle_configure(GcrtCpu* g, const char* version, const unsigned* addresses,
                                             const unsigned* sizes, const char* const* names, unsigned count,
                                             const unsigned* block_first, const unsigned* blocks,
                                             const char* sources_hash) {
    using namespace gc::abi::detail;
    s_version = version;
    s_sources_hash = sources_hash ? sources_hash : "";
    s_functions.clear();
    s_functions.resize(count);
    s_probes.clear();
    for (unsigned i = 0; i < count; i++) {
        function_state& f = s_functions[i];
        f.address = addresses[i];
        f.size = sizes[i];
        f.name = names[i];
        for (unsigned k = block_first[i]; k < block_first[i + 1]; k++) {
            const u32 b = blocks[k];
            const bool dead = b & 1u;
            const u32 start = b & ~1u;
            if (dead) {
                s_probes[start] = {i, 0x80000000u | static_cast<u32>(f.dead_blocks.size())};
                f.dead_blocks.push_back(start);
            } else {
                if (start != f.address)
                    s_probes[start] = {i, static_cast<u32>(f.blocks.size())};
                f.blocks.push_back(start);
            }
        }
        if (f.blocks.empty() || f.blocks[0] != f.address)
            f.blocks.insert(f.blocks.begin(), f.address);
        f.block_hits.assign(f.blocks.size(), 0);
        f.dead_block_hits.assign(f.dead_blocks.size(), 0);
        f.call_blocks.assign(f.blocks.size(), 0);
        f.call_dead.assign(f.dead_blocks.size(), 0);
    }
    for (const auto& [start, where] : s_probes)
        gcrt_hle_register(g, start, "oracle-block", &block_hook);
}
