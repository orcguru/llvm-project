#include "llvm/ExecutionEngine/JITLink/JITLink.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/ExecutionEngine/JITLink/COFF.h"
#include "llvm/ExecutionEngine/JITLink/ELF.h"
#include "llvm/ExecutionEngine/JITLink/MachO.h"
#include "llvm/ExecutionEngine/JITLink/aarch64.h"
#include "llvm/ExecutionEngine/JITLink/i386.h"
#include "llvm/ExecutionEngine/JITLink/loongarch.h"
#include "llvm/ExecutionEngine/JITLink/x86_64.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::object;

#define DEBUG_TYPE "jitlink"

#include <iostream>

extern "C" {
void invoke_jitlink(const char *AotFile) {
  std::cout << "invoke_jitlink begin\n" << std::flush;
  std::string InputFile = AotFile;
  auto F = MemoryBuffer::getFile(InputFile);
  if (!F) {
    std::cerr << "Failed to getFile " << F.getError().message() << "\n";
    return;
  }
  auto GraphOrErr = llvm::jitlink::createLinkGraphFromObject((*F)->getMemBufferRef());
  if (auto Err = GraphOrErr.takeError()) {
    std::cerr << "Failed createLinkGrpahFromObject\n";
    return;
  }
  std::cout << "invoke_jitlink done\n" << std::flush;
}
}
