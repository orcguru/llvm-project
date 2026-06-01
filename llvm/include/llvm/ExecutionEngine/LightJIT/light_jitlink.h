#ifndef LIGHT_JITLINK_H
#define LIGHT_JITLINK_H

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdio>

// PLT 条目结构
struct AArch64PLTEntry {
    uint32_t instr0;  // ldr x16, [x16, #offset]  (loading from GOT.PLT)
    uint32_t instr1;  // br x16                   (jump to target)
};

// 简化的链接上下文
struct JITContext {
    // 符号表
    std::unordered_map<std::string, uint64_t> SymbolTable;
    
    // PLT 和 GOT 相关
    std::vector<AArch64PLTEntry> PLTEntries;
    std::unordered_map<std::string, uint64_t> PLTSymbolMap;  // symbol name -> PLT slot index
    uint64_t PLTBaseAddr = 0;
    uint64_t GOTBaseAddr = 0;
    char* GOTPtr = nullptr;
    
    // 当前分配的内存
    struct Allocation {
        char* Memory = nullptr;
        size_t Size = 0;
        uint64_t BaseAddress = 0;
    } CurrentAlloc;
    
    // 加载的模块信息
    struct LoadedModule {
        uint64_t BaseAddress = 0;
        size_t CodeSize = 0;
    };
    std::vector<LoadedModule> Modules;
};

// 自定义的极简 ELF 解析器
class MinimalELF64Parser {
private:
    const char* Data;
    size_t Size;
    struct Elf64_Ehdr* Header;
    
public:
    MinimalELF64Parser(const char* data, size_t size);
    ~MinimalELF64Parser();
    
    bool isValid() const;
    struct Elf64_Shdr* getSectionHeader(size_t index) const;
    const char* getStringTable() const;
    const char* getSectionName(size_t index) const;
    uint64_t getEntryPoint() const;
    const char* getData() const;
    size_t getSize() const;
};

// 极简链接器
class MinimalJITLinker {
private:
    JITContext& Ctx;
    
public:
    MinimalJITLinker(JITContext& ctx);
    ~MinimalJITLinker();
    
    // 主链接函数
    bool link(const char* objectData, size_t objectSize, uint64_t baseAddress);
    
private:
    size_t calculateTotalSize(const MinimalELF64Parser& parser);
    bool allocateMemory(size_t size, uint64_t preferredAddr);
    void buildSymbolTable(const MinimalELF64Parser& parser);
    bool copySectionsAndRelocate(const MinimalELF64Parser& parser);
    bool processRelocations(const MinimalELF64Parser& parser,
                           struct Elf64_Shdr* relocShdr,
                           char* memory,
                           uint64_t baseAddr);
    bool applyRelocation(uint32_t type, char* location, 
                        uint64_t targetAddr, int64_t addend,
                        uint64_t relocAddr, uint64_t relocOffset);
    bool setupPLTAndGOT();
    uint64_t getPLTEntryForSymbol(const std::string& symbolName);

    void printMemoryLayout(const MinimalELF64Parser& parser);
    void printSectionsInfo(const MinimalELF64Parser& parser);
};

#endif // LIGHT_JITLINK_H
