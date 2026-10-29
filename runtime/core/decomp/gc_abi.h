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
// Guest time (cycle clock). With the cycle clock (GCRT_CLOCK_CYCLES, ticks = guest cycles / 12), guest time is the
// cycles the recompiled code is charged for; a decompiled function costs none, so timed events (end of a load, audio)
// would come earlier than with its original. The cycle clock is not only the bench's: it is the DEFAULT clock of normal
// play too. The platform layer's default pacing (GCRT_PACING=throttle, platform.cpp) switches the CPU to the cycle clock
// and paces it on real time; the A/B bench runs the same clock without pacing (--clock cycles). Only another pacing
// (GCRT_PACING=host), without --clock cycles, keeps the real-time clock. The gateway therefore charges each call of a
// decompiled function the EXCLUSIVE guest cost of its original (its own cycles, without those of the functions it
// calls), measured by the oracle (registration::guest_cost_256ths, 1/256 cycle per call, with the remainder carried
// from call to call: no per-call rounding loss; a mean cost quantized to 1/256 cycle still leaves a linear bias of at
// most 1/512 cycle per call, none for a whole number of cycles). The callees are charged where they run: a recompiled
// one by the chassis, a decompiled one by its own entry (through call_guest) or by gc::abi::call / call_address (plain
// C++ calls). With the real-time clock nothing is charged: the cycle counter is not time there. GCRT_DECOMP_GUEST_COST=0
// (read in every build, production and oracle) charges nothing: its only effect is on the cycle counter, for the
// diagnostic of a timing difference. GCRT_DECOMP_CROSSCHECK (diagnostic, off by default) counts the cross-checked calls
// twice: the chassis charges the recompiled original that crosscheck runs, on top of charge_guest_cost. To be removed
// with the cycle clock coupling at D3.
// Cost by path (gc_path.h): when the original's cost depends on its path, the build gives its registration a graph of
// the original's conditional branches (registration::path, tools/decomp_path_cost.py) and the decompiled code marks
// the outcome of each of them (gc::path::taken / falls_through): each call then charges the exact cycles of the
// original's path instead of the table's mean (kept as the fallback of a path error, counted in the exit report).
//
// Rules (stage document, notes/decomp.md D1.1): decompiled code never names an address; the gateway is only built in
// guest-memory mode and leaves the executable with the recompiled code (stage D3). No function with C linkage returns
// a gc::ptr<T> or gc::be<T> (the Microsoft x64 ABI returns them through a hidden pointer): the C entry points below
// return u32 or nothing.
#ifndef GC_ABI_H
#define GC_ABI_H

#include "decomp/gc_mem.h"
#include "decomp/gc_path.h"

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
    // The function marks the outcome of each of its original's conditional branches (gc::path::taken /
    // falls_through, gc_path.h): when the build gives its registration a graph (path-dependent original), each call
    // is charged the cycles of the original's path. Without this flag the graph is not used (the mean is charged,
    // and install says so).
    flag_path_marked = 1u << 1,
};

using entry_fn = bool (*)(GcrtCpu* g, u32 address);

// Guest cost of one decompiled function (see "Guest time" above), state of the gateway: what each call charges to the
// cycle clock, in 1/256 cycle (set by install from registration::guest_cost_256ths; 0 under GCRT_DECOMP_GUEST_COST=0),
// and the fraction of a cycle not charged yet (carried to the next call: N calls charge N x per_call_256ths / 256 to
// within one cycle in all; per_call_256ths itself differs from a mean cost by at most 1/512 cycle).
// `registered`: set by install when the function is in this build's registration table. gc::abi::call<Fn> to a function
// that is not (its row disabled while its source file is built for another row) charges no guest cost, and an oracle
// build would see a call to address 0: call<Fn> warns once per function (`unregistered_warned`).
// `path`: graph of the original's conditional branches (registration::path; nullptr: constant cost, or costs off), and
// the walks that did not end on a return of the original (`path_errors`: the mean `per_call_256ths` was charged).
struct guest_cost_state {
    u32 per_call_256ths = 0;
    u32 remainder_256ths = 0;
    bool registered = false;
    bool unregistered_warned = false;
    const path_graph* path = nullptr;
    u64 path_walks = 0;
    u64 path_errors = 0;
};

// One decompiled function, defined next to it by GC_DECOMP_EXPORT.
struct export_record {
    const char* name;
    entry_fn entry;        // called by the HLE dispatcher (recompiled -> decompiled)
    void (*direct)();      // the C++ function itself, type erased (decompiled -> decompiled through call_address)
    const void* signature; // address unique to the C++ type of the function
    u32 flags;
    u64* calls;            // calls received through the gateway
    u8 result_kind;        // detail::result_kind of the result (cross-check)
    guest_cost_state* cost; // guest cost of the function (one per C++ function: detail::guest_cost<Fn>)
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
    // Guest cycles the gateway adds to the cycle clock for each call, in 1/256 cycle (decomp/guest_cost.tsv of the
    // game, measured by the oracle: the mean exclusive cycles of the recompiled original, see "Guest time" above).
    // 0: nothing charged.
    u32 guest_cost_256ths;
    // Graph of the original's conditional branches when its cost depends on its path (gc_path.h; generated by the
    // build from the original's code and DolRecomp's cost groups, tools/decomp_path_cost.py): each call is charged the
    // cycles of the path the decompiled code marks, guest_cost_256ths only after a path error. nullptr: constant cost.
    const path_graph* path = nullptr;
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
// Guest cost of the decompiled function Fn (one per C++ function, pointed to by its export record).
template <auto Fn> inline guest_cost_state guest_cost{};
// Name of the decompiled function Fn (GC_DECOMP_EXPORT, set by make_export at static initialization).
template <auto Fn> inline const char* export_name = nullptr;
// call<Fn> to a function not registered in this build: one warning per function (see guest_cost_state).
inline void note_unregistered_call(guest_cost_state& cost, const char* name) noexcept {
    if (cost.unregistered_warned)
        return;
    cost.unregistered_warned = true;
    std::fprintf(stderr, "gc_abi: WARNING: gc::abi::call to %s, which is not registered in this build (row not enabled "
                         "in the functions table): no guest cost charged for it\n",
                 name ? name : "a decompiled function");
}
// Some registration has a guest cost and GCRT_DECOMP_GUEST_COST is not 0 (gc_abi.cpp install). charge_guest_cost adds
// one call of `cost` to the cycle counter (gcrt_cpu_add_cycles) only while the CPU runs on the cycle clock
// (GCRT_CLOCK_CYCLES: the default pacing of normal play and the bench, see "Guest time"); with the real-time clock
// (GCRT_PACING=host) it does nothing.
extern bool guest_cost_enabled;
void charge_guest_cost(GcrtCpu* g, guest_cost_state& cost) noexcept;
// Walk of the graph of one call (gc_path.h), around the C++ function: starts the walk of `cost.path` (none if the
// function has no graph: the markers of the code it runs are then not recorded) and restores the caller's walk.
struct path_scope {
    path_walk walk;
    path_walk* const saved;
    explicit path_scope(const guest_cost_state& cost) noexcept : saved(current_path) {
        if (cost.path) {
            walk.graph = cost.path;
            walk.node = cost.path->entry.next;
            walk.cycles = cost.path->entry.cycles;
            current_path = &walk;
        } else {
            current_path = nullptr;
        }
    }
    ~path_scope() { current_path = saved; }
    path_scope(const path_scope&) = delete;
    path_scope& operator=(const path_scope&) = delete;
};
// True if the walk ended exactly on a return of the original (no outcome missing, none in excess).
inline bool path_complete(const path_walk& w) noexcept {
    return w.graph && !w.error && w.node == path_return;
}
// charge_guest_cost for a call whose path was walked (`scope`): the cycles of the walk when the function has a graph
// and the walk is complete; otherwise as charge_guest_cost, and a path error is counted when there was a graph.
void charge_guest_cost(GcrtCpu* g, guest_cost_state& cost, const path_scope& scope) noexcept;
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
#if defined(GC_DECOMP_ORACLE)
// Oracle builds: a call of decompiled code to the function at `address` (call_guest, call_address, call): run_guest,
// except while the caller is being compared, where the oracle compares it with the original's call and replays it
// (`argument_gprs`, `argument_fprs`: bit n = rn / fn holds an argument of the call's C++ type).
bool call_function(GcrtCpu* g, u32 address, u32 argument_gprs, u32 argument_fprs) noexcept;
// True while the oracle compares a decompiled function (its calls must go through call_function).
bool oracle_replaying() noexcept;
#else
// Other builds: no oracle, the calls are plain (the tests below fold away at compile time; nothing of the oracle is
// left in a production executable).
constexpr bool oracle_replaying() noexcept {
    return false;
}
#endif
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
// The entry of the registered function declined because its original runs (bypass_address): `lr` is the return
// address that original was entered with (the nested run's), which a non-leaf original saves in its caller's frame.
void oracle_bypassed_entry(u32 lr) noexcept;
#endif

} // namespace detail

// HLE entry of the decompiled function Fn (recompiled -> decompiled).
template <auto Fn> bool entry(GcrtCpu* g, u32 address) {
    if (address == detail::bypass_address) {
#if defined(GC_DECOMP_ORACLE)
        detail::oracle_bypassed_entry(*registers_of(g).lr); // the LR the original runs with (gc_oracle.cpp)
#endif
        return false; // cross-check or oracle in progress: the recompiled original runs
    }
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
    {
        const detail::path_scope path(detail::guest_cost<Fn>); // the walk of the original's path (gc_path.h)
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
        if (detail::guest_cost_enabled)
            detail::charge_guest_cost(g, detail::guest_cost<Fn>, path);
    }
    *r.pc = return_address;
    return true;
}

template <auto Fn> export_record make_export(const char* name, u32 flags) {
    using traits = detail::function_traits<Fn>;
    detail::export_name<Fn> = name;
    return export_record{name,
                         &entry<Fn>,
                         reinterpret_cast<void (*)()>(Fn),
                         &detail::signature_tag<typename traits::pointer>::id,
                         flags,
                         &detail::call_count<Fn>,
                         detail::result_kind_of<typename traits::result>(),
                         &detail::guest_cost<Fn>};
}

// Calls the guest function at `address` (decompiled -> recompiled). R: void or a value type (no hidden structure).
template <class R, class... A> R call_guest(u32 address, A... args) {
    (detail::check_argument<A>(), ...);
    detail::check_result<R>();
    static_assert(!detail::is_hidden_struct_v<R>, "gc_abi: call_guest does not return structures yet");
    // static: read by the lambdas below (oracle builds).
    static constexpr auto alloc = detail::allocation<A...>::compute(3u);
    static_assert(alloc.next_gpr <= 11u && alloc.next_fpr <= 9u, "gc_abi: stack arguments not supported");
    GcrtCpu* g = detail::current_cpu();
    const registers r = registers_of(g);
    unsigned i = 0;
    ((detail::write_value<A>(r, alloc.slots[i++], args)), ...);
    (void)i;
#if defined(GC_DECOMP_ORACLE)
    // Argument registers of the call's C++ type, for the oracle's comparison of the call (computed at compile time).
    constexpr u32 gprs = [] {
        u32 m = 0;
        for (std::size_t k = 0; k < sizeof...(A); k++)
            if (alloc.slots[k].kind != detail::slot_fpr)
                m |= (alloc.slots[k].kind == detail::slot_pair ? 3u : 1u) << alloc.slots[k].reg;
        return m;
    }();
    constexpr u32 fprs = [] {
        u32 m = 0;
        for (std::size_t k = 0; k < sizeof...(A); k++)
            if (alloc.slots[k].kind == detail::slot_fpr)
                m |= 1u << alloc.slots[k].reg;
        return m;
    }();
    detail::call_function(g, address, gprs, fprs);
#else
    detail::run_guest(g, address);
#endif
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
    // While the oracle compares the caller (oracle builds only), the call goes through call_guest, so that the oracle
    // records it.
    if (const export_record* e = find(address);
        e && !detail::oracle_replaying() && !(detail::fpu_off() && detail::registered_uses_fpu(address))) {
        using pointer = R (*)(A...);
        if (e->signature == &detail::signature_tag<pointer>::id) {
            // A plain C++ call: the callee's guest cost is charged here, its entry is not reached.
            static const guest_cost_state no_cost{};
            const detail::path_scope path(e->cost ? *e->cost : no_cost); // the callee's own walk (gc_path.h)
            if constexpr (std::is_void_v<R>) {
                reinterpret_cast<pointer>(e->direct)(args...);
                if (detail::guest_cost_enabled && e->cost)
                    detail::charge_guest_cost(detail::current_cpu(), *e->cost, path);
                return;
            } else {
                const R value = reinterpret_cast<pointer>(e->direct)(args...);
                if (detail::guest_cost_enabled && e->cost)
                    detail::charge_guest_cost(detail::current_cpu(), *e->cost, path);
                return value;
            }
        }
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
#if defined(GC_DECOMP_ORACLE)
    if (detail::oracle_replaying()) {
        static const u32 address = detail::address_of_function(reinterpret_cast<void (*)()>(Fn));
        return detail::call_through_oracle(Fn, address, args...);
    }
#endif
    // A plain C++ call: Fn's entry is not reached, so its guest cost is charged here (as the original caller ran the
    // original callee). Fn must be registered in the build for that (its cost is set by install): warned otherwise.
    if (!detail::guest_cost<Fn>.registered)
        detail::note_unregistered_call(detail::guest_cost<Fn>, detail::export_name<Fn>);
    const detail::path_scope path(detail::guest_cost<Fn>); // Fn's own walk (gc_path.h)
    if constexpr (std::is_void_v<decltype(Fn(args...))>) {
        Fn(args...);
        if (detail::guest_cost_enabled)
            detail::charge_guest_cost(detail::current_cpu(), detail::guest_cost<Fn>, path);
    } else {
        auto value = Fn(args...);
        if (detail::guest_cost_enabled)
            detail::charge_guest_cost(detail::current_cpu(), detail::guest_cost<Fn>, path);
        return value;
    }
}

#if defined(GC_DECOMP_ORACLE_NEGATIVE)
// Negative test of the oracle only (tools/build.py --oracle-negative, never in another build): deliberately wrong
// variants of decompiled functions use these to stand for a gateway that would not preserve a register.
namespace testing {
void clobber_gpr(unsigned reg, u32 value) noexcept;
void clobber_cr(u32 value) noexcept;
// A store of `value` at (entry r1 + `offset`), through gc_mem.h: a variant that writes into its caller's frame.
void store_word_at_entry_sp(u32 offset, u32 value) noexcept;
} // namespace testing
#endif

} // namespace gc::abi

// Defines the export record of a decompiled function (in its source file). `name` is the identifier listed in the
// game's functions.tsv; the build generates the table that gives it its address in each version.
#define GC_DECOMP_EXPORT(name, function, flags)                                                                        \
    extern const ::gc::abi::export_record gc_decomp_export_##name;                                                    \
    const ::gc::abi::export_record gc_decomp_export_##name = ::gc::abi::make_export<&function>(#name, flags)

#endif // GC_ABI_H
