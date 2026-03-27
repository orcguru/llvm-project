//===- MachineAOTStackSwitchPass.cpp - AOT Stack Switching Pass -----------===//
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
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

using namespace llvm;

#define DEBUG_TYPE "aarch64-aot-stack-switch"

static bool EnableAOTStackSwitch() {
  static bool Enabled = []() {
    const char *Env = std::getenv("LLVM_ENABLE_AOT_STACK_SWITCH");
    return Env != nullptr && std::strcmp(Env, "1") == 0;
  }();
  return Enabled;
}

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
  if (!EnableAOTStackSwitch()) {
    return false;
  }
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
      
      if (Instr.isCall()) {
        if (!TII.isTailCall(Instr)) {
          // Handle switch from AOT to runtime and return back (need to backup ENV)
          for (const MachineOperand &MO : Instr.operands()) {
            if (MO.isGlobal()) {
              const Function *Callee = dyn_cast<Function>(MO.getGlobal());
              if (Callee && Callee->getCallingConv() != CallingConv::QEMUAOT) {
                // Switch stack
                BuildMI(MBB, InsertPos, DL, TII.get(AArch64::STURXi)).addReg(AArch64::SP).addReg(AArch64::X25).addImm(-64);
                BuildMI(MBB, InsertPos, DL, TII.get(AArch64::LDURXi)).addReg(AArch64::SP).addReg(AArch64::X25).addImm(-56);
                // Backup ENV
                BuildMI(MBB, InsertPos, DL, TII.get(AArch64::STURXi)).addReg(AArch64::X25).addReg(AArch64::SP).addImm(-8);
                BuildMI(MBB, InsertPos, DL, TII.get(AArch64::SUBXri)).addReg(AArch64::SP).addReg(AArch64::SP).addImm(16).addImm(0);
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
                BuildMI(MBB, InsertPos, DL, TII.get(AArch64::STURXi)).addReg(AArch64::SP).addReg(AArch64::X25).addImm(-64);
                BuildMI(MBB, InsertPos, DL, TII.get(AArch64::LDURXi)).addReg(AArch64::SP).addReg(AArch64::X25).addImm(-56);
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
          BuildMI(MBB, InsertPos, DL, TII.get(AArch64::ADDXri)).addReg(AArch64::SP).addReg(AArch64::SP).addImm(16).addImm(0);
          BuildMI(MBB, InsertPos, DL, TII.get(AArch64::LDURXi)).addReg(AArch64::X25).addReg(AArch64::SP).addImm(-8);
          // Switch stack
          BuildMI(MBB, InsertPos, DL, TII.get(AArch64::STURXi)).addReg(AArch64::SP).addReg(AArch64::X25).addImm(-56);
          BuildMI(MBB, InsertPos, DL, TII.get(AArch64::LDURXi)).addReg(AArch64::SP).addReg(AArch64::X25).addImm(-64);
          Changed = true;
        }
      }
    }
  }
  
  return Changed;
}

#if 0
const Function *AArch64AOTStackSwitch::getCallee(const MachineInstr &MI) const {
  // Try to extract the called function from the instruction
  for (const MachineOperand &MO : MI.operands()) {
    if (MO.isGlobal()) {
      return dyn_cast<Function>(MO.getGlobal());
    }
  }
  return nullptr;
}

bool AArch64AOTStackSwitch::processCallInstruction(MachineInstr &MI, 
                                                       MachineBasicBlock &MBB,
                                                       MachineFunction &MF) {
  // Get the callee function
  const Function *Callee = getCallee(MI);
  if (!Callee) {
    // Indirect call or can't determine callee
    // Could be conservative and assume runtime call
    return false;
  }
  
  // Determine the type of caller and callee
  bool CalleeIsAOT = isAOTFunction(*Callee);
  
  // Check if we need to switch stacks
  bool NeedStackSwitch = false;
  bool SwitchToAOT = false;
  
  if (CallerIsAOT) {
    // AOT -> Runtime: Switch to runtime stack before call, back to AOT after
    NeedStackSwitch = true;
    SwitchToAOT = false; // Switch to runtime stack
  } else if (!CallerIsAOT && CalleeIsAOT) {
    // Runtime -> AOT: Switch to AOT stack before call, back to runtime after
    NeedStackSwitch = true;
    SwitchToAOT = true; // Switch to AOT stack
  }
  
  if (!NeedStackSwitch) {
    return false;
  }
  
  DebugLoc DL = MI.getDebugLoc();
  
  return true;
}
#endif

//===----------------------------------------------------------------------===//
// Pass registration
//===----------------------------------------------------------------------===//

namespace llvm {

FunctionPass *createAArch64AOTStackSwitchPass() {
  return new AArch64AOTStackSwitch();
}

} // namespace llvm
