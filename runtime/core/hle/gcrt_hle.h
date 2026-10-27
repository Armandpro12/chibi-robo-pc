/* Generic HLE: recognition of SDK functions by signature and replacement by the host.
 *
 * 1. Catalog (sdk_signatures.tsv, produced by tools/sdksig.py catalog): for each function, its size, the
 *    CRC-32 of its "masked" words (address-dependent fields cleared, same rule as
 *    tools/symbols.py:keep_masks) and the first masked word, used as a key. No address, no code.
 * 2. Scan (gcrt_hle_scan): at startup over the code sections of the DOL, then over each linked REL;
 *    candidate starts = targets of `bl` and words that follow a `blr`.
 * 3. Replacement: the CPU's `host_call` callback. The chassis calls it before each block; the entry points
 *    listed at build time (tools/build.py -> DolRecomp --hle-hooks) also call it from the generated code,
 *    because a `bl` inside a block becomes a `goto` there, which does not go back through the chassis.
 *
 * No reference to a specific game here. A game registers its own replacements by address
 * (gcrt_hle_register) from GcrtGameDesc.install; the address must then be listed in game.toml
 * ([hle.hooks]) to be compiled with a hook. See notes/hle.md.
 */
#ifndef GCRT_HLE_H
#define GCRT_HLE_H

#include "cpu/gcrt_cpu.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GCRT_SIG_HOOK 1u /* the core provides a replacement: entry point compiled with a hook */

typedef struct GcrtSdkSignature {
    const char* name;
    u32 size;     /* bytes */
    u32 crc32;    /* CRC-32 of the masked words */
    u32 key;      /* first masked word */
    u32 key_mask;
    u32 flags;
} GcrtSdkSignature;

extern const GcrtSdkSignature gcrt_sdk_catalog[];
extern const unsigned gcrt_sdk_catalog_count;

/* Replacement: return true if the host executed the function (pc updated, usually to lr),
 * false to let the original code run (possibly after changing the registers). */
typedef bool (*GcrtHleFn)(GcrtCpu* g, u32 address);

/* Continuation of a call: called when guest code returns to the return address provided by
 * gcrt_hle_push_return; must update pc (usually to `return_address`). */
typedef void (*GcrtHleReturnFn)(GcrtCpu* g, u32 return_address, void* data);

/* Installs the dispatcher (host_call of the chassis and of the generated code). `trace`: detailed log. */
void gcrt_hle_init(GcrtCpu* g, bool trace);
bool gcrt_hle_trace_enabled(void);

/* Scans [start, start + size) (guest code in RAM); returns the number of recognized functions. */
unsigned gcrt_hle_scan(GcrtCpu* g, u32 start, u32 size, const char* origin);
/* Same over several ranges seen as a single image (`bl` from one section to another count). */
unsigned gcrt_hle_scan_image(GcrtCpu* g, const u32* starts, const u32* sizes, unsigned count, const char* origin);
/* Address of the catalog function `name` (0 if absent); `count`: number of copies found. */
u32 gcrt_hle_find(const char* name, unsigned* count);
/* Prints the result of the scans (recognized, ambiguous and absent functions). */
void gcrt_hle_report(FILE* out);

/* Calls of the dispatcher since the start (chassis and compiled hooks), and those that found no replacement. */
void gcrt_hle_dispatch_counts(u64* calls, u64* misses);

/* Replacement by address (game); `name` is used in the log. */
bool gcrt_hle_register(GcrtCpu* g, u32 address, const char* name, GcrtHleFn fn);
/* Replaces every recognized copy of a catalog function; returns their number. */
unsigned gcrt_hle_replace(GcrtCpu* g, const char* sdk_name, GcrtHleFn fn);
/* All the addresses recognized for `name` (returns their number, at most `max` written). */
unsigned gcrt_hle_find_all(const char* name, u32* out, unsigned max);
/* SDK function recognized by signature that contains `address` (its catalog name), NULL if none; `start` receives
 * its first address. Diagnostics (crash report). */
const char* gcrt_hle_name_at(u32 address, u32* start);
/* Range [lo, hi) covered by the functions of library `prefix` (names "prefix..." or "__prefix...")
 * recognized exactly once. */
bool gcrt_hle_library_range(const char* prefix, u32* lo, u32* hi);
/* Replacement of a single copy: the unique one, or the only copy located in its library's range (small
 * accessor functions often have the same masked code as a function of another library). Returns 1 if
 * replaced, 0 otherwise (message on stderr if ambiguous). */
unsigned gcrt_hle_replace_in_library(GcrtCpu* g, const char* sdk_name, const char* prefix, GcrtHleFn fn);
/* Anchors a function with a shared signature using the order of the functions in its source file (the linker
 * keeps that order): copy of `name` located right after `chain[0]` (recognized exactly once), skipping the
 * functions `chain[1..]` (catalog sizes). E.g. VISetPostRetraceCallback: {"__VIRetraceHandler",
 * "VISetPreRetraceCallback"}. Returns the address, 0 if no copy falls at that spot. */
u32 gcrt_hle_resolve_following(const char* name, const char* const* chain, unsigned chain_count);
/* Copy of `name` that ends exactly at the start of `anchor` (recognized exactly once), 0 otherwise. */
u32 gcrt_hle_resolve_preceding(const char* name, const char* anchor);
/* Replacements provided by the core (game log, OSLink/OSUnlink). */
void gcrt_hle_install_defaults(GcrtCpu* g);
/* Size (bytes) of a catalog function, 0 if not in the catalog. */
u32 gcrt_hle_catalog_size(const char* name);

/* --- Reading recognized code (milestone 10b) ---
 * Signatures mask the immediates that depend on linking (lis/addi of addresses, offsets relative to r13/r2): a
 * recognized function therefore has the same instruction sequence from one game to another, but its static
 * variables are elsewhere. These helpers find those addresses in the recognized guest code, by linear simulation
 * of the instructions that build constants (li, lis, addi, addis, ori, oris, mr), branches ignored. */
/* Value of register `reg` right before the `nth` (0 = first) `bl target` of [fn, fn + size). `sda`, `sda2`:
 * values of r13 and r2 (0: unknown). Returns false if the call does not exist or if the value is not constant. */
bool gcrt_hle_decode_reg_at_call(GcrtCpu* g, u32 fn, u32 size, u32 target, unsigned nth, unsigned reg, u32 sda,
                                 u32 sda2, u32* out);
/* Value of register `reg` at the first `blr` of [fn, fn + size) (e.g. r13 at the end of __init_registers). */
bool gcrt_hle_decode_reg_at_blr(GcrtCpu* g, u32 fn, u32 size, unsigned reg, u32* out);
/* Displacement d of the `nth` D-form access with primary opcode `opcode` (32 lwz, 36 stw, 14 addi...) relative to
 * register `base` (13: SDA) in [fn, fn + size). */
bool gcrt_hle_decode_d_offset(GcrtCpu* g, u32 fn, u32 size, unsigned opcode, unsigned base, unsigned nth, s16* out);

/* "Magic" return address to put in lr: when guest code returns to it, `fn(g, ret, data)`
 * is called with `ret` = original return address. `data` is copied (at most 64 bytes). */
u32 gcrt_hle_push_return(GcrtCpu* g, u32 original_return, GcrtHleReturnFn fn, const void* data, size_t size);
bool gcrt_hle_is_return_address(u32 address);

/* CRC-32 (zlib) of a host buffer. */
u32 gcrt_hle_crc32(const void* data, size_t size);

/* Game output (OSReport, printf...): stdout, and the --guest-log file if it is open. */
void gcrt_hle_guest_output(const char* text, size_t length);
void gcrt_hle_set_guest_log(FILE* file);

/* --- printf formatting of guest arguments (guest_printf.c) --- */
typedef struct GcrtGuestArgs {
    GcrtCpu* g;
    bool from_registers; /* direct call (r3...r10, f1...f8) or va_list in memory */
    u32 gpr;             /* number of integer registers already consumed (0-8) */
    u32 fpr;             /* number of floating-point registers already consumed (0-8) */
    u32 overflow;        /* stack argument area */
    u32 reg_save;        /* va_list: register save area (8 GPRs then 8 FPRs) */
} GcrtGuestArgs;

/* Arguments of a variadic call, at function entry: `fixed_gprs` = number of fixed integer arguments
 * before "..." (e.g. 1 for printf(fmt, ...)). Stack: r1 + 8 (PowerPC EABI/SVR4 ABI). */
void gcrt_guest_args_from_registers(GcrtGuestArgs* a, GcrtCpu* g, u32 fixed_gprs);
/* Guest `va_list` (struct { u8 gpr, fpr; u16 pad; u32 overflow_arg_area, reg_save_area; }). */
void gcrt_guest_args_from_va_list(GcrtGuestArgs* a, GcrtCpu* g, u32 va_list_address);
/* Formats `format` (guest address); returns the length (truncated to out_size - 1). */
size_t gcrt_guest_vformat(GcrtGuestArgs* a, u32 format, char* out, size_t out_size);
/* Copies a guest string (NUL-terminated); returns false if the address is not in RAM. */
bool gcrt_guest_string(GcrtCpu* g, u32 address, char* out, size_t out_size);
/* Does `address` point to a plausible format string (in RAM, terminated, printable characters)? */
bool gcrt_guest_plausible_text(GcrtCpu* g, u32 address);

/* --- REL (oslink.c) --- */
/* Replacements of OSLink and OSUnlink (installed by gcrt_hle_install_defaults). */
bool gcrt_hle_oslink(GcrtCpu* g, u32 address);
bool gcrt_hle_osunlink(GcrtCpu* g, u32 address);
/* Diagnostic: loads the .rel file `path` into guest RAM as a game would, calls the game's OSLink then
 * OSUnlink and checks the result. Returns 0 if everything is correct. */
int gcrt_hle_rel_selftest(GcrtCpu* g, const char* path);

/* Calls a guest function (r3...r6) and runs until it returns (at most `max_blocks` blocks);
 * the register state is restored afterwards. Returns true if the function returned; *result = r3. */
bool gcrt_hle_call_guest(GcrtCpu* g, u32 function, const u32* args, unsigned arg_count, u64 max_blocks, u32* result);

#ifdef __cplusplus
}
#endif

#endif /* GCRT_HLE_H */
