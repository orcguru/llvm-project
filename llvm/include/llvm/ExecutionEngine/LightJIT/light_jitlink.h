#ifndef LIGHT_JITLINK_H
#define LIGHT_JITLINK_H

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdio>

// 前向声明 ELF 结构
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t Elf64_Sxword;

struct Elf64_Ehdr {
    unsigned char e_ident[16];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off e_phoff;
    Elf64_Off e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
};

struct Elf64_Shdr {
    Elf64_Word sh_name;
    Elf64_Word sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr sh_addr;
    Elf64_Off sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word sh_link;
    Elf64_Word sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
};

struct Elf64_Sym {
    Elf64_Word st_name;
    unsigned char st_info;
    unsigned char st_other;
    Elf64_Half st_shndx;
    Elf64_Addr st_value;
    Elf64_Xword st_size;
};

struct Elf64_Rela {
    Elf64_Addr r_offset;
    Elf64_Xword r_info;
    Elf64_Sxword r_addend;
};

// ELF 常量
#define SHT_RELA 4
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_NOBITS 8
#define SHN_UNDEF 0

// 宏用于访问重定位信息
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffffL)

// 在 light_jitlink.h 的 JITContext 结构中添加
struct Hi20RelocationInfo {
    uint64_t hi20TargetAddr;  // HI20 重定位的目标地址
    uint64_t hi20RelocAddr;   // HI20 重定位的地址
    int64_t hi20Addend;       // HI20 重定位的加数
};

struct GOTEntryInfo {
    uint64_t gotAddr;      // GOT条目的地址
    uint64_t slotIndex;    // 槽位索引
    uint64_t targetAddr;   // 目标地址
};

struct PLTEntryInfo {
    uint64_t pltAddr;      // PLT条目的地址
    uint64_t slotIndex;    // 槽位索引
    uint64_t gotEntryAddr; // 关联的GOT条目地址
};

// 更新 PLT 条目结构为3条指令
struct AArch64PLTEntry {
    uint32_t instr0;  // adrp x16, [GOT entry page]
    uint32_t instr1;  // ldr  x16, [x16, #offset in page]
    uint32_t instr2;  // br   x16
    uint32_t padding; // 填充，使结构为16字节对齐
};

// 添加架构类型定义
enum class ArchType {
    Unknown,
    AArch64,
    RISCV64
};

// 简化的链接上下文
struct JITContext {
    // 架构类型
    ArchType TargetArch = ArchType::Unknown;

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

    // GOT 相关
    std::unordered_map<std::string, uint64_t> GotSymbolMap;  // 符号名 -> GOT 条目索引
    uint64_t NextGOTIndex = 0;
    uint64_t NextPLTIndex = 0;

    // 在 JITContext 中添加
    std::unordered_map<uint64_t, Hi20RelocationInfo> Hi20RelocationMap;
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
    Elf64_Half getMachineType() const;
};

// 极简链接器
class MinimalJITLinker {
private:
    JITContext& Ctx;
    
public:
    MinimalJITLinker(JITContext& ctx);
    ~MinimalJITLinker();
    
    // 主链接函数
    bool link(const char* objectData, size_t objectSize, uint64_t baseAddress,
              uint64_t startCode,
              void (*register_mapping)(uint64_t, uint64_t, uint64_t),
              void (*log_message)(const char *),
              const char *AotFile
              );
    
private:
    size_t calculateTotalSize(const MinimalELF64Parser& parser);
    bool allocateMemory(size_t size, uint64_t preferredAddr);
    void buildSymbolTable(const MinimalELF64Parser& parser);
    bool copySectionsAndRelocate(const MinimalELF64Parser& parser,
                                uint64_t startCode,
                                void (*register_mapping)(uint64_t, uint64_t, uint64_t),
                                void (*log_message)(const char *),
                                const char *AotFile);
    bool processRelocations(const MinimalELF64Parser& parser,
                           struct Elf64_Shdr* relocShdr,
                           char* memory,
                           uint64_t baseAddr);
    bool applyRelocation(uint32_t type, char* location, 
                        uint64_t targetAddr, int64_t addend,
                        uint64_t relocAddr, uint64_t relocOffset);
    bool setupPLTAndGOT();
    uint64_t getPLTEntryForSymbol(const std::string& symbolName);
    uint64_t getGOTEntryForSymbol(const std::string& symbolName, uint64_t targetAddr);
    void generatePLTEntry(uint64_t pltAddr, uint64_t gotAddr, size_t slotIndex);
    void generateRISCVPLTEntry(uint64_t pltAddr, uint64_t gotAddr, size_t slotIndex);
    void detectArchitecture(const MinimalELF64Parser& parser);

    void printMemoryLayout(const MinimalELF64Parser& parser);
    void printSectionsInfo(const MinimalELF64Parser& parser);
};

#endif // LIGHT_JITLINK_H
