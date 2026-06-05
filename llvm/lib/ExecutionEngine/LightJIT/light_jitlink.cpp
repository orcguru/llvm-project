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

//#define DEBUG

using namespace llvm;

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

// 在 MinimalELF64Parser 的方法实现部分添加
Elf64_Half MinimalELF64Parser::getMachineType() const {
    return Header ? Header->e_machine : 0;
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
#ifdef DEBUG
    std::cout << "\n=== ELF Sections Info ===" << std::endl;
#endif
    for (size_t i = 0; ; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;

#ifdef DEBUG
        std::cout << "Section " << i << ":" << std::endl;
        std::cout << "  Type: " << shdr->sh_type
                  << " (1=PROGBITS, 3=SYMTAB, 9=RELA)" << std::endl;
        std::cout << "  Flags: 0x" << std::hex << shdr->sh_flags << std::dec << std::endl;
        std::cout << "  Address: 0x" << std::hex << shdr->sh_addr << std::dec << std::endl;
        std::cout << "  Offset: 0x" << std::hex << shdr->sh_offset << std::dec << std::endl;
        std::cout << "  Size: " << shdr->sh_size << " bytes" << std::endl;
        std::cout << "  Addralign: " << shdr->sh_addralign << std::endl;
        std::cout << std::endl;
#endif
    }
}

size_t MinimalJITLinker::calculateTotalSize(const MinimalELF64Parser& parser) {
    uint64_t minAddr = UINT64_MAX;
    uint64_t maxAddr = 0;

    // 遍历所有节头，直到遇到 nullptr
    for (size_t i = 0; ; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;

        // 考虑需要加载的段（PROGBITS 和 NOBITS）
        if (shdr->sh_type == 1 || shdr->sh_type == 8) {  // SHT_PROGBITS 或 SHT_NOBITS
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
#ifdef DEBUG
    std::cout << "Ctx.CurrentAlloc.BaseAddress:" << std::hex << Ctx.CurrentAlloc.BaseAddress << " Ctx.CurrentAlloc.Size:" << Ctx.CurrentAlloc.Size << std::endl;
#endif
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

                // 包含定义在当前文件中的符号（包括 .bss 节中的符号）
                if (value != 0 && sym->st_shndx != SHN_UNDEF) {
                    Ctx.SymbolTable[name] = value;
                }
            }
            break;
        }
    }
}

bool MinimalJITLinker::copySectionsAndRelocate(const MinimalELF64Parser& parser,
                                              uint64_t startCode,
                                              void (*register_mapping)(uint64_t, uint64_t, uint64_t)) {
    char* memory = Ctx.CurrentAlloc.Memory;
    uint64_t baseAddr = Ctx.CurrentAlloc.BaseAddress;

    // 计算最低的段地址
    uint64_t lowestAddr = UINT64_MAX;
    for (size_t i = 0; ; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;

        if ((shdr->sh_type == 1 || shdr->sh_type == 8) && shdr->sh_size > 0) {  // SHT_PROGBITS 或 SHT_NOBITS
            if (shdr->sh_addr < lowestAddr) {
                lowestAddr = shdr->sh_addr;
            }
        }
    }

    if (lowestAddr == UINT64_MAX) {
        fprintf(stderr, "ERROR: No sections to load\n");
        return false;
    }

    uint64_t *funcmap_ptr = NULL;
    size_t funcmap_size = 0;
    // 首先复制所有 PROGBITS 段
    for (size_t i = 0; ; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;

        if (shdr->sh_type == 1 && shdr->sh_size > 0) { // SHT_PROGBITS
            const char* src = parser.getData() + shdr->sh_offset;

            // 计算内存中的正确偏移
            uint64_t offset = shdr->sh_addr - lowestAddr;

            if (offset + shdr->sh_size > Ctx.CurrentAlloc.Size) {
                fprintf(stderr, "ERROR: Section exceeds allocated memory: offset=%lu, size=%lu, alloc=%lu\n",
                       offset, shdr->sh_size, Ctx.CurrentAlloc.Size);
                return false;
            }

            if (shdr->sh_offset + shdr->sh_size > parser.getSize()) {
                fprintf(stderr, "ERROR: Section out of bounds in ELF file\n");
                return false;
            }

            char* dst = memory + offset;

#ifdef DEBUG
            std::cout << "DEBUG: Copying PROGBITS section at addr=0x" << std::hex << shdr->sh_addr
                     << ", offset=0x" << offset
                     << ", size=" << std::dec << shdr->sh_size
                     << ", dst=0x" << std::hex << (uint64_t)dst
                     << ", memory=0x" << (uint64_t)memory
                     << ", baseAddr=0x" << baseAddr << std::endl;
#endif

            memcpy(dst, src, shdr->sh_size);
            funcmap_ptr = (uint64_t *)dst;
            funcmap_size = shdr->sh_size;

            // 记录可执行段
            if (shdr->sh_flags & 4) { // SHF_EXECINSTR
                Ctx.Modules.push_back({baseAddr + offset, shdr->sh_size});
            }
        }
    }

    if (register_mapping) {
        assert(funcmap_ptr);
        for (size_t i = 0; i < funcmap_size/sizeof(uint64_t); ++i) {
            uint64_t entry = funcmap_ptr[i];
            uint64_t func_hex = (entry & 0xffffffff);
            uint64_t host_addr = (uint64_t)memory + ((entry >> 32) & 0xffffffff);
            register_mapping(startCode, func_hex, host_addr);
        }
    }

    // 初始化所有 NOBITS 段（如 .bss）为 0
    for (size_t i = 0; ; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;

        if (shdr->sh_type == 8 && shdr->sh_size > 0) { // SHT_NOBITS
            // 计算内存中的正确偏移
            uint64_t offset = shdr->sh_addr - lowestAddr;

            if (offset + shdr->sh_size > Ctx.CurrentAlloc.Size) {
                fprintf(stderr, "ERROR: NOBITS section exceeds allocated memory: offset=%lu, size=%lu, alloc=%lu\n",
                       offset, shdr->sh_size, Ctx.CurrentAlloc.Size);
                return false;
            }

            char* dst = memory + offset;

#ifdef DEBUG
            std::cout << "DEBUG: Zeroing NOBITS section at addr=0x" << std::hex << shdr->sh_addr
                     << ", offset=0x" << offset
                     << ", size=" << std::dec << shdr->sh_size
                     << ", dst=0x" << std::hex << (uint64_t)dst << std::endl;
#endif

            // 将 NOBITS 节的内存清零
            memset(dst, 0, shdr->sh_size);
        }
    }

    // 处理重定位节
    for (size_t i = 0; ; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;

        if (shdr->sh_type == SHT_RELA) {  // 处理重定位节
#ifdef DEBUG
            std::cout << "Processing relocation section " << i << std::endl;
#endif
            if (!processRelocations(parser, shdr, memory, baseAddr - lowestAddr)) {
                fprintf(stderr, "ERROR: Failed to process relocations for section %lu\n", i);
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
    size_t pltEntryCount = 2048;
    size_t gotEntryCount = 2048;

    size_t pltSize = pltEntryCount * 16;  // 每个 PLT 条目 16 字节
    size_t gotSize = gotEntryCount * 8;   // 每个 GOT 条目 8 字节

#ifdef DEBUG
    std::cout << "DEBUG: Setting up PLT and GOT" << std::endl;
    std::cout << "  PLT entries: " << pltEntryCount << ", size: " << pltSize << " bytes" << std::endl;
    std::cout << "  GOT entries: " << gotEntryCount << ", size: " << gotSize << " bytes" << std::endl;
#endif

    // 在现有内存之后分配 PLT 和 GOT
    size_t newTotalSize = Ctx.CurrentAlloc.Size + pltSize + gotSize;

    // 尝试在当前内存之后扩展
    void* newMemory = mremap(Ctx.CurrentAlloc.Memory, Ctx.CurrentAlloc.Size,
                            newTotalSize, MREMAP_MAYMOVE);
    if (newMemory == MAP_FAILED) {
        // 如果 mremap 失败，分配新内存并复制
#ifdef DEBUG
        std::cout << "DEBUG: mremap failed, allocating new memory" << std::endl;
#endif
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
        Ctx.CurrentAlloc.BaseAddress = reinterpret_cast<uint64_t>(newMemory);
    }

    // 设置 PLT 和 GOT 地址
    uint64_t codeEnd = Ctx.CurrentAlloc.BaseAddress + (Ctx.CurrentAlloc.Size - pltSize - gotSize);
    Ctx.PLTBaseAddr = codeEnd;
    Ctx.GOTBaseAddr = Ctx.PLTBaseAddr + pltSize;
    Ctx.GOTPtr = Ctx.CurrentAlloc.Memory + (Ctx.GOTBaseAddr - Ctx.CurrentAlloc.BaseAddress);

#ifdef DEBUG
    std::cout << "DEBUG: PLT base: 0x" << std::hex << Ctx.PLTBaseAddr << std::dec << std::endl;
    std::cout << "DEBUG: GOT base: 0x" << std::hex << Ctx.GOTBaseAddr << std::dec << std::endl;
#endif

    // 初始化 PLT 条目向量
    Ctx.PLTEntries.resize(pltEntryCount);

    // 注意：我们不在这里初始化PLT指令，而是在分配时动态生成

    // 初始化 GOT 条目为 0
    uint64_t* gotEntries = reinterpret_cast<uint64_t*>(Ctx.GOTPtr);
    for (size_t i = 0; i < gotEntryCount; i++) {
        gotEntries[i] = 0;
    }

    // 重置索引
    Ctx.NextGOTIndex = 0;
    Ctx.NextPLTIndex = 0;  // 添加这个成员变量
    Ctx.GotSymbolMap.clear();
    Ctx.PLTSymbolMap.clear();

    return true;
}

uint64_t MinimalJITLinker::getGOTEntryForSymbol(const std::string& symbolName, uint64_t targetAddr) {
    // 检查是否已经有这个符号的 GOT 条目
    auto it = Ctx.GotSymbolMap.find(symbolName);
    if (it != Ctx.GotSymbolMap.end()) {
        uint64_t gotEntryAddr = Ctx.GOTBaseAddr + (it->second * 8);
        // 确保 GOT 条目中的地址是最新的
        uint64_t* gotEntries = reinterpret_cast<uint64_t*>(Ctx.GOTPtr);
        gotEntries[it->second] = targetAddr;
#ifdef DEBUG
        std::cout << "DEBUG: Found existing GOT entry for symbol: " << symbolName
                  << " at slot " << it->second
                  << ", updating value to 0x" << std::hex << targetAddr << std::dec << std::endl;
#endif
        return gotEntryAddr;
    }

    // 分配新的 GOT 槽位
    size_t slotIndex = Ctx.NextGOTIndex;

    // 计算 GOT 条目数
    size_t gotSize = 2048 * 8;
    size_t gotEntryCount = gotSize / 8;

    if (slotIndex >= gotEntryCount) {
        std::cerr << "ERROR: GOT table full, cannot allocate entry for symbol: "
                  << symbolName << std::endl;
        return 0;
    }

    // 在映射中记录符号
    Ctx.GotSymbolMap[symbolName] = slotIndex;

    // 写入目标地址到 GOT 条目
    uint64_t* gotEntries = reinterpret_cast<uint64_t*>(Ctx.GOTPtr);
    gotEntries[slotIndex] = targetAddr;

    // 更新下一个可用索引
    Ctx.NextGOTIndex++;

    uint64_t gotAddr = Ctx.GOTBaseAddr + (slotIndex * 8);
#ifdef DEBUG
    std::cout << "DEBUG: Allocated GOT entry for symbol: " << symbolName
              << " at 0x" << std::hex << gotAddr << std::dec
              << " (slot " << slotIndex << "), value=0x"
              << std::hex << targetAddr << std::dec << std::endl;
#endif

    return gotAddr;
}

// 在 MinimalJITLinker 类中添加 RISC-V PLT 生成函数
void MinimalJITLinker::generateRISCVPLTEntry(uint64_t pltAddr, uint64_t gotAddr, size_t slotIndex) {
    // RISC-V PLT 条目的典型布局：
    // 1. auipc t2, %pcrel_hi(got_entry)
    // 2. ld t1, %pcrel_lo(got_entry)(t2)
    // 3. jr t1

    // 计算 GOT 条目相对于 PLT 条目的偏移
    int64_t offset = gotAddr - pltAddr;

    // 1. auipc t2, hi20
    int32_t hi20 = ((offset + 0x800) >> 12) & 0xFFFFF;
    uint32_t auipc_instr = 0x00003e17;  // auipc t3, 0
    auipc_instr |= (hi20 << 12);

    // 2. ld t1, lo12
    int32_t lo12 = offset & 0xFFF;
    if (lo12 >= 0x800) {
        lo12 -= 0x1000;
    }
    uint32_t ld_instr = 0x000e3303;  // ld t1, 0(t3)
    ld_instr |= (lo12 << 20);

    // 3. jr t1
    uint32_t jr_instr = 0x00030067;  // jr t1

    // 将指令写入内存
    char* pltLocation = Ctx.CurrentAlloc.Memory + (pltAddr - Ctx.CurrentAlloc.BaseAddress);

    *reinterpret_cast<uint32_t*>(pltLocation) = auipc_instr;
    *reinterpret_cast<uint32_t*>(pltLocation + 4) = ld_instr;
    *reinterpret_cast<uint32_t*>(pltLocation + 8) = jr_instr;

    // RISC-V PLT 条目通常是 16 字节对齐
    *reinterpret_cast<uint32_t*>(pltLocation + 12) = 0;  // 填充
}

void MinimalJITLinker::generatePLTEntry(uint64_t pltAddr, uint64_t gotAddr, size_t slotIndex) {
    // 计算 GOT 条目相对于 PLT 条目的页偏移
    int64_t pageOffset = ((gotAddr & ~0xFFFULL) - (pltAddr & ~0xFFFULL));
    int64_t pageShifted = pageOffset >> 12;

    // 检查偏移是否在范围内
    if (pageShifted < -((1LL) << 20) || pageShifted >= ((1LL) << 20)) {
        std::cerr << "ERROR: GOT entry too far from PLT entry: "
                  << pageShifted << " pages" << std::endl;
        return;
    }

    // 编码指令
    uint32_t imm = pageShifted & 0x1FFFFF;

    // 1. adrp x16, [page address of GOT entry]
    uint32_t adrpInstr = 0x90000000;
    adrpInstr |= ((imm >> 2) & 0x7FFFF) << 5;
    adrpInstr |= (imm & 0x3) << 29;
    adrpInstr |= 16;  // 目标寄存器 x16

    // 2. ldr x16, [x16, #offset within page]
    uint32_t offsetInPage = gotAddr & 0xFFF;
    if (offsetInPage % 8 != 0) {
        std::cerr << "ERROR: GOT entry address not 8-byte aligned: 0x"
                  << std::hex << gotAddr << std::dec << std::endl;
        return;
    }

    uint32_t ldrInstr = 0xF9400000;
    ldrInstr |= ((offsetInPage >> 3) & 0xFFF) << 10;
    ldrInstr |= 16 << 5;  // Rn = x16
    ldrInstr |= 16;       // Rt = x16

    // 3. br x16
    uint32_t brInstr = 0xD61F0000;
    brInstr |= 16 << 5;  // Rn = x16

    // 将指令写入内存
    char* pltLocation = Ctx.CurrentAlloc.Memory + (pltAddr - Ctx.CurrentAlloc.BaseAddress);

    AArch64PLTEntry entry;
    entry.instr0 = adrpInstr;
    entry.instr1 = ldrInstr;
    entry.instr2 = brInstr;
    entry.padding = 0;

    *reinterpret_cast<AArch64PLTEntry*>(pltLocation) = entry;

    // 更新PLT条目向量
    if (slotIndex < Ctx.PLTEntries.size()) {
        Ctx.PLTEntries[slotIndex] = entry;
    }
}

uint64_t MinimalJITLinker::getPLTEntryForSymbol(const std::string& symbolName) {
    // 检查是否已经有这个符号的 PLT 条目
    auto it = Ctx.PLTSymbolMap.find(symbolName);
    if (it != Ctx.PLTSymbolMap.end()) {
        return Ctx.PLTBaseAddr + (it->second * 16);
    }

    // 分配新的 PLT 槽位
    size_t slotIndex = Ctx.NextPLTIndex;

    // 计算 PLT 条目数
    size_t pltEntryCount = 2048;
    if (slotIndex >= pltEntryCount) {
        std::cerr << "ERROR: PLT table full, cannot allocate entry for symbol: "
                  << symbolName << std::endl;
        return 0;
    }

    // 在映射中记录符号
    Ctx.PLTSymbolMap[symbolName] = slotIndex;
    Ctx.NextPLTIndex++;

    // 获取符号地址
    uint64_t symAddr = 0;
    auto symIt = Ctx.SymbolTable.find(symbolName);
    if (symIt != Ctx.SymbolTable.end()) {
        symAddr = symIt->second;
    }

    // 获取或创建 GOT 条目
    uint64_t gotAddr = getGOTEntryForSymbol(symbolName, symAddr);
    if (gotAddr == 0) {
        std::cerr << "ERROR: Failed to get GOT entry for symbol: "
                  << symbolName << std::endl;
        return 0;
    }

    // 计算PLT条目地址
    uint64_t pltAddr = Ctx.PLTBaseAddr + (slotIndex * 16);

    // 根据架构生成相应的PLT条目
    switch (Ctx.TargetArch) {
    case ArchType::AArch64:
        generatePLTEntry(pltAddr, gotAddr, slotIndex);
        break;
    case ArchType::RISCV64:
        generateRISCVPLTEntry(pltAddr, gotAddr, slotIndex);
        break;
    default:
        std::cerr << "ERROR: Unknown architecture, cannot generate PLT entry" << std::endl;
        return 0;
    }

#ifdef DEBUG
    std::cout << "DEBUG: Allocated PLT entry for symbol: " << symbolName
              << " at 0x" << std::hex << pltAddr << std::dec
              << " (slot " << slotIndex << ")"
              << ", referencing GOT entry at 0x" << std::hex << gotAddr << std::dec
              << std::endl;
#endif

    return pltAddr;
}

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
    int64_t page_shifted;
    int64_t hi20, lo12;
    uint32_t val32;

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
        
        // 计算页偏移（每页 4KB，右移 12 位）
        page_shifted = page_offset >> 12;
        
        // 检查页偏移是否在 21 位有符号范围内 [-2^20, 2^20-1]
        if (page_shifted < -((1LL) << 20) || page_shifted >= ((1LL) << 20)) {
            fprintf(stderr, 
                "ERROR:Relocation R_AARCH64_ADR_PREL_PG_HI21/R_AARCH64_ADR_GOT_PAGE out of range: "
                "byte_offset=0x%lx, page_offset=0x%lx pages, "
                "relocOffset=0x%lx, value=0x%lx, relocAddr=0x%lx\n",
                page_offset, page_shifted, relocOffset, value, relocAddr);
            return false;
        }
        
        // 取 21 位有符号立即数
        imm = page_shifted & 0x1FFFFF;
        
        // 清除指令中的立即数字段
        instr &= ~(0x1FFFFF << 5);  // immhi
        instr &= ~(0x3 << 29);      // immlo
        
        // 设置 immhi 字段（bits [20:2]）
        instr |= ((imm >> 2) & 0x7FFFF) << 5;
        
        // 设置 immlo 字段（bits [1:0]）
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
            fprintf(stderr, "ERROR:R_AARCH64_JUMP26/CALL26 relocation out of range: offset=0x%lx\n", branch_offset);
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

    // 在 applyRelocation 函数中，确保RISC-V重定位类型常量是正确的：
    case 35: // R_RISCV_ADD32
        // 计算 32 位值：S + A
        val32 = static_cast<uint32_t>(value);
        // 写入 32 位值
        *reinterpret_cast<uint32_t*>(location) = val32;
        break;

    case 39: // R_RISCV_SUB32
        // 计算 32 位值：S - A
        val32 = static_cast<uint32_t>(targetAddr - addend);
        *reinterpret_cast<uint32_t*>(location) = val32;
        break;

    case 19: // R_RISCV_CALL_PLT
        // 这个重定位用于函数调用，通过 PLT
        // 计算偏移：S + A - P
        branch_offset = value - relocAddr;
        
        // RISC-V 的 CALL 指令由两个指令组成：auipc 和 jalr
        // auipc 设置高 20 位，jalr 设置低 12 位
        // 检查偏移是否在 32 位有符号范围内
        if (branch_offset < -(1LL << 31) || branch_offset >= (1LL << 31)) {
            fprintf(stderr, "ERROR:R_RISCV_CALL_PLT relocation out of range: offset=0x%lx\n", 
                   branch_offset);
            return false;
        }
        
        // 编码 auipc 指令
        // auipc 指令格式：imm[31:12] | rd | 0010111
        hi20 = (branch_offset + 0x800) >> 12;
        instr = 0x00001797;  // auipc a5, 0
        instr &= ~(0xFFFFF000);  // 清除立即数字段
        instr |= (hi20 & 0xFFFFF) << 12;
        *reinterpret_cast<uint32_t*>(location) = instr;
        
        // 编码 jalr 指令
        // jalr 指令格式：imm[11:0] | rs1 | 000 | rd | 1100111
        lo12 = branch_offset & 0xFFF;
        if (lo12 >= 0x800) {
            lo12 -= 0x1000;  // 符号扩展
        }
        instr = 0x000780e7;  // jalr ra, a5, 0
        instr &= ~(0xFFF00000);  // 清除立即数字段
        instr |= (lo12 & 0xFFF) << 20;
        *reinterpret_cast<uint32_t*>(location + 4) = instr;
        break;

    case 20: // R_RISCV_GOT_HI20
        // 计算 GOT 条目的高 20 位
        // 公式：%got_pcrel_hi(symbol)
        // 计算 GOT 条目相对于 PC 的偏移
        branch_offset = value - relocAddr;
        
        // 检查范围
        if (branch_offset < -(1LL << 31) || branch_offset >= (1LL << 31)) {
            fprintf(stderr, "ERROR:R_RISCV_GOT_HI20 relocation out of range: offset=0x%lx\n", 
                   branch_offset);
            return false;
        }
        
        // 编码 auipc 指令
        hi20 = (branch_offset + 0x800) >> 12;
        instr = *reinterpret_cast<uint32_t*>(location);
        instr &= ~(0xFFFFF000);  // 清除立即数字段
        instr |= (hi20 & 0xFFFFF) << 12;
        *reinterpret_cast<uint32_t*>(location) = instr;
        break;

    case 23: // R_RISCV_PCREL_HI20
        // 计算 PC 相对偏移的高 20 位
        // 公式：%pcrel_hi(symbol)
        branch_offset = value - relocAddr;
        
        if (branch_offset < -(1LL << 31) || branch_offset >= (1LL << 31)) {
            fprintf(stderr, "ERROR:R_RISCV_PCREL_HI20 relocation out of range: offset=0x%lx\n", 
                   branch_offset);
            return false;
        }
        
        hi20 = (branch_offset + 0x800) >> 12;
        instr = *reinterpret_cast<uint32_t*>(location);
        instr &= ~(0xFFFFF000);  // 清除立即数字段
        instr |= (hi20 & 0xFFFFF) << 12;
        *reinterpret_cast<uint32_t*>(location) = instr;
        break;

    case 24: // R_RISCV_PCREL_LO12_I
        // 计算 PC 相对偏移的低 12 位
        // 公式：%pcrel_lo(label)
        // 注意：这个重定位引用之前的一个 R_RISCV_PCREL_HI20 重定位
        // 我们需要找到对应的 HI20 重定位计算的值
        
        // 简化：我们假设 HI20 已经在同一位置计算了相同的偏移
        branch_offset = value - relocAddr;
        lo12 = branch_offset & 0xFFF;
        
        // 对立即数进行符号扩展调整
        if (lo12 >= 0x800) {
            lo12 -= 0x1000;
        }
        
        instr = *reinterpret_cast<uint32_t*>(location);
        // 清除立即数字段
        instr &= ~(0xFFF00000);
        // 设置立即数字段
        instr |= (lo12 & 0xFFF) << 20;
        *reinterpret_cast<uint32_t*>(location) = instr;
        break;

    default:
        fprintf(stderr, "ERROR:Unsupported relocation type: %u (0x%x)\n", type, type);
        return false;
    }
    return true;
}

bool MinimalJITLinker::processRelocations(const MinimalELF64Parser& parser,
                       Elf64_Shdr* relocShdr,
                       char* memory,
                       uint64_t baseAddr) {
    const char* relocData = parser.getData() + relocShdr->sh_offset;
    size_t relocCount = relocShdr->sh_size / sizeof(Elf64_Rela);

    // Find the associated symbol table
    auto symtabShdr = parser.getSectionHeader(relocShdr->sh_link);
    if (!symtabShdr) {
        fprintf(stderr, "ERROR: No symbol table for relocations\n");
        return false;
    }

    const char* symtabData = parser.getData() + symtabShdr->sh_offset;

    // Find string table
    auto strtabShdr = parser.getSectionHeader(symtabShdr->sh_link);
    if (!strtabShdr) {
        fprintf(stderr, "ERROR: No string table for symbols\n");
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
        uint64_t gotAddr = 0;

        // 统一的符号地址计算函数
        auto calculateSymbolAddress = [&]() -> uint64_t {
            if (sym->st_shndx != SHN_UNDEF) {
                auto symSection = parser.getSectionHeader(sym->st_shndx);
                if (symSection) {
                    return baseAddr + symSection->sh_addr + symValue;
                } else {
                    fprintf(stderr, "ERROR: Cannot find section for symbol: %s\n", symName);
                    return 0;
                }
            } else {
                auto it = Ctx.SymbolTable.find(symName);
                if (it != Ctx.SymbolTable.end()) {
                    return it->second;
                } else {
                    fprintf(stderr, "ERROR: Undefined symbol: %s\n", symName);
                    return 0;
                }
            }
        };

        // 在 processRelocations 函数中，找到处理 R_RISCV_CALL_PLT 的部分，将其修改为：
        // 处理 R_RISCV_CALL_PLT
        if (type == 19) {  // R_RISCV_CALL_PLT
            // 计算目标地址
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }

            // 计算分支偏移
            uint64_t location = baseAddr + rela->r_offset;
            int64_t branch_offset = targetAddr + rela->r_addend - location;

            // 检查是否在范围内 (±2GB)
            if (branch_offset >= -(1LL << 31) && branch_offset < (1LL << 31)) {
                // 在范围内，直接应用重定位
                char* locPtr = memory + rela->r_offset;
                if (!applyRelocation(type, locPtr, targetAddr,
                                   rela->r_addend, location, rela->r_offset)) {
                    fprintf(stderr, "ERROR: Failed to apply relocation type %u for symbol %s\n", type, symName);
                    return false;
                }
            } else {
                // 超出范围，使用 PLT
#ifdef DEBUG
                std::cout << "DEBUG: R_RISCV_CALL_PLT for symbol " << symName 
                          << " is out of range (0x" << std::hex << branch_offset 
                          << "), using PLT" << std::dec << std::endl;
#endif

                // 获取或创建 PLT 条目
                uint64_t pltAddr = getPLTEntryForSymbol(symName);
                if (pltAddr == 0) {
                    fprintf(stderr, "ERROR: Failed to get PLT entry for symbol: %s\n", symName);
                    return false;
                }

                // 应用重定位到 PLT 条目
                char* locPtr = memory + rela->r_offset;
                if (!applyRelocation(type, locPtr, pltAddr,
                                   rela->r_addend, location, rela->r_offset)) {
                    fprintf(stderr, "ERROR: Failed to apply PLT relocation for symbol: %s\n", symName);
                    return false;
                }

#ifdef DEBUG
                std::cout << "DEBUG: Redirected " << symName << " to PLT entry at 0x"
                          << std::hex << pltAddr << std::dec << std::endl;
#endif
            }
            continue;
        }
        // 在 processRelocations 函数中，修正RISC-V重定位处理：
        // 处理 R_RISCV_ADD32
        else if (type == 35) {  // R_RISCV_ADD32
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }
        }
        // 处理 R_RISCV_SUB32
        else if (type == 39) {  // R_RISCV_SUB32
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }
        }
        // 处理 R_RISCV_GOT_HI20
        else if (type == 20) {  // R_RISCV_GOT_HI20
            // 计算符号地址
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }

            // 获取或创建 GOT 条目
            gotAddr = getGOTEntryForSymbol(symName, targetAddr);
            if (gotAddr == 0) {
                fprintf(stderr, "ERROR: Failed to get GOT entry for symbol: %s\n", symName);
                return false;
            }

            // 将重定位目标设置为 GOT 条目地址
            targetAddr = gotAddr;
#ifdef DEBUG
            std::cout << "DEBUG: R_RISCV_GOT_HI20 for symbol: " << symName
                      << ", GOT addr=0x" << std::hex << gotAddr << std::dec << std::endl;
#endif
        }
        // 处理 R_RISCV_PCREL_HI20
        else if (type == 23) {  // R_RISCV_PCREL_HI20
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }
        }
        // 处理 R_RISCV_PCREL_LO12_I
        else if (type == 24) {  // R_RISCV_PCREL_LO12_I
            // 这个重定位需要与之前的 R_RISCV_PCREL_HI20 配对
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }
        }
        // 处理 R_AARCH64_JUMP26 和 R_AARCH64_CALL26
        else if (type == 283 || type == 282) {  // R_AARCH64_CALL26 或 R_AARCH64_JUMP26
            // 计算目标地址
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }

            // 计算分支偏移
            uint64_t location = baseAddr + rela->r_offset;
            int64_t branch_offset = targetAddr + rela->r_addend - location;

            // 检查是否在范围内
            if (branch_offset >= -(1 << 27) && branch_offset < (1 << 27)) {
                // 在范围内，直接应用重定位
                char* locPtr = memory + rela->r_offset;
                if (!applyRelocation(type, locPtr, targetAddr,
                                   rela->r_addend, location, rela->r_offset)) {
                    fprintf(stderr, "ERROR: Failed to apply relocation type %u for symbol %s\n", type, symName);
                    return false;
                }
            } else {
                // 超出范围，使用 PLT
#ifdef DEBUG
                std::cout << "DEBUG: Symbol " << symName << " is out of range (0x"
                          << std::hex << branch_offset << "), using PLT" << std::dec << std::endl;
#endif

                // 获取或创建 PLT 条目
                uint64_t pltAddr = getPLTEntryForSymbol(symName);
                if (pltAddr == 0) {
                    fprintf(stderr, "ERROR: Failed to get PLT entry for symbol: %s\n", symName);
                    return false;
                }

                // 应用重定位到 PLT 条目
                char* locPtr = memory + rela->r_offset;
                if (!applyRelocation(type, locPtr, pltAddr,
                                   rela->r_addend, location, rela->r_offset)) {
                    fprintf(stderr, "ERROR: Failed to apply PLT relocation type %u for symbol %s\n", type, symName);
                    return false;
                }

#ifdef DEBUG
                std::cout << "DEBUG: Redirected " << symName << " to PLT entry at 0x"
                          << std::hex << pltAddr << std::dec << std::endl;
#endif
            }

            continue;
        }
        // 处理 R_AARCH64_ADR_GOT_PAGE
        else if (type == 311) {  // R_AARCH64_ADR_GOT_PAGE
            // 计算符号地址
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }

            // 获取或创建 GOT 条目
            gotAddr = getGOTEntryForSymbol(symName, targetAddr);
            if (gotAddr == 0) {
                fprintf(stderr, "ERROR: Failed to get GOT entry for symbol: %s\n", symName);
                return false;
            }

            // 将重定位目标设置为 GOT 条目地址
            targetAddr = gotAddr;
#ifdef DEBUG
            std::cout << "DEBUG: R_AARCH64_ADR_GOT_PAGE for symbol: " << symName
                      << ", GOT addr=0x" << std::hex << gotAddr << std::dec << std::endl;
#endif
        }
        // 处理 R_AARCH64_LD64_GOT_LO12_NC
        else if (type == 312) {  // R_AARCH64_LD64_GOT_LO12_NC
            // 计算符号地址
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }

            // 获取或创建 GOT 条目
            gotAddr = getGOTEntryForSymbol(symName, targetAddr);
            if (gotAddr == 0) {
                fprintf(stderr, "ERROR: Failed to get GOT entry for symbol: %s\n", symName);
                return false;
            }

            // 将重定位目标设置为 GOT 条目地址
            targetAddr = gotAddr;
#ifdef DEBUG
            std::cout << "DEBUG: R_AARCH64_LD64_GOT_LO12_NC for symbol: " << symName
                      << ", GOT addr=0x" << std::hex << gotAddr << std::dec << std::endl;
#endif
        }
        // 处理其他重定位类型
        else {
            // 计算符号地址
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }
        }

        // Apply relocation
        uint64_t location = baseAddr + rela->r_offset;
        char* locPtr = memory + rela->r_offset;

        if (!applyRelocation(type, locPtr, targetAddr,
                           rela->r_addend, location, rela->r_offset)) {
            fprintf(stderr, "ERROR: Failed to apply relocation type %u for symbol %s\n", type, symName);
            return false;
        }
    }

    return true;
}

void MinimalJITLinker::detectArchitecture(const MinimalELF64Parser& parser) {
    // 使用公共方法获取机器类型
    Elf64_Half machine = parser.getMachineType();

    switch (machine) {
    case 183:  // EM_AARCH64
        Ctx.TargetArch = ArchType::AArch64;
        break;
    case 243:  // EM_RISCV
        Ctx.TargetArch = ArchType::RISCV64;
        break;
    default:
        Ctx.TargetArch = ArchType::Unknown;
        break;
    }
}

bool MinimalJITLinker::link(const char* objectData, size_t objectSize, uint64_t baseAddress,
                            uint64_t startCode,
                            void (*register_mapping)(uint64_t, uint64_t, uint64_t)
                            ) {
    MinimalELF64Parser parser(objectData, objectSize);
    if (!parser.isValid()) {
        fprintf(stderr, "ERROR:Invalid ELF file\n");
        return false;
    }

    // 检测目标架构
    detectArchitecture(parser);

    // 打印节信息
    printSectionsInfo(parser);

    // 1. 计算总大小
    size_t totalSize = calculateTotalSize(parser);
    if (totalSize == 0) {
        fprintf(stderr, "ERROR:Failed to calculate total size\n");
        return false;
    }

#ifdef DEBUG
    std::cout << "DEBUG: Required memory size = " << totalSize << " bytes" << std::endl;
    std::cout << "DEBUG: Preferred address = 0x" << std::hex << baseAddress << std::dec << std::endl;
#endif

    // 2. 分配内存
    if (!allocateMemory(totalSize, baseAddress)) {
        fprintf(stderr, "ERROR:Failed to allocate memory\n");
        return false;
    }

    // 3. 设置 PLT 和 GOT
    if (!setupPLTAndGOT()) {
        fprintf(stderr, "ERROR:Failed to set up PLT and GOT\n");
        return false;
    }

    // 4. 构建符号表
    buildSymbolTable(parser);

    // 5. 复制节并处理重定位
    if (!copySectionsAndRelocate(parser, startCode, register_mapping)) {
        fprintf(stderr, "ERROR:Failed to copy sections and relocate\n");
        return false;
    }

    return true;
}
