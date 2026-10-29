//===- AArch64AOTStackSwitchPass.cpp - AOT Stack Switching Pass -----------===//
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

#include "AArch64.h"
#include "AArch64MachineFunctionInfo.h"
#include "AArch64Subtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

using namespace llvm;

#define DEBUG_TYPE "aarch64-aot-stack-switch"

//===----------------------------------------------------------------------===//
// AArch64AOTStackSwitchPass class
//===----------------------------------------------------------------------===//

namespace {

class AArch64AOTStackSwitch : public MachineFunctionPass {
public:
  static char ID; // Pass identification

  AArch64AOTStackSwitch() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "Machine AOT Stack Switch Pass";
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
};
char AArch64AOTStackSwitch::ID = 0;
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Pass initialization
//===----------------------------------------------------------------------===//

INITIALIZE_PASS(AArch64AOTStackSwitch, DEBUG_TYPE,
                      "AArch64 AOT Stack Switch Pass", false, false)

//===----------------------------------------------------------------------===//
// Pass implementation
//===----------------------------------------------------------------------===//

bool AArch64AOTStackSwitch::runOnMachineFunction(MachineFunction &MF) {
  // Get the LLVM function
  const Function &F = MF.getFunction();
  
  // Initialize target information
  const AArch64Subtarget &Subtarget = MF.getSubtarget<AArch64Subtarget>();
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
      
      if (Instr.isInlineAsm()) {
        const char *AsmStr = Instr.getOperand(InlineAsm::MIOp_AsmString).getSymbolName();
        if (strncmp(AsmStr, "mov x25,", 8) == 0) {
          unsigned DidSwitch = 0;
          unsigned RegIdx = 0;
          if (strstr(AsmStr, "mov $0, x0") != NULL) {
            RegIdx = 1;
          }
          for (unsigned i = InlineAsm::MIOp_FirstOperand, ridx = 0, e = Instr.getNumOperands();
               i != e; ++i) {
            const MachineOperand &Op = Instr.getOperand(i);
            if (!Op.isReg())
              continue;
            if (ridx != RegIdx) {
              ridx += 1;
              continue;
            }

            Register Reg = Op.getReg();
            // Switch stack
            BuildMI(MBB, InsertPos, DL, TII.get(AArch64::ADDXri)).addReg(AArch64::X26, RegState::Define).addReg(AArch64::SP).addImm(0).addImm(0);
            BuildMI(MBB, InsertPos, DL, TII.get(AArch64::STURXi)).addReg(AArch64::X26).addReg(Reg).addImm(-64);
            BuildMI(MBB, InsertPos, DL, TII.get(AArch64::LDURXi)).addReg(AArch64::X26, RegState::Define).addReg(Reg).addImm(-56);
            // Backup ENV
            BuildMI(MBB, InsertPos, DL, TII.get(AArch64::STURXi)).addReg(Reg).addReg(AArch64::X26).addImm(-8);
            BuildMI(MBB, InsertPos, DL, TII.get(AArch64::SUBXri)).addReg(AArch64::X26, RegState::Define).addReg(AArch64::X26).addImm(16).addImm(0);
            BuildMI(MBB, InsertPos, DL, TII.get(AArch64::ADDXri)).addReg(AArch64::SP, RegState::Define).addReg(AArch64::X26).addImm(0).addImm(0);
            Changed = true;
            DidSwitch = 1;
            break;
          }
          if (!DidSwitch)
            continue;

          assert(MI != ME);
          InsertPos = MI;
          DL = MI->getDebugLoc();
          // Restore ENV
          BuildMI(MBB, InsertPos, DL, TII.get(AArch64::ADDXri)).addReg(AArch64::SP, RegState::Define).addReg(AArch64::SP).addImm(16).addImm(0);
          BuildMI(MBB, InsertPos, DL, TII.get(AArch64::LDURXi)).addReg(AArch64::X25, RegState::Define).addReg(AArch64::SP).addImm(-8);
          // Switch stack
          BuildMI(MBB, InsertPos, DL, TII.get(AArch64::ADDXri)).addReg(AArch64::X26, RegState::Define).addReg(AArch64::SP).addImm(0).addImm(0);
          BuildMI(MBB, InsertPos, DL, TII.get(AArch64::STURXi)).addReg(AArch64::X26).addReg(AArch64::X25).addImm(-56);
          BuildMI(MBB, InsertPos, DL, TII.get(AArch64::LDURXi)).addReg(AArch64::X26, RegState::Define).addReg(AArch64::X25).addImm(-64);
          BuildMI(MBB, InsertPos, DL, TII.get(AArch64::ADDXri)).addReg(AArch64::SP, RegState::Define).addReg(AArch64::X26).addImm(0).addImm(0);
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

FunctionPass *createAArch64AOTStackSwitchPass() {
  return new AArch64AOTStackSwitch();
}

} // namespace llvm
