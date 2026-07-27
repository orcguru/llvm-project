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

// 定义 helper 函数结构
typedef struct helper_func {
    const char *name;
    uint64_t addr;
} helper_func_t;

// 定义用于匹配函数名的正则表达式
#include <regex>
#include <fstream>
#include <sstream>

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
                     size_t* region_count,
                     uint64_t startCode,
                     void (*register_mapping)(uint64_t, uint64_t, uint64_t),
                     void (*log_message)(const char *),
                     const char *AotFile,
                     void *(*g_malloc0)(uint64_t),
                     uint64_t *aot_code_base_ptr,
                     uint64_t *funcmap_rbtree_root_ptr) {
    if (!ctx || !aot_data || aot_size == 0) {
        return 0;
    }

    // 创建链接器
    MinimalJITLinker linker(ctx->impl);

    // 执行链接
    if (!linker.link(static_cast<const char*>(aot_data),
                     aot_size, base_address, startCode, register_mapping, log_message, AotFile, g_malloc0, aot_code_base_ptr, funcmap_rbtree_root_ptr)) {
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
    return 1;
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
#ifdef DEBUG
                    std::cout << "Added helper: " << name << " = 0x"
                              << std::hex << address << std::dec << std::endl;
#endif
                }
            }
            fclose(f);
        } else {
            fprintf(stderr, "ERROR: Could not open helper file: %s\n", helper_file);
        }
    }

    // 执行链接
    if (!linker.link(static_cast<const char*>(aot_data),
                     aot_size, base_address, 0, NULL, NULL, NULL, NULL, NULL, NULL)) {
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

// 主函数实现
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
    // 验证输入参数
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

    // 2. 创建 JIT 上下文
    jit_context_t* ctx = jit_context_create();
    if (!ctx) {
        if (log_message) {
            log_message("ERROR: Failed to create JIT context");
        }
        munmap(file_data, st.st_size);
        close(fd);
        return 1;
    }

    // 3. 添加助手函数到符号表
    if (HelperFuncs && HelperFuncsCnt > 0) {
        helper_func_t* helpers = static_cast<helper_func_t*>(HelperFuncs);
        for (size_t i = 0; i < HelperFuncsCnt; i++) {
            if (helpers[i].name) {
                ctx->impl.SymbolTable[helpers[i].name] = helpers[i].addr;
            }
        }
    }

    // 4. 链接 ELF 文件
    jit_memory_region* regions = nullptr;
    size_t region_count = 0;

#ifndef DEBUG
    if (jit_link_aot(ctx, file_data, st.st_size, 0, &regions, &region_count, StartCode, register_mapping, log_message, AotFile, g_malloc0, aot_code_base_ptr, funcmap_rbtree_root_ptr) == 0) {
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
    if (jit_link_aot(ctx, file_data, st.st_size, 0, &regions, &region_count, StartCode, register_mapping, log_message, AotFile, g_malloc0, aot_code_base_ptr, funcmap_rbtree_root_ptr) == 0) {
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
