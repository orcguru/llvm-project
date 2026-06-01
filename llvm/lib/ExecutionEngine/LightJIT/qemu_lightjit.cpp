// qemu_lightjit.cpp
#include "llvm/ExecutionEngine/LightJIT/qemu_lightjit.h"
#include "llvm/ExecutionEngine/LightJIT/light_jitlink.h"
#include <cstring>
#include <cstdio>
#include <sys/mman.h>
#include <iostream>

extern "C" {

// 实际的 jit_context 定义
struct jit_context {
    JITContext impl;
};

jit_context_t* jit_context_create(void) {
    auto ctx = new jit_context();
    return ctx;
}

void jit_context_destroy(jit_context_t* ctx) {
    if (ctx) {
        // 释放分配的内存
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
                     size_t* region_count) {
    if (!ctx || !aot_data || aot_size == 0) {
        return 0;
    }

    // 创建链接器
    MinimalJITLinker linker(ctx->impl);

    // 执行链接
    if (!linker.link(static_cast<const char*>(aot_data),
                     aot_size, base_address)) {
        return 0;
    }

    // 设置输出区域
    if (allocated_regions && region_count) {
        *allocated_regions = (jit_memory_region*)malloc(
            sizeof(jit_memory_region));
        if (!*allocated_regions) {
            return 0;
        }

        jit_memory_region* region = *allocated_regions;
        region->start = ctx->impl.CurrentAlloc.Memory;
        region->size = ctx->impl.CurrentAlloc.Size;
        region->permissions = 7; // RWX
        *region_count = 1;
    }

    // 返回入口地址
    MinimalELF64Parser parser(static_cast<const char*>(aot_data), aot_size);
    uint64_t entry = parser.getEntryPoint();
    return ctx->impl.CurrentAlloc.BaseAddress + entry;
}

// Add to qemu_lightjit.cpp, in the extern "C" block
uint64_t jit_link_aot_with_helpers(jit_context_t* ctx,
                                  const void* aot_data,
                                  size_t aot_size,
                                  uint64_t base_address,
                                  const char* helper_file,
                                  jit_memory_region** allocated_regions,
                                  size_t* region_count) {
    if (!ctx || !aot_data || aot_size == 0) {
        return 0;
    }

    // 创建链接器
    MinimalJITLinker linker(ctx->impl);

    // 如果提供了helper文件，解析并添加到符号表
    if (helper_file) {
        FILE* f = fopen(helper_file, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                char name[128];
                uint64_t address;
                // 解析格式: "function_name address"
                if (sscanf(line, "%127s %lx", name, &address) == 2) {
                    ctx->impl.SymbolTable[name] = address;
                    std::cout << "Added helper: " << name << " = 0x"
                              << std::hex << address << std::dec << std::endl;
                }
            }
            fclose(f);
        } else {
            fprintf(stderr, "Warning: Could not open helper file: %s\n", helper_file);
        }
    }

    // 执行链接
    if (!linker.link(static_cast<const char*>(aot_data),
                     aot_size, base_address)) {
        return 0;
    }

    // 设置输出区域
    if (allocated_regions && region_count) {
        *allocated_regions = (jit_memory_region*)malloc(
            sizeof(jit_memory_region));
        if (!*allocated_regions) {
            return 0;
        }

        jit_memory_region* region = *allocated_regions;
        region->start = ctx->impl.CurrentAlloc.Memory;
        region->size = ctx->impl.CurrentAlloc.Size;
        region->permissions = 7; // RWX
        *region_count = 1;
    }

    // 返回入口地址
    MinimalELF64Parser parser(static_cast<const char*>(aot_data), aot_size);
    uint64_t entry = parser.getEntryPoint();
    return ctx->impl.CurrentAlloc.BaseAddress + entry;
}

uint64_t jit_find_symbol(jit_context_t* ctx, const char* name) {
    if (!ctx || !name) {
        return 0;
    }

    auto it = ctx->impl.SymbolTable.find(name);
    if (it != ctx->impl.SymbolTable.end()) {
        return it->second;
    }

    return 0;
}

uint64_t jit_execute(jit_context_t* ctx, uint64_t entry_point,
                     uint64_t arg1, uint64_t arg2) {
    if (!ctx || !entry_point) {
        return 0;
    }

    jit_function func = (jit_function)entry_point;

    // 确保内存可执行
    if (mprotect(ctx->impl.CurrentAlloc.Memory,
                 ctx->impl.CurrentAlloc.Size,
                 PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect failed");
        return 0;
    }

    // 执行代码
    uint64_t result = func(arg1, arg2);

    return result;
}

void jit_free_regions(jit_memory_region* regions, size_t count) {
    if (regions) {
        free(regions);
    }
}

} // extern "C"
