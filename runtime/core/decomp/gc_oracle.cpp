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
//      stack under r1, GCRT_DECOMP_ORACLE_DEAD_STACK bytes, default 64 KiB; nor, for a function with call sites, the
//      original's store of its entry LR into the LR save word 4(r1), lr_save_rule); FPSCR is not compared (see
//      gc_oracle_compare.h). Then the side effects of the stores on the CPU layer: the lwarx reservation after each
//      version, and every byte the decompiled code stored must have been told to the write journal (the store hook
//      of gc_mem.h, installed by gc_abi.cpp), as every store of the original is;
//   5. W2 is undone, W1 and R1 put back: the game goes on with the ORIGINAL's results, the oracle changes nothing
//      in the game's behavior (it only runs both versions);
//   6. the starting state (argument registers and the bytes the decompiled version read) is fingerprinted; distinct
//      ones are counted and the first ones stored as replayable cases (oracle_replay.cpp, tools/decomp_oracle.py);
//      the distinct results of the original (r3, r3:r4 or the bits of f1) and the distinct sets of blocks run are
//      counted too. This is basic-block coverage (not edge or path coverage): which blocks ran, not which edges
//      between them nor which sequences of blocks.
// Functions that call others are compared with their calls recorded during the original and replayed to the
// decompiled version ("Calls" below: target, arguments and memory at each call compared). Not compared (they run as
// their original): a transfer out of the function the build did not hook, a store the journal does not see (dcbz,
// dcbz_l), a thread switch refused during the nested run of the original.
//
// Write journal: another part of the runtime may have attached its own journal (hle/native_os.c, idle detection of a
// looping OSYieldThread). The oracle's journal goes on calling it, so that it sees the original's stores as in a
// normal build; when that journal detaches or replaces itself during a run (the idle detection does so on its first
// store outside the thread's stack), the oracle stays attached and only remembers the change, applied when the run
// ends: W1 is always complete. GCRT_DECOMP_ORACLE_TEST_DETACHING_JOURNAL=1 (test only, refused by the promotion)
// attaches, when no other journal is there, a journal that detaches itself on its first store as the idle detection
// does, to exercise that case on every call; =2 also turns the oracle's re-attachment off, to show what it prevents.
// GCRT_DECOMP_ORACLE_TEST_NO_STORE_HOOK=1 (test only, refused by the promotion) removes the store hook of gc_mem.h
// (gc_abi.cpp): every function whose decompiled version stores must then show an "unjournaled" difference, and a
// "reservation" one when a synthetic case holds a reservation in the stored block.
//
// Synthetic cases (GCRT_DECOMP_ORACLE_SYNTHETIC=<folder of .txt files>, format below): starting states written by
// hand to reach branches the scenarios do not reach. At the first comparable call of F, each case of F is injected
// into a scratch area of the dead stack (under the caller's r1; [hypothesis] no live data there and nothing another
// thread reads during the call, notes/decomp.md D1.3), both versions run from it as above, and everything is put
// back afterwards (registers, scratch area, W1, W2): the game does not see them. For a synthetic case the dead stack
// that is not compared is only the saved part of the scratch area under the case's r1 (synthetic_dead_stack bytes,
// not the 64 KiB of a real call): a store anywhere else, even through a pointer argument that points a few KiB
// under S, is compared. They count apart from the real calls (synthetic_* in the summary) and are written as
// replayable cases with their label. A case of a function this executable does not register (a disabled row, not
// built) is checked for syntax and skipped: counted in the header (synthetic_skipped, synthetic_skipped_functions),
// and tools/decomp_oracle.py decides whether that name may be missing.
//
// FPU off: for a function whose original holds a floating-point instruction, the gateway declines its entry while
// MSR[FP] = 0 (gc_abi.h registration_uses_fpu) before reaching the oracle: the original runs as in the game, "FPU
// unavailable" exception included, and the call is counted as fpu_declined, neither compared nor not compared.
//   case <function> <label>     starts a case (one function, any label without spaces)
//   area <bytes>                size of the scratch area (default 0x100, at most 0x400), zero-filled first
//   rN = <value>                gpr N; fN = <hex bits> (fpr N), pN = <hex bits> (ps1 of fpr N)
//   mem <address> <1|2|4> <value>   big-endian store into the area before the run
//   reserve <value>             a lwarx reservation is valid at that address when both versions start
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
// Every setting that changes what is compared or stored (dead stack, stored cases, cross-check, test mode of the
// journal, content of the synthetic cases) is written in the header, with a run identifier; tools/decomp_oracle.py
// refuses to promote results whose settings are not the defaults, and counts one run only once.
#include "decomp/gc_abi.h"
#include "decomp/gc_float.h"
#include "decomp/gc_oracle_branch.h"
#include "decomp/gc_oracle_compare.h"

// C headers without their own extern "C" (DolRecomp's cpu.h): the write journal is a C symbol.
extern "C" {
#include "cpu/gcrt_cpu.h"
#include "hle/gcrt_hle.h"
}
#include "hle/gcrt_native.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <chrono>
#include <fstream>
#include <iterator>
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

// leaf_yes: no call; leaf_calls: calls other functions, compared with its calls recorded and replayed (see "Calls"
// at the top); leaf_no: not comparable (a transfer out of the function the oracle did not record); leaf_unsupported:
// not comparable (a store the journal does not see, a store outside RAM, a refused thread switch).
enum leaf_state : u8 { leaf_unknown, leaf_yes, leaf_calls, leaf_no, leaf_unsupported };
bool comparable(leaf_state s) {
    return s == leaf_yes || s == leaf_calls;
}

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
    // `reserve <value>`: a lwarx reservation valid at that address when both versions start (default: none, as the
    // scenarios almost always call a function).
    bool reserve = false;
    bool reserve_relative = false;
    s64 reserve_value = 0;
    // `call [rN = <value>] [fN = <hex bits>] [pN = <hex bits>] ...`: the results of the next call the original makes
    // (the callee is never run in a synthetic case: the oracle simulates it); registers not listed keep the values
    // they had at the call. One line per call, in order.
    std::vector<std::vector<reg_set>> calls;
};

// One call the original of the function being compared made (see "Calls" at the top).
struct call_record {
    u32 site = 0, target = 0;
    bool tail = false;
    oracle::reg_state before{}, after{}; // registers at the call and after the callee
    bool reserve_valid_after = false;
    u32 reserve_addr_after = 0;
    std::vector<byte_write> written_before; // bytes the function wrote before the call (current values), not the dead stack
    std::vector<byte_write> callee_writes;  // bytes the callee wrote (value before, value after)
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
    // Entries the gateway declined because MSR[FP] = 0 (gc_abi.h registration_uses_fpu): the original ran in the game
    // as in a build without this function, "FPU unavailable" exception included. Neither compared nor "not compared":
    // nothing was asked of the decompiled version.
    u64 fpu_declined = 0;
    // Compared calls (real and synthetic) during which the decompiled version ran an fmadds under NI = 1 whose result
    // Dolphin's interpreter would flush to zero and DolRecomp's ppc_fma does not (gc_float.h): information only, the
    // reference of the comparison is the recompiled original.
    u64 fmadds_dolphin_divergent = 0;
    u64 stored = 0;        // cases written
    u64 printed = 0;       // differences printed on stderr
    std::unordered_set<u64> fingerprints;     // distinct starting states
    std::unordered_set<u64> bad_fingerprints; // distinct starting states with a difference
    // Basic blocks (tools/build.py): starts of the reachable ones (blocks[0] = entry), and of the unreachable ones.
    std::vector<u32> blocks, dead_blocks;
    std::vector<u8> assert_block; // blocks[i] only leads to a function that never returns (tools/build.py)
    std::vector<u64> block_hits, dead_block_hits; // compared calls (real and synthetic) that ran each block
    std::vector<u64> synthetic_block_hits;        // synthetic cases that ran each block (assertion blocks: listed)
    std::vector<u32> call_sites;                  // instructions that call or leave the function (sorted)
    u64 recorded_calls = 0, max_calls = 0;        // calls recorded during compared runs of the original
    u64 refused_switches = 0;                     // thread switches refused during a nested run of the original
    std::vector<u8> call_blocks, call_dead;       // blocks run by the current call of the original
    std::unordered_set<u64> paths;                 // distinct sets of blocks run (not edges, not paths)
    std::set<std::pair<u64, u64>> results;         // distinct results of the original
    // Synthetic cases.
    std::vector<synthetic_case> synthetic;
    bool synthetic_done = false;
    u64 synthetic_compared = 0, synthetic_mismatches = 0, synthetic_not_compared = 0;
    // Runs of the original during which another journal detached or replaced itself (the oracle stayed attached),
    // and self-checks of the undo and redo of W1 that failed (must stay 0).
    u64 journal_reattached = 0, restore_errors = 0;
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
// FNV-1a 64 of the synthetic files loaded, in load order: name, 0, content with CRLF read as LF, 0 (the same as
// synthetic_content_hash of tools/decomp_oracle.py over the current files).
u64 s_synthetic_content = oracle::fnv_offset;
std::vector<std::string> s_synthetic_errors;
// Cases of a function this executable does not register (a disabled row of the game's table, not built): skipped and
// counted apart, by function. tools/decomp_oracle.py checks that every such name is a disabled row of the table (any
// other name, an enabled row that was not built or a misspelled one, refuses the promotion).
std::vector<std::pair<std::string, unsigned>> s_synthetic_skipped;
unsigned s_synthetic_skipped_count = 0;
u64 s_case_limit = 64;
u32 s_dead_stack = oracle::default_dead_stack;
// Synthetic cases: the case's r1 is 0x100 under S and the saved scratch area starts 0x400 under S, so the frame of
// the function may use 0x300 bytes under its r1; below that, every store is compared.
constexpr u32 synthetic_dead_stack = 0x300u;
unsigned s_test_detaching_journal = 0; // GCRT_DECOMP_ORACLE_TEST_DETACHING_JOURNAL (test only)
unsigned s_test_no_store_hook = 0;     // GCRT_DECOMP_ORACLE_TEST_NO_STORE_HOOK (test only)
unsigned s_depth = 0;                // > 0 while a comparison runs
function_state* s_running = nullptr; // function being compared
bool s_in_original = false;          // the original of s_running is running (block hooks record)
bool s_initialized = false;

// Calls (see "Calls" at the top). Call instructions of the registered functions -> function index.
std::unordered_map<u32, u32> s_call_probes;
std::vector<call_record> s_calls;    // calls of the current run of the original
bool s_call_active = false;          // a recorded callee is running (nested run): hooks ignore what it does
bool s_replaying = false;            // the decompiled version runs: its calls are compared and replayed
std::size_t s_replay_index = 0;      // next recorded call to replay
const synthetic_case* s_synthetic_case = nullptr; // synthetic case being run (its calls are simulated)
bool s_synthetic_call_missing = false;            // the original made more calls than the case has `call` lines
oracle::write_set* s_callee_writes = nullptr;     // journal segment of the running callee
std::vector<difference> s_call_diffs;             // differences found at the calls of the decompiled version
u32 s_entry_r1 = 0, s_compare_dead_stack = 0;     // r1 at the entry of the compared run and its dead stack
// LR the original of the compared run was entered with (the nested run's return address, oracle_bypassed_entry; 0
// until its entry hook declined): the value its prologue saves in the LR save word (gc_oracle_compare.h).
u32 s_entry_lr = 0;
bool s_entry_lr_wanted = false; // the next bypassed entry is the compared original's
std::unordered_map<u32, u32> s_frame_helpers;     // call target -> end of a register save/restore helper (0: not one)

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
// The journal another part of the runtime may have attached (hle/native_os.c, idle detection) keeps being called; if
// it detaches or replaces itself meanwhile (ppc_set_mem_write_journal from inside it), the oracle's journal is put
// back at once and the change is kept for detach_journal (see "Write journal" at the top).
oracle::write_set s_original_writes;
oracle::write_set* s_journal_target = &s_original_writes;
// While the decompiled version runs: every store the write journal is told of (gc_mem.h store hook, gc_abi.cpp), so
// that a store of decompiled code the journal never saw (the idle detection would miss it) is a difference.
oracle::write_set* s_journal_seen = nullptr;
PPCMemWriteJournal s_previous_journal = nullptr;
void* s_previous_user = nullptr;
u64 s_reattached = 0; // re-attachments during the current run

// Test journal (GCRT_DECOMP_ORACLE_TEST_DETACHING_JOURNAL): detaches itself on its first store, as the idle detection
// of hle/native_os.c (spin_journal) does on its first store outside the thread's stack.
void test_detaching_journal(u32 /*offset*/, u32 /*size*/, void* /*user*/) {
    ppc_set_mem_write_journal(nullptr, nullptr);
}

void journal(u32 offset, u32 size, void* /*user*/) {
    if (s_previous_journal) {
        s_previous_journal(offset, size, s_previous_user);
        if (g_mem_write_journal != &journal && s_test_detaching_journal != 2) {
            s_previous_journal = g_mem_write_journal;
            s_previous_user = g_mem_write_journal_user;
            ppc_set_mem_write_journal(&journal, nullptr);
            s_reattached++;
        }
    }
    u64 old_value = 0;
    for (u32 i = 0; i < size && i < 8; i++)
        old_value = (old_value << 8) | (offset + i < s_state->ram_size ? s_state->ram[offset + i] : 0u);
    s_journal_target->add(0x80000000u + offset, size < 8 ? size : 8, old_value);
    if (s_callee_writes)
        s_callee_writes->add(0x80000000u + offset, size < 8 ? size : 8, old_value);
    if (s_journal_seen)
        s_journal_seen->add(0x80000000u + offset, size < 8 ? size : 8, old_value);
}

// `test_previous`: the original's run, where the test journal (if asked for) stands in for an absent one.
void attach_journal(oracle::write_set* target, bool test_previous = false) {
    s_journal_target = target;
    s_previous_journal = g_mem_write_journal;
    s_previous_user = g_mem_write_journal_user;
    if (test_previous && s_test_detaching_journal && !s_previous_journal)
        s_previous_journal = &test_detaching_journal;
    s_reattached = 0;
    ppc_set_mem_write_journal(&journal, nullptr);
}
void detach_journal() {
    // The test journal stands in only during the original's run: never left attached to the game.
    if (s_previous_journal == &test_detaching_journal)
        s_previous_journal = nullptr;
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
    oracle::write_set writes;   // everything to undo: the observed stores and the journal's (guest code it calls)
    oracle::write_set observed; // the stores of the decompiled code itself
    std::vector<read_record> reads;
    void on_read(std::uint32_t address, std::uint32_t size, std::uint64_t value) override {
        reads.push_back(read_record{oracle::canonical_address(address), size, value});
    }
    void on_write(std::uint32_t address, std::uint32_t size, std::uint64_t old_value, std::uint64_t) override {
        writes.add(address, size, old_value);
        observed.add(address, size, old_value);
    }
};

decompiled_observer* s_observer = nullptr; // observer of the decompiled run being compared (replay_call)

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
            // Every such instruction is a call site of the build (tools/decomp_functions.py call_sites), hooked: the
            // calls are recorded and replayed. One the build did not list cannot be followed.
            if (!std::binary_search(f.call_sites.begin(), f.call_sites.end(), a)) {
                f.leaf = leaf_no;
                std::snprintf(f.leaf_reason, sizeof(f.leaf_reason), "%s at %08X: not a hooked call site", reason,
                              static_cast<unsigned>(a));
                return;
            }
            f.leaf = leaf_calls;
        }
    }
    if (f.leaf != leaf_calls)
        f.leaf = leaf_yes;
}

u32 read_word(u32 a) {
    return (u32{read_byte(a)} << 24) | (u32{read_byte(a + 1)} << 16) | (u32{read_byte(a + 2)} << 8) | read_byte(a + 3);
}

// End (address after its blr) of the register save/restore helper of the ABI at `target`, 0 if `target` is not one
// (gc_oracle_branch.h register_helper_end), cached: check_history also accepts the blocks of the helpers seen.
u32 frame_helper_end(u32 target) {
    if (const auto it = s_frame_helpers.find(target); it != s_frame_helpers.end())
        return it->second;
    const u32 end = oracle::register_helper_end(target, &read_word);
    s_frame_helpers[target] = end;
    return end;
}

// Bytes of `writes` (sorted, current values read now) outside the dead stack of the compared run (the LR save word
// included: replay_call applies its rule).
std::vector<byte_write> live_writes(oracle::write_set writes) {
    writes.finish(&read_byte);
    std::vector<byte_write> out;
    for (const byte_write& b : writes.bytes()) {
        if (!oracle::detail::in_dead_stack(b.address, s_entry_r1, s_compare_dead_stack))
            out.push_back(b);
    }
    return out;
}

// --- calls of the original -----------------------------------------------------------------------------------------
//
// Calls. The oracle build compiles a hook at every instruction of a decompiled function F that calls another function
// or leaves for one (tools/decomp_functions.py call_sites). While the original of F runs under comparison, the hook
// at such an instruction (if the branch is taken, BO/BI/CTR evaluated as the instruction would) records the call: its
// target (immediate, ctr or lr), the registers, and the bytes F wrote so far (outside the dead stack); then the callee
// runs ONCE, for real, in a nested run of the chassis with a journal segment of its own (its stores), and the hook
// resumes F after the instruction (or, for a tail branch, at F's return address). Branches to the register
// save/restore helpers of the ABI (frame_helper_end), with link or not, run inline, unrecorded. How the instruction is
// read: gc_oracle_branch.h (tested on hand-written words, tests/decomp/test_call_sites.py). In a synthetic case the
// callee never runs: its results are the case's `call` lines. While the decompiled version runs, each of its calls
// (run_guest: call_guest, call_address, gc::abi::call) is not executed: replay_call compares it with the next
// recorded call (target, the argument registers of its C++ type, the bytes written so far) and replays the callee
// (its stores, then the volatile registers it left). [hypothesis, code read] The callee sees lr = the nested run's
// return address (saved in its own frame, dead stack); a native replacement that stores through the host directly,
// not through cpu.c, is not replayed; thread switches are refused during a nested run (counted: the function becomes
// not comparable).
// Known defect, oracle executable only [verified on a game function]: a callee, or the original of F itself (also
// run nested), that waits for another thread or an interrupt never returns from its nested run (block budget of the
// chassis), and the game stops there instead of F becoming not comparable; a refused switch is only seen once the
// nested run returned, after a run made in altered conditions. Production builds have no nested run of this kind.

void set_regs(CPUState* c, const std::vector<synthetic_case::reg_set>& regs, u32 base) {
    for (const auto& r : regs) {
        const u64 v = static_cast<u64>(r.relative ? static_cast<s64>(base) + r.value : r.value);
        if (r.kind == 'r')
            c->gpr[r.index] = static_cast<u32>(v);
        else if (r.kind == 'f')
            std::memcpy(&c->fpr[r.index], &v, sizeof(v));
        else
            std::memcpy(&c->ps1[r.index], &v, sizeof(v));
    }
}
u32 s_synthetic_base = 0; // S of the running synthetic case
u32 s_ring_checked = 0;    // chassis history checked up to there (blocks of recorded callees are not F's)

void mark_not_comparable(function_state& f, leaf_state state, const char* reason);

// Blocks the chassis ran in [from, to) of its history, outside recorded calls: each must be in F or in a register
// save/restore helper; anything else is a transfer out of F that the oracle did not record (F not comparable). Checked
// before each recorded call and at the end, so a long callee does not overflow the history.
void check_history(GcrtCpu* g, function_state& f, u32 from, u32 to) {
    if (!comparable(f.leaf))
        return;
    if (to - from > g->ring_mask + 1u) {
        char reason[112];
        std::snprintf(reason, sizeof(reason), "more blocks than the history holds (%u)", static_cast<unsigned>(to - from));
        mark_not_comparable(f, leaf_no, reason);
        return;
    }
    for (u32 i = from; i != to; i++) {
        const u32 pc = g->ring[i & g->ring_mask];
        if (pc >= f.address && pc < f.address + f.size)
            continue;
        bool helper = false;
        for (const auto& [start, end] : s_frame_helpers)
            helper |= end && pc >= start && pc < end;
        if (!helper) {
            char reason[112];
            std::snprintf(reason, sizeof(reason), "ran the block %08X", static_cast<unsigned>(pc));
            mark_not_comparable(f, leaf_no, reason);
            return;
        }
    }
}

cpu_snapshot take(const CPUState* c);

bool call_hook(GcrtCpu* g, u32 address, function_state& f) {
    if (s_running != &f || !s_in_original || s_call_active)
        return false;
    CPUState* c = &g->state;
    const oracle::branch_decision d =
        oracle::decode_branch(read_word(address), address, f.address, f.size, c->cr, c->ctr, c->lr);
    // Not taken (the instruction falls through, CTR decremented by itself), a branch inside the function, a return,
    // or a register save/restore helper reached by bl or by a tail b (part of the prologue or epilogue): the
    // instruction runs as compiled.
    const bool taken = d.kind == oracle::branch_kind::call || d.kind == oracle::branch_kind::tail;
    if (!taken || !oracle::is_recorded_call(d, frame_helper_end(d.target)))
        return false;
    const u32 target = d.target;
    const bool lk = d.kind == oracle::branch_kind::call;
    const u32 ctr_after = d.ctr_after;
    // The instruction is replaced by the hook: its own side effects first (CTR, LR).
    c->ctr = ctr_after;
    call_record rec;
    rec.site = address;
    rec.target = oracle::canonical_address(target);
    rec.tail = !lk;
    const u32 return_to = lk ? address + 4u : c->lr;
    if (lk)
        c->lr = address + 4u;
    rec.written_before = live_writes(s_original_writes);
    rec.before = take(c).regs;
    if (s_synthetic_case) {
        // Simulated: the callee never runs (a panic, an allocation... could not be undone).
        if (s_calls.size() < s_synthetic_case->calls.size())
            set_regs(c, s_synthetic_case->calls[s_calls.size()], s_synthetic_base);
        else
            s_synthetic_call_missing = true;
    } else {
        oracle::write_set segment;
        s_callee_writes = &segment;
        s_call_active = true;
        const u64 external = s_external_writes;
        check_history(g, f, s_ring_checked, g->ring_pos);
        const u64 refused = gcrt_native_os_refused_nested();
        gcrt_guest_call_prepared(g, target); // restores pc, lr (= return_to for a call) and MSR[EE]
        f.refused_switches += gcrt_native_os_refused_nested() - refused;
        s_ring_checked = g->ring_pos;
        s_external_writes = external; // the callee's stores outside RAM are not F's: they stay done
        s_call_active = false;
        s_callee_writes = nullptr;
        segment.finish(&read_byte);
        rec.callee_writes = segment.bytes();
    }
    rec.after = take(c).regs;
    rec.reserve_valid_after = c->reserve_valid;
    rec.reserve_addr_after = c->reserve_addr;
    s_calls.push_back(std::move(rec));
    c->pc = return_to;
    return true;
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
// Also the hook of every call instruction (call_hook), which may start a block too: the block is recorded first.
bool block_hook(GcrtCpu* g, u32 address) {
    if (!s_running || !s_in_original || s_call_active)
        return false;
    const auto it = s_probes.find(address);
    if (it != s_probes.end() && &s_functions[it->second.first] == s_running) {
        const u32 index = it->second.second;
        if (index & 0x80000000u)
            s_running->call_dead[index & 0x7FFFFFFFu] = 1;
        else
            s_running->call_blocks[index] = 1;
    }
    const auto call = s_call_probes.find(address);
    if (call != s_call_probes.end() && &s_functions[call->second] == s_running)
        return call_hook(g, address, *s_running);
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
                const char* synthetic, u32 dead_stack) {
    if (!s_out)
        return;
    std::fprintf(s_out, "{\"kind\":\"%s\",\"function\":\"%s\",\"version\":\"%s\",\"address\":\"%08X\",\"size\":%u,"
                        "\"result_kind\":%u,\"fingerprint\":\"%016llX\",\"dead_stack\":\"%08X\",\"call_sites\":%zu,"
                        "\"original_entry_lr\":\"%08X\",",
                 kind, f.name, s_version, static_cast<unsigned>(f.address), static_cast<unsigned>(f.size),
                 static_cast<unsigned>(result_kind), static_cast<unsigned long long>(fingerprint),
                 static_cast<unsigned>(dead_stack), f.call_sites.size(), static_cast<unsigned>(s_entry_lr));
    if (synthetic)
        std::fprintf(s_out, "\"synthetic\":\"%s\",", synthetic);
    print_block_list(s_out, "blocks", f.blocks, &f.call_blocks);
    // Calls of the original: [site, target, r3 after, r4 after, f1 bits after, bytes the callee wrote].
    std::fprintf(s_out, ",\"calls\":[");
    for (std::size_t i = 0; i < s_calls.size(); i++)
        std::fprintf(s_out, "%s[\"%08X\",\"%08X\",\"%08X\",\"%08X\",\"%016llX\",%zu]", i ? "," : "",
                     static_cast<unsigned>(s_calls[i].site), static_cast<unsigned>(s_calls[i].target),
                     static_cast<unsigned>(s_calls[i].after.gpr[3]), static_cast<unsigned>(s_calls[i].after.gpr[4]),
                     static_cast<unsigned long long>(s_calls[i].after.fpr[1]), s_calls[i].callee_writes.size());
    std::fputc(']', s_out);
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
        const char* leaf = f.leaf == leaf_yes            ? "yes"
                           : f.leaf == leaf_calls       ? "calls"
                           : f.leaf == leaf_no          ? "no"
                           : f.leaf == leaf_unsupported ? "unsupported"
                                                        : "unknown";
        u64 covered = 0;
        for (u64 n : f.block_hits)
            covered += n != 0;
        std::fprintf(stdout,
                     "ORACLE %-32s %s %08X calls=%llu compared=%llu mismatches=%llu cases=%zu bad_cases=%zu "
                     "stored=%llu not_compared=%llu leaf=%s blocks=%llu/%zu results=%zu synthetic=%llu/%zu "
                     "synthetic_mismatches=%llu journal_reattached=%llu restore_errors=%llu fpu_declined=%llu "
                     "fmadds_dolphin_divergent=%llu%s%s\n",
                     f.name, s_version, static_cast<unsigned>(f.address), static_cast<unsigned long long>(f.calls),
                     static_cast<unsigned long long>(f.compared), static_cast<unsigned long long>(f.mismatches),
                     f.fingerprints.size(), f.bad_fingerprints.size(), static_cast<unsigned long long>(f.stored),
                     static_cast<unsigned long long>(f.not_compared), leaf, static_cast<unsigned long long>(covered),
                     f.blocks.size(), f.results.size(), static_cast<unsigned long long>(f.synthetic_compared),
                     f.synthetic.size(), static_cast<unsigned long long>(f.synthetic_mismatches),
                     static_cast<unsigned long long>(f.journal_reattached),
                     static_cast<unsigned long long>(f.restore_errors),
                     static_cast<unsigned long long>(f.fpu_declined),
                     static_cast<unsigned long long>(f.fmadds_dolphin_divergent), f.leaf_reason[0] ? " reason=" : "",
                     f.leaf_reason);
        if (!s_out)
            continue;
        std::fprintf(s_out,
                     "{\"kind\":\"summary\",\"function\":\"%s\",\"version\":\"%s\",\"address\":\"%08X\","
                     "\"size\":%u,\"calls\":%llu,\"compared\":%llu,\"mismatches\":%llu,\"cases\":%zu,"
                     "\"bad_cases\":%zu,\"stored\":%llu,\"not_compared\":%llu,\"leaf\":\"%s\",\"reason\":\"%s\","
                     "\"synthetic_cases\":%zu,\"synthetic_compared\":%llu,\"synthetic_mismatches\":%llu,"
                     "\"synthetic_not_compared\":%llu,\"journal_reattached\":%llu,\"restore_errors\":%llu,"
                     "\"fpu_declined\":%llu,\"fmadds_dolphin_divergent\":%llu,\"paths\":%zu,"
                     "\"recorded_calls\":%llu,\"max_calls\":%llu,\"refused_switches\":%llu,",
                     f.name, s_version, static_cast<unsigned>(f.address), static_cast<unsigned>(f.size),
                     static_cast<unsigned long long>(f.calls), static_cast<unsigned long long>(f.compared),
                     static_cast<unsigned long long>(f.mismatches), f.fingerprints.size(), f.bad_fingerprints.size(),
                     static_cast<unsigned long long>(f.stored), static_cast<unsigned long long>(f.not_compared), leaf,
                     f.leaf_reason, f.synthetic.size(), static_cast<unsigned long long>(f.synthetic_compared),
                     static_cast<unsigned long long>(f.synthetic_mismatches),
                     static_cast<unsigned long long>(f.synthetic_not_compared),
                     static_cast<unsigned long long>(f.journal_reattached),
                     static_cast<unsigned long long>(f.restore_errors),
                     static_cast<unsigned long long>(f.fpu_declined),
                     static_cast<unsigned long long>(f.fmadds_dolphin_divergent), f.paths.size(),
                     static_cast<unsigned long long>(f.recorded_calls), static_cast<unsigned long long>(f.max_calls),
                     static_cast<unsigned long long>(f.refused_switches));
        print_block_list(s_out, "blocks", f.blocks, nullptr);
        print_u64_list(s_out, "block_hits", f.block_hits);
        print_u64_list(s_out, "synthetic_block_hits", f.synthetic_block_hits);
        std::fputc(',', s_out);
        print_block_list(s_out, "assert_blocks", f.blocks, &f.assert_block);
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
    std::string skipped; // function of the current case when it is not registered in this build
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
            // Not registered: skipped (its lines are still checked), counted apart; the report tool decides whether
            // the name may be absent (a disabled row) or not.
            skipped = f ? std::string() : name;
            if (name.empty())
                error("case without a function");
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
        } else if (tag == "reserve") {
            std::string v;
            ls >> v;
            if (!parse_value(v, c.reserve_relative, c.reserve_value)) {
                error("reserve <value>");
                continue;
            }
            c.reserve = true;
        } else if (tag == "call") {
            // call [rN = v] [fN = bits] [pN = bits] ...: results of the next call (simulated)
            std::vector<synthetic_case::reg_set> regs;
            std::string reg, eq, v;
            bool ok = true;
            while (ls >> reg) {
                synthetic_case::reg_set r{};
                if (!(ls >> eq >> v) || eq != "=" || reg.size() < 2 ||
                    (reg[0] != 'r' && reg[0] != 'f' && reg[0] != 'p')) {
                    ok = false;
                    break;
                }
                r.kind = reg[0];
                r.index = static_cast<unsigned>(std::strtoul(reg.c_str() + 1, nullptr, 10));
                if (r.index >= 32 || (r.kind == 'r' && r.index == 1) || !parse_value(v, r.relative, r.value) ||
                    (r.relative && r.kind != 'r')) {
                    ok = false;
                    break;
                }
                regs.push_back(r);
            }
            if (!ok)
                error("call [rN = <value>] [fN = <hex bits>] [pN = <hex bits>] ...");
            else
                c.calls.push_back(std::move(regs));
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
            } else if (!skipped.empty()) {
                auto it = std::find_if(s_synthetic_skipped.begin(), s_synthetic_skipped.end(),
                                       [&](const auto& s) { return s.first == skipped; });
                if (it == s_synthetic_skipped.end())
                    s_synthetic_skipped.emplace_back(skipped, 1u);
                else
                    it->second++;
                s_synthetic_skipped_count++;
            }
            f = nullptr;
            skipped.clear();
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
    for (const auto& file : files) {
        std::ifstream raw(file, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(raw)), std::istreambuf_iterator<char>());
        std::string normalized;
        normalized.reserve(content.size());
        for (std::size_t i = 0; i < content.size(); i++)
            if (!(content[i] == '\r' && i + 1 < content.size() && content[i + 1] == '\n'))
                normalized.push_back(content[i]);
        const std::string name = file.filename().string();
        const char zero = 0;
        s_synthetic_content = oracle::fnv1a(s_synthetic_content, name.data(), name.size());
        s_synthetic_content = oracle::fnv1a(s_synthetic_content, &zero, 1);
        s_synthetic_content = oracle::fnv1a(s_synthetic_content, normalized.data(), normalized.size());
        s_synthetic_content = oracle::fnv1a(s_synthetic_content, &zero, 1);
        load_synthetic_file(file.string());
    }
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
                  const char* synthetic, u32 dead_stack) {
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
    s_calls.clear();
    s_synthetic_call_missing = false;
    s_entry_r1 = r0.regs.gpr[1];
    s_entry_lr = 0;
    s_entry_lr_wanted = true;
    s_compare_dead_stack = dead_stack;
    const u64 refused_before = f.refused_switches;
    std::fill(f.call_blocks.begin(), f.call_blocks.end(), u8{0});
    std::fill(f.call_dead.begin(), f.call_dead.end(), u8{0});
    attach_journal(&s_original_writes, true);
    s_external_writes = 0;
    s_external_write = c->external_write;
    s_external_write32 = c->external_write32;
    c->external_write = &external_write;
    c->external_write32 = &external_write32;
    s_ring_checked = g->ring_pos;
    s_depth++;
    s_running = &f;
    s_in_original = true;
    bypass_address = f.address;
    const bool returned = run_guest(g, f.address);
    bypass_address = 0;
    s_entry_lr_wanted = false;
    s_in_original = false;
    s_running = nullptr;
    s_depth--;
    c->external_write = s_external_write;
    c->external_write32 = s_external_write32;
    if (s_reattached)
        f.journal_reattached++;
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
    if (f.refused_switches != refused_before) {
        // A thread switch the game would have made was refused in the nested run: the original did not behave as in
        // the game; nothing of this run is evidence.
        mark_not_comparable(f, leaf_unsupported, "a thread switch was refused during the original's nested run");
        if (synthetic)
            undo_original();
        return not_compared();
    }
    if (s_synthetic_call_missing) {
        if (synthetic)
            undo_original();
        std::fprintf(stderr, "gc_oracle: %s synthetic %s: the original made more calls than the case's `call` lines\n",
                     f.name, synthetic ? synthetic : "?");
        return not_compared();
    }
    check_history(g, f, s_ring_checked, g->ring_pos);
    if (!comparable(f.leaf)) {
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
    oracle::write_set journaled;
    gc::memory_observer* previous = gc::set_memory_observer(&observer);
    attach_journal(&observer.writes);
    s_journal_seen = &journaled;
    const u64 guest_calls_before = guest_calls;
    const u64 divergences_before = gc::fp::detail::fmadds_dolphin_divergences;
    s_call_diffs.clear();
    s_replay_index = 0;
    s_observer = &observer;
    s_depth++;
    s_running = &f;
    s_replaying = true;
    invoke(registers_of(g));
    s_replaying = false;
    s_running = nullptr;
    s_depth--;
    s_observer = nullptr;
    if (s_replay_index < s_calls.size())
        s_call_diffs.push_back(difference{"calls-missing", s_calls[s_replay_index].target,
                                          static_cast<u64>(s_calls.size()), static_cast<u64>(s_replay_index)});
    const bool fmadds_divergent = gc::fp::detail::fmadds_dolphin_divergences != divergences_before;
    s_journal_seen = nullptr;
    detach_journal();
    gc::set_memory_observer(previous);
    const cpu_snapshot r2 = take(c);
    observer.writes.finish(&read_byte);
    observer.observed.finish(&read_byte);
    journaled.finish(&read_byte);
    const std::vector<byte_write>& w2 = observer.writes.bytes();
    // Self-check of the undo: a byte both versions wrote had, before the decompiled version, the value it had before
    // the original (both sorted by address).
    for (std::size_t i = 0, j = 0; i < w1.size() && j < w2.size();) {
        if (w1[i].address < w2[j].address)
            i++;
        else if (w2[j].address < w1[i].address)
            j++;
        else {
            if (w1[i].before != w2[j].before) {
                f.restore_errors++;
                break;
            }
            i++, j++;
        }
    }
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
        // Self-check of the redo: the game goes on with exactly the bytes the original wrote.
        for (const byte_write& b : w1)
            if (read_byte(b.address) != b.after) {
                f.restore_errors++;
                break;
            }
    };
    if (guest_calls != guest_calls_before || !comparable(f.leaf)) {
        if (comparable(f.leaf))
            mark_not_comparable(f, leaf_no, "the decompiled version ran guest code outside the replay of its calls");
        finish();
        return not_compared();
    }

    // 4. Comparison.
    std::vector<difference> diffs;
    // The LR save word: the original's store of the LR it was entered with is left out for a function with call sites
    // only (gc_oracle_compare.h lr_save_rule; s_entry_lr = 0 if its entry was not seen: never left out then).
    oracle::compare(r1.regs, r2.regs, signature.result_kind, r0.regs.gpr[1], w1, w2, diffs, dead_stack,
                    oracle::lr_save_rule{!f.call_sites.empty() && s_entry_lr != 0, s_entry_lr});
    // The calls: target, arguments and memory at each call, extra or missing calls (replay_call).
    diffs.insert(diffs.end(), s_call_diffs.begin(), s_call_diffs.end());
    // Side effects of the stores on the CPU layer, which the comparison of the bytes does not see: the lwarx
    // reservation (cleared by a store to the reserved block), and the write journal, which must be told of every store
    // of the decompiled code as it is of every store of the original (both sorted by address).
    if (r1.reserve_valid != r2.reserve_valid)
        diffs.push_back(difference{"reservation", r0.reserve_addr, r1.reserve_valid ? 1u : 0u,
                                   r2.reserve_valid ? 1u : 0u});
    {
        const std::vector<byte_write>& seen = journaled.bytes();
        std::size_t j = 0;
        for (const byte_write& b : observer.observed.bytes()) {
            while (j < seen.size() && seen[j].address < b.address)
                j++;
            if (j == seen.size() || seen[j].address != b.address) {
                diffs.push_back(difference{"unjournaled", b.address, 0, 0});
                break; // one is enough: the store hook is missing or bypassed
            }
        }
    }

    // Coverage and results (compared calls only).
    f.fmadds_dolphin_divergent += fmadds_divergent;
    for (std::size_t i = 0; i < f.call_blocks.size(); i++) {
        f.block_hits[i] += f.call_blocks[i];
        if (synthetic)
            f.synthetic_block_hits[i] += f.call_blocks[i];
    }
    f.recorded_calls += s_calls.size();
    f.max_calls = std::max<u64>(f.max_calls, s_calls.size());
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
    for (const call_record& k : s_calls) {
        // What each callee returned is an input of the rest of the function.
        h = oracle::fnv1a(h, &k.target, 4);
        h = oracle::fnv1a(h, &k.after.gpr[3], 8);
        h = oracle::fnv1a(h, &k.after.fpr[1], 8);
    }
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
                       &diffs, synthetic, dead_stack);
    } else if (synthetic || (is_new && f.stored < s_case_limit)) {
        if (!synthetic)
            f.stored++;
        print_case("case", f, h, signature.result_kind, r0.regs, observer.reads, r1.regs, w1, nullptr, nullptr,
                   nullptr, synthetic, dead_stack);
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
        if (sc.reserve) {
            c->reserve_addr = static_cast<u32>(sc.reserve_relative ? static_cast<s64>(base) + sc.reserve_value
                                                                   : sc.reserve_value);
            c->reserve_valid = true;
        }
        s_synthetic_case = &sc;
        s_synthetic_base = base;
        compare_call(g, f, invoke, signature, sc.label.c_str(), synthetic_dead_stack);
        s_synthetic_case = nullptr;
        for (u32 i = 0; i < save_size; i++)
            write_byte(save_from + i, saved_bytes[i]);
        put(c, saved);
        if (!comparable(f.leaf))
            break;
    }
}

void initialize() {
    s_initialized = true;
    if (const char* n = std::getenv("GCRT_DECOMP_ORACLE_CASES"); n && n[0])
        s_case_limit = std::strtoull(n, nullptr, 0);
    if (const char* n = std::getenv("GCRT_DECOMP_ORACLE_DEAD_STACK"); n && n[0])
        s_dead_stack = static_cast<u32>(std::strtoul(n, nullptr, 0));
    if (const char* n = std::getenv("GCRT_DECOMP_ORACLE_TEST_DETACHING_JOURNAL"); n && n[0])
        s_test_detaching_journal = static_cast<unsigned>(std::strtoul(n, nullptr, 0));
    if (const char* n = std::getenv("GCRT_DECOMP_ORACLE_TEST_NO_STORE_HOOK"); n && n[0])
        s_test_no_store_hook = static_cast<unsigned>(std::strtoul(n, nullptr, 0));
    if (s_test_no_store_hook)
        gc::set_guest_store_hook(nullptr); // the stores of decompiled code bypass the CPU layer, as before mission 8
    if (const char* dir = std::getenv("GCRT_DECOMP_ORACLE_SYNTHETIC"); dir && dir[0])
        load_synthetic(dir);
    for (const std::string& e : s_synthetic_errors)
        std::fprintf(stderr, "gc_oracle: synthetic cases: %s\n", e.c_str());
    if (const char* dir = std::getenv("GCRT_DECOMP_ORACLE_OUT"); dir && dir[0]) {
        char name[96];
        const int pid = static_cast<int>(GC_ORACLE_GETPID());
        const long long started = static_cast<long long>(std::time(nullptr));
        std::snprintf(name, sizeof(name), "/oracle-%s-%d-%lld.jsonl", s_version, pid, started);
        // Run identifier: process, start time and a high-resolution clock reading (two runs never share one, a copy
        // of a result file keeps it).
        char run[80];
        std::snprintf(run, sizeof(run), "%d-%lld-%016llX", pid, started,
                      static_cast<unsigned long long>(
                          std::chrono::high_resolution_clock::now().time_since_epoch().count()));
        s_out_path = std::string(dir) + name;
        s_out = std::fopen(s_out_path.c_str(), "w");
        if (!s_out)
            std::fprintf(stderr, "gc_oracle: cannot write %s: no report file\n", s_out_path.c_str());
        else {
            std::fprintf(s_out,
                         "{\"kind\":\"header\",\"version\":\"%s\",\"run\":\"%s\",\"functions\":%zu,\"negative\":%s,"
                         "\"sources\":\"%s\",\"dead_stack\":\"%08X\",\"synthetic_dead_stack\":\"%08X\","
                         "\"case_limit\":%llu,\"crosscheck\":%s,\"synthetic_cases\":%u,\"synthetic_content\":\"%016llX\","
                         "\"synthetic_errors\":%zu,\"test_detaching_journal\":%u,\"test_no_store_hook\":%u,"
                         "\"synthetic_skipped\":%u,\"synthetic_skipped_functions\":{",
                         s_version, run, s_functions.size(), negative_build ? "true" : "false", s_sources_hash,
                         static_cast<unsigned>(s_dead_stack), static_cast<unsigned>(synthetic_dead_stack),
                         static_cast<unsigned long long>(s_case_limit), crosscheck_enabled ? "true" : "false",
                         s_synthetic_loaded, static_cast<unsigned long long>(s_synthetic_content),
                         s_synthetic_errors.size(), s_test_detaching_journal, s_test_no_store_hook,
                         s_synthetic_skipped_count);
            for (std::size_t i = 0; i < s_synthetic_skipped.size(); i++) {
                // Identifier characters only (the name comes from a case file): anything else is written as '?'.
                std::string name = s_synthetic_skipped[i].first;
                for (char& ch : name)
                    if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_'))
                        ch = '?';
                std::fprintf(s_out, "%s\"%s\":%u", i ? "," : "", name.c_str(), s_synthetic_skipped[i].second);
            }
            std::fprintf(s_out, "}}\n");
            std::fflush(s_out);
        }
    }
    std::fprintf(stderr, "gc_oracle: %zu decompiled functions compared with their recompiled originals (%s), %u "
                         "synthetic cases (%u skipped: function not built)%s%s\n",
                 s_functions.size(), s_version, s_synthetic_loaded, s_synthetic_skipped_count,
                 s_out ? ", report " : "", s_out ? s_out_path.c_str() : "");
    std::atexit(&report_at_exit);
}

} // namespace

bool oracle_entry(GcrtCpu* g, u32 address, void (*invoke)(const registers&), const oracle_signature& signature) {
    if (s_depth) {
        // Inside a recorded callee (its nested run): it runs as its ORIGINAL, as everything the oracle runs for the
        // game (returning false here would run the decompiled version for real: its errors would reach the game and
        // the record of the caller's original, e.g. a wrong variant of the negative test).
        if (s_call_active) {
            const u32 outer_bypass = bypass_address;
            bypass_address = address;
            run_guest(g, address);
            bypass_address = outer_bypass;
            return true;
        }
        // Another decompiled function reached during a comparison otherwise (no recorded call): the function being
        // compared cannot be compared.
        if (s_running && comparable(s_running->leaf)) {
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
    if (comparable(f->leaf) && !f->synthetic_done)
        run_synthetic(g, *f, invoke, signature);
    if (!comparable(f->leaf)) {
        // Not comparable: the original runs alone (the game keeps its results).
        f->not_compared++;
        bypass_address = address;
        run_guest(g, address);
        bypass_address = 0;
        return true;
    }
    compare_call(g, *f, invoke, signature, nullptr, s_dead_stack);
    return true; // registers and memory are the original's
}

bool oracle_is_replaying() noexcept {
    return s_replaying;
}

void oracle_bypassed_entry(u32 lr) noexcept {
    // The first declined entry after compare_call armed it is the compared original's own (its nested run starts at
    // its address); later ones are functions reached inside recorded callees.
    if (s_entry_lr_wanted) {
        s_entry_lr = lr;
        s_entry_lr_wanted = false;
    }
}

// A call of the decompiled version being compared (run_guest): compared with the next call the original made, then
// replayed instead of run (see "Calls" at the top). False when no comparison runs: the call is made as usual.
bool oracle_replay_call(GcrtCpu* g, u32 address, u32 argument_gprs, u32 argument_fprs) noexcept {
    if (!s_replaying || !s_running)
        return false;
    CPUState* c = &g->state;
    const std::size_t index = s_replay_index++;
    const u32 target = oracle::canonical_address(address);
    char what[16];
    if (index >= s_calls.size()) {
        std::snprintf(what, sizeof(what), "call%zu+", index);
        s_call_diffs.push_back(difference{"", target, 0, target});
        std::snprintf(s_call_diffs.back().what, sizeof(what), "%s", what);
        return true; // an extra call: not run (its result registers keep their values)
    }
    const call_record& k = s_calls[index];
    if (k.target != target) {
        std::snprintf(what, sizeof(what), "call%zu", index);
        s_call_diffs.push_back(difference{"", target, k.target, target});
        std::snprintf(s_call_diffs.back().what, sizeof(what), "%s", what);
    }
    for (unsigned r = 3; r <= 10; r++)
        if ((argument_gprs >> r) & 1u && c->gpr[r] != k.before.gpr[r]) {
            std::snprintf(what, sizeof(what), "call%zu.r%u", index, r);
            s_call_diffs.push_back(difference{"", target, k.before.gpr[r], c->gpr[r]});
            std::snprintf(s_call_diffs.back().what, sizeof(what), "%s", what);
        }
    for (unsigned r = 1; r <= 8; r++) {
        u64 bits;
        std::memcpy(&bits, &c->fpr[r], sizeof(bits));
        if ((argument_fprs >> r) & 1u && bits != k.before.fpr[r]) {
            std::snprintf(what, sizeof(what), "call%zu.f%u", index, r);
            s_call_diffs.push_back(difference{"", target, k.before.fpr[r], bits});
            std::snprintf(s_call_diffs.back().what, sizeof(what), "%s", what);
        }
    }
    // Memory at the call: the bytes each side wrote so far (outside the dead stack), same rules as the final
    // comparison (written by one side only = a difference; the LR save word rule, gc_oracle_compare.h: a function
    // with a call site). The callee may read any of them.
    if (s_observer) {
        const std::vector<byte_write> mine = live_writes(s_observer->writes);
        const std::vector<byte_write>& theirs = k.written_before;
        const bool mask_lr =
            oracle::lr_word_masked(oracle::lr_save_rule{s_entry_lr != 0, s_entry_lr}, theirs, s_entry_r1);
        std::size_t i = 0, j = 0;
        while (i < theirs.size() || j < mine.size()) {
            difference d{};
            std::snprintf(d.what, sizeof(d.what), "callmem%zu", index);
            if (j == mine.size() || (i < theirs.size() && theirs[i].address < mine[j].address)) {
                d.address = theirs[i].address, d.original = theirs[i].after, d.decompiled = oracle::not_written;
                i++;
                if (mask_lr && oracle::detail::in_lr_word(d.address, s_entry_r1))
                    continue;
            } else if (i == theirs.size() || mine[j].address < theirs[i].address) {
                d.address = mine[j].address, d.original = oracle::not_written, d.decompiled = mine[j].after;
                j++;
            } else {
                d.address = theirs[i].address, d.original = theirs[i].after, d.decompiled = mine[j].after;
                const bool same = theirs[i].after == mine[j].after;
                i++, j++;
                if (same)
                    continue;
            }
            s_call_diffs.push_back(d);
            break; // the first one tells
        }
        // Replay of the callee: its stores (added to the decompiled side's writes, so they are compared and undone).
        for (const byte_write& b : k.callee_writes) {
            s_observer->writes.add(b.address, 1, read_byte(b.address));
            write_byte(b.address, b.after);
        }
    }
    // Then the registers the callee may change (EABI volatile): r0, r3..r12, f0..f13, CR0, CR1, CR5..CR7, CTR, XER,
    // and the reservation; the preserved ones are left as they are (a callee that changed them shows in the final
    // comparison, since the original's final registers come after it).
    c->gpr[0] = k.after.gpr[0];
    for (unsigned r = 3; r <= 12; r++)
        c->gpr[r] = k.after.gpr[r];
    for (unsigned r = 0; r <= 13; r++) {
        std::memcpy(&c->fpr[r], &k.after.fpr[r], sizeof(u64));
        std::memcpy(&c->ps1[r], &k.after.ps1[r], sizeof(u64));
    }
    c->cr = (c->cr & oracle::preserved_cr_mask) | (k.after.cr & ~oracle::preserved_cr_mask);
    c->ctr = k.after.ctr;
    c->xer = k.after.xer;
    c->reserve_valid = k.reserve_valid_after;
    c->reserve_addr = k.reserve_addr_after;
    return true;
}

void oracle_declined_fpu(u32 address) noexcept {
    if (s_call_active)
        return; // inside a recorded callee: not the compared function's business
    if (s_depth) {
        // Reached during a comparison (in the original's nested run, or by guest code the decompiled version called):
        // the function being compared is not a leaf, as when oracle_entry is reached (the gateway declined first).
        if (s_running && comparable(s_running->leaf)) {
            s_running->leaf = leaf_no;
            std::snprintf(s_running->leaf_reason, sizeof(s_running->leaf_reason),
                          "called the decompiled function at %08X", static_cast<unsigned>(address));
        }
        return;
    }
    if (function_state* f = find_function(address))
        f->fpu_declined++;
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
void store_word_at_entry_sp(u32 offset, u32 value) noexcept {
    // r1 is the entry r1 while a decompiled function runs (the C++ never moves the guest stack pointer). Through
    // gc_mem.h, so that the oracle's observer sees the store as one of the decompiled version's.
    if (GcrtCpu* g = detail::current_cpu())
        gc::store(*gc::ptr_from_guest_address<gc::be<u32>>(g->state.gpr[1] + offset), value);
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
                                             const unsigned* call_first, const unsigned* calls,
                                             const char* sources_hash) {
    using namespace gc::abi::detail;
    s_version = version;
    s_sources_hash = sources_hash ? sources_hash : "";
    s_functions.clear();
    s_functions.resize(count);
    s_probes.clear();
    s_call_probes.clear();
    for (unsigned i = 0; i < count; i++) {
        function_state& f = s_functions[i];
        f.address = addresses[i];
        f.size = sizes[i];
        f.name = names[i];
        for (unsigned k = block_first[i]; k < block_first[i + 1]; k++) {
            const u32 b = blocks[k];
            const bool dead = b & 1u;
            const bool assertion = b & 2u; // only leads to a function that never returns (tools/build.py)
            const u32 start = b & ~3u;
            if (dead) {
                s_probes[start] = {i, 0x80000000u | static_cast<u32>(f.dead_blocks.size())};
                f.dead_blocks.push_back(start);
            } else {
                if (start != f.address)
                    s_probes[start] = {i, static_cast<u32>(f.blocks.size())};
                f.blocks.push_back(start);
                f.assert_block.push_back(assertion ? 1 : 0);
            }
        }
        if (f.blocks.empty() || f.blocks[0] != f.address) {
            f.blocks.insert(f.blocks.begin(), f.address);
            f.assert_block.insert(f.assert_block.begin(), u8{0});
        }
        for (unsigned k = call_first[i]; k < call_first[i + 1]; k++) {
            f.call_sites.push_back(calls[k]);
            s_call_probes[calls[k]] = i;
        }
        std::sort(f.call_sites.begin(), f.call_sites.end());
        f.synthetic_block_hits.assign(f.blocks.size(), 0);
        f.block_hits.assign(f.blocks.size(), 0);
        f.dead_block_hits.assign(f.dead_blocks.size(), 0);
        f.call_blocks.assign(f.blocks.size(), 0);
        f.call_dead.assign(f.dead_blocks.size(), 0);
    }
    for (const auto& [start, where] : s_probes)
        gcrt_hle_register(g, start, "oracle-block", &block_hook);
    for (const auto& [site, where] : s_call_probes)
        if (!s_probes.count(site))
            gcrt_hle_register(g, site, "oracle-call", &block_hook); // the same hook: block, then call
}
