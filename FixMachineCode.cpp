//===---------- FixMachineCode.cpp - Fix The Machine Code  ------*- C++ -*-===//
//
//                            The Verilog Backend
//
// Copyright: 2010 by Hongbin Zheng. all rights reserved.
// IMPORTANT: This software is supplied to you by Hongbin Zheng in consideration
// of your agreement to the following terms, and your use, installation,
// modification or redistribution of this software constitutes acceptance
// of these terms.  If you do not agree with these terms, please do not use,
// install, modify or redistribute this software. You may not redistribute,
// install copy or modify this software without written permission from
// Hongbin Zheng.
//
//===----------------------------------------------------------------------===//
//
// This file implements a pass that fix the machine code to simpler the code in
// later pass.
//
//===----------------------------------------------------------------------===//
#include "vtm/Passes.h"
#include "vtm/VTM.h"
#include "vtm/MicroState.h"

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "elim-set-ri"
#include "llvm/Support/Debug.h"
#include <set>

using namespace llvm;
STATISTIC(UnconditionalBranches,
          "Number of unconditionnal branches inserted for fall through edges");
STATISTIC(Unreachables,
     "Number of Unreachable inserted for machine basic block without sucessor");
namespace {
struct FixMachineCode : public MachineFunctionPass {
  static char ID;

  FixMachineCode() : MachineFunctionPass(ID) {}

  void getAnalysisUsage(AnalysisUsage &AU) const {
    MachineFunctionPass::getAnalysisUsage(AU);
    // Is this true?
    // AU.setPreservesAll();
  }

  void insertUnconditionalBranch(MachineBasicBlock &MBB);

  bool runOnMachineFunction(MachineFunction &MF);

  void eliminateMVImm(std::vector<MachineInstr*> &Worklist,
                      MachineRegisterInfo &MRI);

  const char *getPassName() const {
    return "Fix machine code for Verilog backend";
  }
};
}

char FixMachineCode::ID = 0;

bool FixMachineCode::runOnMachineFunction(MachineFunction &MF) {
  bool Changed = false;
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const TargetInstrInfo *TII = MF.getTarget().getInstrInfo();

  std::vector<MachineInstr*> Imms;
  std::set<MachineBasicBlock*> MissedSuccs;
   // Find out all VOpMvImm.
  for (MachineFunction::iterator BI = MF.begin(), BE = MF.end();BI != BE;++BI) {
    MachineBasicBlock *MBB = BI;
    MissedSuccs.insert(MBB->succ_begin(), MBB->succ_end());

    for (MachineBasicBlock::iterator II = MBB->begin(), IE = MBB->end();
         II != IE; ++II) {
      MachineInstr *Inst = II;
      // Try to eliminate unnecessary moves.
      if (Inst->getOpcode() == VTM::VOpMvImm && Inst->getOperand(1).isImm())
        Imms.push_back(Inst);

      // Remove the explicit successors from the missed successors set.
      if (Inst->getOpcode() == VTM::VOpToState)
        MissedSuccs.erase(Inst->getOperand(0).getMBB());
    }

    // Make sure each basic block have a terminator.
    if (!MissedSuccs.empty()) {
      assert(MissedSuccs.size() == 1 && "Fall through to multiple blocks?");
      ++UnconditionalBranches;

      BuildMI(MBB, DebugLoc(), TII->get(VTM::VOpToState))
        .addMBB(*MissedSuccs.begin()).addOperand(ucOperand::CreatePredicate());
    }

    if (MBB->succ_size() == 0 && MBB->getFirstTerminator() == MBB->end()) {
      ++Unreachables;
      BuildMI(MBB, DebugLoc(), TII->get(VTM::VOpUnreachable))
        .addOperand(ucOperand::CreatePredicate());
    }
    MissedSuccs.clear();
  }

  // Try to replace the register operand with the constant for users of VOpMvImm.
  if (!Imms.empty()) {
    eliminateMVImm(Imms, MRI);
    Changed = true;
  }

  return Changed;
}

void FixMachineCode::eliminateMVImm(std::vector<MachineInstr*> &Worklist,
                                    MachineRegisterInfo &MRI) {
  while (!Worklist.empty()) {
    MachineInstr *MI = Worklist.back();
    Worklist.pop_back();

    unsigned DstReg = MI->getOperand(0).getReg();

    // Find all replaceable operand.
    std::vector<MachineOperand*> ImmUsers;
    for (MachineRegisterInfo::use_iterator I = MRI.use_begin(DstReg),
      E = MRI.use_end(); I != E; ++I) {
        MachineOperand &MO = I.getOperand();

        // Only replace if user is not a PHINode.
        if (I->getOpcode() == VTM::PHI) continue;

        ImmUsers.push_back(&MO);
    }

    // Perform the replacement.
    int64_t Imm = MI->getOperand(1).getImm();

    while (!ImmUsers.empty()) {
      ImmUsers.back()->ChangeToImmediate(Imm);
      ImmUsers.pop_back();
    }

    // Eliminate the instruction if it dead.
    if (MRI.use_empty(DstReg)) MI->eraseFromParent();
  }
}


Pass *llvm::createFixMachineCodePass() {
  return new FixMachineCode();
}
