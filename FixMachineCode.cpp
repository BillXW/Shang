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
#include "vtm/VerilgoBackendMCTargetDesc.h"
#include "vtm/VInstrInfo.h"
#include "vtm/MicroState.h"

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/raw_ostream.h"
#define DEBUG_TYPE "vtm-fix-machine-code"
#include "llvm/Support/Debug.h"
#include <set>

using namespace llvm;

namespace {
struct FixMachineCode : public MachineFunctionPass {
  static char ID;
  MachineRegisterInfo *MRI;
  const TargetInstrInfo *TII;
  bool IsPreOpt;

  FixMachineCode(bool isPreOpt) : MachineFunctionPass(ID), MRI(0), TII(0),
    IsPreOpt(isPreOpt) {}

  //void getAnalysisUsage(AnalysisUsage &AU) const {
  //  MachineFunctionPass::getAnalysisUsage(AU);
  //  // Is this true?
  //  // AU.setPreservesAll();
  //}

  bool runOnMachineFunction(MachineFunction &MF);

  void handlePHI(MachineInstr *PN, MachineBasicBlock *CurBB);

  bool handleImplicitDefs(MachineInstr *MI);
  bool mergeSel(MachineInstr *MI);
  void mergeSelToCase(MachineInstr *CaseMI, MachineInstr *SelMI, MachineOperand Cnd);

  void FoldInstructions(std::vector<MachineInstr*> &InstrToFold);

  void FoldImmediate(MachineInstr *MI, std::vector<MachineInstr*> &InstrToFold);
  void FoldAdd(MachineInstr *MI, std::vector<MachineInstr*> &InstrToFold);

  bool canbeFold(MachineInstr *MI) const;

  const char *getPassName() const {
    return "Fix machine code for Verilog backend";
  }
};
}

char FixMachineCode::ID = 0;

bool FixMachineCode::runOnMachineFunction(MachineFunction &MF) {
  MRI = &MF.getRegInfo();
  TII = MF.getTarget().getInstrInfo();
  std::vector<MachineInstr*> InstrToFold, PNs;

   // Find out all VOpMove_mi.
  for (MachineFunction::iterator BI = MF.begin(), BE = MF.end();BI != BE;++BI) {
    MachineBasicBlock *MBB = BI;

    for (MachineBasicBlock::iterator II = MBB->begin(), IE = MBB->end();
         II != IE; /*++II*/) {
      MachineInstr *Inst = II;

      ++II; // We may delete the current instruction.      

      if (Inst->isPHI() && !IsPreOpt) {
        PNs.push_back(Inst);
        continue;
      }

      if (handleImplicitDefs(Inst)) continue;

      if (Inst->isCopy()) VInstrInfo::ChangeCopyToMove(Inst);

      // Try to eliminate unnecessary moves.
      if (canbeFold(Inst)) {
        InstrToFold.push_back(Inst);
        continue;
      }

      // Do not perform select merging in pre-optimization run, because selects
      // may only appear after if conversion.
      if (IsPreOpt) continue;

      // Try to merge the Select to improve parallelism.
      mergeSel(Inst);
    }

    //MachineInstr *FirstNotPHI = 0;

    while (!PNs.empty()) {
      MachineInstr *PN = PNs.back();
      PNs.pop_back();

      //if (FirstNotPHI == 0)
      //  FirstNotPHI = llvm::next(MachineBasicBlock::instr_iterator(PN));

      handlePHI(PN, MBB);
    }
  }

  FoldInstructions(InstrToFold);

  return true;
}


void FixMachineCode::handlePHI(MachineInstr *PN, MachineBasicBlock *CurBB) {  
  unsigned BitWidth = cast<ucOperand>(PN->getOperand(0)).getBitWidth();
  //bool isAllImpDef = true;

  for (unsigned i = 1, e = PN->getNumOperands(); i != e; i += 2) {
    MachineOperand &SrcMO = PN->getOperand(i);
    MachineInstr *DefMI = MRI->getVRegDef(SrcMO.getReg());
    assert(DefMI && "Not in SSA form?");
    if (DefMI->isImplicitDef())
      continue;

    MachineBasicBlock *SrcBB = PN->getOperand(i + 1).getMBB();
    VInstrInfo::JT SrcJT;
    bool success = !VInstrInfo::extractJumpTable(*SrcBB, SrcJT, false);
    assert(success && "Broken machine code?");

    // Insert the PHI copy.
    MachineBasicBlock::instr_iterator IP = SrcBB->getFirstInstrTerminator();
    unsigned NewSrcReg =
      MRI->createVirtualRegister(MRI->getRegClass(SrcMO.getReg()));
    VInstrInfo::JT::iterator at = SrcJT.find(CurBB);
    assert(at != SrcJT.end() && "Broken CFG?");

    BuildMI(*SrcBB, IP, DebugLoc(), TII->get(VTM::VOpMvPhi))
      .addOperand(ucOperand::CreateReg(NewSrcReg, BitWidth, true))
      .addOperand(SrcMO).addMBB(CurBB)
      // The phi copy is only active when SrcBB jumping to CurBB.
      .addOperand(at->second)
      .addImm(0);

    SrcMO.ChangeToRegister(NewSrcReg, false);
    //isAllImpDef = false;
  }

  // TODO: Incoming copy?
  // TODO: if all the incoming value of the PHI is ImpDef, erase the PN.
}

bool FixMachineCode::canbeFold(MachineInstr *MI) const {
  if (MI->getOpcode() == VTM::VOpMove_ri)
    return true;

  if (MI->getOpcode() == VTM::VOpAdd || MI->getOpcode() == VTM::VOpAdd_c) {
    // Fold the add only if carry input is 0.
    if (!MI->getOperand(3).isImm() || MI->getOperand(3).getImm() != 0)
      return false;

    if (MI->getOperand(1).isImm() && MI->getOperand(1).getImm() == 0)
      return true;

    if (MI->getOperand(2).isImm() && MI->getOperand(2).getImm() == 0)
      return true;
  }

  return false;
}

bool FixMachineCode::handleImplicitDefs(MachineInstr *MI) {
  if (!MI->isImplicitDef()) return false;

  unsigned Reg = MI->getOperand(0).getReg();
  bool use_empty = true;

  typedef MachineRegisterInfo::use_iterator use_it;
  for (use_it I = MRI->use_begin(Reg), E = MRI->use_end(); I != E; /*++I*/) {
    ucOperand *MO = cast<ucOperand>(&I.getOperand());
    MachineInstr &UserMI = *I;
    ++I;
    // Implicit value always have 64 bit.
    MO->setBitWidth(64);

    if (UserMI.isPHI()) {
      use_empty = false;
      continue;
    }

    // Change to register 0.
    MO->ChangeToRegister(0, false);
  }

  if (use_empty) MI->removeFromParent();
  return false;
}

void FixMachineCode::FoldImmediate(MachineInstr *MI,
                                   std::vector<MachineInstr*> &InstrToFold) {
  unsigned DstReg = MI->getOperand(0).getReg();

  std::set<MachineInstr*> FoldList;
  for (MachineRegisterInfo::use_iterator I = MRI->use_begin(DstReg),
       E = MRI->use_end(); I != E; /*++I*/) {
    MachineInstr &UserMI = *I;
    ++I;

    // Only replace if user is not a PHINode.
    if (UserMI.getOpcode() == VTM::PHI) continue;

    // There maybe an instruction read the same register twice.
    FoldList.insert(&UserMI);
  }

  // Replace the register operand by a immediate operand.
  typedef std::set<MachineInstr*>::iterator it;
  for (it I = FoldList.begin(), E = FoldList.end(); I != E; ++I) {
    MachineInstr *UserMI = *I;
    if (TII->FoldImmediate(UserMI, MI, DstReg, MRI) && canbeFold(UserMI))
      InstrToFold.push_back(UserMI);
  }

  // Eliminate the instruction if it dead.
  if (MRI->use_empty(DstReg)) MI->eraseFromParent();
}

void FixMachineCode::FoldAdd(MachineInstr *MI,
                             std::vector<MachineInstr*> &InstrToFold) {
  unsigned NoneZeroIdx = 1;
  if (MI->getOperand(1).isImm() && MI->getOperand(1).getImm() == 0)
    NoneZeroIdx = 2;

  // Change the add to bitcat(0, NoneZeroOperand) to construct the result.
  MI->setDesc(TII->get(VTM::VOpBitCat));
  MI->RemoveOperand(3);

  if (NoneZeroIdx != 2) {
    unsigned NoneZeroReg = MI->getOperand(NoneZeroIdx).getReg();
    MI->getOperand(2).ChangeToRegister(NoneZeroReg, false);
  }

  // Build the carry bit of the original
  ucOperand &DummyCarry = cast<ucOperand>(MI->getOperand(1));
  DummyCarry.ChangeToImmediate(0);
  DummyCarry.setBitWidth(1);
}

void FixMachineCode::FoldInstructions(std::vector<MachineInstr*> &InstrToFold) {
  while (!InstrToFold.empty()) {
    MachineInstr *MI = InstrToFold.back();
    InstrToFold.pop_back();

    switch (MI->getOpcode()) {
    case VTM::VOpMove_ri:
      FoldImmediate(MI, InstrToFold);
      break;
    case VTM::VOpAdd:
    case VTM::VOpAdd_c:
      FoldAdd(MI, InstrToFold);
      break;
    default:
      llvm_unreachable("Trying to fold unexpected instruction!");
    }
  }
}

bool FixMachineCode::mergeSel(MachineInstr *MI) {
  if (MI->getOpcode() != VTM::VOpSel) return false;

  MachineOperand TVal = MI->getOperand(2), FVal = MI->getOperand(3);
  MachineInstr *TMI =0, *FMI = 0;
  if (TVal.isReg()) {
    TMI = MRI->getVRegDef(TVal.getReg());
    if (TMI && TMI->getOpcode() != VTM::VOpSel)
      TMI = 0;
  }

  if (FVal.isReg()) {
    FMI = MRI->getVRegDef(FVal.getReg());
    if (FMI && FMI->getOpcode() != VTM::VOpSel)
      FMI = 0;
  }

  // Both operands are not read from VOpSel
  if (!TMI && !FMI) return false;

  MachineInstr *CaseMI =
    BuildMI(*MI->getParent(), MI, MI->getDebugLoc(), TII->get(VTM::VOpCase))
      .addOperand(MI->getOperand(0)). // Result
      addOperand(MI->getOperand(4)). // Predicate
      addOperand(MI->getOperand(5)); // Trace number

  // Merge the select in to the newly build case.
  MachineOperand Cnd = MI->getOperand(1);
  if (TMI)
    mergeSelToCase(CaseMI, TMI, Cnd);
  else {
    // Re-add the condition into the case statement.
    CaseMI->addOperand(Cnd);
    CaseMI->addOperand(TVal);
  }

  VInstrInfo::ReversePredicateCondition(Cnd);
  if (FMI)
    mergeSelToCase(CaseMI, FMI, Cnd);
  else {
    // Re-add the condition into the case statement.
    CaseMI->addOperand(Cnd);
    CaseMI->addOperand(FVal);
  }

  MI->eraseFromParent();
  return true;
}

void FixMachineCode::mergeSelToCase(MachineInstr *CaseMI, MachineInstr *SelMI,
                                    MachineOperand Cnd) {
  MachineOperand SelTCnd = SelMI->getOperand(1);
  MachineOperand SelFCnd = SelTCnd;
  VInstrInfo::ReversePredicateCondition(SelFCnd);

  // Merge the condition with predicate of the select operation.
  if (TII->isPredicated(SelMI)) {
    // DIRTYHACK: This pass the testsuite, why?
    //MachineOperand SelPred = *VInstrInfo::getPredOperand(SelMI);

    //SelTCnd = VInstrInfo::MergePred(SelTCnd, SelPred, *CaseMI->getParent(),
    //                                CaseMI, &MRI, TII, VTM::VOpAnd);
    //SelFCnd = VInstrInfo::MergePred(SelFCnd, SelPred, *CaseMI->getParent(),
    //                                CaseMI, &MRI, TII, VTM::VOpAnd);
  }

  // Merge the condition with the condition to select this SelMI.
  SelTCnd = VInstrInfo::MergePred(SelTCnd, Cnd, *CaseMI->getParent(),
                                  CaseMI, MRI, TII, VTM::VOpAnd);
  SelFCnd = VInstrInfo::MergePred(SelFCnd, Cnd, *CaseMI->getParent(),
                                  CaseMI, MRI, TII, VTM::VOpAnd);

  // Add the value for True condition.
  CaseMI->addOperand(SelTCnd);
  CaseMI->addOperand(SelMI->getOperand(2));
  // Add the value for False condition.
  CaseMI->addOperand(SelFCnd);
  CaseMI->addOperand(SelMI->getOperand(3));
}

Pass *llvm::createFixMachineCodePass(bool IsPreOpt) {
  return new FixMachineCode(IsPreOpt);
}
