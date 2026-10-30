#include "llvm/CodeGen/AOTEnvFlags.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/CallingConv.h"

using namespace llvm;

cl::opt<AOTEnvRegMode> llvm::QEMUAOTEnvRegMode(
    "qemu-aot-env-reg",
    cl::desc("Reserve GPRs for the QEMU AOT env pointer and scratch regs"),
    cl::values(
        clEnumValN(AOTEnvRegMode::Never,  "never",  "Never reserve (default)"),
        clEnumValN(AOTEnvRegMode::AOTOnly,"aot",    "Only in QEMUAOT-CC functions")),
    cl::init(AOTEnvRegMode::Never));

bool llvm::shouldReserveAOTEnvRegs(const MachineFunction &MF) {
  switch (QEMUAOTEnvRegMode) {
  case AOTEnvRegMode::Never:  return false;
  case AOTEnvRegMode::AOTOnly:
    return MF.getFunction().getCallingConv() == CallingConv::QEMUAOT;
  }
  llvm_unreachable("bad AOTEnvRegMode");
}
