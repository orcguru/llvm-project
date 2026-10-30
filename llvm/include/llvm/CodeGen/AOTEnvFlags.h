//===-- llvm/include/llvm/CodeGen/AOTEnvFlags.h -----------------*- C++ -*-===//
#ifndef LLVM_CODEGEN_AOTENVFLAGS_H
#define LLVM_CODEGEN_AOTENVFLAGS_H

#include "llvm/Support/CommandLine.h"

namespace llvm {

class MachineFunction;

/// How to reserve the AOT env/scratch GPRs.  The actual register numbers are
/// chosen by each backend; this only controls *whether* they are reserved.
enum class AOTEnvRegMode {
  Never,    ///< Do not reserve; also disables the stack-switch pass.
  AOTOnly,  ///< Reserve only in functions using the QEMUAOT calling convention.
};

/// -qemu-aot-env-reg=<never|aot|always>
extern cl::opt<AOTEnvRegMode> QEMUAOTEnvRegMode;

/// Policy predicate: must reserve the env/scratch GPRs for this function?
bool shouldReserveAOTEnvRegs(const MachineFunction &MF);

} // end namespace llvm
#endif
