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
// Public API (what decompiled code may use)
//   gc::be<T>            big-endian scalar stored in guest memory (integers, enums, float, double; not bool)
//   gc::ptr<T>           32-bit guest pointer to T (T is a guest-layout type: be<...>, ptr<...>, guest structs, u8/s8)
//   gc::array<T, N>      fixed-size array with the guest layout
//   gc::bitword<U>       storage word of bit-fields, accessed with gc::bits<U, First, Width, T> descriptors
//   gc::global<T, Id>    a game global, resolved through the game's per-version address table
//   gc::address_of(obj)  pointer to an object that lives in guest memory (C++ `&obj`)
//   gc::to_host(p)       host pointer for a guest pointer (for host code that needs a T*)
//   gc::ptr_cast<U>(p), gc::static_ptr_cast<U>(p)   pointer conversions (reinterpret / base-to-derived)
// Guest-mode only (gateway, oracle and table code; they do not exist in native mode on purpose):
//   gc::guest_address(p), gc::ptr_from_guest_address<T>(address), gc::init_guest_memory, gc::set_global_table
// Everything in gc::detail is internal: decompiled code never uses it.
//
// Options (preprocessor)
//   GC_NATIVE_MEMORY   native mode (final switch to native memory): plain host types, no guest RAM.
//   GC_MEM_OBSERVER    guest mode only: every load and store of a gc::be (hence of ptr and bitword) that lies in
//                      guest RAM is reported to the observer of the calling thread (oracle executable only).
//
// Layout rule: a struct made of these types has, in guest mode, the size, alignment and field offsets of the same
// struct compiled for the console with natural alignment (each scalar aligned on its size). Games check their own
// structures with static_assert(offsetof(...)) in guest mode only; in native mode pointers are 8 bytes.
#ifndef GC_MEM_H
#define GC_MEM_H

#include <bit>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

// Scalars that may live in guest memory. bool is excluded on purpose: a guest byte holding 2 read as a C++ bool
// would come back as 1 when stored again; game booleans are declared as be<u8> (or u8) and compared explicitly.
template <class T>
concept memory_scalar = (std::is_integral_v<T> || std::is_enum_v<T> || std::is_same_v<T, float> ||
                         std::is_same_v<T, double>)&&!std::is_same_v<std::remove_cv_t<T>, bool> &&
                        !std::is_const_v<T> && !std::is_volatile_v<T> &&
                        (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);

namespace detail {

template <std::size_t N> struct uint_of;
template <> struct uint_of<1> { using type = u8; };
template <> struct uint_of<2> { using type = u16; };
template <> struct uint_of<4> { using type = u32; };
template <> struct uint_of<8> { using type = u64; };
template <std::size_t N> using uint_of_t = typename uint_of<N>::type;

// Guest byte order (big-endian) <-> host value. Identity on a big-endian host.
template <class U> constexpr U to_big_endian(U v) noexcept {
    static_assert(std::is_unsigned_v<U>);
    if constexpr (sizeof(U) == 1 || std::endian::native == std::endian::big) {
        return v;
    } else if constexpr (sizeof(U) == 2) {
        return static_cast<U>(__builtin_bswap16(v));
    } else if constexpr (sizeof(U) == 4) {
        return static_cast<U>(__builtin_bswap32(v));
    } else {
        return static_cast<U>(__builtin_bswap64(v));
    }
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
// `size` is the size of main RAM, at most 1 GiB (the two mirrors must not overlap).
inline void init_guest_memory(void* host_ram, std::uint32_t size) {
    if (host_ram == nullptr || size == 0 || size > detail::kMaxRamSize) {
        detail::fault("init_guest_memory: invalid guest RAM", 0, size);
    }
    detail::g_memory.host = static_cast<std::byte*>(host_ram);
    detail::g_memory.size = size;
}

// Forgets the guest RAM (tests, shutdown).
inline void reset_guest_memory() noexcept {
    detail::g_memory = {};
}

// ---------------------------------------------------------------------------------------------------------------
// Observer (oracle only)
// ---------------------------------------------------------------------------------------------------------------

#if defined(GC_MEM_OBSERVER)

// Receives the guest accesses made through this layer. `value`, `old_value`, `new_value`: the accessed bytes read as a
// big-endian unsigned integer (what the console CPU sees), zero-extended to 64 bits. Accesses to be<T> objects that
// are not in guest RAM (local copies on the host stack) are not reported.
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

inline void observe_read(const void* object, std::uint32_t size, std::uint64_t value) {
    std::uint32_t address = 0;
    if (memory_observer* observer = g_observer; observer && guest_of(object, size, &address)) {
        observer->on_read(address, size, value);
    }
}

inline void observe_write(const void* object, std::uint32_t size, std::uint64_t old_value, std::uint64_t new_value) {
    std::uint32_t address = 0;
    if (memory_observer* observer = g_observer; observer && guest_of(object, size, &address)) {
        observer->on_write(address, size, old_value, new_value);
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

template <memory_scalar T> struct be {
    using value_type = T;
    using raw_type = detail::uint_of_t<sizeof(T)>;

    be() = default; // trivial: guest structures overlay guest RAM without being constructed
    constexpr be(T value) noexcept : stored_(detail::to_big_endian(std::bit_cast<raw_type>(value))) {}
    be(const be&) = default;

    // Copy between two be<T>: the bytes are copied as they are (a float NaN payload is kept bit for bit).
    constexpr be& operator=(const be& other) noexcept {
        const raw_type stored = other.stored_;
#if defined(GC_MEM_OBSERVER)
        if (!std::is_constant_evaluated()) {
            detail::observe_read(&other, sizeof(T), detail::to_big_endian(stored));
            detail::observe_write(this, sizeof(T), detail::to_big_endian(stored_), detail::to_big_endian(stored));
        }
#endif
        stored_ = stored;
        return *this;
    }

    constexpr be& operator=(T value) noexcept {
        store(value);
        return *this;
    }

    constexpr operator T() const noexcept { return load(); }

    constexpr T load() const noexcept { return std::bit_cast<T>(raw()); }

    constexpr void store(T value) noexcept { set_raw(std::bit_cast<raw_type>(value)); }

    // The bits of the value (for a float: its IEEE-754 encoding), in host order.
    constexpr raw_type raw() const noexcept {
        const raw_type value = detail::to_big_endian(stored_);
#if defined(GC_MEM_OBSERVER)
        if (!std::is_constant_evaluated()) {
            detail::observe_read(this, sizeof(T), value);
        }
#endif
        return value;
    }

    constexpr void set_raw(raw_type value) noexcept {
#if defined(GC_MEM_OBSERVER)
        if (!std::is_constant_evaluated()) {
            detail::observe_write(this, sizeof(T), detail::to_big_endian(stored_), value);
        }
#endif
        stored_ = detail::to_big_endian(value);
    }

    // Compound assignments: same result as the C++ operator on a T variable (promotion, then conversion to T).
    constexpr be& operator+=(T v) noexcept
        requires(!std::is_enum_v<T>)
    {
        store(static_cast<T>(load() + v));
        return *this;
    }
    constexpr be& operator-=(T v) noexcept
        requires(!std::is_enum_v<T>)
    {
        store(static_cast<T>(load() - v));
        return *this;
    }
    constexpr be& operator*=(T v) noexcept
        requires(!std::is_enum_v<T>)
    {
        store(static_cast<T>(load() * v));
        return *this;
    }
    constexpr be& operator/=(T v) noexcept
        requires(!std::is_enum_v<T>)
    {
        store(static_cast<T>(load() / v));
        return *this;
    }
    constexpr be& operator%=(T v) noexcept
        requires std::is_integral_v<T>
    {
        store(static_cast<T>(load() % v));
        return *this;
    }
    constexpr be& operator&=(T v) noexcept
        requires std::is_integral_v<T>
    {
        store(static_cast<T>(load() & v));
        return *this;
    }
    constexpr be& operator|=(T v) noexcept
        requires std::is_integral_v<T>
    {
        store(static_cast<T>(load() | v));
        return *this;
    }
    constexpr be& operator^=(T v) noexcept
        requires std::is_integral_v<T>
    {
        store(static_cast<T>(load() ^ v));
        return *this;
    }
    constexpr be& operator<<=(int n) noexcept
        requires std::is_integral_v<T>
    {
        store(static_cast<T>(load() << n));
        return *this;
    }
    constexpr be& operator>>=(int n) noexcept
        requires std::is_integral_v<T>
    {
        store(static_cast<T>(load() >> n));
        return *this;
    }
    constexpr be& operator++() noexcept
        requires std::is_integral_v<T>
    {
        store(static_cast<T>(load() + 1));
        return *this;
    }
    constexpr be& operator--() noexcept
        requires std::is_integral_v<T>
    {
        store(static_cast<T>(load() - 1));
        return *this;
    }
    constexpr T operator++(int) noexcept
        requires std::is_integral_v<T>
    {
        const T old = load();
        store(static_cast<T>(old + 1));
        return old;
    }
    constexpr T operator--(int) noexcept
        requires std::is_integral_v<T>
    {
        const T old = load();
        store(static_cast<T>(old - 1));
        return old;
    }

private:
    // The value's bytes in guest order. Alignment of T, so that structures keep the console's natural layout.
    alignas(alignof(T)) raw_type stored_;
};

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

template <class T> constexpr std::uint32_t object_size() noexcept {
    if constexpr (std::is_void_v<T>) {
        return 1;
    } else {
        return static_cast<std::uint32_t>(sizeof(T));
    }
}

// Offset added when converting a pointer to Derived into a pointer to Base (non-virtual inheritance): computed on a
// suitably aligned dummy buffer, never on a real object, so converting a wild guest pointer does not fault.
template <class Base, class Derived> std::uint32_t base_offset() noexcept {
    if constexpr (std::is_void_v<std::remove_cv_t<Base>> || std::is_same_v<std::remove_cv_t<Base>, std::remove_cv_t<Derived>>) {
        return 0;
    } else {
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
                  "gc::ptr<T>: T must have the guest layout (gc::be<...>, gc::ptr<...>, a guest struct, u8 or s8)");

public:
    using element_type = T;

    ptr() = default; // trivial: may overlay guest RAM
    constexpr ptr(std::nullptr_t) noexcept : address_(0u) {}
    ptr(const ptr&) = default;
    ptr& operator=(const ptr&) = default;

    // Implicit upcast / to-const / to-void conversion, adjusted like the C++ pointer conversion (null stays null).
    template <class U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U*, T*>)
    ptr(const ptr<U>& other) noexcept {
        const std::uint32_t address = other.address_;
        address_ = address == 0 ? 0u : address + detail::base_offset<T, U>();
    }

    explicit operator bool() const noexcept { return address_ != 0u; }

    std::add_lvalue_reference_t<T> operator*() const
        requires(!std::is_void_v<T>)
    {
        return *host();
    }
    T* operator->() const
        requires(!std::is_void_v<T>)
    {
        return host();
    }
    std::add_lvalue_reference_t<T> operator[](std::ptrdiff_t index) const
        requires(!std::is_void_v<T>)
    {
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
    std::ptrdiff_t operator-(const ptr& other) const noexcept
        requires(!std::is_void_v<T>)
    {
        const auto bytes = static_cast<std::int32_t>(static_cast<std::uint32_t>(address_ - other.address_));
        return static_cast<std::ptrdiff_t>(bytes / static_cast<std::int32_t>(stride()));
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
    friend bool operator==(const ptr& a, const ptr& b) noexcept { return a.address_.load() == b.address_.load(); }
    friend bool operator==(const ptr& a, std::nullptr_t) noexcept { return a.address_.load() == 0u; }
    friend std::strong_ordering operator<=>(const ptr& a, const ptr& b) noexcept {
        return a.address_.load() <=> b.address_.load();
    }

private:
    template <class U> friend class ptr;
    friend struct detail::ptr_access<T>;

    static constexpr std::uint32_t stride() noexcept { return detail::object_size<T>(); }

    static ptr from_address(std::uint32_t address) noexcept {
        ptr p;
        p.address_ = address;
        return p;
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

#endif // GC_NATIVE_MEMORY

// ---------------------------------------------------------------------------------------------------------------
// array<T, N>
// ---------------------------------------------------------------------------------------------------------------

// Fixed-size array with the layout of `T[N]`. operator[] is unchecked on purpose: the original code may index out
// of bounds and the decompiled code must do the same; at() faults instead.
template <class T, std::size_t N> struct array {
#if !defined(GC_NATIVE_MEMORY)
    static_assert(!detail::is_host_only_v<T>, "gc::array<T, N>: T must have the guest layout");
#endif
    static_assert(N > 0, "gc::array<T, 0> has no console equivalent");

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
    const auto index = static_cast<std::size_t>(Id);
    const std::uint32_t address =
        index < detail::g_globals.count ? detail::g_globals.addresses[index] : 0u;
    if (address == 0u) {
        detail::fault("gc::global has no address in this version", static_cast<std::uint32_t>(index),
                      static_cast<std::uint32_t>(sizeof(T)));
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
#endif

} // namespace gc

#endif // GC_MEM_H
