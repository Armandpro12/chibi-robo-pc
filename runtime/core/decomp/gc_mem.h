// Guest memory type layer for decompiled code (generic GameCube runtime; no reference to any specific game).
//
// Why this layer exists
//   While a single function of a game is still recompiled, every function (recompiled or decompiled) shares the
//   console's memory: 24 MB of RAM, big-endian bytes, 32-bit pointers. Decompiled code must read and write that
//   memory, but never through raw addresses (`*(u32*)(base + 0x1C)`): it goes through the types below. When the
//   whole game is decompiled (native memory), defining GC_NATIVE_MEMORY turns these types into plain host types
//   (`be<T>` = T, `ptr<T>` = T*) without changing the decompiled code.
//
// What the layer depends on
//   Only a host pointer to the guest RAM and its size, given once by the runtime (`gc::init_guest_memory`). It does
//   not know the CPU layer (no register structure, no DolRecomp header): it must survive the removal of the virtual
//   CPU. Guest addresses of game globals are not in this header either: the game provides a table per version
//   (`gc::set_global_table`).
//
// Public API (what decompiled code may use; every name exists in both modes)
//   gc::be<T>            big-endian scalar in guest memory (fixed-width integers, enums, float, double; not bool)
//   gc::ptr<T>           32-bit guest pointer to T (T is a guest-layout type: be<...>, ptr<...>, guest structs;
//                        u8/s8/char may be pointed to but not dereferenced in guest mode: see the rules below)
//   gc::array<T, N>      fixed-size array with the guest layout
//   gc::bitword<U>       storage word of bit-fields, accessed with gc::bits<U, First, Width, T> descriptors
//   gc::global<T, Id>    a game global, resolved through the game's per-version address table
//   gc::load(x), gc::store(x, v), gc::load_raw(x), gc::store_raw(x, bits)
//                        explicit read / write of a scalar field (value, or its bits in host order)
//   gc::address_of(obj)  pointer to an object that lives in guest memory (C++ `&obj`)
//   gc::to_host(p)       host pointer for a guest pointer (for host code that needs a T*)
//   gc::ptr_cast<U>(p), gc::static_ptr_cast<U>(p)   pointer conversions (reinterpret / base-to-derived)
//   gc::marker_ptr<T>(v), gc::is_marker_ptr(p, v)   marker pointer values such as C `(T*)-1` (never dereferenced)
// Guest-mode only (gateway, oracle and table code; they do not exist in native mode on purpose):
//   gc::guest_address(p), gc::ptr_from_guest_address<T>(address), gc::init_guest_memory, gc::set_global_table,
//   gc::set_guest_store_hook (the runtime sees each store, see "Store hook"),
//   gc::host_reuses_tail_padding (host ABI property, see "Layout")
// Everything in gc::detail is internal: decompiled code never uses it.
//
// Rules for decompiled code (the same source must compile and behave the same in both modes)
//   - Fields are used like plain variables (`s->a + 1`, `s->a = v`, `s->a += v`), or through the free functions
//     gc::load / gc::store / gc::load_raw / gc::store_raw. be<T> has no public member function: `x.load()` would not
//     exist in native mode, where be<T> is T.
//   - Varargs and conditional expressions need an explicit value: `printf("%u", gc::load(s->a))`, not
//     `printf("%u", s->a)` (in guest mode that passes the big-endian object itself, i.e. byte-swapped bits; clang's
//     -Wformat, on with /W4, warns only for functions declared with a format attribute; with GC_MEM_OBSERVER it does
//     not compile); `c ? gc::load(s->a) : 0u`, not `c ? s->a : 0u` (refused in guest mode as ambiguous: be<u32>
//     converts to u32 and u32 to be<u32>).
//   - Every field of a guest structure is a layer type: be<...> (be<u8> and be<s8> included), ptr<...>, array<...>,
//     bitword<...> or another guest structure. A plain u8/s8 field has the right layout and value, but its accesses
//     are plain C++ accesses that GC_MEM_OBSERVER cannot see: the oracle would miss them (a byte written there is
//     not compared, a byte read there is missing from the captured case). Not enforceable for a field (no
//     reflection in C++20): to be read in review. Byte buffers are ptr<be<u8>> / array<be<u8>, N>: in guest mode,
//     dereferencing a ptr<u8>/ptr<s8>/ptr<char> (*, [], ->) and array<u8|s8|char, N>, global<u8|s8|char, Id> are
//     refused at compile time for that reason (D1 mission 4); a ptr<char> may still be held, compared, converted
//     and moved by pointer arithmetic (e.g. a name returned by address).
//   - Structure assignment `*a = *b` copies field by field in guest mode (with and without GC_MEM_OBSERVER), never
//     the padding bytes. In native mode the assignment is the compiler's trivial one, which may copy the padding
//     bytes too. The original code may have copied the whole block, padding included [hypothesis: not checked on
//     CodeWarrior output]; the oracle may then see padding bytes that differ. Copy construction
//     (`Thing local = *a;`) is trivial without GC_MEM_OBSERVER (the compiler may copy the whole block into the host
//     local) and field by field with it: the values written back to guest memory are the same, only the observed
//     reads differ.
//   - Pointer difference `p - q` is a std::int32_t in guest mode (the console's ptrdiff_t is 32 bits wide, its
//     pointers being 32 bits) and std::ptrdiff_t (64 bits) in native mode. Where it meets unsigned arithmetic the
//     result differs (`u32 n / (p - q)` is an unsigned 32-bit division on the console, a signed 64-bit one with a
//     64-bit ptrdiff_t): write `static_cast<s32>(p - q)`, which gives the console's result in both modes.
//   - Game structures are described by composition (`Base base;` as first member) or with explicit fields, not by
//     C++ inheritance, unless the derived layout is checked (see "Layout" below).
//   - A guest class is an aggregate or is trivially default constructible: a class that declares a constructor also
//     declares `Thing() = default;`. (This is also how virtual bases, which have no type trait, are refused.)
//   - Enums stored in guest memory declare their underlying type (`enum class Kind : u16`): the size CodeWarrior gives
//     to an enum without one is not known here [hypothesis].
//
// Options (preprocessor)
//   GC_NATIVE_MEMORY   native mode (final switch to native memory): plain host types, no guest RAM.
//   GC_MEM_OBSERVER    guest mode only: every load and store of a gc::be (hence of ptr and bitword) that lies in
//                      guest RAM is reported to the observer of the calling thread (oracle executable only).
//   GC_MEM_COMPOSITION_ONLY
//                      guest mode: guest structures are described by composition only; C++ inheritance is refused
//                      where it meets the layer (see "Layout"). Required on a host whose ABI reuses tail padding;
//                      may be set on any host to check that the code would build there.
//
// Layout (guest mode)
//   Intended rule: a struct made of these types has the size, alignment and field offsets of the same struct compiled
//   for the console. [hypothesis] CodeWarrior uses natural alignment (each scalar aligned on its size, double and u64
//   on 8) and allocates bit-fields from the most significant bit; neither is checked yet on a real structure of a game
//   (US and PAL). The layer only fixes the size and alignment of each type (static_asserts at the end of this file);
//   the rest is the host compiler's layout algorithm, so it also depends on the host C++ ABI:
//   - Microsoft ABI (clang-cl, the compiler this project uses): a derived class starts its own members after the whole
//     base, padding included. The Itanium ABI (GCC and clang on Linux, Android, MinGW; ARM targets such as the PS Vita
//     use a variant of it) reuses the tail padding of a base that is not a C++03 POD, which be<T> makes every guest
//     struct: `struct A { be<u32> x; be<u8> y; }; struct B : A { be<u8> z; };` puts z at 8 with clang-cl and at 5
//     with Itanium. gc::host_reuses_tail_padding tells which kind of host this is (measured on such a pair of
//     structs). On a host that reuses tail padding, guest mode compiles only with GC_MEM_COMPOSITION_ONLY
//     (static_assert at the end of this file); in that mode inheritance is refused where it meets the layer: pointer
//     conversions between a derived class and its base (implicit upcast, gc::static_ptr_cast), and any class that
//     is not standard-layout (a derived class with members in both itself and its base) reached through ptr, array,
//     global or address_of. Code that describes game structures by composition compiles unchanged on both kinds of
//     host.
//     Which of the two CodeWarrior does is itself a [hypothesis] (no reuse expected; to check on a real derived class
//     whose base has tail padding, US and PAL).
//   - Rule: composition, or C++ inheritance with static_assert(sizeof / offsetof) on the derived structure. Games check
//     their own structures with static_assert(sizeof / offsetof) in guest mode only (in native mode pointers are 8
//     bytes). offsetof is only valid on standard-layout structs, another reason to prefer composition.
//   - Polymorphic classes (C++ virtual functions) and virtual bases are refused wherever a guest object is reached
//     (ptr, array, global, address_of, pointer conversions): their host layout has nothing to do with the console's.
//     A game object with a vtable declares its vtable pointer as an explicit ptr field.
//
// Alignment of accesses: a guest pointer may be misaligned (the console accepts misaligned integer loads and stores
// [hypothesis: not checked for every instruction], e.g. when a file parser reads a u32 at an odd offset). C++ formally
// requires an object to be aligned; this layer reads and writes the bytes with memcpy through a byte pointer, which
// clang-cl x64 compiles to ordinary loads and stores that accept any alignment (misaligned accesses are tested).
// Other hosts with strict alignment (32-bit ARM) are not checked [hypothesis].
#ifndef GC_MEM_H
#define GC_MEM_H

#include <bit>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

#if defined(GC_NATIVE_MEMORY) && defined(GC_MEM_OBSERVER)
#error "GC_MEM_OBSERVER observes guest memory: it cannot be combined with GC_NATIVE_MEMORY"
#endif

namespace gc {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using f32 = float;
using f64 = double;

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "float must be IEEE-754 binary32");
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559, "double must be IEEE-754 binary64");

namespace detail {

// Arithmetic types whose size is the same on every host, named by the <cstdint> types: int16_t, int32_t and int64_t
// are whatever the host defines them as. On Windows (LLP64), int64_t is long long and long (32 bits) is refused. On
// 64-bit Linux and macOS (LP64), int64_t is long [verified for clang's x86_64-linux-gnu target: __INT64_TYPE__ is
// long; glibc's typedef follows it, hypothesis]: there be<long> is accepted (as be<s64>, 8 bytes) and be<long long>
// is refused. The console's long is 32 bits, so decompiled code never writes long or long long: it writes s32, s64.
// wchar_t and char16_t are not accepted either (their size or identity varies by host).
template <class T>
inline constexpr bool is_fixed_width_arithmetic_v =
    std::is_same_v<T, char> || std::is_same_v<T, signed char> || std::is_same_v<T, unsigned char> ||
    std::is_same_v<T, std::int16_t> || std::is_same_v<T, std::uint16_t> || std::is_same_v<T, std::int32_t> ||
    std::is_same_v<T, std::uint32_t> || std::is_same_v<T, std::int64_t> || std::is_same_v<T, std::uint64_t> ||
    std::is_same_v<T, float> || std::is_same_v<T, double>;

template <class T, bool = std::is_enum_v<T>>
struct is_fixed_width_scalar : std::bool_constant<is_fixed_width_arithmetic_v<T>> {};
template <class T>
struct is_fixed_width_scalar<T, true> : std::bool_constant<is_fixed_width_arithmetic_v<std::underlying_type_t<T>>> {};

} // namespace detail

// Scalars that may live in guest memory: fixed-width integers, enums whose underlying type is one, float, double
// (cv-qualified types are not scalars of memory: write `const be<T>`). bool is excluded on purpose: a guest byte
// holding 2 read as a C++ bool would come back as 1 when stored again; game booleans are declared as be<u8> (or u8)
// and compared explicitly.
template <class T>
concept memory_scalar = detail::is_fixed_width_scalar<T>::value;

namespace detail {

template <std::size_t N> struct uint_of;
template <> struct uint_of<1> { using type = u8; };
template <> struct uint_of<2> { using type = u16; };
template <> struct uint_of<4> { using type = u32; };
template <> struct uint_of<8> { using type = u64; };
template <std::size_t N> using uint_of_t = typename uint_of<N>::type;

// Byte swap without compiler builtins (used when they are missing; clang and GCC turn it into one bswap anyway).
template <class U> constexpr U byteswap_portable(U v) noexcept {
    static_assert(std::is_unsigned_v<U>);
    U result = 0;
    for (std::size_t i = 0; i < sizeof(U); ++i) {
        // In 64 bits: no promotion of a narrow U to int, hence no signed intermediate.
        result = static_cast<U>((static_cast<std::uint64_t>(result) << 8) |
                                ((static_cast<std::uint64_t>(v) >> (8 * i)) & 0xFFu));
    }
    return result;
}

#if defined(__has_builtin)
#if __has_builtin(__builtin_bswap16) && __has_builtin(__builtin_bswap32) && __has_builtin(__builtin_bswap64)
#define GC_MEM_DETAIL_BSWAP_BUILTINS 1
#endif
#endif

// Guest byte order (big-endian) <-> host value. Identity on a big-endian host.
template <class U> constexpr U to_big_endian(U v) noexcept {
    static_assert(std::is_unsigned_v<U>);
    if constexpr (sizeof(U) == 1 || std::endian::native == std::endian::big) {
        return v;
    } else {
#if defined(GC_MEM_DETAIL_BSWAP_BUILTINS)
        if constexpr (sizeof(U) == 2) {
            return static_cast<U>(__builtin_bswap16(v));
        } else if constexpr (sizeof(U) == 4) {
            return static_cast<U>(__builtin_bswap32(v));
        } else {
            return static_cast<U>(__builtin_bswap64(v));
        }
#else
        return byteswap_portable(v);
#endif
    }
}

// The bytes [p, p + size) read as a big-endian unsigned integer (what the console CPU sees).
inline std::uint64_t big_endian_bytes(const void* p, std::uint32_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(p);
    std::uint64_t value = 0;
    for (std::uint32_t i = 0; i < size; ++i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

// Underlying unsigned representation of an integral or enum type (used by bit-fields).
template <class T> struct underlying_of { using type = T; };
template <class T>
    requires std::is_enum_v<T>
struct underlying_of<T> {
    using type = std::underlying_type_t<T>;
};
template <class T> using underlying_of_t = typename underlying_of<T>::type;

} // namespace detail

// Called when decompiled code reaches memory that is not guest RAM (null or wild pointer, unresolved global).
// The default handler prints the reason and aborts. A replacement must not return (it may throw, e.g. in tests);
// if it returns, the program aborts anyway.
using memory_fault_handler = void (*)(const char* reason, std::uint32_t address, std::uint32_t size);

namespace detail {

inline void default_fault_handler(const char* reason, std::uint32_t address, std::uint32_t size) {
    std::fprintf(stderr, "gc_mem: %s (guest address 0x%08X, %u bytes)\n", reason, static_cast<unsigned>(address),
                 static_cast<unsigned>(size));
    std::fflush(stderr);
}

inline memory_fault_handler g_fault_handler = &default_fault_handler;

[[noreturn]] inline void fault(const char* reason, std::uint32_t address, std::uint32_t size) {
    g_fault_handler(reason, address, size);
    std::abort();
}

} // namespace detail

inline memory_fault_handler set_memory_fault_handler(memory_fault_handler handler) noexcept {
    memory_fault_handler previous = detail::g_fault_handler;
    detail::g_fault_handler = handler ? handler : &detail::default_fault_handler;
    return previous;
}

#if !defined(GC_NATIVE_MEMORY)

// ---------------------------------------------------------------------------------------------------------------
// Guest RAM
// ---------------------------------------------------------------------------------------------------------------

namespace detail {

// The two views of main RAM on the console: cached (0x80000000) and uncached (0xC0000000) mirrors. The CPU layer of
// this runtime serves both from the same host buffer (gcrt_cpu_inline.h, fast path); this layer does the same.
inline constexpr std::uint32_t kCachedBase = 0x80000000u;
inline constexpr std::uint32_t kUncachedBase = 0xC0000000u;
inline constexpr std::uint32_t kMaxRamSize = 0x40000000u; // the mirrors must not overlap

struct guest_memory {
    std::byte* host = nullptr;
    std::uint32_t size = 0;
};
inline guest_memory g_memory;

// Host address of `bytes` bytes at guest address `address`; faults if they are not entirely inside guest RAM.
inline std::byte* host_of(std::uint32_t address, std::uint32_t bytes) {
    const std::uint32_t size = g_memory.size;
    std::uint32_t offset = address - kCachedBase;
    if (offset < size && bytes <= size - offset) {
        return g_memory.host + offset;
    }
    offset = address - kUncachedBase;
    if (offset < size && bytes <= size - offset) {
        return g_memory.host + offset;
    }
    fault(address == 0 ? "null guest pointer dereferenced" : "guest address outside guest RAM", address, bytes);
}

// True (and the cached-mirror guest address in *address) when host bytes [p, p + bytes) lie in guest RAM.
inline bool guest_of(const void* p, std::uint32_t bytes, std::uint32_t* address) noexcept {
    const auto host = reinterpret_cast<std::uintptr_t>(p);
    const auto base = reinterpret_cast<std::uintptr_t>(g_memory.host);
    if (g_memory.host == nullptr || host < base) {
        return false;
    }
    const std::uintptr_t offset = host - base;
    if (offset >= g_memory.size || bytes > g_memory.size - offset) {
        return false;
    }
    *address = kCachedBase + static_cast<std::uint32_t>(offset);
    return true;
}

inline std::uint32_t guest_of_checked(const void* p, std::uint32_t bytes) {
    std::uint32_t address = 0;
    if (!guest_of(p, bytes, &address)) {
        fault("host object is not in guest RAM", 0, bytes);
    }
    return address;
}

} // namespace detail

// Gives the layer the host buffer that holds guest RAM (set once by the runtime, before any decompiled code runs).
// `size` is the size of main RAM, at most 1 GiB (the two mirrors must not overlap). The buffer must be aligned on 8
// bytes, so that every naturally aligned guest object (up to double and u64) is aligned on the host too.
inline void init_guest_memory(void* host_ram, std::uint32_t size) {
    if (host_ram == nullptr || size == 0 || size > detail::kMaxRamSize) {
        detail::fault("init_guest_memory: invalid guest RAM", 0, size);
    }
    if (reinterpret_cast<std::uintptr_t>(host_ram) % 8 != 0) {
        detail::fault("init_guest_memory: guest RAM buffer not aligned on 8 bytes", 0, size);
    }
    detail::g_memory.host = static_cast<std::byte*>(host_ram);
    detail::g_memory.size = size;
}

// Forgets the guest RAM (tests, shutdown).
inline void reset_guest_memory() noexcept {
    detail::g_memory = {};
}

// ---------------------------------------------------------------------------------------------------------------
// Store hook (guest mode)
// ---------------------------------------------------------------------------------------------------------------

// While decompiled and recompiled functions share guest RAM, the runtime may need to see every store into it, the
// decompiled ones included, exactly as it sees the stores of recompiled code: a CPU layer clears a lwarx reservation
// on a store to the reserved block and calls its write journal before each store (idle detection, oracle). The
// layer itself knows nothing of a CPU: the runtime installs one hook, called before every store into guest RAM made
// through be<T> (hence ptr, array, bitword, global), with the cached-mirror guest address of the stored object and
// its size in bytes (1, 2, 4 or 8). Stores to be<T> objects outside guest RAM (host locals) do not call it. One hook
// per process, not per thread: guest RAM belongs to the one emulated CPU. Without a hook a store costs one load and
// one test more than a plain memcpy.
using guest_store_hook = void (*)(std::uint32_t address, std::uint32_t size) noexcept;

namespace detail {
inline guest_store_hook g_store_hook = nullptr;

inline void before_guest_store(const void* object, std::uint32_t size) noexcept {
    if (const guest_store_hook hook = g_store_hook) {
        std::uint32_t address = 0;
        if (guest_of(object, size, &address)) {
            hook(address, size);
        }
    }
}
} // namespace detail

// Installs the store hook (nullptr: none); returns the previous one.
inline guest_store_hook set_guest_store_hook(guest_store_hook hook) noexcept {
    const guest_store_hook previous = detail::g_store_hook;
    detail::g_store_hook = hook;
    return previous;
}

// ---------------------------------------------------------------------------------------------------------------
// Observer (oracle only)
// ---------------------------------------------------------------------------------------------------------------

#if defined(GC_MEM_OBSERVER)

// Receives the guest accesses made through this layer. `value`, `old_value`, `new_value`: the accessed bytes read as a
// big-endian unsigned integer (what the console CPU sees), zero-extended to 64 bits. Accesses to be<T> objects that
// are not in guest RAM (local copies on the host stack) are not reported. Accesses made by the observer itself (from
// on_read / on_write, on the same thread) are not reported either: an observer may read guest memory through this
// layer (gc::load) without calling itself again. The accessors that call it are noexcept: an observer that throws
// terminates the program.
struct memory_observer {
    virtual void on_read(std::uint32_t address, std::uint32_t size, std::uint64_t value) = 0;
    virtual void on_write(std::uint32_t address, std::uint32_t size, std::uint64_t old_value,
                          std::uint64_t new_value) = 0;

protected:
    ~memory_observer() = default;
};

namespace detail {
// Per thread: the oracle observes the decompiled function on the thread that runs it, not other threads that may
// read guest memory at the same time.
inline thread_local memory_observer* g_observer = nullptr;
// True while the observer of this thread runs: its own accesses are not reported (no endless recursion).
inline thread_local bool g_in_observer = false;

// Marks the observer as running for the duration of one call.
struct observer_call {
    observer_call() noexcept { g_in_observer = true; }
    ~observer_call() { g_in_observer = false; }
    observer_call(const observer_call&) = delete;
    observer_call& operator=(const observer_call&) = delete;
};

inline void observe_read(const void* object, std::uint32_t size, std::uint64_t value) {
    std::uint32_t address = 0;
    if (memory_observer* observer = g_observer; observer && !g_in_observer && guest_of(object, size, &address)) {
        const observer_call running;
        observer->on_read(address, size, value);
    }
}

// Called before the store: the old value is read from guest RAM only once the object is known to be there (never
// from a host local, which may be uninitialized).
inline void observe_write(const void* object, std::uint32_t size, std::uint64_t new_value) {
    std::uint32_t address = 0;
    if (memory_observer* observer = g_observer; observer && !g_in_observer && guest_of(object, size, &address)) {
        const observer_call running;
        observer->on_write(address, size, big_endian_bytes(object, size), new_value);
    }
}
} // namespace detail

// Installs the observer of the calling thread (nullptr: none); returns the previous one.
inline memory_observer* set_memory_observer(memory_observer* observer) noexcept {
    memory_observer* previous = detail::g_observer;
    detail::g_observer = observer;
    return previous;
}

#endif // GC_MEM_OBSERVER

// ---------------------------------------------------------------------------------------------------------------
// be<T>: big-endian scalar
// ---------------------------------------------------------------------------------------------------------------

namespace detail {
struct be_access;
} // namespace detail

template <memory_scalar T> struct be {
    using value_type = T;
    using raw_type = detail::uint_of_t<sizeof(T)>;

    be() = default; // trivial: guest structures overlay guest RAM without being constructed
    constexpr be(T value) noexcept : stored_(detail::to_big_endian(std::bit_cast<raw_type>(value))) {}
#if defined(GC_MEM_OBSERVER)
    // Copy construction reads the source: reported like any other read.
    constexpr be(const be& other) noexcept : stored_(other.read_stored()) {}
#else
    // Trivial without the observer, so that ptr<T> and be<T> stay register-passed by value (Microsoft x64 ABI).
    be(const be&) = default;
#endif

    // Copy between two be<T>: the bytes are copied as they are (a float NaN payload is kept bit for bit).
    constexpr be& operator=(const be& other) noexcept {
        write_stored(other.read_stored());
        return *this;
    }

    constexpr be& operator=(T value) noexcept {
        store(value);
        return *this;
    }

    constexpr operator T() const noexcept { return load(); }

    // Compound assignments: the same as the C++ operator on a T variable. The operand keeps its own type, the usual
    // arithmetic conversions apply to (T value, operand), and the result is converted back to T: a be<u8> holding 200
    // `/= 300` gives 0, a be<s32> holding 10 `*= 0.5` gives 5, a be<f32> `+= 1.0` adds in double.
    // The signed/unsigned conversions inside them (`be<u32> x; x += 1;` converts 1 to unsigned) are the C++ operator's
    // own, wanted here; -Wsign-conversion is silenced for these lines only, so that it reports nothing in guest mode
    // that it would not report in native mode (where `x += 1` on a u32 variable is plain C++).
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
    template <class V>
        requires(!std::is_enum_v<T> && requires(T a, const V& b) { a + b; })
    constexpr be& operator+=(const V& v) noexcept {
        store(static_cast<T>(load() + v));
        return *this;
    }
    template <class V>
        requires(!std::is_enum_v<T> && requires(T a, const V& b) { a - b; })
    constexpr be& operator-=(const V& v) noexcept {
        store(static_cast<T>(load() - v));
        return *this;
    }
    template <class V>
        requires(!std::is_enum_v<T> && requires(T a, const V& b) { a * b; })
    constexpr be& operator*=(const V& v) noexcept {
        store(static_cast<T>(load() * v));
        return *this;
    }
    template <class V>
        requires(!std::is_enum_v<T> && requires(T a, const V& b) { a / b; })
    constexpr be& operator/=(const V& v) noexcept {
        store(static_cast<T>(load() / v));
        return *this;
    }
    template <class V>
        requires(std::is_integral_v<T> && requires(T a, const V& b) { a % b; })
    constexpr be& operator%=(const V& v) noexcept {
        store(static_cast<T>(load() % v));
        return *this;
    }
    template <class V>
        requires(std::is_integral_v<T> && requires(T a, const V& b) { a & b; })
    constexpr be& operator&=(const V& v) noexcept {
        store(static_cast<T>(load() & v));
        return *this;
    }
    template <class V>
        requires(std::is_integral_v<T> && requires(T a, const V& b) { a | b; })
    constexpr be& operator|=(const V& v) noexcept {
        store(static_cast<T>(load() | v));
        return *this;
    }
    template <class V>
        requires(std::is_integral_v<T> && requires(T a, const V& b) { a ^ b; })
    constexpr be& operator^=(const V& v) noexcept {
        store(static_cast<T>(load() ^ v));
        return *this;
    }
    template <class V>
        requires(std::is_integral_v<T> && requires(T a, const V& b) { a << b; })
    constexpr be& operator<<=(const V& n) noexcept {
        store(static_cast<T>(load() << n));
        return *this;
    }
    template <class V>
        requires(std::is_integral_v<T> && requires(T a, const V& b) { a >> b; })
    constexpr be& operator>>=(const V& n) noexcept {
        store(static_cast<T>(load() >> n));
        return *this;
    }
    constexpr be& operator++() noexcept
        requires(!std::is_enum_v<T>)
    {
        store(static_cast<T>(load() + 1));
        return *this;
    }
    constexpr be& operator--() noexcept
        requires(!std::is_enum_v<T>)
    {
        store(static_cast<T>(load() - 1));
        return *this;
    }
    constexpr T operator++(int) noexcept
        requires(!std::is_enum_v<T>)
    {
        const T old = load();
        store(static_cast<T>(old + 1));
        return old;
    }
    constexpr T operator--(int) noexcept
        requires(!std::is_enum_v<T>)
    {
        const T old = load();
        store(static_cast<T>(old - 1));
        return old;
    }
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

private:
    // Member accessors are private on purpose: decompiled code uses gc::load / gc::store / gc::load_raw /
    // gc::store_raw, which also exist in native mode (where be<T> is T and has no members).
    friend struct detail::be_access;

    constexpr T load() const noexcept { return std::bit_cast<T>(raw()); }
    constexpr void store(T value) noexcept { set_raw(std::bit_cast<raw_type>(value)); }
    // The bits of the value (for a float: its IEEE-754 encoding), in host order.
    constexpr raw_type raw() const noexcept { return detail::to_big_endian(read_stored()); }
    constexpr void set_raw(raw_type value) noexcept { write_stored(detail::to_big_endian(value)); }

    // The stored bytes (guest order), read or written through a byte pointer (no alignment assumed, see the top of
    // this file); the observer, if any, sees the access.
    constexpr raw_type read_stored() const noexcept {
        if (std::is_constant_evaluated()) {
            return stored_;
        }
        raw_type stored;
        std::memcpy(&stored, reinterpret_cast<const unsigned char*>(&stored_), sizeof(stored));
#if defined(GC_MEM_OBSERVER)
        detail::observe_read(this, sizeof(T), detail::to_big_endian(stored));
#endif
        return stored;
    }
    constexpr void write_stored(raw_type stored) noexcept {
        if (std::is_constant_evaluated()) {
            stored_ = stored;
            return;
        }
        // The runtime's hook first (reservation, write journal: before the store, as the CPU layer does for
        // recompiled code), then the observer, which reads the old value; both before the bytes change.
        detail::before_guest_store(this, sizeof(T));
#if defined(GC_MEM_OBSERVER)
        detail::observe_write(this, sizeof(T), detail::to_big_endian(stored));
#endif
        std::memcpy(reinterpret_cast<unsigned char*>(&stored_), &stored, sizeof(stored));
    }

    // The value's bytes in guest order. Alignment of T, so that structures keep the console's natural layout.
    alignas(alignof(T)) raw_type stored_;
};

namespace detail {
struct be_access {
    template <class T> static constexpr T load(const be<T>& x) noexcept { return x.load(); }
    template <class T> static constexpr void store(be<T>& x, T value) noexcept { x.store(value); }
    template <class T> static constexpr typename be<T>::raw_type raw(const be<T>& x) noexcept { return x.raw(); }
    template <class T> static constexpr void set_raw(be<T>& x, typename be<T>::raw_type bits) noexcept {
        x.set_raw(bits);
    }
};
} // namespace detail

// Explicit access to a guest scalar (guest-mode overloads; the plain-scalar overloads after the native-mode section
// serve both modes).
template <memory_scalar T> constexpr T load(const be<T>& x) noexcept {
    return detail::be_access::load(x);
}
template <memory_scalar T> constexpr void store(be<T>& x, std::type_identity_t<T> value) noexcept {
    detail::be_access::store(x, value);
}
template <memory_scalar T> constexpr detail::uint_of_t<sizeof(T)> load_raw(const be<T>& x) noexcept {
    return detail::be_access::raw(x);
}
template <memory_scalar T>
constexpr void store_raw(be<T>& x, std::type_identity_t<detail::uint_of_t<sizeof(T)>> bits) noexcept {
    detail::be_access::set_raw(x, bits);
}

// ---------------------------------------------------------------------------------------------------------------
// Host ABI and guest classes (see "Layout" at the top)
// ---------------------------------------------------------------------------------------------------------------

namespace detail::layout_probe {
struct base {
    be<u32> word;
    be<u8> byte; // followed by 3 bytes of tail padding
};
struct derived : base {
    be<u8> extra; // offset 8 (whole base first) or 5 (tail padding reused)
};
} // namespace detail::layout_probe

// True when this host's C++ ABI places a derived class's members in the tail padding of a guest-structure base
// (Itanium ABI: GCC and clang on Linux, Android, MinGW, ARM variants); false when it places them after the whole base
// (Microsoft ABI: clang-cl). Measured on the probe above, made of be<T> fields like every guest structure.
inline constexpr bool host_reuses_tail_padding = sizeof(detail::layout_probe::derived) != 12;

namespace detail {

#if defined(GC_MEM_COMPOSITION_ONLY)
inline constexpr bool composition_only = true;
#else
inline constexpr bool composition_only = false;
#endif
// Dependent form, for static_asserts inside templates.
template <class> inline constexpr bool composition_only_v = composition_only;

// Checks made wherever a guest object is reached (ptr dereference and arithmetic, address_of, array, global), where
// the type is complete. Only class types are concerned.
template <class T, bool = std::is_class_v<T>> struct guest_object_check : std::true_type {};
template <class T> struct guest_object_check<T, true> : std::true_type {
    static_assert(!std::is_polymorphic_v<T>,
                  "gc: a class with virtual functions has a host layout (vtable pointer), not the guest one; "
                  "declare the guest vtable pointer as an explicit gc::ptr field instead");
    // There is no type trait for "has a virtual base". A virtual base makes every constructor of the class
    // non-trivial and the class not an aggregate, while a guest class is one or the other (rules at the top).
    static_assert(std::is_aggregate_v<T> || std::is_trivially_default_constructible_v<T>,
                  "gc: a guest class must be an aggregate or trivially default constructible: virtual base classes "
                  "have no guest layout (a class with its own constructors also declares T() = default)");
    static_assert(!composition_only_v<T> || std::is_standard_layout_v<T>,
                  "gc (GC_MEM_COMPOSITION_ONLY): this guest class is not standard-layout (members in both a derived "
                  "class and its base, whose offsets depend on the host ABI): describe it by composition");
};
template <class T> inline constexpr bool guest_object_ok_v = guest_object_check<std::remove_cv_t<T>>::value;

} // namespace detail

// ---------------------------------------------------------------------------------------------------------------
// ptr<T>: 32-bit guest pointer
// ---------------------------------------------------------------------------------------------------------------

template <class T> class ptr;

namespace detail {

template <class T> struct is_guest_ptr : std::false_type {};
template <class T> struct is_guest_ptr<ptr<T>> : std::true_type {};

// A multi-byte host scalar or a host pointer in guest memory would be read in the wrong byte order or size: guest
// structures use be<T> and ptr<T> instead. Single bytes (u8, s8, char) have no byte order and are allowed.
// (sizeof is only taken for scalars, so that ptr<Node> works inside the definition of an incomplete Node.)
template <class T, bool = std::is_arithmetic_v<std::remove_cv_t<T>> || std::is_enum_v<std::remove_cv_t<T>>>
struct is_multibyte_scalar : std::false_type {};
template <class T> struct is_multibyte_scalar<T, true> : std::bool_constant<(sizeof(T) > 1)> {};

template <class T>
inline constexpr bool is_host_only_v = std::is_pointer_v<std::remove_cv_t<T>> ||
                                       std::is_same_v<std::remove_cv_t<T>, bool> || is_multibyte_scalar<T>::value;

// A plain single byte (u8, s8, char): right layout and value, but its accesses are plain C++ accesses the oracle's
// observer (GC_MEM_OBSERVER) cannot see; guest code accesses bytes through be<u8> / be<s8> instead.
template <class T, bool = std::is_arithmetic_v<std::remove_cv_t<T>>> struct is_plain_byte : std::false_type {};
template <class T> struct is_plain_byte<T, true> : std::bool_constant<(sizeof(T) == 1)> {};
template <class T> inline constexpr bool is_plain_byte_v = is_plain_byte<T>::value;

// Guest size of the object a pointer designates (dereference, arithmetic, address_of). The type is complete here.
template <class T> constexpr std::uint32_t object_size() noexcept {
    if constexpr (std::is_void_v<T>) {
        return 1;
    } else {
        static_assert(guest_object_ok_v<T>);
        return static_cast<std::uint32_t>(sizeof(T));
    }
}

// Offset added when converting a pointer to Derived into a pointer to Base (non-virtual inheritance): computed on a
// suitably aligned dummy buffer, never on a real object, so converting a wild guest pointer does not fault. It is the
// host layout's offset: see "Layout" at the top of this file.
template <class Base, class Derived> std::uint32_t base_offset() noexcept {
    using base_type = std::remove_cv_t<Base>;
    using derived_type = std::remove_cv_t<Derived>;
    if constexpr (std::is_void_v<base_type> || std::is_same_v<base_type, derived_type>) {
        return 0;
    } else {
        static_assert(!std::is_polymorphic_v<base_type> && !std::is_polymorphic_v<derived_type>,
                      "gc::ptr conversion: classes with virtual functions have no guest layout");
        static_assert(requires(base_type* b) { static_cast<derived_type*>(b); },
                      "gc::ptr conversion: virtual (or ambiguous) base classes have no guest layout");
        static_assert(!composition_only_v<Derived>,
                      "gc::ptr conversion between a derived class and its base (GC_MEM_COMPOSITION_ONLY): on a host "
                      "that reuses tail padding the base offsets are not the console's; describe it by composition");
        alignas(Derived) static unsigned char buffer[sizeof(Derived)];
        auto* derived = reinterpret_cast<Derived*>(buffer);
        Base* base = derived;
        return static_cast<std::uint32_t>(reinterpret_cast<unsigned char*>(const_cast<std::remove_cv_t<Base>*>(base)) -
                                          buffer);
    }
}

template <class T> struct ptr_access;

} // namespace detail

template <class T> class ptr {
    static_assert(!detail::is_host_only_v<T>,
                  "gc::ptr<T>: T must have the guest layout (gc::be<...>, gc::ptr<...>, a guest struct; u8/s8/char not "
                  "dereferenced)");

public:
    using element_type = T;

    ptr() = default; // trivial: may overlay guest RAM
    constexpr ptr(std::nullptr_t) noexcept : address_(0u) {}
    ptr(const ptr&) = default;
    ptr& operator=(const ptr&) = default;

    // Implicit upcast / to-const / to-void conversion, adjusted like the C++ pointer conversion (null stays null).
    template <class U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U*, T*>)
    ptr(const ptr<U>& other) noexcept : address_(upcast_address(other)) {}

    explicit operator bool() const noexcept { return address_ != 0u; }

    std::add_lvalue_reference_t<T> operator*() const
        requires(!std::is_void_v<T>)
    {
        static_assert(!detail::is_plain_byte_v<T>,
                      "gc::ptr<u8|s8|char>: a plain byte access is not seen by the oracle's observer; point to "
                      "gc::be<u8> / gc::be<s8> (gc::ptr<gc::be<u8>>)");
        return *host();
    }
    T* operator->() const
        requires(!std::is_void_v<T>)
    {
        static_assert(!detail::is_plain_byte_v<T>,
                      "gc::ptr<u8|s8|char>: a plain byte access is not seen by the oracle's observer; point to "
                      "gc::be<u8> / gc::be<s8> (gc::ptr<gc::be<u8>>)");
        return host();
    }
    std::add_lvalue_reference_t<T> operator[](std::ptrdiff_t index) const
        requires(!std::is_void_v<T>)
    {
        static_assert(!detail::is_plain_byte_v<T>,
                      "gc::ptr<u8|s8|char>: a plain byte access is not seen by the oracle's observer; point to "
                      "gc::be<u8> / gc::be<s8> (gc::ptr<gc::be<u8>>)");
        return *(*this + index).host();
    }

    // Typed arithmetic: like T*, in steps of sizeof(T) (the guest size of T in guest mode); wraps modulo 2^32 as on
    // the console.
    ptr operator+(std::ptrdiff_t n) const noexcept
        requires(!std::is_void_v<T>)
    {
        return from_address(static_cast<std::uint32_t>(address_ + static_cast<std::uint32_t>(n) * stride()));
    }
    ptr operator-(std::ptrdiff_t n) const noexcept
        requires(!std::is_void_v<T>)
    {
        return from_address(static_cast<std::uint32_t>(address_ - static_cast<std::uint32_t>(n) * stride()));
    }
    friend ptr operator+(std::ptrdiff_t n, const ptr& p) noexcept
        requires(!std::is_void_v<T>)
    {
        return p + n;
    }
    // Difference: a signed 32-bit integer, the console's ptrdiff_t (see the rules at the top for native mode).
    std::int32_t operator-(const ptr& other) const noexcept
        requires(!std::is_void_v<T>)
    {
        const auto bytes = static_cast<std::int32_t>(static_cast<std::uint32_t>(address_ - other.address_));
        return bytes / static_cast<std::int32_t>(stride());
    }
    ptr& operator+=(std::ptrdiff_t n) noexcept
        requires(!std::is_void_v<T>)
    {
        return *this = *this + n;
    }
    ptr& operator-=(std::ptrdiff_t n) noexcept
        requires(!std::is_void_v<T>)
    {
        return *this = *this - n;
    }
    ptr& operator++() noexcept
        requires(!std::is_void_v<T>)
    {
        return *this += 1;
    }
    ptr& operator--() noexcept
        requires(!std::is_void_v<T>)
    {
        return *this -= 1;
    }
    ptr operator++(int) noexcept
        requires(!std::is_void_v<T>)
    {
        ptr old = *this;
        *this += 1;
        return old;
    }
    ptr operator--(int) noexcept
        requires(!std::is_void_v<T>)
    {
        ptr old = *this;
        *this -= 1;
        return old;
    }

    // Comparisons on the 32-bit address, unsigned (cmplw), as the console does.
    friend bool operator==(const ptr& a, const ptr& b) noexcept { return gc::load(a.address_) == gc::load(b.address_); }
    friend bool operator==(const ptr& a, std::nullptr_t) noexcept { return gc::load(a.address_) == 0u; }
    friend std::strong_ordering operator<=>(const ptr& a, const ptr& b) noexcept {
        return gc::load(a.address_) <=> gc::load(b.address_);
    }

private:
    template <class U> friend class ptr;
    friend struct detail::ptr_access<T>;

    static constexpr std::uint32_t stride() noexcept { return detail::object_size<T>(); }

    struct address_tag {};
    constexpr ptr(address_tag, std::uint32_t address) noexcept : address_(address) {}

    // Built directly from the address (no store into an uninitialized local, hence nothing for the observer).
    static ptr from_address(std::uint32_t address) noexcept { return ptr(address_tag{}, address); }

    template <class U> static std::uint32_t upcast_address(const ptr<U>& other) noexcept {
        const std::uint32_t address = other.address_; // read once
        return address == 0 ? 0u : address + detail::base_offset<T, U>();
    }

    T* host() const {
        const std::uint32_t address = address_;
        return reinterpret_cast<T*>(detail::host_of(address, detail::object_size<T>()));
    }

    be<std::uint32_t> address_;
};

namespace detail {
template <class T> struct ptr_access {
    static std::uint32_t address(const ptr<T>& p) noexcept { return p.address_; }
    static ptr<T> make(std::uint32_t address) noexcept { return ptr<T>::from_address(address); }
    static T* host(const ptr<T>& p) { return p.host(); }
};
} // namespace detail

// Guest address held by a pointer (gateway and oracle code: passing a pointer in a guest register).
template <class T> std::uint32_t guest_address(const ptr<T>& p) noexcept {
    return detail::ptr_access<T>::address(p);
}

// Pointer from a guest address (gateway, oracle and symbol-table code only; decompiled code never builds a pointer
// from a number).
template <class T> ptr<T> ptr_from_guest_address(std::uint32_t address) noexcept {
    return detail::ptr_access<T>::make(address);
}

// Host pointer for host code (nullptr for a null guest pointer; faults if the pointer is outside guest RAM).
template <class T> T* to_host(const ptr<T>& p) {
    return p ? detail::ptr_access<T>::host(p) : nullptr;
}

// Pointer to an object that lives in guest RAM (C++ `&object`); faults for an object outside guest RAM.
template <class T> ptr<T> address_of(T& object) {
    return ptr_from_guest_address<T>(detail::guest_of_checked(&object, detail::object_size<T>()));
}

// Same address, another type (C++ reinterpret_cast / static_cast from void*). Cannot drop const.
template <class U, class T>
    requires(std::is_const_v<U> || !std::is_const_v<T>)
ptr<U> ptr_cast(const ptr<T>& p) noexcept {
    return ptr_from_guest_address<U>(guest_address(p));
}

// Base to derived (C++ static_cast<Derived*>(base)), adjusted by the base offset; null stays null.
template <class Derived, class Base>
    requires std::is_base_of_v<std::remove_cv_t<Base>, std::remove_cv_t<Derived>> &&
             (std::is_const_v<Derived> || !std::is_const_v<Base>)
ptr<Derived> static_ptr_cast(const ptr<Base>& p) noexcept {
    const std::uint32_t address = guest_address(p);
    return ptr_from_guest_address<Derived>(address == 0 ? 0u : address - detail::base_offset<Base, Derived>());
}

// Marker pointer: a small integer that original code stores in a pointer as a special value pointing to nothing (C
// `(T*)-1`). It is never dereferenced (in guest mode, -1 or -2 is outside guest RAM: a dereference faults), only
// compared. The same calls compile in both modes: the guest mode holds the value's 32-bit pattern, the native mode
// the sign-extended value, as a C cast of the integer to a pointer does.
template <class T> ptr<T> marker_ptr(std::int32_t value) noexcept {
    return ptr_from_guest_address<T>(static_cast<std::uint32_t>(value));
}
template <class T> bool is_marker_ptr(const ptr<T>& p, std::int32_t value) noexcept {
    return guest_address(p) == static_cast<std::uint32_t>(value);
}

#else // GC_NATIVE_MEMORY

// ---------------------------------------------------------------------------------------------------------------
// Native mode: plain host types (no guest RAM, host byte order, host pointers)
// ---------------------------------------------------------------------------------------------------------------

template <memory_scalar T> using be = T;
template <class T> using ptr = T*;

template <class T> T* to_host(T* p) noexcept {
    return p;
}

template <class T> T* address_of(T& object) noexcept {
    return &object;
}

template <class U, class T>
    requires(std::is_const_v<U> || !std::is_const_v<T>)
U* ptr_cast(T* p) noexcept {
    using void_type = std::conditional_t<std::is_const_v<T>, const void, void>;
    return static_cast<U*>(static_cast<void_type*>(p));
}

template <class Derived, class Base>
    requires std::is_base_of_v<std::remove_cv_t<Base>, std::remove_cv_t<Derived>> &&
             (std::is_const_v<Derived> || !std::is_const_v<Base>)
Derived* static_ptr_cast(Base* p) noexcept {
    return static_cast<Derived*>(p);
}

template <class T> T* marker_ptr(std::int32_t value) noexcept {
    return reinterpret_cast<T*>(static_cast<std::intptr_t>(value));
}
template <class T> bool is_marker_ptr(const T* p, std::int32_t value) noexcept {
    return reinterpret_cast<std::intptr_t>(p) == static_cast<std::intptr_t>(value);
}

#endif // GC_NATIVE_MEMORY

// Explicit access to a plain scalar (a local, a u8/s8 field in guest mode, any scalar field in native mode, where
// be<T> is T): the same calls compile in both modes. The be<T> overloads of guest mode are above.
template <memory_scalar T> constexpr T load(const T& x) noexcept {
    return x;
}
template <memory_scalar T> constexpr void store(T& x, std::type_identity_t<T> value) noexcept {
    x = value;
}
template <memory_scalar T> constexpr detail::uint_of_t<sizeof(T)> load_raw(const T& x) noexcept {
    return std::bit_cast<detail::uint_of_t<sizeof(T)>>(x);
}
template <memory_scalar T>
constexpr void store_raw(T& x, std::type_identity_t<detail::uint_of_t<sizeof(T)>> bits) noexcept {
    x = std::bit_cast<T>(bits);
}

// ---------------------------------------------------------------------------------------------------------------
// array<T, N>
// ---------------------------------------------------------------------------------------------------------------

// Fixed-size array with the layout of `T[N]`. operator[] is unchecked on purpose: the original code may index out
// of bounds and the decompiled code must do the same; at() faults instead.
template <class T, std::size_t N> struct array {
#if !defined(GC_NATIVE_MEMORY)
    static_assert(!detail::is_host_only_v<T>, "gc::array<T, N>: T must have the guest layout");
    static_assert(!std::is_polymorphic_v<T>, "gc::array<T, N>: a class with virtual functions has no guest layout");
    static_assert(detail::guest_object_ok_v<T>); // virtual bases, composition-only mode
#endif
    static_assert(N > 0, "gc::array<T, 0> has no console equivalent");
#if !defined(GC_NATIVE_MEMORY)
    static_assert(!detail::is_plain_byte_v<T>,
                  "gc::array<u8|s8|char, N>: plain byte accesses are not seen by the oracle's observer; use "
                  "gc::array<gc::be<u8>, N>");
#endif

    T elements[N];

    static constexpr std::size_t size() noexcept { return N; }
    constexpr T& operator[](std::size_t i) noexcept { return elements[i]; }
    constexpr const T& operator[](std::size_t i) const noexcept { return elements[i]; }
    T& at(std::size_t i) {
        if (i >= N) {
            detail::fault("gc::array index out of range", static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(N));
        }
        return elements[i];
    }
    const T& at(std::size_t i) const { return const_cast<array*>(this)->at(i); }
    constexpr T* begin() noexcept { return elements; }
    constexpr T* end() noexcept { return elements + N; }
    constexpr const T* begin() const noexcept { return elements; }
    constexpr const T* end() const noexcept { return elements + N; }
};

// ---------------------------------------------------------------------------------------------------------------
// Bit-fields
// ---------------------------------------------------------------------------------------------------------------

// Descriptor of one bit-field in a storage word of type U. `First` counts from the most significant bit, in
// declaration order: `unsigned a : 1; unsigned b : 3;` is bits<u32, 0, 1> then bits<u32, 1, 3>.
// [hypothesis] CodeWarrior allocates bit-fields from the most significant bit of the storage unit, as big-endian
// PowerPC ABIs do to our knowledge; not yet checked against a real structure of a game. The descriptor makes the order
// explicit, so a correction only changes the `First` values, never this layer.
// T: the field's type. A signed T is sign-extended from Width bits (like a signed C++ bit-field); an enum T uses its
// underlying type. Writing truncates the value to Width bits (modulo, like a C++ bit-field).
template <std::unsigned_integral U, unsigned First, unsigned Width, class T = U> struct bits {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "gc::bits: T must be an integer or an enum");
    static_assert(!std::is_same_v<T, bool>, "gc::bits: use an unsigned 1-bit field instead of bool");
    static_assert(Width >= 1 && First + Width <= 8 * sizeof(U), "gc::bits: field outside its storage word");
    static_assert(Width <= 8 * sizeof(detail::underlying_of_t<T>), "gc::bits: field wider than its type T");

    using storage_type = U;
    using value_type = T;
    static constexpr unsigned first = First;
    static constexpr unsigned width = Width;
    static constexpr unsigned shift = 8 * sizeof(U) - First - Width;
    static constexpr std::uint64_t value_mask = Width == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << Width) - 1;
    static constexpr U mask = static_cast<U>(value_mask << shift);

    static constexpr T extract(U word) noexcept {
        using underlying = detail::underlying_of_t<T>;
        std::uint64_t value = (static_cast<std::uint64_t>(word) >> shift) & value_mask;
        if constexpr (std::is_signed_v<underlying>) {
            if (Width < 64 && (value >> (Width - 1)) != 0) {
                value |= ~value_mask; // sign extension
            }
        }
        return static_cast<T>(static_cast<underlying>(value));
    }

    static constexpr U insert(U word, T value) noexcept {
        using underlying = detail::underlying_of_t<T>;
        const auto bits_of_value = static_cast<std::uint64_t>(static_cast<underlying>(value)) & value_mask;
        return static_cast<U>((word & static_cast<U>(~mask)) | static_cast<U>(bits_of_value << shift));
    }
};

namespace detail {
template <class F, class U>
concept bits_of = requires {
    typename F::storage_type;
    typename F::value_type;
} && std::is_same_v<typename F::storage_type, U>;
} // namespace detail

// Storage word of bit-fields. Fields are read and written through descriptors, usually declared next to the
// structure:  struct Thing { gc::bitword<u32> flags; static constexpr gc::bits<u32, 0, 1> visible{}; };
//             if (thing->flags[Thing::visible]) ...;  thing->flags[Thing::visible] = 0;
// A field write is a read-modify-write of the whole word, as on the console (rlwimi + store).
template <std::unsigned_integral U> struct bitword {
    be<U> word;

    template <class F> class reference {
    public:
        constexpr operator typename F::value_type() const noexcept { return F::extract(owner_.word); }
        constexpr reference& operator=(typename F::value_type value) noexcept {
            owner_.word = F::insert(owner_.word, value);
            return *this;
        }
        // Copies the field's value, never the whole word.
        constexpr reference& operator=(const reference& other) noexcept {
            return *this = static_cast<typename F::value_type>(other);
        }
        constexpr reference(const reference&) noexcept = default;

    private:
        friend struct bitword;
        constexpr explicit reference(bitword& owner) noexcept : owner_(owner) {}
        bitword& owner_;
    };

    template <class F>
        requires detail::bits_of<F, U>
    constexpr reference<F> operator[](F) noexcept {
        return reference<F>(*this);
    }
    template <class F>
        requires detail::bits_of<F, U>
    constexpr typename F::value_type operator[](F) const noexcept {
        return F::extract(word);
    }
    template <class F>
        requires detail::bits_of<F, U>
    constexpr typename F::value_type get(F) const noexcept {
        return F::extract(word);
    }
    template <class F>
        requires detail::bits_of<F, U>
    constexpr void set(F, typename F::value_type value) noexcept {
        word = F::insert(word, value);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------------------------------------------

// A game global, named by an identifier of the game (an enum value). In guest mode its address comes from the table
// the game installs for the running version (gc::set_global_table): there is no address in decompiled code. In native
// mode the global is an ordinary host variable (how it gets its initial value is decided with the native memory).
// Usage:  inline constexpr gc::global<GameState, GlobalId::game_state> game_state{};   game_state->field = 1;
template <class T, auto Id> struct global {
    static_assert(std::is_enum_v<decltype(Id)> || std::is_integral_v<decltype(Id)>,
                  "gc::global: the identifier must be an enum value or an integer");
#if !defined(GC_NATIVE_MEMORY)
    // Like ptr<T>: a global<u32, Id> would read the console's big-endian bytes in host order. (T may still be
    // incomplete here; the class checks are made in get().)
    static_assert(!detail::is_host_only_v<T>,
                  "gc::global<T, Id>: T must have the guest layout (gc::be<...>, gc::ptr<...>, a guest struct)");
    static_assert(!detail::is_plain_byte_v<T>,
                  "gc::global<u8|s8|char, Id>: a plain byte access is not seen by the oracle's observer; use "
                  "gc::be<u8> / gc::be<s8>");
#endif

    static T& get();
    T* operator->() const { return &get(); }
    T& operator*() const { return get(); }
    ptr<T> address() const { return address_of(get()); }
};

#if !defined(GC_NATIVE_MEMORY)

namespace detail {
struct global_table {
    const std::uint32_t* addresses = nullptr;
    std::size_t count = 0;
};
inline global_table g_globals;
} // namespace detail

// Installs the guest addresses of the game's globals for the running version: addresses[i] is the address of the
// global whose identifier converts to i (0: not available in this version). The table must outlive its use.
inline void set_global_table(const std::uint32_t* addresses, std::size_t count) noexcept {
    detail::g_globals.addresses = addresses;
    detail::g_globals.count = count;
}

template <class T, auto Id> T& global<T, Id>::get() {
    static_assert(detail::guest_object_ok_v<T>); // virtual functions or bases, composition-only mode
    const auto index = static_cast<std::size_t>(Id);
    const std::uint32_t address =
        index < detail::g_globals.count ? detail::g_globals.addresses[index] : 0u;
    if (address == 0u) {
        // The identifier goes in the text: passed as the address, it would read as a guest address.
        char reason[192];
        std::snprintf(reason, sizeof(reason),
                      "gc::global identifier %u has no address in this version (global table not installed, or no "
                      "address for this version in the game's globals table)",
                      static_cast<unsigned>(index));
        detail::fault(reason, 0u, static_cast<std::uint32_t>(sizeof(T)));
    }
    return *reinterpret_cast<T*>(detail::host_of(address, static_cast<std::uint32_t>(sizeof(T))));
}

#else

template <class T, auto Id> T& global<T, Id>::get() {
    static T storage{};
    return storage;
}

#endif // GC_NATIVE_MEMORY

// ---------------------------------------------------------------------------------------------------------------
// Layout guarantees of the guest mode
// ---------------------------------------------------------------------------------------------------------------

#if !defined(GC_NATIVE_MEMORY)
static_assert(sizeof(be<u8>) == 1 && alignof(be<u8>) == 1);
static_assert(sizeof(be<u16>) == 2 && alignof(be<u16>) == 2);
static_assert(sizeof(be<u32>) == 4 && alignof(be<u32>) == 4);
static_assert(sizeof(be<u64>) == 8 && alignof(be<u64>) == 8);
static_assert(sizeof(be<f32>) == 4 && alignof(be<f32>) == 4);
static_assert(sizeof(be<f64>) == 8 && alignof(be<f64>) == 8);
static_assert(sizeof(ptr<void>) == 4 && alignof(ptr<void>) == 4);
static_assert(sizeof(bitword<u32>) == 4 && alignof(bitword<u32>) == 4);
static_assert(std::is_standard_layout_v<be<u32>> && std::is_trivially_default_constructible_v<be<u32>>);
static_assert(std::is_standard_layout_v<ptr<void>> && std::is_trivially_default_constructible_v<ptr<void>>);

// Host ABI check (see "Layout" at the top): on a host that reuses the tail padding of a base (Itanium ABI), guest
// structures described by inheritance would get offsets the console does not use; guest mode compiles there only in
// the explicit composition-only mode.
static_assert(!host_reuses_tail_padding || detail::composition_only,
              "gc_mem.h guest mode: this host C++ ABI puts derived members in the tail padding of a base (Itanium "
              "ABI); define GC_MEM_COMPOSITION_ONLY (guest structures by composition only; inheritance is then "
              "refused where it meets the layer). See Layout in gc_mem.h.");
#endif

} // namespace gc

#undef GC_MEM_DETAIL_BSWAP_BUILTINS

#endif // GC_MEM_H
