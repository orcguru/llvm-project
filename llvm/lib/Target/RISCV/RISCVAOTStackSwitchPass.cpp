//===- RISCVAOTStackSwitchPass.cpp - AOT Stack Switching Pass -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a MachineFunctionPass that inserts stack switching
// instructions when execution transitions between AOT-compiled regions and
// runtime regions.
//
//===----------------------------------------------------------------------===//

#include "RISCV.h"
#include "RISCVSubtarget.h"
#include "llvm/CodeGen/AOTEnvFlags.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

using namespace llvm;

#define DEBUG_TYPE "riscv-aot-stack-switch"

//===----------------------------------------------------------------------===//
// RISCVAOTStackSwitchPass class
//===----------------------------------------------------------------------===//

namespace {

class RISCVAOTStackSwitch : public MachineFunctionPass {
public:
  static char ID; // Pass identification

  RISCVAOTStackSwitch() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "Machine AOT Stack Switch Pass";
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
};
char RISCVAOTStackSwitch::ID = 0;
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Pass initialization
//===----------------------------------------------------------------------===//

INITIALIZE_PASS(RISCVAOTStackSwitch, DEBUG_TYPE,
                      "RISCV AOT Stack Switch Pass", false, false)

static void emitStackSwitch(const MachineFunction &MF,
                            MachineBasicBlock &MBB, MachineInstr &CallMI,
                            const TargetInstrInfo &TII) {
  MachineBasicBlock::iterator I(CallMI);
  MachineBasicBlock::iterator After = std::next(I);
  DebugLoc DL = CallMI.getDebugLoc();
  const unsigned ENV = RISCV::X7;

  // Switch stack
  BuildMI(MBB, I, DL, TII.get(RISCV::SD)).addReg(RISCV::X2).addReg(ENV).addImm(-64);
  BuildMI(MBB, I, DL, TII.get(RISCV::LD)).addReg(RISCV::X2, RegState::Define).addReg(ENV).addImm(-56);
  // Reload thread-pointer
  BuildMI(MBB, I, DL, TII.get(RISCV::LD)).addReg(RISCV::X4).addReg(ENV).addImm(-80);
  // Backup ENV
  BuildMI(MBB, I, DL, TII.get(RISCV::SD)).addReg(ENV).addReg(RISCV::X2).addImm(-8);
  BuildMI(MBB, I, DL, TII.get(RISCV::ADDI)).addReg(RISCV::X2).addReg(RISCV::X2).addImm(-16);

  // ---- the call sits here ----
  if (!TII.isTailCall(CallMI)) {
    // Restore ENV
    BuildMI(MBB, After, DL, TII.get(RISCV::ADDI)).addReg(RISCV::X2).addReg(RISCV::X2).addImm(16);
    BuildMI(MBB, After, DL, TII.get(RISCV::LD)).addReg(ENV).addReg(RISCV::X2).addImm(-8);
    // Switch stack
    BuildMI(MBB, After, DL, TII.get(RISCV::SD)).addReg(RISCV::X2).addReg(ENV).addImm(-56);
    BuildMI(MBB, After, DL, TII.get(RISCV::LD)).addReg(RISCV::X2).addReg(ENV).addImm(-64);
  }
}

static bool needsSwitch(const MachineInstr &MI) {
  if (!MI.isCall()) return false;
  for (const MachineOperand &MO : MI.operands()) {
    if (MO.isSymbol()) return true;                 // __divti3, memcpy, ... : always C ABI
    if (MO.isGlobal()) {
      const Function *F = dyn_cast<Function>(MO.getGlobal());
      return !F || F->getCallingConv() != CallingConv::QEMUAOT;
    }
  }
  return false;                                     // BLR: unknown callee
}

//===----------------------------------------------------------------------===//
// Pass implementation
//===----------------------------------------------------------------------===//

bool RISCVAOTStackSwitch::runOnMachineFunction(MachineFunction &MF) {
  // Get the LLVM function
  const Function &F = MF.getFunction();
  
  // Initialize target information
  const RISCVSubtarget &Subtarget = MF.getSubtarget<RISCVSubtarget>();
  const TargetInstrInfo &TII = *Subtarget.getInstrInfo();
  
  // Check if this function needs stack switching instrumentation
  if (F.getCallingConv() != CallingConv::QEMUAOT) {
    return false;
  }
  bool Changed = false;
  
  // Process all basic blocks
  for (MachineBasicBlock &MBB : MF) {
    // We need to be careful with iterators as we may insert instructions
    MachineBasicBlock::iterator MI = MBB.begin();
    MachineBasicBlock::iterator ME = MBB.end();
    
    while (MI != ME) {
      MachineInstr &Instr = *MI;
      DebugLoc DL = MI->getDebugLoc();
      ++MI; // Advance iterator before we potentially modify the instruction
      
      if (needsSwitch(Instr)) {
        if (!shouldReserveAOTEnvRegs(MF))
          report_fatal_error("Please set flag --qemu-aot-env-reg=aot");
        emitStackSwitch(MF, MBB, Instr, TII);
        Changed = true;
      }
    }
  }
  
  return Changed;
}

//===----------------------------------------------------------------------===//
// Pass registration
//===----------------------------------------------------------------------===//

namespace llvm {

FunctionPass *createRISCVAOTStackSwitchPass() {
  return new RISCVAOTStackSwitch();
}

} // namespace llvm
