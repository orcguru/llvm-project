#ifndef LIGHT_JITLINK_H
#define LIGHT_JITLINK_H

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdio>

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

#define SHT_RELA 4
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_NOBITS 8
#define SHN_UNDEF 0

#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffffL)

struct Hi20RelocationInfo {
    uint64_t hi20TargetAddr;
    uint64_t hi20RelocAddr;
    int64_t hi20Addend;
};

struct GOTEntryInfo {
    uint64_t gotAddr;
    uint64_t slotIndex;
    uint64_t targetAddr;
};

struct PLTEntryInfo {
    uint64_t pltAddr;
    uint64_t slotIndex;
    uint64_t gotEntryAddr;
};

struct AArch64PLTEntry {
    uint32_t instr0;
    uint32_t instr1;
    uint32_t instr2;
    uint32_t padding;
};

enum class ArchType {
    Unknown,
    AArch64,
    RISCV64
};

struct JITContext {
    ArchType TargetArch = ArchType::Unknown;

    struct SymbolInfo {
        uint64_t value;
        bool resolved;
    };

    std::vector<SymbolInfo> SymbolTable;
    
    std::vector<AArch64PLTEntry> PLTEntries;
    std::vector<uint64_t> PLTSymbolMap;
    uint64_t PLTBaseAddr = 0;
    uint64_t GOTBaseAddr = 0;
    char* GOTPtr = nullptr;
    
    struct Allocation {
        char* Memory = nullptr;
        size_t Size = 0;
        uint64_t BaseAddress = 0;
    } CurrentAlloc;
    
    struct LoadedModule {
        uint64_t BaseAddress = 0;
        size_t CodeSize = 0;
    };
    std::vector<LoadedModule> Modules;

    std::vector<uint64_t> GotSymbolMap;
    uint64_t NextGOTIndex = 0;
    uint64_t NextPLTIndex = 0;

    std::unordered_map<uint64_t, Hi20RelocationInfo> Hi20RelocationMap;
};

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

class MinimalJITLinker {
private:
    JITContext& Ctx;
    
public:
    MinimalJITLinker(JITContext& ctx);
    ~MinimalJITLinker();
    
    bool link(const char* objectData, size_t objectSize, uint64_t baseAddress,
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
    
private:
    size_t calculateTotalSize(const MinimalELF64Parser& parser);
    bool allocateMemory(size_t size, uint64_t preferredAddr);
    void buildSymbolTable(const MinimalELF64Parser& parser, void *HelperFuncs, size_t HelperFuncsCnt);
    bool copySectionsAndRelocate(const MinimalELF64Parser& parser,
                                uint64_t startCode,
                                void (*register_mapping)(uint64_t, uint64_t, uint64_t),
                                void (*log_message)(const char *),
                                const char *AotFile,
                                void *(*g_malloc0)(uint64_t),
                                uint64_t *aot_code_base_ptr,
                                uint64_t *funcmap_rbtree_root_ptr
                                );
    bool processRelocations(const MinimalELF64Parser& parser,
                           struct Elf64_Shdr* relocShdr,
                           char* memory,
                           uint64_t baseAddr);
    bool applyRelocation(uint32_t type, char* location, 
                        uint64_t targetAddr, int64_t addend,
                        uint64_t relocAddr, uint64_t relocOffset);
    bool setupPLTAndGOT();
    uint64_t getPLTEntryForSymbol(uint32_t symIndex);
    uint64_t getGOTEntryForSymbol(uint32_t symIndex, uint64_t targetAddr);
    void generatePLTEntry(uint64_t pltAddr, uint64_t gotAddr, size_t slotIndex);
    void generateRISCVPLTEntry(uint64_t pltAddr, uint64_t gotAddr, size_t slotIndex);
    void detectArchitecture(const MinimalELF64Parser& parser);

    void printMemoryLayout(const MinimalELF64Parser& parser);
    void printSectionsInfo(const MinimalELF64Parser& parser);
};

#endif
