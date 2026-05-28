// qemu_lightjit.h
#ifndef QEMU_LIGHTJIT_H
#define QEMU_LIGHTJIT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 内存区域描述
typedef struct {
    void* start;
    size_t size;
    uint32_t permissions;  // 1=读, 2=写, 4=执行
} jit_memory_region;

// 重定位信息
typedef struct {
    uint64_t offset;      // 在代码中的偏移
    uint32_t type;        // 重定位类型
    int64_t addend;       // 加数
    const char* symbol;   // 符号名
} jit_relocation;

// 不透明的链接上下文结构
typedef struct jit_context jit_context_t;

// 创建链接上下文
jit_context_t* jit_context_create(void);

// 销毁链接上下文
void jit_context_destroy(jit_context_t* ctx);

// 加载 AOT 文件并执行最小化链接
// 参数：
//   ctx: 上下文
//   aot_data: AOT 文件数据
//   aot_size: 数据大小
//   base_address: 加载基址（0 表示自动分配）
//   allocated_regions: 输出的内存区域列表
//   region_count: 区域数量
// 返回：链接后的入口地址，0 表示失败
uint64_t jit_link_aot(jit_context_t* ctx,
                     const void* aot_data,
                     size_t aot_size,
                     uint64_t base_address,
                     jit_memory_region** allocated_regions,
                     size_t* region_count);

// 查找符号地址
uint64_t jit_find_symbol(jit_context_t* ctx, const char* name);

// 执行代码
typedef uint64_t (*jit_function)(uint64_t arg1, uint64_t arg2);
uint64_t jit_execute(jit_context_t* ctx, uint64_t entry_point,
                     uint64_t arg1, uint64_t arg2);

// 释放分配的内存区域
void jit_free_regions(jit_memory_region* regions, size_t count);

#ifdef __cplusplus
}
#endif

#endif // QEMU_LIGHTJIT_H
