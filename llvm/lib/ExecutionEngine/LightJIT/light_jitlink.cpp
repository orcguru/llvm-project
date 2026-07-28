// light_jitlink.cpp
#include "llvm/ExecutionEngine/LightJIT/qemu_lightjit.h"
#include "llvm/ExecutionEngine/LightJIT/light_jitlink.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Format.h"
#include "llvm/ADT/StringRef.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <stdint.h>
#include <stddef.h>

//#define DEBUG

using namespace llvm;

MinimalELF64Parser::MinimalELF64Parser(const char* data, size_t size)
    : Data(data), Size(size) {
    if (Size >= sizeof(Elf64_Ehdr)) {
        Header = reinterpret_cast<Elf64_Ehdr*>(const_cast<char*>(Data));
    } else {
        Header = nullptr;
    }
}

MinimalELF64Parser::~MinimalELF64Parser() {
}

Elf64_Half MinimalELF64Parser::getMachineType() const {
    return Header ? Header->e_machine : 0;
}

bool MinimalELF64Parser::isValid() const {
    if (!Header) return false;
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

    for (size_t i = 0; ; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;

        if (shdr->sh_type == 1 || shdr->sh_type == 8) {
            if (shdr->sh_size > 0) {
                uint64_t start = shdr->sh_addr;
                uint64_t end = shdr->sh_addr + shdr->sh_size;

                if (start < minAddr) minAddr = start;
                if (end > maxAddr) maxAddr = end;
            }
        }
    }

    if (minAddr == UINT64_MAX) {
        return 0;
    }

    return maxAddr - minAddr;
}

bool MinimalJITLinker::allocateMemory(size_t size, uint64_t preferredAddr) {
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
    for (size_t i = 0; i < 16; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;

        if (shdr->sh_type == SHT_SYMTAB) {
            const char* symtabData = parser.getData() + shdr->sh_offset;
            size_t symCount = shdr->sh_size / sizeof(Elf64_Sym);

            auto strtabShdr = parser.getSectionHeader(shdr->sh_link);
            if (!strtabShdr) continue;
            const char* strtab = parser.getData() + strtabShdr->sh_offset;

            for (size_t j = 0; j < symCount; j++) {
                const Elf64_Sym* sym = reinterpret_cast<const Elf64_Sym*>(
                    symtabData + j * sizeof(Elf64_Sym));

                if (sym->st_name == 0) continue;

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

typedef struct {
    uint32_t left;
    uint32_t right;
    uint8_t  color;
    uint8_t  _pad[3];
    uint64_t key;
    uint64_t value;
    uint32_t _pad2;
} __attribute__((packed, aligned(8))) RBNode;

typedef struct {
    uint32_t root_index;
    uint32_t node_count;
    RBNode nodes[];
} FuncMapSection;

uint64_t lookup_function(const FuncMapSection* funcmap, uint64_t guest_pc) {
    if (!funcmap || funcmap->root_index == 0xFFFFFFFF) {
        return 0;
    }
    
    uint32_t current = funcmap->root_index;
    
    while (current != 0xFFFFFFFF) {
        const RBNode* node = &funcmap->nodes[current];
        
        if (guest_pc == node->key) {
            return node->value;
        } else if (guest_pc < node->key) {
            current = node->left;
        } else {
            current = node->right;
        }
    }
    
    return 0;
}

bool MinimalJITLinker::copySectionsAndRelocate(const MinimalELF64Parser& parser,
                                              uint64_t startCode,
                                              void (*register_mapping)(uint64_t, uint64_t, uint64_t),
                                              void (*log_message)(const char *),
                                              const char *AotFile,
                                              void *(*g_malloc0)(uint64_t),
                                              uint64_t *aot_code_base_ptr,
                                              uint64_t *funcmap_rbtree_root_ptr) {
    char* memory = Ctx.CurrentAlloc.Memory;
    uint64_t baseAddr = Ctx.CurrentAlloc.BaseAddress;

    uint64_t lowestAddr = UINT64_MAX;
    for (size_t i = 0; ; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;

        if ((shdr->sh_type == 1 || shdr->sh_type == 8) && shdr->sh_size > 0) {
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
    uint64_t x64_exec_end = 0;
    int progbits_cnt = 0;
    uint64_t host_exec_start = 0;
    uint64_t host_exec_size = 0;
    uint64_t *shadow_map_ptr = 0;
    for (size_t i = 0; ; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;
        auto shdr_next = parser.getSectionHeader(i+1);

        if (shdr->sh_type == 1 && shdr->sh_size > 0) {
            progbits_cnt += 1;
            const char* src = parser.getData() + shdr->sh_offset;
            // FIXME: the second SHT_PROGBITS section is .rodata
            if (progbits_cnt == 2) {
                uint64_t *ptr = (uint64_t *)(parser.getData() + shdr->sh_offset);
                x64_exec_end = *ptr;
            }

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
            if (shdr_next && shdr_next->sh_type == 8 && shdr_next->sh_size > 0) {
                // If next section is .bss, then current is .data, and shadow_map_ptr is located at the head
                shadow_map_ptr = (uint64_t *)dst;
            }

            // FIXME: the first section is .text
            if (progbits_cnt == 1) {
                host_exec_start = (uint64_t)dst;
                host_exec_size = shdr->sh_size;
                if (log_message) {
                    llvm::SmallString<256> text_log;
                    llvm::raw_svector_ostream os(text_log);

                    os << AotFile << " .text start:" << llvm::format_hex(host_exec_start, 0) << " length:" << llvm::format_hex(host_exec_size, 0) << "\n";
                    log_message(text_log.c_str());
                }
            }

#ifdef DEBUG
            std::cout << "DEBUG: Copying PROGBITS section at addr=0x" << std::hex << shdr->sh_addr
                     << ", offset=0x" << offset
                     << ", size=" << std::dec << shdr->sh_size
                     << ", dst=0x" << std::hex << (uint64_t)dst
                     << ", memory=0x" << (uint64_t)memory
                     << ", baseAddr=0x" << baseAddr
                     << ", parser.getData()=0x" << (uint64_t)parser.getData()
                     << ", shdr->sh_offset=0x" << shdr->sh_offset << std::endl;
#endif

            memcpy(dst, src, shdr->sh_size);
            funcmap_ptr = (uint64_t *)dst;
            funcmap_size = shdr->sh_size;

            if (shdr->sh_flags & 4) {
                Ctx.Modules.push_back({baseAddr + offset, shdr->sh_size});
            }
        }
    }
    assert(x64_exec_end != 0);

//#define FUNCMAP_ARRAY
#define FUNCMAP_RBTREE
    if (register_mapping) {
        assert(funcmap_ptr);
#if defined(FUNCMAP_ARRAY)
        for (size_t i = 0; i < funcmap_size/sizeof(uint64_t); ++i) {
            uint64_t entry = funcmap_ptr[i];
            uint64_t func_hex = (entry & 0xffffffff);
            uint64_t host_addr = (uint64_t)memory + ((entry >> 32) & 0xffffffff);
            register_mapping(startCode, func_hex, host_addr);
        }
#elif defined(FUNCMAP_RBTREE)
        *aot_code_base_ptr = (uint64_t)memory;
        *funcmap_rbtree_root_ptr = (uint64_t)funcmap_ptr;
#ifdef DEBUG_FIX_FUNCMAP
        for (uint64_t x64di = 0; x64di < x64_exec_end; ++x64di) {
            uint64_t host_delta = lookup_function((const FuncMapSection *)funcmap_ptr, x64di);
            if (host_delta) {
                uint64_t host_addr = (uint64_t)memory + host_delta;
                register_mapping(startCode, x64di, host_addr);
            }
        }
#endif
#endif
    }

    for (size_t i = 0; ; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;

        if (shdr->sh_type == 8 && shdr->sh_size > 0) {
            uint64_t offset = shdr->sh_addr - lowestAddr;

            if (offset + shdr->sh_size > Ctx.CurrentAlloc.Size) {
                fprintf(stderr, "ERROR: NOBITS section exceeds allocated memory: offset=%lu, size=%lu, alloc=%lu\n",
                       offset, shdr->sh_size, Ctx.CurrentAlloc.Size);
                return false;
            }

            char* dst = memory + offset;
            // Set the address of shadow_map into shadow_map_ptr located in previous section
            *shadow_map_ptr = (uint64_t)dst;

#ifdef DEBUG
            std::cout << "DEBUG: Zeroing NOBITS section at addr=0x" << std::hex << shdr->sh_addr
                     << ", offset=0x" << offset
                     << ", size=" << std::dec << shdr->sh_size
                     << ", dst=0x" << std::hex << (uint64_t)dst << std::endl;
#endif

            // Setup shadow_map
            volatile uint64_t *x64_elf_exec_start = (uint64_t *)dst;
            volatile uint64_t *x64_delta_end = (uint64_t *)(dst + 8);
            volatile uint64_t *host_exec_start_ptr = (uint64_t *)(dst + 16);
            volatile uint64_t *aux_array_ptr = (uint64_t *)(dst + 24);
            *x64_elf_exec_start = startCode;
            *x64_delta_end = x64_exec_end;
            *host_exec_start_ptr = host_exec_start;
            *aux_array_ptr = 0;

            if (log_message) {
                llvm::SmallString<256> bss_log;
                llvm::raw_svector_ostream os(bss_log);

                os << AotFile << " .bss start:" << llvm::format_hex((uint64_t)dst, 0) << " length:" << llvm::format_hex(shdr->sh_size, 0) << "\n";
                log_message(bss_log.c_str());
            }
        }
    }

    for (size_t i = 0; ; i++) {
        auto shdr = parser.getSectionHeader(i);
        if (!shdr) break;

        if (shdr->sh_type == SHT_RELA) {
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

        if (shdr->sh_type == 1 && shdr->sh_size > 0) {
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
    if (Ctx.PLTBaseAddr != 0 || Ctx.GOTBaseAddr != 0) {
        return true;
    }

    size_t pltEntryCount = 2048;
    size_t gotEntryCount = 2048;

    size_t pltSize = pltEntryCount * 16;
    size_t gotSize = gotEntryCount * 8;

#ifdef DEBUG
    std::cout << "DEBUG: Setting up PLT and GOT" << std::endl;
    std::cout << "  PLT entries: " << pltEntryCount << ", size: " << pltSize << " bytes" << std::endl;
    std::cout << "  GOT entries: " << gotEntryCount << ", size: " << gotSize << " bytes" << std::endl;
#endif

    size_t newTotalSize = Ctx.CurrentAlloc.Size + pltSize + gotSize;

    void* newMemory = mremap(Ctx.CurrentAlloc.Memory, Ctx.CurrentAlloc.Size,
                            newTotalSize, MREMAP_MAYMOVE);
    if (newMemory == MAP_FAILED) {
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

        Ctx.CurrentAlloc.Memory = static_cast<char*>(newMemory);
        Ctx.CurrentAlloc.Size = newTotalSize;
        Ctx.CurrentAlloc.BaseAddress = reinterpret_cast<uint64_t>(newMemory);
    } else {
        Ctx.CurrentAlloc.Memory = static_cast<char*>(newMemory);
        Ctx.CurrentAlloc.Size = newTotalSize;
        Ctx.CurrentAlloc.BaseAddress = reinterpret_cast<uint64_t>(newMemory);
    }

#ifdef DEBUG
    std::cout << "Ctx.CurrentAlloc.BaseAddress:" << std::hex << Ctx.CurrentAlloc.BaseAddress << " Ctx.CurrentAlloc.Size:" << Ctx.CurrentAlloc.Size << std::endl;
#endif
    uint64_t codeEnd = Ctx.CurrentAlloc.BaseAddress + (Ctx.CurrentAlloc.Size - pltSize - gotSize);
    Ctx.PLTBaseAddr = codeEnd;
    Ctx.GOTBaseAddr = Ctx.PLTBaseAddr + pltSize;
    Ctx.GOTPtr = Ctx.CurrentAlloc.Memory + (Ctx.GOTBaseAddr - Ctx.CurrentAlloc.BaseAddress);

#ifdef DEBUG
    std::cout << "DEBUG: PLT base: 0x" << std::hex << Ctx.PLTBaseAddr << std::dec << std::endl;
    std::cout << "DEBUG: GOT base: 0x" << std::hex << Ctx.GOTBaseAddr << std::dec << std::endl;
#endif

    Ctx.PLTEntries.resize(pltEntryCount);

    uint64_t* gotEntries = reinterpret_cast<uint64_t*>(Ctx.GOTPtr);
    for (size_t i = 0; i < gotEntryCount; i++) {
        gotEntries[i] = 0;
    }

    Ctx.NextGOTIndex = 0;
    Ctx.NextPLTIndex = 0;
    Ctx.GotSymbolMap.clear();
    Ctx.PLTSymbolMap.clear();

    return true;
}

uint64_t MinimalJITLinker::getGOTEntryForSymbol(const std::string& symbolName, uint64_t targetAddr) {
    auto it = Ctx.GotSymbolMap.find(symbolName);
    if (it != Ctx.GotSymbolMap.end()) {
        uint64_t gotEntryAddr = Ctx.GOTBaseAddr + (it->second * 8);
        uint64_t* gotEntries = reinterpret_cast<uint64_t*>(Ctx.GOTPtr);
        gotEntries[it->second] = targetAddr;
#ifdef DEBUG
        std::cout << "DEBUG: Found existing GOT entry for symbol: " << symbolName
                  << " at slot " << it->second
                  << ", updating value to 0x" << std::hex << targetAddr << std::dec << std::endl;
#endif
        return gotEntryAddr;
    }

    size_t slotIndex = Ctx.NextGOTIndex;

    size_t gotSize = 2048 * 8;
    size_t gotEntryCount = gotSize / 8;

    if (slotIndex >= gotEntryCount) {
        std::cerr << "ERROR: GOT table full, cannot allocate entry for symbol: "
                  << symbolName << std::endl;
        return 0;
    }

    Ctx.GotSymbolMap[symbolName] = slotIndex;

    uint64_t* gotEntries = reinterpret_cast<uint64_t*>(Ctx.GOTPtr);
    gotEntries[slotIndex] = targetAddr;

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

void MinimalJITLinker::generateRISCVPLTEntry(uint64_t pltAddr, uint64_t gotAddr, size_t slotIndex) {
    int64_t offset = gotAddr - pltAddr;

    int32_t hi20 = ((offset + 0x800) >> 12) & 0xFFFFF;

    uint32_t auipc_instr = 0x00000397;
    auipc_instr &= ~(0xFFFFF000);
    auipc_instr |= (hi20 & 0xFFFFF) << 12;

    int32_t lo12 = offset & 0xFFF;
    if (lo12 >= 0x800) {
        lo12 -= 0x1000;
    }

    uint32_t ld_instr = 0x0003b303;
    ld_instr &= ~(0xFFF00000);
    ld_instr &= ~(0x1F << 15);
    ld_instr &= ~(0x1F << 7);
    ld_instr |= (lo12 & 0xFFF) << 20;
    ld_instr |= (7 << 15);
    ld_instr |= (7 << 7);

    uint32_t jr_instr = 0x00038067;

    char* pltLocation = Ctx.CurrentAlloc.Memory + (pltAddr - Ctx.CurrentAlloc.BaseAddress);

    *reinterpret_cast<uint32_t*>(pltLocation) = auipc_instr;
    *reinterpret_cast<uint32_t*>(pltLocation + 4) = ld_instr;
    *reinterpret_cast<uint32_t*>(pltLocation + 8) = jr_instr;

    *reinterpret_cast<uint32_t*>(pltLocation + 12) = 0;

#ifdef DEBUG
    std::cout << "DEBUG: Generated RISC-V PLT entry at 0x" << std::hex << pltAddr
              << " (slot " << std::dec << slotIndex << "):" << std::endl;
    std::cout << "  auipc t2, 0x" << std::hex << hi20 << std::endl;
    std::cout << "  ld    t2, 0x" << std::hex << (lo12 & 0xFFF) << "(t2)" << std::endl;
    std::cout << "  jr    t2" << std::endl;
    std::cout << "  (GOT entry at 0x" << std::hex << gotAddr << ")" << std::dec << std::endl;
#endif
}

void MinimalJITLinker::generatePLTEntry(uint64_t pltAddr, uint64_t gotAddr, size_t slotIndex) {
    int64_t pageOffset = ((gotAddr & ~0xFFFULL) - (pltAddr & ~0xFFFULL));
    int64_t pageShifted = pageOffset >> 12;

    if (pageShifted < -((1LL) << 20) || pageShifted >= ((1LL) << 20)) {
        std::cerr << "ERROR: GOT entry too far from PLT entry: "
                  << pageShifted << " pages" << std::endl;
        return;
    }

    uint32_t imm = pageShifted & 0x1FFFFF;

    uint32_t adrpInstr = 0x90000000;
    adrpInstr |= ((imm >> 2) & 0x7FFFF) << 5;
    adrpInstr |= (imm & 0x3) << 29;
    adrpInstr |= 16;

    uint32_t offsetInPage = gotAddr & 0xFFF;
    if (offsetInPage % 8 != 0) {
        std::cerr << "ERROR: GOT entry address not 8-byte aligned: 0x"
                  << std::hex << gotAddr << std::dec << std::endl;
        return;
    }

    uint32_t ldrInstr = 0xF9400000;
    ldrInstr |= ((offsetInPage >> 3) & 0xFFF) << 10;
    ldrInstr |= 16 << 5;
    ldrInstr |= 16;

    uint32_t brInstr = 0xD61F0000;
    brInstr |= 16 << 5;

    char* pltLocation = Ctx.CurrentAlloc.Memory + (pltAddr - Ctx.CurrentAlloc.BaseAddress);

    AArch64PLTEntry entry;
    entry.instr0 = adrpInstr;
    entry.instr1 = ldrInstr;
    entry.instr2 = brInstr;
    entry.padding = 0;

    *reinterpret_cast<AArch64PLTEntry*>(pltLocation) = entry;

    if (slotIndex < Ctx.PLTEntries.size()) {
        Ctx.PLTEntries[slotIndex] = entry;
    }
}

uint64_t MinimalJITLinker::getPLTEntryForSymbol(const std::string& symbolName) {
    auto it = Ctx.PLTSymbolMap.find(symbolName);
    if (it != Ctx.PLTSymbolMap.end()) {
        return Ctx.PLTBaseAddr + (it->second * 16);
    }

    size_t slotIndex = Ctx.NextPLTIndex;

    size_t pltEntryCount = 2048;
    if (slotIndex >= pltEntryCount) {
        std::cerr << "ERROR: PLT table full, cannot allocate entry for symbol: "
                  << symbolName << std::endl;
        return 0;
    }

    Ctx.PLTSymbolMap[symbolName] = slotIndex;
    Ctx.NextPLTIndex++;

    uint64_t symAddr = 0;
    auto symIt = Ctx.SymbolTable.find(symbolName);
    if (symIt != Ctx.SymbolTable.end()) {
        symAddr = symIt->second;
    }

    uint64_t gotAddr = getGOTEntryForSymbol(symbolName, symAddr);
    if (gotAddr == 0) {
        std::cerr << "ERROR: Failed to get GOT entry for symbol: "
                  << symbolName << std::endl;
        return 0;
    }

    uint64_t pltAddr = Ctx.PLTBaseAddr + (slotIndex * 16);

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
    uint32_t original_jump_instr;
    uint32_t opcode;
    uint32_t funct3;
    uint32_t rd;
    uint32_t rs1;

    switch (type) {
    case 277:
        instr = *reinterpret_cast<uint32_t*>(location);
        result = (instr & ~(0xFFF << 10)) | ((value & 0xFFF) << 10);
        *reinterpret_cast<uint32_t*>(location) = result;
        break;

    case 311:
    case 275:
        instr = *reinterpret_cast<uint32_t*>(location);
        page_offset = ((value & ~0xFFFULL) - (relocAddr & ~0xFFFULL));
        
        page_shifted = page_offset >> 12;
        
        if (page_shifted < -((1LL) << 20) || page_shifted >= ((1LL) << 20)) {
            fprintf(stderr, 
                "ERROR:Relocation R_AARCH64_ADR_PREL_PG_HI21/R_AARCH64_ADR_GOT_PAGE out of range: "
                "byte_offset=0x%lx, page_offset=0x%lx pages, "
                "relocOffset=0x%lx, value=0x%lx, relocAddr=0x%lx\n",
                page_offset, page_shifted, relocOffset, value, relocAddr);
            return false;
        }
        
        imm = page_shifted & 0x1FFFFF;
        
        instr &= ~(0x1FFFFF << 5);
        instr &= ~(0x3 << 29);
        
        instr |= ((imm >> 2) & 0x7FFFF) << 5;
        
        instr |= (imm & 0x3) << 29;
        
        *reinterpret_cast<uint32_t*>(location) = instr;
        break;

    case 283:
    case 282:
        branch_offset = value - relocAddr;
        
        if (branch_offset >= -(1 << 27) && branch_offset < (1 << 27)) {
            instr = *reinterpret_cast<uint32_t*>(location);
            imm26 = (branch_offset >> 2) & 0x3FFFFFF;
            instr = (instr & 0xFC000000) | imm26;
            *reinterpret_cast<uint32_t*>(location) = instr;
        } else {
            fprintf(stderr, "ERROR:R_AARCH64_JUMP26/CALL26 relocation out of range: offset=0x%lx\n", branch_offset);
            fprintf(stderr, "  Target=0x%lx, Relocation=0x%lx, Distance=0x%lx\n", 
                   value, relocAddr, static_cast<uint64_t>(llabs(branch_offset)));
            fprintf(stderr, "  PLT support not fully implemented in this version\n");
            return false;
        }
        break;

    case 312:
        instr = *reinterpret_cast<uint32_t*>(location);
        result = (instr & ~(0xFFF << 10)) | (((value & 0xFF8) >> 3) << 10);
        *reinterpret_cast<uint32_t*>(location) = result;
        break;

    case 35:
        {
            uint32_t original_val = *reinterpret_cast<uint32_t*>(location);
            int64_t new_val = static_cast<int64_t>(original_val) + 
                             static_cast<int64_t>(targetAddr) + addend;
            val32 = static_cast<uint32_t>(new_val);
            *reinterpret_cast<uint32_t*>(location) = val32;
            
#ifdef DEBUG
            std::cout << "DEBUG: R_RISCV_ADD32 applyRelocation:" << std::endl
                      << "  location=0x" << std::hex << reinterpret_cast<uint64_t>(location) << std::endl
                      << "  targetAddr=0x" << targetAddr << std::endl
                      << "  addend=0x" << addend << std::endl
                      << "  original_val=0x" << original_val << std::endl
                      << "  new_val=0x" << new_val << std::endl
                      << "  val32=0x" << val32 << std::endl
                      << "  relocAddr=0x" << relocAddr << std::endl
                      << "  relocOffset=0x" << relocOffset << std::dec << std::endl;
#endif
        }
        break;

    case 39:
        {
            uint32_t original_val = *reinterpret_cast<uint32_t*>(location);
            int64_t new_val = static_cast<int64_t>(original_val) - 
                             static_cast<int64_t>(targetAddr) - addend;
            val32 = static_cast<uint32_t>(new_val);
            *reinterpret_cast<uint32_t*>(location) = val32;
            
#ifdef DEBUG
            std::cout << "DEBUG: R_RISCV_SUB32 applyRelocation:" << std::endl
                      << "  location=0x" << std::hex << reinterpret_cast<uint64_t>(location) << std::endl
                      << "  targetAddr=0x" << targetAddr << std::endl
                      << "  addend=0x" << addend << std::endl
                      << "  original_val=0x" << original_val << std::endl
                      << "  new_val=0x" << new_val << std::endl
                      << "  val32=0x" << val32 << std::endl
                      << "  relocAddr=0x" << relocAddr << std::endl
                      << "  relocOffset=0x" << relocOffset << std::dec << std::endl;
#endif
        }
        break;

    case 19:
        branch_offset = value - relocAddr;
        
        if (branch_offset < -(1LL << 31) || branch_offset >= (1LL << 31)) {
            fprintf(stderr, "ERROR:R_RISCV_CALL_PLT relocation out of range: offset=0x%lx\n", 
                   branch_offset);
            return false;
        }
        
        hi20 = (branch_offset + 0x800) >> 12;
        
        instr = 0x00000397;
        instr &= ~(0xFFFFF000);
        instr |= (hi20 & 0xFFFFF) << 12;
        *reinterpret_cast<uint32_t*>(location) = instr;
        
        original_jump_instr = *reinterpret_cast<uint32_t*>(location + 4);
        
        opcode = original_jump_instr & 0x7F;
        funct3 = (original_jump_instr >> 12) & 0x7;
        rd = (original_jump_instr >> 7) & 0x1F;
        rs1 = (original_jump_instr >> 15) & 0x1F;
        
        if (opcode == 0x67) {
            if (funct3 == 0x0) {
                if (rd == 0) {
                    lo12 = branch_offset & 0xFFF;
                    if (lo12 >= 0x800) {
                        lo12 -= 0x1000;
                    }
                    
                    instr = 0x00038067;
                    instr &= ~(0xFFF00000);
                    instr |= (lo12 & 0xFFF) << 20;
                    *reinterpret_cast<uint32_t*>(location + 4) = instr;
                    
#ifdef DEBUG
                    std::cout << "DEBUG: R_RISCV_CALL_PLT: using jr instruction" << std::endl;
#endif
                } else {
                    lo12 = branch_offset & 0xFFF;
                    if (lo12 >= 0x800) {
                        lo12 -= 0x1000;
                    }
                    
                    instr = 0x00000067;
                    instr |= (lo12 & 0xFFF) << 20;
                    instr |= (7 << 15);
                    instr |= (rd << 7);
                    *reinterpret_cast<uint32_t*>(location + 4) = instr;
                    
#ifdef DEBUG
                    std::cout << "DEBUG: R_RISCV_CALL_PLT: using jalr instruction with rd=" << rd << std::endl;
#endif
                }
            } else {
                fprintf(stderr, "ERROR: R_RISCV_CALL_PLT: invalid funct3 in original jalr instruction: 0x%x\n", funct3);
                return false;
            }
        } else {
            fprintf(stderr, "ERROR: R_RISCV_CALL_PLT: original instruction is not jr or jalr (opcode=0x%x)\n", opcode);
            return false;
        }
        break;

    case 20:
        branch_offset = value - relocAddr;
        
        if (branch_offset < -(1LL << 31) || branch_offset >= (1LL << 31)) {
            fprintf(stderr, "ERROR:R_RISCV_GOT_HI20 relocation out of range: offset=0x%lx\n", 
                   branch_offset);
            return false;
        }
        
        hi20 = (branch_offset + 0x800) >> 12;
        instr = *reinterpret_cast<uint32_t*>(location);
        instr &= ~(0xFFFFF000);
        instr |= (hi20 & 0xFFFFF) << 12;
        *reinterpret_cast<uint32_t*>(location) = instr;
        break;

    case 23:
        branch_offset = value - relocAddr;
        
        if (branch_offset < -(1LL << 31) || branch_offset >= (1LL << 31)) {
            fprintf(stderr, "ERROR:R_RISCV_PCREL_HI20 relocation out of range: offset=0x%lx\n", 
                   branch_offset);
            return false;
        }
        
        hi20 = (branch_offset + 0x800) >> 12;
        instr = *reinterpret_cast<uint32_t*>(location);
        instr &= ~(0xFFFFF000);
        instr |= (hi20 & 0xFFFFF) << 12;
        *reinterpret_cast<uint32_t*>(location) = instr;
        break;

    case 24:
        branch_offset = value - relocAddr;
        lo12 = branch_offset & 0xFFF;
        if (lo12 >= 0x800) {
            lo12 -= 0x1000;
        }
        
        instr = *reinterpret_cast<uint32_t*>(location);
        instr &= ~(0xFFF00000);
        instr |= (lo12 & 0xFFF) << 20;
        *reinterpret_cast<uint32_t*>(location) = instr;
        
#ifdef DEBUG
        std::cout << "DEBUG: R_RISCV_PCREL_LO12_I applyRelocation: " << std::hex
                  << " branch_offset=0x" << branch_offset
                  << ", value=0x" << value
                  << ", relocAddr=0x" << relocAddr
                  << ", lo12=0x" << lo12
                  << ", targetAddr=0x" << targetAddr
                  << ", addend=0x" << addend << std::endl;
#endif
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
#ifdef DEBUG
    std::cout << "DEBUG: processRelocations baseAddr:0x" << std::hex << baseAddr << std::endl;
#endif
    const char* relocData = parser.getData() + relocShdr->sh_offset;
    size_t relocCount = relocShdr->sh_size / sizeof(Elf64_Rela);

    auto symtabShdr = parser.getSectionHeader(relocShdr->sh_link);
    if (!symtabShdr) {
        fprintf(stderr, "ERROR: No symbol table for relocations\n");
        return false;
    }

    const char* symtabData = parser.getData() + symtabShdr->sh_offset;

    auto strtabShdr = parser.getSectionHeader(symtabShdr->sh_link);
    if (!strtabShdr) {
        fprintf(stderr, "ERROR: No string table for symbols\n");
        return false;
    }
    const char* strtab = parser.getData() + strtabShdr->sh_offset;

    auto targetShdr = parser.getSectionHeader(relocShdr->sh_info);
    if (!targetShdr) {
        fprintf(stderr, "ERROR: Cannot find target section for relocation section\n");
        return false;
    }

#ifdef DEBUG
    std::cout << "DEBUG: Relocation section targets section at address: 0x"
              << std::hex << targetShdr->sh_addr << std::dec << std::endl;
#endif

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
                    if (sym->st_shndx != SHN_UNDEF) {
                        auto labelSection = parser.getSectionHeader(sym->st_shndx);
                        if (labelSection) {
                            uint64_t labelAddr = baseAddr + labelSection->sh_addr + symValue;
#ifdef DEBUG
                            std::cout << "DEBUG: Local label " << symName
                                      << " at section " << sym->st_shndx
                                      << ", offset=0x" << std::hex << symValue
                                      << ", addr=0x" << labelAddr << std::dec << std::endl;
#endif
                            return labelAddr;
                        }
                    }
                    fprintf(stderr, "ERROR: Undefined symbol: %s\n", symName);
                    return 0;
                }
            }
        };

        uint64_t location = baseAddr + targetShdr->sh_addr + rela->r_offset;
        char* locPtr = memory + targetShdr->sh_addr + rela->r_offset;

        if (type == 19) {
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }

            int64_t branch_offset = targetAddr + rela->r_addend - location;

            if (branch_offset >= -(1LL << 31) && branch_offset < (1LL << 31)) {
                if (!applyRelocation(type, locPtr, targetAddr,
                                   rela->r_addend, location, rela->r_offset)) {
                    fprintf(stderr, "ERROR: Failed to apply relocation type %u for symbol %s\n", type, symName);
                    return false;
                }
            } else {
#ifdef DEBUG
                std::cout << "DEBUG: R_RISCV_CALL_PLT for symbol " << symName
                          << " is out of range (0x" << std::hex << branch_offset
                          << "), using PLT" << std::dec << std::endl;
#endif

                uint64_t pltAddr = getPLTEntryForSymbol(symName);
                if (pltAddr == 0) {
                    fprintf(stderr, "ERROR: Failed to get PLT entry for symbol: %s\n", symName);
                    return false;
                }

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
        else if (type == 35) {
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }
            
#ifdef DEBUG
            std::cout << "DEBUG: R_RISCV_ADD32 for symbol: " << symName << std::endl
                      << "  sym->st_shndx: " << sym->st_shndx << std::endl
                      << "  sym->st_value: 0x" << std::hex << sym->st_value << std::endl
                      << "  targetAddr calculated: 0x" << targetAddr << std::endl
                      << "  location: 0x" << location << std::endl
                      << "  locPtr: 0x" << reinterpret_cast<uint64_t>(locPtr) << std::endl
                      << "  rela->r_offset: 0x" << rela->r_offset << std::endl
                      << "  rela->r_addend: 0x" << rela->r_addend << std::endl
                      << "  value to write (targetAddr + addend): 0x" << (targetAddr + rela->r_addend) << std::dec << std::endl;
#endif

            if (!applyRelocation(type, locPtr, targetAddr,
                               rela->r_addend, location, rela->r_offset)) {
                fprintf(stderr, "ERROR: Failed to apply R_RISCV_ADD32 for symbol: %s\n", symName);
                return false;
            }
            continue;
        }
        else if (type == 39) {
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }
            
#ifdef DEBUG
            std::cout << "DEBUG: R_RISCV_SUB32 for symbol: " << symName << std::endl
                      << "  sym->st_shndx: " << sym->st_shndx << std::endl
                      << "  sym->st_value: 0x" << std::hex << sym->st_value << std::endl
                      << "  targetAddr calculated: 0x" << targetAddr << std::endl
                      << "  location: 0x" << location << std::endl
                      << "  locPtr: 0x" << reinterpret_cast<uint64_t>(locPtr) << std::endl
                      << "  rela->r_offset: 0x" << rela->r_offset << std::endl
                      << "  rela->r_addend: 0x" << rela->r_addend << std::endl
                      << "  value to write (targetAddr - addend): 0x" << (targetAddr - rela->r_addend) << std::dec << std::endl;
#endif

            if (!applyRelocation(type, locPtr, targetAddr,
                               rela->r_addend, location, rela->r_offset)) {
                fprintf(stderr, "ERROR: Failed to apply R_RISCV_SUB32 for symbol: %s\n", symName);
                return false;
            }
            continue;
        }
        else if (type == 20) {
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }

            gotAddr = getGOTEntryForSymbol(symName, targetAddr);
            if (gotAddr == 0) {
                fprintf(stderr, "ERROR: Failed to get GOT entry for symbol: %s\n", symName);
                return false;
            }

            targetAddr = gotAddr;

            Hi20RelocationInfo hi20Info;
            hi20Info.hi20TargetAddr = gotAddr;
            hi20Info.hi20RelocAddr = location;
            hi20Info.hi20Addend = rela->r_addend;
            Ctx.Hi20RelocationMap[location] = hi20Info;

            if (!applyRelocation(type, locPtr, targetAddr,
                               rela->r_addend, location, rela->r_offset)) {
                fprintf(stderr, "ERROR: Failed to apply R_RISCV_GOT_HI20 for symbol: %s\n", symName);
                return false;
            }

#ifdef DEBUG
            std::cout << "DEBUG: R_RISCV_GOT_HI20 at location=0x" << std::hex << location
                      << " for symbol: " << symName
                      << ", GOT addr=0x" << gotAddr
                      << ", addend=0x" << rela->r_addend << std::dec << std::endl;
#endif
            continue;
        }
        else if (type == 23) {
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }

            Hi20RelocationInfo hi20Info;
            hi20Info.hi20TargetAddr = targetAddr;
            hi20Info.hi20RelocAddr = location;
            hi20Info.hi20Addend = rela->r_addend;

            Ctx.Hi20RelocationMap[location] = hi20Info;

            if (!applyRelocation(type, locPtr, targetAddr,
                               rela->r_addend, location, rela->r_offset)) {
                fprintf(stderr, "ERROR: Failed to apply R_RISCV_PCREL_HI20 for symbol: %s\n", symName);
                return false;
            }

#ifdef DEBUG
            std::cout << "DEBUG: R_RISCV_PCREL_HI20 at location=0x" << std::hex << location
                      << " for symbol: " << symName
                      << ", targetAddr=0x" << targetAddr
                      << ", addend=0x" << rela->r_addend << std::dec << std::endl;
#endif
            continue;
        }
        else if (type == 24) {
            uint64_t labelAddr = calculateSymbolAddress();
            if (labelAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for label: %s\n", symName);
                return false;
            }

#ifdef DEBUG
            std::cout << "DEBUG: R_RISCV_PCREL_LO12_I for label: " << symName
                      << ", labelAddr/HI20_addr=0x" << std::hex << labelAddr
                      << ", LO12_location=0x" << location
                      << ", delta=" << (int64_t)(location - labelAddr) << std::dec << std::endl;
#endif

            auto hi20It = Ctx.Hi20RelocationMap.find(labelAddr);
            if (hi20It == Ctx.Hi20RelocationMap.end()) {
                for (const auto& entry : Ctx.Hi20RelocationMap) {
                    uint64_t hi20Addr = entry.first;
                    if (hi20Addr == labelAddr ||
                        (labelAddr >= hi20Addr && labelAddr - hi20Addr <= 16) ||
                        (hi20Addr >= labelAddr && hi20Addr - labelAddr <= 16)) {
                        hi20It = Ctx.Hi20RelocationMap.find(hi20Addr);
                        goto found;
                    }
                }
                fprintf(stderr,
                    "ERROR: R_RISCV_PCREL_LO12_I without matching HI20 for label: %s "
                    "(labelAddr=0x%lx, map_size=%zu)\n",
                    symName, labelAddr, Ctx.Hi20RelocationMap.size());
                return false;
            found:;
            }

            const Hi20RelocationInfo& hi20Info = hi20It->second;

            int64_t full_offset = (int64_t)(hi20Info.hi20TargetAddr + hi20Info.hi20Addend)
                                 - (int64_t)hi20Info.hi20RelocAddr;

            int64_t lo12 = full_offset & 0xFFF;
            if (lo12 & 0x800) lo12 |= ~0xFFF;

#ifdef DEBUG
            std::cout << "DEBUG: R_RISCV_PCREL_LO12_I patching: "
                      << "full_offset=0x" << std::hex << full_offset
                      << ", lo12_imm=0x" << (uint64_t)(lo12 & 0xFFF)
                      << " (signed " << std::dec << lo12 << ")"
                      << std::endl;
#endif

            uint32_t instr = *reinterpret_cast<uint32_t*>(locPtr);
            instr &= ~(0xFFF << 20);
            instr |= ((lo12 & 0xFFF) << 20);
            *reinterpret_cast<uint32_t*>(locPtr) = instr;

            continue;
        }
        else if (type == 283 || type == 282) {
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }

            int64_t branch_offset = targetAddr + rela->r_addend - location;

            if (branch_offset >= -(1 << 27) && branch_offset < (1 << 27)) {
                if (!applyRelocation(type, locPtr, targetAddr,
                                   rela->r_addend, location, rela->r_offset)) {
                    fprintf(stderr, "ERROR: Failed to apply relocation type %u for symbol %s\n", type, symName);
                    return false;
                }
            } else {
#ifdef DEBUG
                std::cout << "DEBUG: Symbol " << symName << " is out of range (0x"
                          << std::hex << branch_offset << "), using PLT" << std::dec << std::endl;
#endif

                uint64_t pltAddr = getPLTEntryForSymbol(symName);
                if (pltAddr == 0) {
                    fprintf(stderr, "ERROR: Failed to get PLT entry for symbol: %s\n", symName);
                    return false;
                }

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
        else if (type == 311) {
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }

            gotAddr = getGOTEntryForSymbol(symName, targetAddr);
            if (gotAddr == 0) {
                fprintf(stderr, "ERROR: Failed to get GOT entry for symbol: %s\n", symName);
                return false;
            }

            targetAddr = gotAddr;

            if (!applyRelocation(type, locPtr, targetAddr,
                               rela->r_addend, location, rela->r_offset)) {
                fprintf(stderr, "ERROR: Failed to apply R_AARCH64_ADR_GOT_PAGE for symbol: %s\n", symName);
                return false;
            }

#ifdef DEBUG
            std::cout << "DEBUG: R_AARCH64_ADR_GOT_PAGE for symbol: " << symName
                      << ", GOT addr=0x" << std::hex << gotAddr << std::dec << std::endl;
#endif
            continue;
        }
        else if (type == 312) {
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }

            gotAddr = getGOTEntryForSymbol(symName, targetAddr);
            if (gotAddr == 0) {
                fprintf(stderr, "ERROR: Failed to get GOT entry for symbol: %s\n", symName);
                return false;
            }

            targetAddr = gotAddr;

            if (!applyRelocation(type, locPtr, targetAddr,
                               rela->r_addend, location, rela->r_offset)) {
                fprintf(stderr, "ERROR: Failed to apply R_AARCH64_LD64_GOT_LO12_NC for symbol: %s\n", symName);
                return false;
            }

#ifdef DEBUG
            std::cout << "DEBUG: R_AARCH64_LD64_GOT_LO12_NC for symbol: " << symName
                      << ", GOT addr=0x" << std::hex << gotAddr << std::dec << std::endl;
#endif
            continue;
        }
        else {
            targetAddr = calculateSymbolAddress();
            if (targetAddr == 0) {
                fprintf(stderr, "ERROR: Failed to calculate address for symbol: %s\n", symName);
                return false;
            }

            if (!applyRelocation(type, locPtr, targetAddr,
                               rela->r_addend, location, rela->r_offset)) {
                fprintf(stderr, "ERROR: Failed to apply relocation type %u for symbol %s\n", type, symName);
                return false;
            }
        }
    }

    return true;
}

void MinimalJITLinker::detectArchitecture(const MinimalELF64Parser& parser) {
    Elf64_Half machine = parser.getMachineType();

    switch (machine) {
    case 183:
        Ctx.TargetArch = ArchType::AArch64;
        break;
    case 243:
        Ctx.TargetArch = ArchType::RISCV64;
        break;
    default:
        Ctx.TargetArch = ArchType::Unknown;
        break;
    }
}

bool MinimalJITLinker::link(const char* objectData, size_t objectSize, uint64_t baseAddress,
                            uint64_t startCode,
                            void (*register_mapping)(uint64_t, uint64_t, uint64_t),
                            void (*log_message)(const char *),
                            const char *AotFile,
                            void *(*g_malloc0)(uint64_t),
                            uint64_t *aot_code_base_ptr,
                            uint64_t *funcmap_rbtree_root_ptr) {
    MinimalELF64Parser parser(objectData, objectSize);
    if (!parser.isValid()) {
        fprintf(stderr, "ERROR:Invalid ELF file\n");
        return false;
    }

    detectArchitecture(parser);

    printSectionsInfo(parser);

    size_t totalSize = calculateTotalSize(parser);
    if (totalSize == 0) {
        fprintf(stderr, "ERROR:Failed to calculate total size\n");
        return false;
    }

#ifdef DEBUG
    std::cout << "DEBUG: Required memory size = " << totalSize << " bytes" << std::endl;
    std::cout << "DEBUG: Preferred address = 0x" << std::hex << baseAddress << std::dec << std::endl;
#endif

    if (!allocateMemory(totalSize, baseAddress)) {
        fprintf(stderr, "ERROR:Failed to allocate memory\n");
        return false;
    }

    if (!setupPLTAndGOT()) {
        fprintf(stderr, "ERROR:Failed to set up PLT and GOT\n");
        return false;
    }

    buildSymbolTable(parser);

    if (!copySectionsAndRelocate(parser, startCode, register_mapping, log_message, AotFile, g_malloc0, aot_code_base_ptr, funcmap_rbtree_root_ptr)) {
        fprintf(stderr, "ERROR:Failed to copy sections and relocate\n");
        return false;
    }

    return true;
}
