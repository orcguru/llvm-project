// qemu_lightjit.cpp
#include "llvm/ExecutionEngine/LightJIT/qemu_lightjit.h"
#include "llvm/ExecutionEngine/LightJIT/light_jitlink.h"
#include <cstring>
#include <cstdio>
#include <sys/mman.h>
#include <iostream>

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
            fprintf(stderr, "ERROR: Could not open helper file: %s\n", helper_file);
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

// 主函数实现
uint64_t invoke_lightlink(const char *AotFile,
                         uint64_t StartCode,
                         void (*register_mapping)(uint64_t, uint64_t, uint64_t),
                         void (*log_mapping)(const char *, uint64_t),
                         void (*log_message)(const char *),
                         void *HelperFuncs,
                         size_t HelperFuncsCnt,
                         int enable_llvm_debug,
                         const char *entry) {

    // 设置调试输出
    if (log_message) {
        log_message("Starting invoke_lightlink...");
    }

    // 验证输入参数
    if (!AotFile) {
        if (log_message) {
            log_message("ERROR: AotFile is null");
        }
        return 1;
    }

    // 1. 读取 ELF 文件
    std::ifstream file(AotFile, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        if (log_message) {
            std::string msg = "ERROR: Cannot open file: " + std::string(AotFile);
            log_message(msg.c_str());
        }
        return 1;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    char* file_data = new char[size];
    if (!file.read(file_data, size)) {
        if (log_message) {
            std::string msg = "ERROR: Cannot read file: " + std::string(AotFile);
            log_message(msg.c_str());
        }
        delete[] file_data;
        return 1;
    }

    if (log_message) {
        std::stringstream msg;
        msg << "Loaded ELF file: " << AotFile << ", size: " << size << " bytes";
        log_message(msg.str().c_str());
    }

    // 2. 创建 JIT 上下文
    jit_context_t* ctx = jit_context_create();
    if (!ctx) {
        if (log_message) {
            log_message("ERROR: Failed to create JIT context");
        }
        delete[] file_data;
        return 1;
    }

    // 3. 添加助手函数到符号表
    if (HelperFuncs && HelperFuncsCnt > 0) {
        helper_func_t* helpers = static_cast<helper_func_t*>(HelperFuncs);
        for (size_t i = 0; i < HelperFuncsCnt; i++) {
            if (helpers[i].name) {
                ctx->impl.SymbolTable[helpers[i].name] = helpers[i].addr;
                if (log_message) {
                    std::stringstream msg;
                    msg << "Added helper: " << helpers[i].name
                        << " = 0x" << std::hex << helpers[i].addr << std::dec;
                    log_message(msg.str().c_str());
                }
            }
        }
    }

    // 4. 链接 ELF 文件
    jit_memory_region* regions = nullptr;
    size_t region_count = 0;

    uint64_t entry_point = jit_link_aot(ctx, file_data, size, 0,
                                        &regions, &region_count);

    if (entry_point == 0) {
        if (log_message) {
            log_message("ERROR: Failed to link AOT file");
        }
        jit_context_destroy(ctx);
        delete[] file_data;
        jit_free_regions(regions, region_count);
        return 1;
    }

    if (log_message) {
        std::stringstream msg;
        msg << "Successfully linked ELF file. Entry point: 0x" << std::hex << entry_point;
        log_message(msg.str().c_str());
    }

    // 5. 解析 ELF 符号表，处理函数映射
    MinimalELF64Parser parser(file_data, size);
    if (parser.isValid()) {
        // 查找符号表
        for (size_t i = 0; ; i++) {
            auto shdr = parser.getSectionHeader(i);
            if (!shdr) break;

            if (shdr->sh_type == SHT_SYMTAB) {
                const char* symtabData = parser.getData() + shdr->sh_offset;
                size_t symCount = shdr->sh_size / sizeof(Elf64_Sym);

                // 查找关联的字符串表
                auto strtabShdr = parser.getSectionHeader(shdr->sh_link);
                if (!strtabShdr) continue;
                const char* strtab = parser.getData() + strtabShdr->sh_offset;

                // 计算基地址偏移
                uint64_t baseAddr = ctx->impl.CurrentAlloc.BaseAddress;

                for (size_t j = 0; j < symCount; j++) {
                    const Elf64_Sym* sym = reinterpret_cast<const Elf64_Sym*>(
                        symtabData + j * sizeof(Elf64_Sym));

                    if (sym->st_name == 0) continue; // 无名符号

                    const char* symName = strtab + sym->st_name;

                    // 检查是否为函数符号（简单检查：有定义且不为0）
                    if (sym->st_shndx == 1) {
                        // 计算函数在内存中的地址
                        uint64_t funcAddr = 0;

                        if (sym->st_shndx < parser.getSize() / sizeof(Elf64_Shdr)) {
                            auto symSection = parser.getSectionHeader(sym->st_shndx);
                            if (symSection) {
                                // 计算函数地址
                                funcAddr = baseAddr + sym->st_value;
                            }
                        }

                        if (funcAddr != 0) {
                            // 检查函数名是否符合模式 *_func_[0-9a-f]+
                            std::string nameStr(symName);
                            std::regex pattern(".*_func_[0-9a-f]+$", std::regex_constants::icase);

                            if (std::regex_match(nameStr, pattern)) {
                                // 提取尾部的十六进制数
                                size_t funcPos = nameStr.rfind("_func_");
                                if (funcPos != std::string::npos) {
                                    std::string hexStr = nameStr.substr(funcPos + 6);
                                    char* endptr;
                                    uint64_t hexNum = strtoull(hexStr.c_str(), &endptr, 16);

                                    if (endptr != hexStr.c_str() && *endptr == '\0') {
                                        // 有效的十六进制数
                                        if (register_mapping) {
                                            register_mapping(StartCode, hexNum, funcAddr);
                                            /*
                                            if (log_message) {
                                                std::stringstream msg;
                                                msg << "Registered mapping: " << nameStr
                                                    << " (0x" << std::hex << hexNum << std::dec
                                                    << ") -> 0x" << std::hex << funcAddr
                                                    << " baseAddr: 0x" << baseAddr << " + sym->st_value: 0x" << sym->st_value << std::endl;
                                                log_message(msg.str().c_str());
                                            }
                                            */
                                        }
                                    } else {
                                        // 不是有效的十六进制数，使用 log_mapping
                                        if (log_mapping) {
                                            log_mapping(symName, funcAddr);
                                        }
                                    }
                                }
                            } else {
                                // 不符合模式，使用 log_mapping
                                if (log_mapping) {
                                    log_mapping(symName, funcAddr);
                                }
                            }
                        }
                    }
                }
                break;
            }
        }
    }

    /*
    // 6. 处理指定的入口点
    uint64_t final_entry = entry_point;
    if (entry && strlen(entry) > 0) {
        // 查找指定的入口符号
        uint64_t custom_entry = jit_find_symbol(ctx, entry);
        if (custom_entry != 0) {
            final_entry = custom_entry;
            if (log_message) {
                std::stringstream msg;
                msg << "Using custom entry point: " << entry
                    << " = 0x" << std::hex << final_entry;
                log_message(msg.str().c_str());
            }
        } else {
            if (log_message) {
                std::stringstream msg;
                msg << "WARNING: Custom entry point '" << entry
                    << "' not found, using default";
                log_message(msg.str().c_str());
            }
        }
    }
    */

    // 7. 清理
    //jit_context_destroy(ctx);
    delete[] file_data;
    jit_free_regions(regions, region_count);

    if (log_message) {
        log_message("invoke_lightlink completed successfully");
    }

    return 0;
}

} // extern "C"
