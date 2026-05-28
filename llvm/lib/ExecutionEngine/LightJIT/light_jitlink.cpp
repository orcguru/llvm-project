// light_jitlink.cpp
#include "llvm/ExecutionEngine/LightJIT/qemu_lightjit.h"
#include "llvm/ExecutionEngine/LightJIT/light_jitlink.h"

// 包含必要的 LLVM 头文件
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Format.h"
#include "llvm/ADT/StringRef.h"

#include <memory>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <iostream>

using namespace llvm;

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
#define SHN_UNDEF 0

// 宏用于访问重定位信息
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffffL)

// MinimalELF64Parser 实现
MinimalELF64Parser::MinimalELF64Parser(const char* data, size_t size)
    : Data(data), Size(size) {
    if (Size >= sizeof(Elf64_Ehdr)) {
        Header = reinterpret_cast<Elf64_Ehdr*>(const_cast<char*>(Data));
    } else {
        Header = nullptr;
    }
}

MinimalELF64Parser::~MinimalELF64Parser() {
    // 无动态分配资源
}

bool MinimalELF64Parser::isValid() const {
    if (!Header) return false;
    // 简单的 ELF 魔数检查
    return (Header->e_ident[0] == 0x7f &&
            Header->e_ident[1] == 'E' &&
            Header->e_ident[2] == 'L' &&
            Header->e_ident[3] == 'F');
}

Elf64_Shdr* MinimalELF64Parser::getSectionHeader(size_t index) const {
    if (!Header || index >= Header->e_shnum) return nullptr;
    if (Header->e_shoff + (index + 1) * sizeof(Elf64_Shdr) > Size)
        return nullptr;
    return reinterpret_cast<Elf64_Shdr*>(
        const_cast<char*>(Data) + Header->e_shoff + index * sizeof(Elf64_Shdr));
}

const char* MinimalELF64Parser::getStringTable() const {
    if (!Header || Header->e_shstrndx >= Header->e_shnum)
        return nullptr;
    auto shstrtab = getSectionHeader(Header->e_shstrndx);
    if (!shstrtab) return nullptr;
    if (shstrtab->sh_offset + shstrtab->sh_size > Size)
        return nullptr;
    return Data + shstrtab->sh_offset;
}

const char* MinimalELF64Parser::getSectionName(size_t index) const {
    auto strtab = getStringTable();
    if (!strtab) return nullptr;
    auto shdr = getSectionHeader(index);
    if (!shdr) return nullptr;
    if (shdr->sh_name >= Size) return nullptr;
    return strtab + shdr->sh_name;
}

uint64_t MinimalELF64Parser::getEntryPoint() const {
    return Header ? Header->e_entry : 0;
}

const char* MinimalELF64Parser::getData() const {
    return Data;
}

size_t MinimalELF64Parser::getSize() const {
    return Size;
}

// MinimalJITLinker 实现
MinimalJITLinker::MinimalJITLinker(JITContext& ctx) : Ctx(ctx) {}

MinimalJITLinker::~MinimalJITLinker() {}

void MinimalJITLinker::printSectionsInfo(const MinimalELF64Parser& parser) {
    std::cout << "\n=== ELF Sections Info ===" << std::endl;
    for (size_t i = 0; ; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;

        std::cout << "Section " << i << ":" << std::endl;
        std::cout << "  Type: " << shdr->sh_type
                  << " (1=PROGBITS, 3=SYMTAB, 9=RELA)" << std::endl;
        std::cout << "  Flags: 0x" << std::hex << shdr->sh_flags << std::dec << std::endl;
        std::cout << "  Address: 0x" << std::hex << shdr->sh_addr << std::dec << std::endl;
        std::cout << "  Offset: 0x" << std::hex << shdr->sh_offset << std::dec << std::endl;
        std::cout << "  Size: " << shdr->sh_size << " bytes" << std::endl;
        std::cout << "  Addralign: " << shdr->sh_addralign << std::endl;
        std::cout << std::endl;
    }
}

bool MinimalJITLinker::link(const char* objectData, size_t objectSize, uint64_t baseAddress) {
    MinimalELF64Parser parser(objectData, objectSize);
    if (!parser.isValid()) {
        fprintf(stderr, "Invalid ELF file\n");
        return false;
    }

    printSectionsInfo(parser);

    // 1. 计算总大小
    size_t totalSize = calculateTotalSize(parser);
    if (totalSize == 0) {
        fprintf(stderr, "Failed to calculate total size\n");
        return false;
    }

    std::cout << "DEBUG: Required memory size = " << totalSize << " bytes" << std::endl;
    std::cout << "DEBUG: Preferred address = 0x" << std::hex << baseAddress << std::dec << std::endl;

    // 2. 分配内存
    if (!allocateMemory(totalSize, baseAddress)) {
        fprintf(stderr, "Failed to allocate memory\n");
        return false;
    }

    printMemoryLayout(parser);

    // 3. 构建符号表
    buildSymbolTable(parser);

    // 4. 复制段并处理重定位
    if (!copySectionsAndRelocate(parser)) {
        fprintf(stderr, "Failed to copy sections and relocate\n");
        return false;
    }

    return true;
}

size_t MinimalJITLinker::calculateTotalSize(const MinimalELF64Parser& parser) {
    uint64_t minAddr = UINT64_MAX;
    uint64_t maxAddr = 0;
    
    // 遍历所有节头，直到遇到 nullptr
    for (size_t i = 0; ; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;
        
        // 只考虑需要加载的段（如 PROGBITS）
        if (shdr->sh_type == 1) {  // SHT_PROGBITS
            if (shdr->sh_size > 0) {
                uint64_t start = shdr->sh_addr;
                uint64_t end = shdr->sh_addr + shdr->sh_size;
                
                if (start < minAddr) minAddr = start;
                if (end > maxAddr) maxAddr = end;
            }
        }
    }
    
    if (minAddr == UINT64_MAX) {
        return 0;  // 没有需要加载的段
    }
    
    return maxAddr - minAddr;
}

bool MinimalJITLinker::allocateMemory(size_t size, uint64_t preferredAddr) {
    // 使用 mmap 分配可读写执行内存
    void* mem = mmap(reinterpret_cast<void*>(preferredAddr),
                    size,
                    PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS |
                    (preferredAddr ? MAP_FIXED : 0),
                    -1, 0);

    if (mem == MAP_FAILED) {
        perror("mmap failed");
        return false;
    }

    Ctx.CurrentAlloc.Memory = static_cast<char*>(mem);
    Ctx.CurrentAlloc.Size = size;
    Ctx.CurrentAlloc.BaseAddress = reinterpret_cast<uint64_t>(mem);
    std::cout << "Ctx.CurrentAlloc.Size:" << Ctx.CurrentAlloc.Size << std::endl;
    return true;
}

void MinimalJITLinker::buildSymbolTable(const MinimalELF64Parser& parser) {
    // 查找符号表
    for (size_t i = 0; i < 16; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;

        if (shdr->sh_type == SHT_SYMTAB) {
            const char* symtabData = parser.getData() + shdr->sh_offset;
            size_t symCount = shdr->sh_size / sizeof(Elf64_Sym);

            // 查找关联的字符串表
            auto strtabShdr = parser.getSectionHeader(shdr->sh_link);
            if (!strtabShdr) continue;
            const char* strtab = parser.getData() + strtabShdr->sh_offset;

            for (size_t j = 0; j < symCount; j++) {
                const Elf64_Sym* sym = reinterpret_cast<const Elf64_Sym*>(
                    symtabData + j * sizeof(Elf64_Sym));

                if (sym->st_name == 0) continue; // 无名符号

                const char* name = strtab + sym->st_name;
                uint64_t value = sym->st_value;

                if (value != 0 && sym->st_shndx != SHN_UNDEF) {
                    Ctx.SymbolTable[name] = value;
                }
            }
            break;
        }
    }
}

bool MinimalJITLinker::copySectionsAndRelocate(const MinimalELF64Parser& parser) {
    char* memory = Ctx.CurrentAlloc.Memory;
    uint64_t baseAddr = Ctx.CurrentAlloc.BaseAddress;
    
    // 计算最低的段地址
    uint64_t lowestAddr = UINT64_MAX;
    for (size_t i = 0; ; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;
        
        if (shdr->sh_type == 1 && shdr->sh_size > 0) {  // SHT_PROGBITS
            if (shdr->sh_addr < lowestAddr) {
                lowestAddr = shdr->sh_addr;
            }
        }
    }
    
    if (lowestAddr == UINT64_MAX) {
        fprintf(stderr, "No sections to load\n");
        return false;
    }
    
    // 首先复制所有 PROGBITS 段
    for (size_t i = 0; ; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;
        
        if (shdr->sh_type == 1 && shdr->sh_size > 0) { // SHT_PROGBITS
            const char* src = parser.getData() + shdr->sh_offset;
            
            // 计算内存中的正确偏移
            uint64_t offset = shdr->sh_addr - lowestAddr;
            
            if (offset + shdr->sh_size > Ctx.CurrentAlloc.Size) {
                fprintf(stderr, "Section exceeds allocated memory: offset=%lu, size=%lu, alloc=%lu\n",
                       offset, shdr->sh_size, Ctx.CurrentAlloc.Size);
                return false;
            }
            
            if (shdr->sh_offset + shdr->sh_size > parser.getSize()) {
                fprintf(stderr, "Section out of bounds in ELF file\n");
                return false;
            }
            
            char* dst = memory + offset;
            
            std::cout << "DEBUG: shdr->sh_addr=0x" << std::hex << shdr->sh_addr 
                     << ", lowestAddr=0x" << lowestAddr 
                     << ", offset=0x" << offset 
                     << ", size=" << std::dec << shdr->sh_size << std::endl;
            
            memcpy(dst, src, shdr->sh_size);
            
            // 记录可执行段
            if (shdr->sh_flags & 4) { // SHF_EXECINSTR
                Ctx.Modules.push_back({baseAddr + offset, shdr->sh_size});
            }
        }
    }
    return true;
}

void MinimalJITLinker::printMemoryLayout(const MinimalELF64Parser& parser) {
    std::cout << "=== Memory Layout Analysis ===" << std::endl;
    std::cout << "Allocated size: " << Ctx.CurrentAlloc.Size << " bytes" << std::endl;

    uint64_t minAddr = UINT64_MAX;
    uint64_t maxAddr = 0;

    for (size_t i = 0; ; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;

        if (shdr->sh_type == 1 && shdr->sh_size > 0) { // SHT_PROGBITS
            uint64_t start = shdr->sh_addr;
            uint64_t end = start + shdr->sh_size;

            std::cout << "Section " << i
                     << ": addr=0x" << std::hex << start
                     << ", size=0x" << shdr->sh_size
                     << ", end=0x" << end << std::dec << std::endl;

            if (start < minAddr) minAddr = start;
            if (end > maxAddr) maxAddr = end;
        }
    }

    if (minAddr != UINT64_MAX) {
        uint64_t required = maxAddr - minAddr;
        std::cout << "Memory range: 0x" << std::hex << minAddr
                 << " - 0x" << maxAddr
                 << " (size: 0x" << required << " = " << std::dec << required << " bytes)" << std::endl;
    }
}

bool MinimalJITLinker::processRelocations(const MinimalELF64Parser& parser,
                       Elf64_Shdr* relocShdr,
                       char* memory,
                       uint64_t baseAddr) {
    const char* relocData = parser.getData() + relocShdr->sh_offset;
    size_t relocCount = relocShdr->sh_size / sizeof(Elf64_Rela);

    // 查找关联的符号表
    auto symtabShdr = parser.getSectionHeader(relocShdr->sh_link);
    if (!symtabShdr) {
        fprintf(stderr, "No symbol table for relocations\n");
        return false;
    }

    const char* symtabData = parser.getData() + symtabShdr->sh_offset;

    // 查找字符串表
    auto strtabShdr = parser.getSectionHeader(symtabShdr->sh_link);
    if (!strtabShdr) {
        fprintf(stderr, "No string table for symbols\n");
        return false;
    }
    const char* strtab = parser.getData() + strtabShdr->sh_offset;

    for (size_t i = 0; i < relocCount; i++) {
        const Elf64_Rela* rela = reinterpret_cast<const Elf64_Rela*>(
            relocData + i * sizeof(Elf64_Rela));

        uint32_t symIndex = ELF64_R_SYM(rela->r_info);
        uint32_t type = ELF64_R_TYPE(rela->r_info);

        const Elf64_Sym* sym = reinterpret_cast<const Elf64_Sym*>(
            symtabData + symIndex * sizeof(Elf64_Sym));

        const char* symName = strtab + sym->st_name;
        uint64_t symValue = sym->st_value;
        uint64_t targetAddr = 0;

        // 解析符号地址
        if (sym->st_shndx != SHN_UNDEF) {
            targetAddr = baseAddr + symValue;
        } else {
            // 查找全局符号表
            auto it = Ctx.SymbolTable.find(symName);
            if (it != Ctx.SymbolTable.end()) {
                targetAddr = it->second;
            } else {
                fprintf(stderr, "Undefined symbol: %s\n", symName);
                return false;
            }
        }

        // 应用重定位
        uint64_t location = baseAddr + rela->r_offset;
        char* locPtr = memory + rela->r_offset;

        if (!applyRelocation(type, locPtr, targetAddr,
                           rela->r_addend, location)) {
            fprintf(stderr, "Failed to apply relocation type %u\n", type);
            return false;
        }
    }

    return true;
}

bool MinimalJITLinker::applyRelocation(uint32_t type, char* location,
                    uint64_t targetAddr, int64_t addend,
                    uint64_t relocAddr) {
    uint64_t value = targetAddr + addend;

    switch (type) {
    case 1: // R_X86_64_64
        *reinterpret_cast<uint64_t*>(location) = value;
        break;
    case 2: // R_X86_64_PC32
        {
            int64_t pcRelative = value - (relocAddr + 4);
            *reinterpret_cast<int32_t*>(location) = pcRelative;
        }
        break;
    case 10: // R_X86_64_32
        *reinterpret_cast<uint32_t*>(location) = value;
        break;
    case 11: // R_X86_64_32S
        *reinterpret_cast<int32_t*>(location) = value;
        break;
    // 添加更多重定位类型...
    default:
        fprintf(stderr, "Unsupported relocation type: %u\n", type);
        return false;
    }
    return true;
}
