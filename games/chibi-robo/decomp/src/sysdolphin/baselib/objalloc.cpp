// HSD object allocator (sysdolphin): a pool of fixed-size blocks with a list of free ones.
//
// Original unit: sysdolphin/baselib/objalloc.c [hypothesis: the function sits next to that source-file string in both
// versions]. Names are hypotheses (notes of the decompilation stage); the code was written from the disassembly of
// both versions (identical in US and PAL).
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::ptr;
using gc::u32;
using gc::u8;

// A block of the pool while it is free: its first word links it to the next free block.
struct FreeBlock {
    ptr<FreeBlock> next;
};

// The pool's bookkeeping; only the fields this function uses are named (the first word is described elsewhere).
struct ObjAllocData {
    gc::array<be<u8>, 4> not_described_here;
    ptr<FreeBlock> free_head; // most recently freed block
    be<u32> used_count;       // blocks handed out
    be<u32> free_count;       // blocks in the free list
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(ObjAllocData, free_head) == 4 && offsetof(ObjAllocData, used_count) == 8 &&
              offsetof(ObjAllocData, free_count) == 0xC);
#endif

// Gives `block` back to the pool: it becomes the head of the free list, one more free, one less used. Every access in
// the original's order (a block that overlaps the bookkeeping, never expected, gives the same bytes).
void obj_alloc_free(ptr<ObjAllocData> pool, ptr<FreeBlock> block) {
    const ptr<FreeBlock> previous_head = pool->free_head;
    block->next = previous_head;
    pool->free_head = block;
    pool->free_count += 1u;
    pool->used_count -= 1u;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_obj_alloc_free, hsd::obj_alloc_free, gc::abi::flag_none);
