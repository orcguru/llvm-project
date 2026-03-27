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
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

using namespace llvm;

#define DEBUG_TYPE "riscv-aot-stack-switch"

static bool EnableAOTStackSwitch() {
  static bool Enabled = []() {
    const char *Env = std::getenv("LLVM_ENABLE_AOT_STACK_SWITCH");
    return Env != nullptr && std::strcmp(Env, "1") == 0;
  }();
  return Enabled;
}

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

//===----------------------------------------------------------------------===//
// Pass implementation
//===----------------------------------------------------------------------===//

bool RISCVAOTStackSwitch::runOnMachineFunction(MachineFunction &MF) {
  if (!EnableAOTStackSwitch()) {
    return false;
  }
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
      MachineBasicBlock::iterator InsertPos = MI;
      DebugLoc DL = MI->getDebugLoc();
      ++MI; // Advance iterator before we potentially modify the instruction
      
      if (Instr.isCall()) {
        if (!TII.isTailCall(Instr)) {
          // Handle switch from AOT to runtime and return back (need to backup ENV)
          for (const MachineOperand &MO : Instr.operands()) {
            if (MO.isGlobal()) {
              const Function *Callee = dyn_cast<Function>(MO.getGlobal());
              if (Callee && Callee->getCallingConv() != CallingConv::QEMUAOT) {
                // Switch stack
                BuildMI(MBB, InsertPos, DL, TII.get(RISCV::SD)).addReg(RISCV::X2).addReg(RISCV::X25).addImm(-64);
                BuildMI(MBB, InsertPos, DL, TII.get(RISCV::LD)).addReg(RISCV::X2).addReg(RISCV::X25).addImm(-56);
                // Backup ENV
                BuildMI(MBB, InsertPos, DL, TII.get(RISCV::SD)).addReg(RISCV::X25).addReg(RISCV::X2).addImm(-8);
                BuildMI(MBB, InsertPos, DL, TII.get(RISCV::ADDI)).addReg(RISCV::X2).addReg(RISCV::X2).addImm(-16);
                Changed = true;
              }
            }
          }
        } else {
          // Handle switch from AOT to runtime and without return back (do not backup ENV)
          for (const MachineOperand &MO : Instr.operands()) {
            if (MO.isGlobal()) {
              const Function *Callee = dyn_cast<Function>(MO.getGlobal());
              if (Callee && Callee->getCallingConv() != CallingConv::QEMUAOT) {
                BuildMI(MBB, InsertPos, DL, TII.get(RISCV::SD)).addReg(RISCV::X2).addReg(RISCV::X25).addImm(-64);
                BuildMI(MBB, InsertPos, DL, TII.get(RISCV::LD)).addReg(RISCV::X2).addReg(RISCV::X25).addImm(-56);
                Changed = true;
              }
            }
          }
        }
        if (!TII.isTailCall(Instr) && MI != ME) {
          // Handle switch from runtime to AOT
          InsertPos = MI;
          DL = MI->getDebugLoc();
          // Restore ENV
          BuildMI(MBB, InsertPos, DL, TII.get(RISCV::ADDI)).addReg(RISCV::X2).addReg(RISCV::X2).addImm(16);
          BuildMI(MBB, InsertPos, DL, TII.get(RISCV::LD)).addReg(RISCV::X25).addReg(RISCV::X2).addImm(-8);
          // Switch stack
          BuildMI(MBB, InsertPos, DL, TII.get(RISCV::SD)).addReg(RISCV::X2).addReg(RISCV::X25).addImm(-56);
          BuildMI(MBB, InsertPos, DL, TII.get(RISCV::LD)).addReg(RISCV::X2).addReg(RISCV::X25).addImm(-64);
          Changed = true;
        }
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
