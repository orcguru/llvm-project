// qemu_lightjit.cpp
#include "llvm/ExecutionEngine/LightJIT/qemu_lightjit.h"
#include "llvm/ExecutionEngine/LightJIT/light_jitlink.h"
#include <cstring>
#include <cstdio>
#include <sys/mman.h>
#include <iostream>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>

//#define DEBUG

#include <regex>
#include <fstream>
#include <sstream>

extern "C" {

struct jit_context {
    JITContext impl;
};

jit_context_t* jit_context_create(void) {
    auto ctx = new jit_context();
    return ctx;
}

void jit_context_destroy(jit_context_t* ctx) {
    if (ctx) {
        if (ctx->impl.CurrentAlloc.Memory) {
            munmap(ctx->impl.CurrentAlloc.Memory,
                   ctx->impl.CurrentAlloc.Size);
        }
        delete ctx;
    }
}

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
                     ) {
    if (!ctx || !aot_data || aot_size == 0) {
        return 0;
    }

    MinimalJITLinker linker(ctx->impl);

    if (!linker.link(static_cast<const char*>(aot_data),
                     aot_size, base_address, startCode, register_mapping, log_message, AotFile, g_malloc0, aot_code_base_ptr, funcmap_rbtree_root_ptr, HelperFuncs, HelperFuncsCnt)) {
        return 0;
    }

    if (allocated_regions && region_count) {
        *allocated_regions = (jit_memory_region*)malloc(
            sizeof(jit_memory_region));
        if (!*allocated_regions) {
            return 0;
        }

        jit_memory_region* region = *allocated_regions;
        region->start = ctx->impl.CurrentAlloc.Memory;
        region->size = ctx->impl.CurrentAlloc.Size;
        region->permissions = 7;
        *region_count = 1;
    }
    return 1;
}

uint64_t jit_execute(jit_context_t* ctx, uint64_t entry_point,
                     uint64_t arg1, uint64_t arg2) {
    if (!ctx || !entry_point) {
        return 0;
    }

    jit_function func = (jit_function)entry_point;

    if (mprotect(ctx->impl.CurrentAlloc.Memory,
                 ctx->impl.CurrentAlloc.Size,
                 PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect failed");
        return 0;
    }

    uint64_t result = func(arg1, arg2);

    return result;
}

void jit_free_regions(jit_memory_region* regions, size_t count) {
    if (regions) {
        free(regions);
    }
}

uint64_t invoke_lightlink(const char *AotFile,
                         uint64_t StartCode,
                         void (*register_mapping)(uint64_t, uint64_t, uint64_t),
                         void (*log_mapping)(const char *, uint64_t),
                         void (*log_message)(const char *),
                         void *(*g_malloc0)(uint64_t),
                         void *HelperFuncs,
                         size_t HelperFuncsCnt,
                         int enable_llvm_debug,
                         const char *entry,
                         uint64_t *aot_code_base_ptr,
                         uint64_t *funcmap_rbtree_root_ptr) {
    if (!AotFile) {
        if (log_message) {
            log_message("ERROR: AotFile is null");
        }
        return 1;
    }

    int fd = open(AotFile, O_RDONLY);
    assert(fd != -1);
    struct stat st;
    fstat(fd, &st);
    void *file_data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(file_data);

    jit_context_t* ctx = jit_context_create();
    if (!ctx) {
        if (log_message) {
            log_message("ERROR: Failed to create JIT context");
        }
        munmap(file_data, st.st_size);
        close(fd);
        return 1;
    }

    jit_memory_region* regions = nullptr;
    size_t region_count = 0;

#ifndef DEBUG
    if (jit_link_aot(ctx, file_data, st.st_size, 0, &regions, &region_count, StartCode, register_mapping, log_message, AotFile, g_malloc0, aot_code_base_ptr, funcmap_rbtree_root_ptr, HelperFuncs, HelperFuncsCnt) == 0) {
        if (log_message) {
            log_message("ERROR: Failed to link AOT file");
        }
        jit_context_destroy(ctx);
        munmap(file_data, st.st_size);
        close(fd);
        jit_free_regions(regions, region_count);
        return 1;
    }
#else
    if (jit_link_aot(ctx, file_data, st.st_size, 0, &regions, &region_count, StartCode, register_mapping, log_message, AotFile, g_malloc0, aot_code_base_ptr, funcmap_rbtree_root_ptr, HelperFuncs, HelperFuncsCnt) == 0) {
        if (log_message) {
            log_message("ERROR: Failed to link AOT file");
        }
        jit_context_destroy(ctx);
        munmap(file_data, st.st_size);
        close(fd);
        jit_free_regions(regions, region_count);
        return 1;
    }
#endif

    munmap(file_data, st.st_size);
    close(fd);
    jit_free_regions(regions, region_count);

    return 0;
}

} // extern "C"
