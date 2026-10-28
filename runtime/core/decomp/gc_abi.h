// gc_abi.h: gateway between recompiled guest code and decompiled C++ functions (generic, no game here).
//
// A decompiled function is an ordinary C++ function whose parameters and result are types of gc_mem.h (fixed-width
// integers, enumerations, f32/f64, gc::ptr<T>) or a guest structure returned by value. The gateway lets the three
// kinds of calls of a mixed executable reach it:
//   * recompiled -> decompiled: the function is registered with gcrt_hle_register at its guest address (address taken
//     from the game's table for the version that runs; the address is compiled with an HLE hook, so direct `bl`
//     calls reach it as well as the indirect ones that go through the chassis). gc::abi::entry<F> reads the arguments
//     from the guest registers following the PowerPC EABI, calls F, writes the result and returns to LR;
//   * decompiled -> recompiled: gc::abi::call_guest<R>(address, args...) sets the argument registers and runs the
//     guest function in a nested run of the chassis until it returns (runtime/core/hle: gcrt_guest_call_prepared);
//   * decompiled -> decompiled: gc::abi::call<&Fn>(args...), a plain C++ call of Fn (in an oracle build, while the
//     caller is being compared, the call goes through the oracle instead: recorded calls replayed, gc_oracle.cpp).
//     When the target is only known at run time (function pointer or virtual function read from guest memory),
//     gc::abi::call_address<R>(address, args...) calls the decompiled C++ function directly if the registry has one
//     at that address with the same C++ type, otherwise call_guest. Decompiled code never calls another decompiled
//     function by a bare C++ call: the oracle would not see that call.
//
// PowerPC EABI followed here (SVR4 PowerPC supplement and the EABI; cross-checked on game code, notes/decomp.md D1.1):
//   * integer, enumeration and pointer arguments: r3..r10, in order; 64-bit integers: an odd/even pair (r3:r4,
//     r5:r6, r7:r8, r9:r10; high word in the lower register), skipping one register if needed;
//   * f32/f64 arguments: f1..f8, in order, counted separately from the integer registers;
//   * result: integer in r3 (sign- or zero-extended to 32 bits), 64-bit integer in r3:r4 (high word in r3), f32 in f1
//     with ps0 = ps1 (like write_single_result of DolRecomp's cpu.c), f64 in f1 (ps1 untouched); a structure is
//     written to the memory a hidden pointer in r3 designates, and the arguments then start at r4;
//   * arguments that would go on the stack (more than 8 integer or 8 floating-point registers) are refused at
//     compile time, as are bool (its guest representation is not checked: use u8) and the gc::be<T> wrapper (pass T).
//
// Rules (stage document, notes/decomp.md D1.1): decompiled code never names an address; the gateway is only built in
// guest-memory mode and leaves the executable with the recompiled code (stage D3). No function with C linkage returns
// a gc::ptr<T> or gc::be<T> (the Microsoft x64 ABI returns them through a hidden pointer): the C entry points below
// return u32 or nothing.
#ifndef GC_ABI_H
#define GC_ABI_H

#include "decomp/gc_mem.h"

#if defined(GC_NATIVE_MEMORY)
#error "gc_abi.h is the gateway of the guest-memory mode; it is not built with native memory (stage D4)"
#endif

#include <array>
#include <cstdint>
#include <cstdio>
#include <type_traits>

struct GcrtCpu; // runtime/core/cpu/gcrt_cpu.h (C)

namespace gc::abi {

// View of the guest registers of the running CPU (filled by gc_abi.cpp, which knows the CPU state).
struct registers {
    u32* gpr; // r0..r31
    f64* fpr; // f0..f31 (ps0)
    f64* ps1; // f0..f31 (ps1)
    u32* lr;
    u32* pc;
};
registers registers_of(GcrtCpu* g) noexcept;

// Properties of a decompiled function that the gateway needs to know.
enum : u32 {
    flag_none = 0,
    // No write to guest memory and no call: the function may be run twice without changing the game (diagnostic
    // GCRT_DECOMP_CROSSCHECK, which compares it with its recompiled original on real calls).
    flag_reads_only = 1u << 0,
};

using entry_fn = bool (*)(GcrtCpu* g, u32 address);

// One decompiled function, defined next to it by GC_DECOMP_EXPORT.
struct export_record {
    const char* name;
    entry_fn entry;        // called by the HLE dispatcher (recompiled -> decompiled)
    void (*direct)();      // the C++ function itself, type erased (decompiled -> decompiled through call_address)
    const void* signature; // address unique to the C++ type of the function
    u32 flags;
    u64* calls;            // calls received through the gateway
    u8 result_kind;        // detail::result_kind of the result (cross-check)
};

// Properties of the recompiled ORIGINAL at a registered address (scan of its code by the build, tools/build.py).
enum : u32 {
    registration_none = 0,
    // The original holds a floating-point instruction. While MSR[FP] = 0 the console's OS gives the FPU lazily: the
    // first floating-point instruction of a thread raises "FPU unavailable", and the OS handler saves the floating-
    // point registers of the thread that owned the FPU, loads those of the running thread and sets MSR[FP] = 1
    // (DolRecomp emits ppc_fp_available_inline before every such instruction; the native OS handles the exception).
    // The decompiled version does not raise it: it would read the floating-point registers of another thread, and
    // leave the FPU to the wrong thread. So, while MSR[FP] = 0, the entry declines and the original runs, exception
    // included; once the handler has set MSR[FP] = 1, the next call (and, for an original whose first instruction is
    // a floating-point one, the same call, resumed at that instruction) reaches the decompiled version again.
    registration_uses_fpu = 1u << 0,
};

// Address of a decompiled function in the version that runs (table generated by the build, never written by hand).
struct registration {
    u32 address;
    const export_record* record;
    u32 flags; // registration_*
};

// Registers every function of `table` (gcrt_hle_register) and prepares the gateway (guest memory of gc_mem.h, report
// at exit, cross-check). `table` must outlive the run.
void install(GcrtCpu* g, const registration* table, unsigned count);
// Decompiled function registered at `address`, nullptr if none.
const export_record* find(u32 address) noexcept;
// Calls received by each registered function, one line each ("DECOMP name address calls=N ..."), and the totals.
void report(std::FILE* out);

namespace detail {

template <class T> struct is_ptr : std::false_type {};
template <class T> struct is_ptr<gc::ptr<T>> : std::true_type {};
template <class T> struct is_be : std::false_type {};
template <class T> struct is_be<gc::be<T>> : std::true_type {};

template <class T>
inline constexpr bool is_fpr_v = std::is_same_v<T, f32> || std::is_same_v<T, f64>;
template <class T>
inline constexpr bool is_pair_v = std::is_integral_v<T> && sizeof(T) == 8;
template <class T>
inline constexpr bool is_word_v = (std::is_integral_v<T> && !std::is_same_v<T, bool> && sizeof(T) <= 4) ||
                                  (std::is_enum_v<T> && sizeof(T) <= 4) || is_ptr<T>::value;
template <class T>
inline constexpr bool is_value_v = is_fpr_v<T> || is_pair_v<T> || is_word_v<T>;
// Structure returned through a hidden pointer: a guest class (gc_mem.h rules) that is neither ptr nor be.
template <class T>
inline constexpr bool is_hidden_struct_v = std::is_class_v<T> && !is_ptr<T>::value && !is_be<T>::value;

template <class T> constexpr void check_argument() {
    static_assert(!std::is_same_v<T, bool>, "gc_abi: no bool argument (guest representation not checked): use u8");
    static_assert(!is_be<T>::value, "gc_abi: pass the value (T), not gc::be<T>");
    static_assert(!std::is_reference_v<T> && !std::is_pointer_v<T>,
                  "gc_abi: guest objects are passed as gc::ptr<T>, never as host pointers or references");
    static_assert(is_value_v<T>, "gc_abi: argument type not supported by the gateway (integer, enum, f32, f64, gc::ptr)");
}

enum slot_kind : u8 { slot_word, slot_pair, slot_fpr };
struct slot {
    u8 kind;
    u8 reg;
};

// Register of each argument, the first integer register being `first_gpr` (3, or 4 after a hidden result pointer).
template <class... A> struct allocation {
    struct result_type {
        std::array<slot, sizeof...(A) == 0 ? 1 : sizeof...(A)> slots{};
        unsigned next_gpr = 0;
        unsigned next_fpr = 0;
    };
    static constexpr result_type compute(unsigned first_gpr) {
        result_type out{};
        unsigned gpr = first_gpr, fpr = 1, i = 0;
        (
            [&] {
                if constexpr (is_fpr_v<A>) {
                    out.slots[i++] = slot{slot_fpr, static_cast<u8>(fpr++)};
                } else if constexpr (is_pair_v<A>) {
                    if ((gpr & 1u) == 0)
                        gpr++; // 64-bit integers start in an odd register
                    out.slots[i++] = slot{slot_pair, static_cast<u8>(gpr)};
                    gpr += 2;
                } else {
                    out.slots[i++] = slot{slot_word, static_cast<u8>(gpr++)};
                }
            }(),
            ...);
        out.next_gpr = gpr;
        out.next_fpr = fpr;
        return out;
    }
};

template <class T> T read_value(const registers& r, slot s) noexcept {
    if constexpr (std::is_same_v<T, f32>) {
        // A single-precision argument is held as a double rounded to single (lfs, frsp, single operations).
        return static_cast<f32>(r.fpr[s.reg]);
    } else if constexpr (std::is_same_v<T, f64>) {
        return r.fpr[s.reg];
    } else if constexpr (is_pair_v<T>) {
        return static_cast<T>((static_cast<u64>(r.gpr[s.reg]) << 32) | r.gpr[s.reg + 1]);
    } else if constexpr (is_ptr<T>::value) {
        return ptr_from_guest_address<typename T::element_type>(r.gpr[s.reg]);
    } else if constexpr (std::is_enum_v<T>) {
        return static_cast<T>(static_cast<std::underlying_type_t<T>>(r.gpr[s.reg]));
    } else {
        return static_cast<T>(r.gpr[s.reg]); // low bits, whatever the caller left above them
    }
}

template <class T> void write_value(const registers& r, slot s, const T& v) noexcept {
    if constexpr (std::is_same_v<T, f32>) {
        r.fpr[s.reg] = static_cast<f64>(v);
        r.ps1[s.reg] = static_cast<f64>(v);
    } else if constexpr (std::is_same_v<T, f64>) {
        r.fpr[s.reg] = v;
    } else if constexpr (is_pair_v<T>) {
        const u64 bits = static_cast<u64>(v);
        r.gpr[s.reg] = static_cast<u32>(bits >> 32);
        r.gpr[s.reg + 1] = static_cast<u32>(bits);
    } else if constexpr (is_ptr<T>::value) {
        r.gpr[s.reg] = guest_address(v);
    } else if constexpr (std::is_enum_v<T>) {
        write_value(r, s, static_cast<std::underlying_type_t<T>>(v));
    } else if constexpr (std::is_signed_v<T>) {
        r.gpr[s.reg] = static_cast<u32>(static_cast<s32>(v)); // sign-extended
    } else {
        r.gpr[s.reg] = static_cast<u32>(v); // zero-extended
    }
}

// What the result occupies (cross-check: registers to compare).
enum result_kind : u8 { result_none, result_word, result_pair, result_f32, result_f64, result_memory };
template <class R> constexpr u8 result_kind_of() {
    if constexpr (std::is_void_v<R>)
        return result_none;
    else if constexpr (std::is_same_v<R, f32>)
        return result_f32;
    else if constexpr (std::is_same_v<R, f64>)
        return result_f64;
    else if constexpr (is_pair_v<R>)
        return result_pair;
    else if constexpr (is_hidden_struct_v<R>)
        return result_memory;
    else
        return result_word;
}

template <class R> constexpr void check_result() {
    static_assert(!std::is_same_v<R, bool>, "gc_abi: no bool result (guest representation not checked): use u8");
    static_assert(!is_be<R>::value, "gc_abi: return the value (T), not gc::be<T>");
    static_assert(!std::is_reference_v<R> && !std::is_pointer_v<R>, "gc_abi: return gc::ptr<T>, not a host pointer");
    if constexpr (!std::is_void_v<R>)
        static_assert(is_value_v<R> || is_hidden_struct_v<R>, "gc_abi: result type not supported by the gateway");
}

template <auto Fn> struct function_traits;
template <class R, class... A, R (*Fn)(A...)> struct function_traits<Fn> {
    using result = R;
    using pointer = R (*)(A...);
    static constexpr bool hidden_result = is_hidden_struct_v<R>;
    static constexpr auto alloc = allocation<A...>::compute(hidden_result ? 4u : 3u);
    static_assert(alloc.next_gpr <= 11u, "gc_abi: more than 8 integer argument registers (stack arguments)");
    static_assert(alloc.next_fpr <= 9u, "gc_abi: more than 8 floating-point argument registers (stack arguments)");

#if defined(GC_DECOMP_ORACLE)
    // Argument and result registers, for the oracle's fingerprint of a starting state (gc_oracle.cpp).
    static constexpr u32 argument_gpr_mask() {
        u32 mask = hidden_result ? (1u << 3) : 0u;
        for (std::size_t i = 0; i < sizeof...(A); i++) {
            if (alloc.slots[i].kind == slot_word)
                mask |= 1u << alloc.slots[i].reg;
            else if (alloc.slots[i].kind == slot_pair)
                mask |= 3u << alloc.slots[i].reg;
        }
        return mask;
    }
    static constexpr u32 argument_fpr_mask() {
        u32 mask = 0;
        for (std::size_t i = 0; i < sizeof...(A); i++)
            if (alloc.slots[i].kind == slot_fpr)
                mask |= 1u << alloc.slots[i].reg;
        return mask;
    }
#endif

    template <std::size_t... I> static R call(const registers& r, std::index_sequence<I...>) {
        return Fn(read_value<A>(r, alloc.slots[I])...);
    }
    static void invoke(const registers& r) {
        (check_argument<A>(), ...);
        check_result<R>();
        if constexpr (std::is_void_v<R>) {
            call(r, std::index_sequence_for<A...>{});
        } else if constexpr (hidden_result) {
            const u32 destination = r.gpr[3];
            const R value = call(r, std::index_sequence_for<A...>{});
            *ptr_from_guest_address<R>(destination) = value; // field by field (gc_mem.h); r3 keeps the pointer
        } else {
            write_value<R>(r, slot{is_pair_v<R> ? slot_pair : (is_fpr_v<R> ? slot_fpr : slot_word),
                                   static_cast<u8>(is_fpr_v<R> ? 1 : 3)},
                           call(r, std::index_sequence_for<A...>{}));
        }
    }
};

template <class F> struct signature_tag {
    static constexpr char id = 0;
};

template <auto Fn> inline u64 call_count = 0;

// Set while the cross-check runs the recompiled original of `address`: the entry declines (lets the original run).
extern u32 bypass_address;
// MSR of the CPU the gateway is installed on (gc_abi.cpp install), read at every entry; MSR[FP] is bit 0x2000 (PPC bit
// 18, as DolRecomp's ppc_fp_available_inline tests it).
extern const u32* guest_msr;
inline constexpr u32 msr_fp = 0x00002000u;
// MSR[FP] = 0 at the entry of `address`: true if its registration has registration_uses_fpu (the entry then declines,
// counted as fpu_declined in the report and, in an oracle build, apart from the compared calls), false otherwise.
bool decline_without_fpu(u32 address) noexcept;
// True if the function registered at `address` has registration_uses_fpu (not counted).
bool registered_uses_fpu(u32 address) noexcept;
// True if the gateway is installed and MSR[FP] = 0.
inline bool fpu_off() noexcept {
    return guest_msr && !(*guest_msr & msr_fp);
}
// Called by entry after the decompiled function when GCRT_DECOMP_CROSSCHECK is on (gc_abi.cpp).
extern bool crosscheck_enabled;
void crosscheck(GcrtCpu* g, u32 address, const u32* saved_gpr, const f64* saved_fpr, const f64* saved_ps1);
void first_call(u32 address);
// Nested run of guest code with the registers already set (runtime/core/hle: gcrt_guest_call_prepared).
bool run_guest(GcrtCpu* g, u32 address) noexcept;
// A call of decompiled code to the function at `address` (call_guest, call_address, call): run_guest, except in an
// oracle build while the caller is being compared, where the oracle compares it with the original's call and replays
// it (`argument_gprs`, `argument_fprs`: bit n = rn / fn holds an argument of the call's C++ type).
bool call_function(GcrtCpu* g, u32 address, u32 argument_gprs, u32 argument_fprs) noexcept;
// True while the oracle compares a decompiled function (its calls must go through call_function).
bool oracle_replaying() noexcept;
// Guest address, in this build, of the registered decompiled function whose C++ function is `direct`, 0 if none.
u32 address_of_function(void (*direct)()) noexcept;
GcrtCpu* current_cpu() noexcept;

#if defined(GC_DECOMP_ORACLE)
// Oracle executable only (build option GCRT_DECOMP_ORACLE, tools/build.py --oracle; gc_oracle.cpp): what the oracle
// needs to know of a decompiled function's C++ signature.
struct oracle_signature {
    u32 argument_gprs; // bit n: rn holds an argument (or the hidden result pointer)
    u32 argument_fprs; // bit n: fn holds an argument
    u8 result_kind;    // result_kind
};
// Runs the recompiled original, then the decompiled version (`invoke`) from the same state, compares them and leaves
// the original's results (registers and memory). False: not handled (nested call during another comparison), the
// caller runs the decompiled version as usual.
bool oracle_entry(GcrtCpu* g, u32 address, void (*invoke)(const registers&), const oracle_signature& signature);
// Nested guest runs started through run_guest (gc_abi.cpp), counted so that the oracle sees a decompiled version that
// calls guest code (not comparable as a leaf).
extern u64 guest_calls;
// Oracle: compares a call of the decompiled version being compared with the original's and replays it; false when
// no comparison runs (gc_oracle.cpp).
bool oracle_replay_call(GcrtCpu* g, u32 address, u32 argument_gprs, u32 argument_fprs) noexcept;
bool oracle_is_replaying() noexcept;
// The entry of `address` declined because MSR[FP] = 0 (decline_without_fpu): counted apart by the oracle ("declined,
// FPU off"), neither compared nor "not compared"; during another comparison it also makes that function not a leaf.
void oracle_declined_fpu(u32 address) noexcept;
#endif

} // namespace detail

// HLE entry of the decompiled function Fn (recompiled -> decompiled).
template <auto Fn> bool entry(GcrtCpu* g, u32 address) {
    if (address == detail::bypass_address)
        return false; // cross-check in progress: the recompiled original runs
    if (detail::fpu_off() && detail::decline_without_fpu(address))
        return false; // FPU not given to this thread yet: the original runs and raises "FPU unavailable" itself
    const registers r = registers_of(g);
    const u32 return_address = *r.lr;
    if (detail::call_count<Fn>++ == 0)
        detail::first_call(address);
#if defined(GC_DECOMP_ORACLE)
    {
        using traits = detail::function_traits<Fn>;
        static constexpr detail::oracle_signature signature{traits::argument_gpr_mask(), traits::argument_fpr_mask(),
                                                            detail::result_kind_of<typename traits::result>()};
        if (detail::oracle_entry(g, address, &traits::invoke, signature)) {
            *r.pc = return_address; // the registers and memory are the original's
            return true;
        }
    }
#endif
    if (detail::crosscheck_enabled) {
        u32 gpr[11];
        f64 fpr[9], ps1[9];
        for (unsigned i = 3; i <= 10; i++)
            gpr[i] = r.gpr[i];
        for (unsigned i = 1; i <= 8; i++) {
            fpr[i] = r.fpr[i];
            ps1[i] = r.ps1[i];
        }
        detail::function_traits<Fn>::invoke(r);
        detail::crosscheck(g, address, gpr, fpr, ps1);
    } else {
        detail::function_traits<Fn>::invoke(r);
    }
    *r.pc = return_address;
    return true;
}

template <auto Fn> export_record make_export(const char* name, u32 flags) {
    using traits = detail::function_traits<Fn>;
    return export_record{name,
                         &entry<Fn>,
                         reinterpret_cast<void (*)()>(Fn),
                         &detail::signature_tag<typename traits::pointer>::id,
                         flags,
                         &detail::call_count<Fn>,
                         detail::result_kind_of<typename traits::result>()};
}

// Calls the guest function at `address` (decompiled -> recompiled). R: void or a value type (no hidden structure).
template <class R, class... A> R call_guest(u32 address, A... args) {
    (detail::check_argument<A>(), ...);
    detail::check_result<R>();
    static_assert(!detail::is_hidden_struct_v<R>, "gc_abi: call_guest does not return structures yet");
    constexpr auto alloc = detail::allocation<A...>::compute(3u);
    static_assert(alloc.next_gpr <= 11u && alloc.next_fpr <= 9u, "gc_abi: stack arguments not supported");
    GcrtCpu* g = detail::current_cpu();
    const registers r = registers_of(g);
    unsigned i = 0;
    ((detail::write_value<A>(r, alloc.slots[i++], args)), ...);
    (void)i;
    u32 gprs = 0, fprs = 0;
    for (std::size_t k = 0; k < sizeof...(A); k++) {
        if (alloc.slots[k].kind == detail::slot_fpr)
            fprs |= 1u << alloc.slots[k].reg;
        else
            gprs |= (alloc.slots[k].kind == detail::slot_pair ? 3u : 1u) << alloc.slots[k].reg;
    }
    detail::call_function(g, address, gprs, fprs);
    if constexpr (!std::is_void_v<R>) {
        constexpr detail::slot result{detail::is_pair_v<R> ? detail::slot_pair
                                                           : (detail::is_fpr_v<R> ? detail::slot_fpr : detail::slot_word),
                                      static_cast<u8>(detail::is_fpr_v<R> ? 1 : 3)};
        return detail::read_value<R>(r, result);
    }
}

// Calls the function at a run-time `address` (pointer read from guest memory): the decompiled C++ function if one is
// registered there with exactly this C++ type, otherwise the guest code. A decompiled function whose original uses the
// FPU is reached through the guest code while MSR[FP] = 0 (its entry declines there, registration_uses_fpu).
// Limit: a plain C++ call between decompiled functions does not go through the gateway; see notes/decomp.md.
template <class R, class... A> R call_address(u32 address, A... args) {
    if (const export_record* e = find(address);
        e && !detail::oracle_replaying() && !(detail::fpu_off() && detail::registered_uses_fpu(address))) {
        using pointer = R (*)(A...);
        if (e->signature == &detail::signature_tag<pointer>::id)
            return reinterpret_cast<pointer>(e->direct)(args...);
        std::fprintf(stderr, "gc_abi: %s (%08X) called with another C++ type: guest code used\n", e->name,
                     static_cast<unsigned>(address));
    }
    return call_guest<R, A...>(address, args...);
}

// Calls the decompiled function Fn (decompiled -> decompiled): a plain C++ call, except while the oracle compares the
// caller (oracle builds), where it goes through the oracle as a call to Fn's guest address (recorded calls replayed).
// The export record of Fn must be registered in this build for that (a disabled row is not: the comparison then shows
// a call to address 0).
namespace detail {
// call_guest with the callee's own parameter types (their registers), the arguments converted to them.
template <class R, class... P, class... A> R call_through_oracle(R (*)(P...), u32 address, A... args) {
    return call_guest<R, P...>(address, static_cast<P>(args)...);
}
} // namespace detail

template <auto Fn, class... A> auto call(A... args) {
    if (detail::oracle_replaying()) {
        static const u32 address = detail::address_of_function(reinterpret_cast<void (*)()>(Fn));
        return detail::call_through_oracle(Fn, address, args...);
    }
    return Fn(args...);
}

#if defined(GC_DECOMP_ORACLE_NEGATIVE)
// Negative test of the oracle only (tools/build.py --oracle-negative, never in another build): deliberately wrong
// variants of decompiled functions use these to stand for a gateway that would not preserve a register.
namespace testing {
void clobber_gpr(unsigned reg, u32 value) noexcept;
void clobber_cr(u32 value) noexcept;
} // namespace testing
#endif

} // namespace gc::abi

// Defines the export record of a decompiled function (in its source file). `name` is the identifier listed in the
// game's functions.tsv; the build generates the table that gives it its address in each version.
#define GC_DECOMP_EXPORT(name, function, flags)                                                                        \
    extern const ::gc::abi::export_record gc_decomp_export_##name;                                                    \
    const ::gc::abi::export_record gc_decomp_export_##name = ::gc::abi::make_export<&function>(#name, flags)

#endif // GC_ABI_H
