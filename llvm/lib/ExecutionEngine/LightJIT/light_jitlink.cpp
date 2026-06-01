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
    std::cout << "Ctx.CurrentAlloc.BaseAddress:" << std::hex << Ctx.CurrentAlloc.BaseAddress << " Ctx.CurrentAlloc.Size:" << Ctx.CurrentAlloc.Size << std::endl;
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

    // 处理重定位节
    for (size_t i = 0; ; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;

        if (shdr->sh_type == SHT_RELA) {  // 处理重定位节
            std::cout << "Processing relocation section " << i << std::endl;
            if (!processRelocations(parser, shdr, memory, baseAddr - lowestAddr)) {
                fprintf(stderr, "Failed to process relocations for section %lu\n", i);
                return false;
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

bool MinimalJITLinker::setupPLTAndGOT() {
    // 检查 PLT 和 GOT 是否已经设置
    if (Ctx.PLTBaseAddr != 0 || Ctx.GOTBaseAddr != 0) {
        return true;  // 已经设置过了
    }

    // 计算 PLT 和 GOT 所需大小
    // 初始分配 16 个 PLT 条目，每个条目 2 条指令（8 字节）
    // 实际上 AArch64 PLT 条目通常是 16 字节，但为了简化我们先使用 8 字节
    size_t pltEntryCount = 16;
    size_t gotEntryCount = 16;

    size_t pltSize = pltEntryCount * 8;  // 每个 PLT 条目 8 字节
    size_t gotSize = gotEntryCount * 8;  // 每个 GOT 条目 8 字节

    std::cout << "DEBUG: Setting up PLT and GOT" << std::endl;
    std::cout << "  PLT entries: " << pltEntryCount << ", size: " << pltSize << " bytes" << std::endl;
    std::cout << "  GOT entries: " << gotEntryCount << ", size: " << gotSize << " bytes" << std::endl;

    // 删除未使用的变量 'newBase'，因为我们直接使用 Ctx.CurrentAlloc.BaseAddress

    // 在现有内存之后分配 PLT 和 GOT
    // 注意：我们需要确保 PLT 在代码附近（在 ±128MB 范围内）
    size_t newTotalSize = Ctx.CurrentAlloc.Size + pltSize + gotSize;

    // 尝试在当前内存之后扩展
    void* newMemory = mremap(Ctx.CurrentAlloc.Memory, Ctx.CurrentAlloc.Size,
                            newTotalSize, MREMAP_MAYMOVE);
    if (newMemory == MAP_FAILED) {
        // 如果 mremap 失败，分配新内存并复制
        std::cout << "DEBUG: mremap failed, allocating new memory" << std::endl;
        newMemory = mmap(nullptr, newTotalSize,
                        PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (newMemory == MAP_FAILED) {
            perror("mmap failed for PLT/GOT");
            return false;
        }

        // 复制现有数据
        memcpy(newMemory, Ctx.CurrentAlloc.Memory, Ctx.CurrentAlloc.Size);

        // 更新上下文
        Ctx.CurrentAlloc.Memory = static_cast<char*>(newMemory);
        Ctx.CurrentAlloc.Size = newTotalSize;
        Ctx.CurrentAlloc.BaseAddress = reinterpret_cast<uint64_t>(newMemory);
    } else {
        // 更新上下文
        Ctx.CurrentAlloc.Memory = static_cast<char*>(newMemory);
        Ctx.CurrentAlloc.Size = newTotalSize;
    }

    // 设置 PLT 和 GOT 地址
    Ctx.PLTBaseAddr = Ctx.CurrentAlloc.BaseAddress + (Ctx.CurrentAlloc.Size - pltSize - gotSize);
    Ctx.GOTBaseAddr = Ctx.PLTBaseAddr + pltSize;
    Ctx.GOTPtr = Ctx.CurrentAlloc.Memory + (Ctx.GOTBaseAddr - Ctx.CurrentAlloc.BaseAddress);

    std::cout << "DEBUG: PLT base: 0x" << std::hex << Ctx.PLTBaseAddr << std::dec << std::endl;
    std::cout << "DEBUG: GOT base: 0x" << std::hex << Ctx.GOTBaseAddr << std::dec << std::endl;

    // 初始化 PLT 条目向量
    Ctx.PLTEntries.resize(pltEntryCount);

    // 初始化 PLT 条目为跳转桩
    for (size_t i = 0; i < pltEntryCount; i++) {
        AArch64PLTEntry* entry = &Ctx.PLTEntries[i];

        // 简单的 PLT 桩代码：
        // 1. 从 GOT 加载目标地址到 x16
        // 2. 跳转到 x16
        // 注意：这里需要正确的 AArch64 指令编码

        // 删除未使用的变量 'gotEntryAddr'，因为我们目前还没有使用它
        // 后续如果需要生成实际的 PLT 指令，会需要这个地址

        // 简化：我们暂时不生成实际的指令，只是设置占位符
        // 实际实现中需要正确的 AArch64 指令编码
        entry->instr0 = 0x58000000;  // 占位符
        entry->instr1 = 0x58000000;  // 占位符

        // 将 PLT 条目写入内存
        char* pltLocation = Ctx.CurrentAlloc.Memory +
                           (Ctx.PLTBaseAddr - Ctx.CurrentAlloc.BaseAddress) +
                           (i * 8);
        *reinterpret_cast<AArch64PLTEntry*>(pltLocation) = *entry;
    }

    // 初始化 GOT 条目为 0
    uint64_t* gotEntries = reinterpret_cast<uint64_t*>(Ctx.GOTPtr);
    for (size_t i = 0; i < gotEntryCount; i++) {
        gotEntries[i] = 0;
    }

    return true;
}

uint64_t MinimalJITLinker::getPLTEntryForSymbol(const std::string& symbolName) {
    // 检查是否已经有这个符号的 PLT 条目
    auto it = Ctx.PLTSymbolMap.find(symbolName);
    if (it != Ctx.PLTSymbolMap.end()) {
        return Ctx.PLTBaseAddr + (it->second * 8);  // 每个条目 8 字节
    }

    // 分配新的 PLT 槽位
    size_t slotIndex = Ctx.PLTSymbolMap.size();
    if (slotIndex >= Ctx.PLTEntries.size()) {
        std::cerr << "ERROR: PLT table full, cannot allocate entry for symbol: "
                  << symbolName << std::endl;
        return 0;
    }

    // 在映射中记录符号
    Ctx.PLTSymbolMap[symbolName] = slotIndex;

    // 设置 GOT 条目
    uint64_t* gotEntries = reinterpret_cast<uint64_t*>(Ctx.GOTPtr);
    auto symIt = Ctx.SymbolTable.find(symbolName);
    if (symIt != Ctx.SymbolTable.end()) {
        // 如果符号已经在符号表中，直接设置 GOT 条目
        gotEntries[slotIndex] = symIt->second;
    } else {
        // 否则，设置为 0，稍后解析
        gotEntries[slotIndex] = 0;
    }

    uint64_t pltAddr = Ctx.PLTBaseAddr + (slotIndex * 8);
    std::cout << "DEBUG: Allocated PLT entry for symbol: " << symbolName
              << " at 0x" << std::hex << pltAddr << std::dec
              << " (slot " << slotIndex << ")" << std::endl;

    return pltAddr;
}

// Update the applyRelocation function to handle far jumps
bool MinimalJITLinker::applyRelocation(uint32_t type, char* location,
                    uint64_t targetAddr, int64_t addend,
                    uint64_t relocAddr, uint64_t relocOffset) {
    uint64_t value = targetAddr + addend;
    uint64_t instr;
    uint64_t result;
    int64_t page_offset;
    int64_t branch_offset;
    uint32_t imm;
    uint32_t imm26;

    switch (type) {
    // --- Existing AArch64 relocations ---
    case 277: // R_AARCH64_ADD_ABS_LO12_NC
        instr = *reinterpret_cast<uint32_t*>(location);
        result = (instr & ~(0xFFF << 10)) | ((value & 0xFFF) << 10);
        *reinterpret_cast<uint32_t*>(location) = result;
        break;

    case 311: // R_AARCH64_ADR_GOT_PAGE
    case 275: // R_AARCH64_ADR_PREL_PG_HI21
        instr = *reinterpret_cast<uint32_t*>(location);
        page_offset = ((value & ~0xFFFULL) - (relocAddr & ~0xFFFULL));
        if (page_offset < -((1LL) << 20) || page_offset >= ((1LL) << 20)) {
            fprintf(stderr, "Relocation R_AARCH64_ADR_PREL_PG_HI21/R_AARCH64_ADR_GOT_PAGE out of range: 0x%lx relocOffset:0x%lx\n", page_offset, relocOffset);
            return false;
        }
        imm = (page_offset >> 12) & 0x1FFFFF;
        instr &= ~(0x1FFFFF << 5);
        instr &= ~(0x3 << 29);
        instr |= ((imm >> 2) & 0x7FFFF) << 5;
        instr |= (imm & 0x3) << 29;
        *reinterpret_cast<uint32_t*>(location) = instr;
        break;

    case 283: // R_AARCH64_CALL26
    case 282: // R_AARCH64_JUMP26
        // Calculate the actual branch offset
        branch_offset = value - relocAddr;
        
        // Check if target is within range (±128MB)
        if (branch_offset >= -(1 << 27) && branch_offset < (1 << 27)) {
            // Target is within range, encode directly
            instr = *reinterpret_cast<uint32_t*>(location);
            imm26 = (branch_offset >> 2) & 0x3FFFFFF;
            instr = (instr & 0xFC000000) | imm26;
            *reinterpret_cast<uint32_t*>(location) = instr;
        } else {
            // Target is out of range, use PLT
            // We need to know which symbol this is for
            // Since we don't have the symbol name here, we'll need to modify
            // the caller to pass it. For now, we'll assume it's handled elsewhere
            fprintf(stderr, "R_AARCH64_JUMP26/CALL26 relocation out of range: offset=0x%lx\n", branch_offset);
            fprintf(stderr, "  Target=0x%lx, Relocation=0x%lx, Distance=0x%lx\n", 
                   value, relocAddr, static_cast<uint64_t>(llabs(branch_offset)));
            fprintf(stderr, "  PLT support not fully implemented in this version\n");
            return false;
        }
        break;

    case 312: // R_AARCH64_LD64_GOT_LO12_NC
        instr = *reinterpret_cast<uint32_t*>(location);
        result = (instr & ~(0xFFF << 10)) | (((value & 0xFF8) >> 3) << 10);
        *reinterpret_cast<uint32_t*>(location) = result;
        break;

    default:
        fprintf(stderr, "Unsupported relocation type: %u (0x%x)\n", type, type);
        return false;
    }
    return true;
}

// We need to update the processRelocations function to pass symbol names
bool MinimalJITLinker::processRelocations(const MinimalELF64Parser& parser,
                       Elf64_Shdr* relocShdr,
                       char* memory,
                       uint64_t baseAddr) {
    const char* relocData = parser.getData() + relocShdr->sh_offset;
    size_t relocCount = relocShdr->sh_size / sizeof(Elf64_Rela);

    // Find the associated symbol table
    auto symtabShdr = parser.getSectionHeader(relocShdr->sh_link);
    if (!symtabShdr) {
        fprintf(stderr, "No symbol table for relocations\n");
        return false;
    }

    const char* symtabData = parser.getData() + symtabShdr->sh_offset;

    // Find string table
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
        bool usePLT = false;

        // 在 processRelocations 函数中，找到处理 R_AARCH64_JUMP26 和 R_AARCH64_CALL26 的部分
        if (type == 283 || type == 282) {  // R_AARCH64_CALL26 或 R_AARCH64_JUMP26
            // 计算目标地址
            uint64_t targetAddr = 0;
            if (sym->st_shndx != SHN_UNDEF) {
                targetAddr = baseAddr + symValue;
            } else {
                // 查找外部符号
                auto it = Ctx.SymbolTable.find(symName);
                if (it != Ctx.SymbolTable.end()) {
                    targetAddr = it->second;
                } else {
                    // 如果符号未定义，可能需要创建 PLT 条目
                    std::cout << "WARNING: Undefined symbol: " << symName
                              << ", creating PLT entry" << std::endl;
                    targetAddr = getPLTEntryForSymbol(symName);
                    if (targetAddr == 0) {
                        fprintf(stderr, "ERROR: Failed to create PLT entry for symbol: %s\n", symName);
                        return false;
                    }
                }
            }

            // 计算分支偏移
            uint64_t location = baseAddr + rela->r_offset;
            int64_t branch_offset = targetAddr + rela->r_addend - location;

            // 检查是否在范围内
            if (branch_offset >= -(1 << 27) && branch_offset < (1 << 27)) {
                // 在范围内，直接应用重定位
                char* locPtr = memory + rela->r_offset;
                if (!applyRelocation(type, locPtr, targetAddr + rela->r_addend,
                                   rela->r_addend, location, rela->r_offset)) {
                    fprintf(stderr, "Failed to apply relocation type %u for symbol %s\n", type, symName);
                    return false;
                }
            } else {
                // 超出范围，使用 PLT
                std::cout << "DEBUG: Symbol " << symName << " is out of range (0x"
                          << std::hex << branch_offset << "), using PLT" << std::dec << std::endl;

                // 获取或创建 PLT 条目
                uint64_t pltAddr = getPLTEntryForSymbol(symName);
                if (pltAddr == 0) {
                    fprintf(stderr, "ERROR: Failed to get PLT entry for symbol: %s\n", symName);
                    return false;
                }

                // 应用重定位到 PLT 条目
                char* locPtr = memory + rela->r_offset;
                // 删除未使用的变量 'plt_offset'，因为我们直接使用 applyRelocation 计算偏移
                // applyRelocation 内部会计算正确的偏移

                if (!applyRelocation(type, locPtr, pltAddr + rela->r_addend,
                                   rela->r_addend, location, rela->r_offset)) {
                    fprintf(stderr, "Failed to apply PLT relocation type %u for symbol %s\n", type, symName);
                    return false;
                }

                std::cout << "DEBUG: Redirected " << symName << " to PLT entry at 0x"
                          << std::hex << pltAddr << std::dec << std::endl;
            }

            continue;  // 继续处理下一个重定位
        }
        
        if (!usePLT) {
            // Normal symbol resolution
            if (sym->st_shndx != SHN_UNDEF) {
                targetAddr = baseAddr + symValue;
            } else {
                auto it = Ctx.SymbolTable.find(symName);
                if (it != Ctx.SymbolTable.end()) {
                    targetAddr = it->second;
                } else {
                    fprintf(stderr, "Undefined symbol: %s\n", symName);
                    return false;
                }
            }
        }

        // Apply relocation
        uint64_t location = baseAddr + rela->r_offset;
        char* locPtr = memory + rela->r_offset;

        if (!applyRelocation(type, locPtr, targetAddr + rela->r_addend,
                           rela->r_addend, location, rela->r_offset)) {
            fprintf(stderr, "Failed to apply relocation type %u for symbol %s\n", type, symName);
            return false;
        }
    }

    return true;
}

bool MinimalJITLinker::link(const char* objectData, size_t objectSize, uint64_t baseAddress) {
    MinimalELF64Parser parser(objectData, objectSize);
    if (!parser.isValid()) {
        fprintf(stderr, "Invalid ELF file\n");
        return false;
    }

    // 打印节信息
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

    // 3. 设置 PLT 和 GOT
    if (!setupPLTAndGOT()) {
        fprintf(stderr, "Failed to set up PLT and GOT\n");
        return false;
    }

    // 4. 构建符号表
    buildSymbolTable(parser);

    // 5. 复制节并处理重定位
    if (!copySectionsAndRelocate(parser)) {
        fprintf(stderr, "Failed to copy sections and relocate\n");
        return false;
    }

    return true;
}
