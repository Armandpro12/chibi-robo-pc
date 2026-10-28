// Offline replay of the oracle's captured cases (tools/decomp_oracle.py replay): each case gives a decompiled function's
// entry registers, the bytes it read and the outputs of its recompiled original (registers, bytes written). This small
// executable runs the decompiled function on that state alone (no CPU, no recompiled code: guest RAM is a local
// buffer holding only the case's bytes) and compares it with the original's outputs by the oracle's rules
// (gc_oracle_compare.h). A read outside the case's bytes (and outside what the function wrote itself) is reported:
// the case does not cover that version of the function. Generic: no game here; the functions come from the table that
// tools/decomp_oracle.py generates (gc_replay_records).
//
// Built with GC_MEM_OBSERVER (the observer lists the accesses), without GC_DECOMP_ORACLE.
// Input (text, one case after the other): see tools/decomp_oracle.py case_lines.
#include "decomp/gc_abi.h"
#include "decomp/gc_oracle_compare.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

// The CPU state the gateway reads (gc_abi.h registers): only the registers.
struct GcrtCpu {
    gc::u32 gpr[32];
    gc::f64 fpr[32];
    gc::f64 ps1[32];
    gc::u32 lr, pc;
};

extern const ::gc::abi::export_record* const gc_replay_records[];
extern const unsigned gc_replay_record_count;

namespace {
GcrtCpu* s_cpu = nullptr; // CPU state of the case being replayed
}

namespace gc::abi {
registers registers_of(GcrtCpu* g) noexcept {
    return registers{g->gpr, g->fpr, g->ps1, &g->lr, &g->pc};
}
const export_record* find(u32) noexcept {
    return nullptr;
}
namespace detail {
u32 bypass_address = 0;
bool crosscheck_enabled = false;
void crosscheck(GcrtCpu*, u32, const u32*, const f64*, const f64*) {}
void first_call(u32) {}
bool run_guest(GcrtCpu*, u32 address) noexcept {
    std::printf("ERROR the decompiled function called guest code at %08X: not a leaf, not replayable\n",
                static_cast<unsigned>(address));
    return false;
}
GcrtCpu* current_cpu() noexcept {
    return s_cpu;
}
} // namespace detail
#if defined(GC_DECOMP_ORACLE_NEGATIVE)
// Wrong variants of the oracle's negative test (tools/decomp_oracle.py replay --negative). The replay compares only
// gpr, fpr and ps1 (it keeps the entry's cr): a clobbered cr is visible in the game's oracle only.
namespace testing {
void clobber_gpr(unsigned reg, u32 value) noexcept {
    if (s_cpu && reg < 32)
        s_cpu->gpr[reg] = value;
}
void clobber_cr(u32) noexcept {}
} // namespace testing
#endif
} // namespace gc::abi

namespace {

using gc::oracle::byte_write;
using gc::oracle::reg_state;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 ram_size = 0x04000000u; // covers the RAM and the RELs placed after it
std::unique_ptr<u8[]> s_ram;
std::vector<u32> s_touched; // offsets set by the previous case
std::unordered_set<u32> s_known; // canonical addresses the case gives or the function wrote
std::vector<std::string> s_outside;

u8* byte_at(u32 address) {
    const u32 offset = address & 0x3FFFFFFFu;
    return offset < ram_size ? s_ram.get() + offset : nullptr;
}

struct replay_observer final : gc::memory_observer {
    gc::oracle::write_set writes;
    void on_read(std::uint32_t address, std::uint32_t size, std::uint64_t) override {
        for (u32 i = 0; i < size; i++) {
            const u32 a = gc::oracle::canonical_address(address + i);
            if (!s_known.count(a) && s_outside.size() < 8) {
                char text[64];
                std::snprintf(text, sizeof(text), "%08X", static_cast<unsigned>(a));
                s_outside.push_back(text);
            }
        }
    }
    void on_write(std::uint32_t address, std::uint32_t size, std::uint64_t old_value, std::uint64_t) override {
        writes.add(address, size, old_value);
        for (u32 i = 0; i < size; i++)
            s_known.insert(gc::oracle::canonical_address(address + i));
    }
};

struct replay_case {
    std::string function, fingerprint;
    u32 address = 0;
    unsigned result_kind = 0;
    u32 dead_stack = gc::oracle::default_dead_stack; // size the in-game oracle compared this case with
    reg_state entry{}, original{};
    std::vector<std::pair<u32, std::string>> reads;  // address, value hex (big-endian)
    std::vector<byte_write> original_writes;
};

u64 hex(const std::string& s) {
    return std::strtoull(s.c_str(), nullptr, 16);
}

void read_regs(std::istringstream& in, char what, reg_state& r) {
    std::string w;
    if (what == 'G')
        for (u32& v : r.gpr) {
            in >> w;
            v = static_cast<u32>(hex(w));
        }
    else if (what == 'F')
        for (u64& v : r.fpr) {
            in >> w;
            v = hex(w);
        }
    else if (what == 'P')
        for (u64& v : r.ps1) {
            in >> w;
            v = hex(w);
        }
    else {
        std::string cr, xer, lr, ctr, fpscr;
        in >> cr >> xer >> lr >> ctr >> fpscr;
        r.cr = static_cast<u32>(hex(cr));
        r.xer = static_cast<u32>(hex(xer));
        r.lr = static_cast<u32>(hex(lr));
        r.ctr = static_cast<u32>(hex(ctr));
        r.fpscr = static_cast<u32>(hex(fpscr));
    }
}

bool next_case(std::istream& in, replay_case& c) {
    std::string line;
    c = replay_case{};
    bool started = false;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "case") {
            std::string address;
            std::string dead_stack;
            ls >> c.function >> address >> c.result_kind >> c.fingerprint >> dead_stack;
            c.address = static_cast<u32>(hex(address));
            if (!dead_stack.empty())
                c.dead_stack = static_cast<u32>(hex(dead_stack));
            started = true;
        } else if (tag.size() == 2 && (tag[0] == 'I' || tag[0] == 'O')) {
            read_regs(ls, tag[1], tag[0] == 'I' ? c.entry : c.original);
        } else if (tag == "R") {
            std::string address, value;
            unsigned size = 0;
            ls >> address >> size >> value;
            c.reads.emplace_back(static_cast<u32>(hex(address)), value);
        } else if (tag == "W") {
            std::string address, before, after;
            ls >> address >> before >> after;
            const u32 a = static_cast<u32>(hex(address));
            for (std::size_t i = 0; 2 * i + 1 < before.size(); i++)
                c.original_writes.push_back(byte_write{a + static_cast<u32>(i),
                                                       static_cast<u8>(hex(before.substr(2 * i, 2))),
                                                       static_cast<u8>(hex(after.substr(2 * i, 2)))});
        } else if (tag == "end" && started) {
            return true;
        }
    }
    return false;
}

const gc::abi::export_record* record_named(const std::string& name) {
    for (unsigned i = 0; i < gc_replay_record_count; i++)
        if (name == gc_replay_records[i]->name)
            return gc_replay_records[i];
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: oracle_replay <cases.txt>\n");
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    s_ram = std::make_unique<u8[]>(ram_size);
    std::memset(s_ram.get(), 0, ram_size);
    gc::init_guest_memory(s_ram.get(), ram_size);
    unsigned passed = 0, failed = 0;
    replay_case c;
    while (next_case(in, c)) {
        for (u32 offset : s_touched)
            s_ram[offset] = 0;
        s_touched.clear();
        s_known.clear();
        s_outside.clear();
        // Memory of the case: the bytes the decompiled version read (and, as their old values, the bytes the
        // original wrote: they belong to the starting state too).
        auto put = [&](u32 address, u8 value) {
            if (u8* p = byte_at(address)) {
                *p = value;
                s_touched.push_back(address & 0x3FFFFFFFu);
                s_known.insert(gc::oracle::canonical_address(address));
            }
        };
        for (const auto& [address, value] : c.reads)
            for (std::size_t i = 0; 2 * i + 1 < value.size(); i++)
                put(address + static_cast<u32>(i), static_cast<u8>(hex(value.substr(2 * i, 2))));
        for (const byte_write& b : c.original_writes)
            put(b.address, b.before);
        const gc::abi::export_record* record = record_named(c.function);
        if (!record) {
            std::printf("FAIL %s %s: no decompiled function of that name in this build\n", c.function.c_str(),
                        c.fingerprint.c_str());
            failed++;
            continue;
        }
        GcrtCpu cpu{};
        std::memcpy(cpu.gpr, c.entry.gpr, sizeof(cpu.gpr));
        std::memcpy(cpu.fpr, c.entry.fpr, sizeof(cpu.fpr));
        std::memcpy(cpu.ps1, c.entry.ps1, sizeof(cpu.ps1));
        cpu.lr = c.entry.lr;
        cpu.pc = c.address;
        replay_observer observer;
        gc::set_memory_observer(&observer);
        s_cpu = &cpu;
        record->entry(&cpu, c.address);
        s_cpu = nullptr;
        gc::set_memory_observer(nullptr);
        reg_state after = c.entry; // the replay touches only gpr, fpr, ps1 (cr and the rest stay the entry's)
        std::memcpy(after.gpr, cpu.gpr, sizeof(cpu.gpr));
        std::memcpy(after.fpr, cpu.fpr, sizeof(cpu.fpr));
        std::memcpy(after.ps1, cpu.ps1, sizeof(cpu.ps1));
        observer.writes.finish([](u32 address) -> u8 {
            const u8* p = byte_at(address);
            return p ? *p : 0;
        });
        std::vector<gc::oracle::difference> diffs;
        gc::oracle::compare(c.original, after, static_cast<u8>(c.result_kind), c.entry.gpr[1], c.original_writes,
                            observer.writes.bytes(), diffs, c.dead_stack);
        if (diffs.empty() && s_outside.empty()) {
            passed++;
            std::printf("PASS %s %s\n", c.function.c_str(), c.fingerprint.c_str());
            continue;
        }
        failed++;
        std::printf("FAIL %s %s:", c.function.c_str(), c.fingerprint.c_str());
        for (std::size_t i = 0; i < diffs.size() && i < 6; i++) {
            char text[160];
            gc::oracle::describe(diffs[i], text, sizeof(text));
            std::printf("%s %s", i ? ";" : "", text);
        }
        if (!s_outside.empty()) {
            std::printf("%s read outside the case at", diffs.empty() ? "" : ";");
            for (const std::string& a : s_outside)
                std::printf(" %s", a.c_str());
        }
        std::printf("\n");
    }
    std::printf("REPLAY %u cases: %u pass, %u fail\n", passed + failed, passed, failed);
    return failed ? 1 : 0;
}
