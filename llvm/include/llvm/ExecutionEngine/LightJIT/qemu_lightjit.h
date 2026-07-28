// qemu_lightjit.h
#ifndef QEMU_LIGHTJIT_H
#define QEMU_LIGHTJIT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void* start;
    size_t size;
    uint32_t permissions;
} jit_memory_region;

typedef struct {
    uint64_t offset;
    uint32_t type;
    int64_t addend;
    const char* symbol;
} jit_relocation;

typedef struct jit_context jit_context_t;

jit_context_t* jit_context_create(void);

void jit_context_destroy(jit_context_t* ctx);

uint64_t jit_link_aot(jit_context_t* ctx,
                     const void* aot_data,
                     size_t aot_size,
                     uint64_t base_address,
                     jit_memory_region** allocated_regions,
                     size_t* region_count,
                     uint64_t startCode,
                     void (*register_mapping)(uint64_t, uint64_t, uint64_t),
                     void (*log_message)(const char *),
                     const char *AotFile,
                     void *(*g_malloc0)(uint64_t),
                     uint64_t *aot_code_base_ptr,
                     uint64_t *funcmap_rbtree_root_ptr,
                     void *HelperFuncs,
                     size_t HelperFuncsCnt
                     );

uint64_t jit_find_symbol(jit_context_t* ctx, const char* name);

typedef uint64_t (*jit_function)(uint64_t arg1, uint64_t arg2);
uint64_t jit_execute(jit_context_t* ctx, uint64_t entry_point,
                     uint64_t arg1, uint64_t arg2);

void jit_free_regions(jit_memory_region* regions, size_t count);

#ifdef __cplusplus
}
#endif

#endif
