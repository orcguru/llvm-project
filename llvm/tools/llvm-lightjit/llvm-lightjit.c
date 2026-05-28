// test_lightjit.c
#include "llvm/ExecutionEngine/LightJIT/qemu_lightjit.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <object_file.o>\n", argv[0]);
        return 1;
    }
    
    // 打开目标文件
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    
    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return 1;
    }
    
    // 映射文件
    void* file_data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file_data == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    
    // 创建 JIT 上下文
    jit_context_t* ctx = jit_context_create();
    if (!ctx) {
        printf("Failed to create JIT context\n");
        munmap(file_data, st.st_size);
        close(fd);
        return 1;
    }
    
    // 链接
    jit_memory_region* regions = NULL;
    size_t region_count = 0;
    
    uint64_t entry = jit_link_aot(ctx, file_data, st.st_size, 0,
                                 &regions, &region_count);
    
    if (entry == 0) {
        printf("Failed to link object file\n");
    } else {
        printf("Successfully linked object file\n");
        printf("Entry point: 0x%lx\n", entry);
        printf("Allocated %zu regions:\n", region_count);
        
        for (size_t i = 0; i < region_count; i++) {
            printf("  Region %zu: 0x%lx - 0x%lx (size: 0x%zx)\n",
                   i, (uint64_t)regions[i].start,
                   (uint64_t)regions[i].start + regions[i].size,
                   regions[i].size);
        }
        
        // 查找符号
        const char* test_symbols[] = {"main", "_start", "foo", "bar", NULL};
        for (int i = 0; test_symbols[i]; i++) {
            uint64_t addr = jit_find_symbol(ctx, test_symbols[i]);
            if (addr) {
                printf("Found symbol '%s' at 0x%lx\n", test_symbols[i], addr);
            }
        }
        
        // 执行代码（可选）
        // uint64_t result = jit_execute(ctx, entry, 0, 0);
        // printf("Execution result: 0x%lx\n", result);
    }
    
    // 清理
    jit_free_regions(regions, region_count);
    jit_context_destroy(ctx);
    munmap(file_data, st.st_size);
    close(fd);
    
    return 0;
}
