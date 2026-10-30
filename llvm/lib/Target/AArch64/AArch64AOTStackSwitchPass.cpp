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
#include "llvm/CodeGen/AOTEnvFlags.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Constants.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "MCTargetDesc/AArch64AddressingModes.h"

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

static void emitLoadEnvOffset(const MachineFunction &MF,
                              MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator I,
                              const DebugLoc &DL,
                              const TargetInstrInfo &TII) {
  const Module *M = MF.getFunction().getParent();
  assert(M);
  const GlobalVariable *GV = M->getNamedGlobal("env_offset");
  assert(GV && GV->hasInitializer());
  auto *CI = dyn_cast<ConstantInt>(GV->getInitializer());
  assert(CI);
  int64_t Off = CI->getSExtValue();      // env_offset fits MOVZ imm16
  BuildMI(MBB, I, DL, TII.get(AArch64::MOVZXi)).addReg(AArch64::X26, RegState::Define)
      .addImm(Off).addImm(0);
}

static void emitStackSwitch(const MachineFunction &MF,
                            MachineBasicBlock &MBB, MachineInstr &CallMI,
                            const TargetInstrInfo &TII) {
  MachineBasicBlock::iterator I(CallMI);
  MachineBasicBlock::iterator After = std::next(I);
  DebugLoc DL = CallMI.getDebugLoc();
  const unsigned ENV = AArch64::X25;

  // ---- before: x25 = cpu + off  (env) ----
  emitLoadEnvOffset(MF, MBB, I, DL, TII);
  BuildMI(MBB, I, DL, TII.get(AArch64::ADDXrx)).addReg(ENV, RegState::Define)
      .addReg(ENV).addReg(AArch64::X26).addImm(AArch64_AM::getArithExtendImm(AArch64_AM::UXTX, 0));

  // Switch stack
  BuildMI(MBB, I, DL, TII.get(AArch64::ADDXri)).addReg(AArch64::X26, RegState::Define).addReg(AArch64::SP).addImm(0).addImm(0);
  BuildMI(MBB, I, DL, TII.get(AArch64::STURXi)).addReg(AArch64::X26).addReg(ENV).addImm(-64);
  BuildMI(MBB, I, DL, TII.get(AArch64::LDURXi)).addReg(AArch64::X26, RegState::Define).addReg(ENV).addImm(-56);
  // Backup ENV
  BuildMI(MBB, I, DL, TII.get(AArch64::STURXi)).addReg(ENV).addReg(AArch64::X26).addImm(-8);
  BuildMI(MBB, I, DL, TII.get(AArch64::SUBXri)).addReg(AArch64::X26, RegState::Define).addReg(AArch64::X26).addImm(16).addImm(0);
  BuildMI(MBB, I, DL, TII.get(AArch64::ADDXri)).addReg(AArch64::SP, RegState::Define).addReg(AArch64::X26).addImm(0).addImm(0);

  // ---- the call sits here ----

  // Restore ENV
  BuildMI(MBB, After, DL, TII.get(AArch64::ADDXri)).addReg(AArch64::SP, RegState::Define).addReg(AArch64::SP).addImm(16).addImm(0);
  BuildMI(MBB, After, DL, TII.get(AArch64::LDURXi)).addReg(AArch64::X25, RegState::Define).addReg(AArch64::SP).addImm(-8);
  // Switch stack
  BuildMI(MBB, After, DL, TII.get(AArch64::ADDXri)).addReg(AArch64::X26, RegState::Define).addReg(AArch64::SP).addImm(0).addImm(0);
  BuildMI(MBB, After, DL, TII.get(AArch64::STURXi)).addReg(AArch64::X26).addReg(AArch64::X25).addImm(-56);
  BuildMI(MBB, After, DL, TII.get(AArch64::LDURXi)).addReg(AArch64::X26, RegState::Define).addReg(AArch64::X25).addImm(-64);
  BuildMI(MBB, After, DL, TII.get(AArch64::ADDXri)).addReg(AArch64::SP, RegState::Define).addReg(AArch64::X26).addImm(0).addImm(0);

  // ---- after: restore x25 ----
  emitLoadEnvOffset(MF, MBB, After, DL, TII);
  BuildMI(MBB, After, DL, TII.get(AArch64::SUBXrx)).addReg(ENV, RegState::Define)
      .addReg(ENV).addReg(AArch64::X26).addImm(AArch64_AM::getArithExtendImm(AArch64_AM::UXTX, 0));
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
      } else if (needsSwitch(Instr)) {
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

FunctionPass *createAArch64AOTStackSwitchPass() {
  return new AArch64AOTStackSwitch();
}

} // namespace llvm
