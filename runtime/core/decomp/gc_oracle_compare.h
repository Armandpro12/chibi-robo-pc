// gc_oracle_compare.h: what the decompilation oracle compares between a recompiled original and its decompiled
// version, shared by the in-game oracle (gc_oracle.cpp) and the offline replay of captured cases
// (oracle_replay.cpp). Generic: no game here, no CPU dependency (plain values only).
//
// Rules (stage document, notes/decomp.md D1.3):
//   * result registers, by result kind: r3 (word, and hidden structure pointer), r3:r4 (64-bit), f1 as bits
//     (ps0 and ps1 for f32, ps0 for f64);
//   * registers the PowerPC EABI preserves: r1, r2, r13, r14..r31, f14..f31 (ps0 and ps1 bits), cr2..cr4;
//   * bytes written to guest memory, address and final value, except the dead stack under the entry r1
//     ([r1 - dead stack, r1), default_dead_stack bytes unless the caller gives another size): a byte written by only
//     one side is a difference even if its value did not change;
//   * FPSCR is not compared. This is only sound for a game whose own code never reads FPSCR (no mffs, mtfsf, mtfsb,
//     mcrfs): a sticky exception bit or a rounding mode left by one side would then never change a result. Each game
//     states why that holds for it next to its functions table (games/<game>/decomp/); a game that reads FPSCR needs
//     it compared first.
#ifndef GC_ORACLE_COMPARE_H
#define GC_ORACLE_COMPARE_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace gc::oracle {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// Guest registers the oracle keeps (floating-point registers as raw IEEE-754 bits).
struct reg_state {
    u32 gpr[32];
    u64 fpr[32];
    u64 ps1[32];
    u32 cr, xer, lr, ctr, fpscr;
};

// One byte of guest memory written during a run: its canonical address (0x80000000 | offset in RAM), the value it
// had before the run and the value it has after.
struct byte_write {
    u32 address;
    u8 before;
    u8 after;
};

// Same values as gc::abi::detail::result_kind (gc_abi.h).
enum result_kind : u8 { result_none, result_word, result_pair, result_f32, result_f64, result_memory };

// cr2, cr3, cr4: CR field n occupies bits 31-4n..28-4n counted from the least significant bit (cr0 is the top
// nibble).
inline constexpr u32 preserved_cr_mask = 0x00FFF000u;
// Default size of the dead stack under the entry r1 that is not compared (stack frame of the function itself, dead
// after its return): 64 KiB. The in-game oracle takes another size from GCRT_DECOMP_ORACLE_DEAD_STACK; each case
// records the size it was compared with.
inline constexpr u32 default_dead_stack = 0x10000u;

inline u32 canonical_address(u32 address) noexcept {
    return 0x80000000u | (address & 0x3FFFFFFFu);
}

// One difference between the original and the decompiled version.
struct difference {
    char what[16];  // "r3", "f1.ps1", "cr", "mem"...
    u32 address;    // "mem": guest address of the byte
    u64 original;   // value seen after the original (mem: byte, or 0x100 if the original did not write it)
    u64 decompiled; // value seen after the decompiled version (mem: byte, or 0x100 if it did not write it)
};
inline constexpr u64 not_written = 0x100u;

// Bytes written by one run, each address once, sorted by address: `before` is the first old value seen, `after`
// the value in memory after the run.
class write_set {
public:
    // Records a write of `size` bytes (big-endian `old_value`) at `address`; `after` is read later by finish().
    void add(u32 address, u32 size, u64 old_value) {
        for (u32 i = 0; i < size; i++) {
            const u8 old_byte = static_cast<u8>(old_value >> (8u * (size - 1u - i)));
            bytes_.push_back(byte_write{canonical_address(address + i), old_byte, 0});
        }
    }
    // Keeps the first old value of each byte and reads the final values with `read_byte(address)`.
    template <class ReadByte> void finish(ReadByte read_byte) {
        std::stable_sort(bytes_.begin(), bytes_.end(),
                         [](const byte_write& a, const byte_write& b) { return a.address < b.address; });
        bytes_.erase(std::unique(bytes_.begin(), bytes_.end(),
                                 [](const byte_write& a, const byte_write& b) { return a.address == b.address; }),
                     bytes_.end());
        for (byte_write& b : bytes_)
            b.after = read_byte(b.address);
    }
    const std::vector<byte_write>& bytes() const noexcept { return bytes_; }
    void clear() noexcept { bytes_.clear(); }

private:
    std::vector<byte_write> bytes_;
};

namespace detail {
inline void push(std::vector<difference>& out, const char* what, u32 address, u64 original, u64 decompiled) {
    difference d{};
    std::snprintf(d.what, sizeof(d.what), "%s", what);
    d.address = address;
    d.original = original;
    d.decompiled = decompiled;
    out.push_back(d);
}
inline bool in_dead_stack(u32 address, u32 entry_r1, u32 dead_stack) noexcept {
    const u32 r1 = canonical_address(entry_r1);
    return address < r1 && r1 - address <= dead_stack;
}
} // namespace detail

// Differences between the original's outputs (registers `original`, writes `original_writes`) and the decompiled
// version's, for a function whose result is of kind `kind`, entered with r1 = `entry_r1`, `dead_stack` bytes under
// it not compared. Appends to `out`.
inline void compare(const reg_state& original, const reg_state& decompiled, u8 kind, u32 entry_r1,
                    const std::vector<byte_write>& original_writes, const std::vector<byte_write>& decompiled_writes,
                    std::vector<difference>& out, u32 dead_stack = default_dead_stack) {
    char name[16];
    auto gpr = [&](unsigned i) {
        if (original.gpr[i] != decompiled.gpr[i]) {
            std::snprintf(name, sizeof(name), "r%u", i);
            detail::push(out, name, 0, original.gpr[i], decompiled.gpr[i]);
        }
    };
    auto fpr = [&](unsigned i, bool with_ps1) {
        if (original.fpr[i] != decompiled.fpr[i]) {
            std::snprintf(name, sizeof(name), "f%u", i);
            detail::push(out, name, 0, original.fpr[i], decompiled.fpr[i]);
        }
        if (with_ps1 && original.ps1[i] != decompiled.ps1[i]) {
            std::snprintf(name, sizeof(name), "f%u.ps1", i);
            detail::push(out, name, 0, original.ps1[i], decompiled.ps1[i]);
        }
    };
    switch (kind) {
    case result_word:
    case result_memory:
        gpr(3);
        break;
    case result_pair:
        gpr(3);
        gpr(4);
        break;
    case result_f32:
        fpr(1, true);
        break;
    case result_f64:
        fpr(1, false);
        break;
    default:
        break;
    }
    gpr(1);
    gpr(2);
    for (unsigned i = 13; i < 32; i++)
        gpr(i);
    for (unsigned i = 14; i < 32; i++)
        fpr(i, true);
    if ((original.cr & preserved_cr_mask) != (decompiled.cr & preserved_cr_mask))
        detail::push(out, "cr2-cr4", 0, original.cr & preserved_cr_mask, decompiled.cr & preserved_cr_mask);
    // Memory: merge of the two sorted write sets.
    std::size_t i = 0, j = 0;
    const auto& a = original_writes;
    const auto& b = decompiled_writes;
    while (i < a.size() || j < b.size()) {
        if (j == b.size() || (i < a.size() && a[i].address < b[j].address)) {
            if (!detail::in_dead_stack(a[i].address, entry_r1, dead_stack))
                detail::push(out, "mem", a[i].address, a[i].after, not_written);
            i++;
        } else if (i == a.size() || b[j].address < a[i].address) {
            if (!detail::in_dead_stack(b[j].address, entry_r1, dead_stack))
                detail::push(out, "mem", b[j].address, not_written, b[j].after);
            j++;
        } else {
            if (a[i].after != b[j].after && !detail::in_dead_stack(a[i].address, entry_r1, dead_stack))
                detail::push(out, "mem", a[i].address, a[i].after, b[j].after);
            i++;
            j++;
        }
    }
}

// Readable form of one difference ("r3: original 00000001, decompiled 00000000").
inline void describe(const difference& d, char* text, std::size_t size) {
    if (d.what[0] == 'm') {
        char o[8], c[8];
        std::snprintf(o, sizeof(o), d.original == not_written ? "--" : "%02X", static_cast<unsigned>(d.original));
        std::snprintf(c, sizeof(c), d.decompiled == not_written ? "--" : "%02X", static_cast<unsigned>(d.decompiled));
        std::snprintf(text, size, "byte %08X: original %s, decompiled %s (-- = not written)",
                      static_cast<unsigned>(d.address), o, c);
    } else if (d.what[0] == 'f') {
        std::snprintf(text, size, "%s bits: original %016llX, decompiled %016llX", d.what,
                      static_cast<unsigned long long>(d.original), static_cast<unsigned long long>(d.decompiled));
    } else {
        std::snprintf(text, size, "%s: original %08llX, decompiled %08llX", d.what,
                      static_cast<unsigned long long>(d.original), static_cast<unsigned long long>(d.decompiled));
    }
}

// FNV-1a 64 of a byte range (fingerprint of a starting state).
inline u64 fnv1a(u64 hash, const void* data, std::size_t size) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; i++) {
        hash ^= p[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}
inline constexpr u64 fnv_offset = 0xCBF29CE484222325ull;

} // namespace gc::oracle

#endif // GC_ORACLE_COMPARE_H
