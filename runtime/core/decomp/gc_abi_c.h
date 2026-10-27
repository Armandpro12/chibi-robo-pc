/* C side of the decompiled-code gateway (gc_abi.h): what a game's C entry point calls. Generic, no game here. */
#ifndef GC_ABI_C_H
#define GC_ABI_C_H

#ifdef __cplusplus
extern "C" {
#endif

struct GcrtCpu;

/* Registers the decompiled functions of `table` (array of gc::abi::registration, `count` entries). */
void gcrt_decomp_install_table(struct GcrtCpu* g, const void* table, unsigned count);

/* Defined by the registration table that tools/build.py generates for the game and the version being built
 * (decomp_registry.cpp in the build folder): calls gcrt_decomp_install_table with the addresses of that version. */
void gcrt_decomp_install_generated(struct GcrtCpu* g);

#ifdef __cplusplus
}
#endif

#endif /* GC_ABI_C_H */
