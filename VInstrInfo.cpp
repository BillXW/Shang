//===---------- VInstrInfo.cpp - VTM Instruction Information -----*- C++ -*-===//
//
// Copyright: 2011 by SYSU EDA Group. all rights reserved.
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
// This file contains the VTM implementation of the TargetInstrInfo class.
//
//===----------------------------------------------------------------------===//


#include "VTargetMachine.h"

#include "vtm/VerilgoBackendMCTargetDesc.h"
#include "vtm/VFInfo.h"
#include "vtm/VInstrInfo.h"
#include "vtm/MicroState.h"

#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/MC/MCInstrDesc.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "vtm-instr-info"
#include "llvm/Support/Debug.h"
#define GET_INSTRINFO_CTOR
#include "VerilogBackendGenInstrInfo.inc"
#include <float.h>

namespace llvm {
extern const MCInstrDesc VTMInsts[];
}

using namespace llvm;
static cl::opt<bool>
EnableBLC("vtm-enable-blc", cl::desc("Enable bit level chaining"),
          cl::init(true));

//----------------------------------------------------------------------------//
// Halper function.
static MachineInstr *addOperandsToMI(MachineInstr *MI,
                                     ArrayRef<MachineOperand> Ops) {
  for (unsigned i = 0; i < Ops.size(); ++i)
    MI->addOperand(Ops[i]);

  return MI;
}

//----------------------------------------------------------------------------//
// VInstrInfo implementation.
VInstrInfo::VInstrInfo() : VTMGenInstrInfo(), RI() {}

const MCInstrDesc &VInstrInfo::getDesc(unsigned Opcode)  {
  return VTMInsts[Opcode];
}

const MachineOperand *VInstrInfo::getPredOperand(const MachineInstr *MI) {
  if (MI->getOpcode() <= TargetOpcode::COPY) return 0;

  unsigned Idx = MI->getDesc().NumOperands - 2;
  assert(MI->getDesc().OpInfo[Idx].isPredicate() && "Cannot get PredOperand!");
  return &MI->getOperand(Idx);
}

MachineOperand *VInstrInfo::getPredOperand(MachineInstr *MI) {
  return const_cast<MachineOperand*>(getPredOperand((const MachineInstr*)MI));
}

bool VInstrInfo::isReallyTriviallyReMaterializable(const MachineInstr *MI,
                                                   AliasAnalysis *AA) const {
  const MCInstrDesc &TID = MI->getDesc();
  return !TID.isBarrier() && hasTrivialFU(TID.getOpcode());
}

bool VInstrInfo::isPredicable(MachineInstr *MI) const {
  return MI->getOpcode() > VTM::COPY && MI->getOpcode() != VTM::VOpRet;
}

bool VInstrInfo::isPredicated(const MachineInstr *MI) const {
  // Pseudo machine instruction are never predicated.
  if (!isPredicable(const_cast<MachineInstr*>(MI))) return false;

  if (const MachineOperand *Pred = getPredOperand(MI))
    return (Pred->isReg() && Pred->getReg() != 0);

  return false;
}

void VInstrInfo::ChangeCopyToMove(MachineInstr *CopyMI) {
  CopyMI->setDesc(getDesc(VTM::VOpMove));
  CopyMI->addOperand(ucOperand::CreatePredicate());
  CopyMI->addOperand(ucOperand::CreateTrace(CopyMI->getParent()));
}

bool VInstrInfo::FoldImmediate(MachineInstr *UseMI, MachineInstr *DefMI,
                               unsigned Reg, MachineRegisterInfo *MRI) const {
  // Simply change the machine operand in UseMI to imediate.
  MachineOperand &ImmediateMO = DefMI->getOperand(1);
  unsigned char ImmediateTFs = ImmediateMO.getTargetFlags();
  
  typedef MachineInstr::mop_iterator it;
  if (ImmediateMO.isImm()) {
    int64_t ImmediateValue = ImmediateMO.getImm();

    typedef MachineInstr::mop_iterator it;
    for (it I = UseMI->operands_begin(), E = UseMI->operands_end(); I != E; ++I) {
      MachineOperand &MO = *I;
      if (MO.isReg() && MO.getReg() == Reg) {
        assert(MO.getTargetFlags() <= ImmediateTFs
          && "Folding immediate with different Bitwidth?");
        MO.ChangeToImmediate(ImmediateValue);
      }
    }
  } else {
    // Else we need to rebuild the UserMI.
    SmallVector<MachineOperand, 6> MOs;
    for (it I = UseMI->operands_begin(), E = UseMI->operands_end(); I != E; ++I) {
      ucOperand &MO = cast<ucOperand>(*I);
      // Are we going to replace the operand?
      if (MO.isReg() && MO.getReg() == Reg) {
        assert(MO.getBitWidth() <= ImmediateTFs
          && "Folding immediate with different Bitwidth?");
        MOs.push_back(ImmediateMO);
        // Keep the original flags, because the target flags of ImmediateMO may
        // difference from MO after simplify bitslice.
        MOs.back().setTargetFlags(MO.getTargetFlags());
        continue;
      }

      MOs.push_back(MO);
    }

    while (UseMI->getNumOperands())
      UseMI->RemoveOperand(UseMI->getNumOperands() - 1);
    addOperandsToMI(UseMI, MOs);
  }

  // Are we fold a immediate into a copy?
  if (UseMI->isCopy())  ChangeCopyToMove(UseMI);

  return true;
}

bool VInstrInfo::shouldAvoidSinking(MachineInstr *MI) const {
  return MI->getOpcode() == VTM::VOpMoveArg || isDatapath(MI->getOpcode());
}

MachineInstr *VInstrInfo::commuteInstruction(MachineInstr *MI, bool NewMI)const{
  const MCInstrDesc &TID = MI->getDesc();
  bool HasDef = TID.getNumDefs();
  assert(HasDef && MI->getOperand(0).isReg() && "Bad instruction to commute!");
  // Build the operand list and swap the operands to cummuted.
  SmallVector<MachineOperand, 6> MOs;
  typedef MachineInstr::mop_iterator it;
  for (it I = MI->operands_begin(), E = MI->operands_end(); I != E; ++I)
    MOs.push_back(*I);

  std::swap(MOs[1], MOs[2]);

  // Build a empty MI or clear the operands in the original MI, and re-insert
  // the operands to MI.
  if (NewMI) {
    MachineFunction &MF = *MI->getParent()->getParent();
    MI = BuildMI(MF, MI->getDebugLoc(), TID);
  } else
    while (MI->getNumOperands())
      MI->RemoveOperand(MI->getNumOperands() - 1);

  return addOperandsToMI(MI, MOs);
}

bool VInstrInfo::isUnpredicatedTerminator(const MachineInstr *MI) const{
  const MCInstrDesc &TID = MI->getDesc();
  if (!TID.isTerminator()) return false;

  return !isPredicated(MI);
}

bool VInstrInfo::isUnConditionalBranch(MachineInstr *MI) {
  if (!VInstrInfo::isBrCndLike(MI->getOpcode())) return false;

  MachineOperand &Cnd = MI->getOperand(0);
  return (Cnd.isReg() && Cnd.getReg() == 0) || Cnd.isImm();
}

bool VInstrInfo::AnalyzeBranch(MachineBasicBlock &MBB, MachineBasicBlock *&TBB,
                               MachineBasicBlock *&FBB,
                               SmallVectorImpl<MachineOperand> &Cond,
                               bool AllowModify /* = false */) const {
  if (MBB.empty()) return false;

  // Do not mess with the scheduled code.
  if (MBB.back().getOpcode() == VTM::EndState)
    return true;

  /// 1. If this block ends with no branches (it just falls through to its succ)
  ///    just return false, leaving TBB/FBB null.
  if (MBB.getFirstTerminator() == MBB.end()) return false;

  SmallVector<MachineInstr*, 4> Terms;
  for (MachineBasicBlock::iterator I = MBB.getFirstTerminator(), E = MBB.end();
       I != E; ++I) {
    MachineInstr *Inst = I;
    if (!Inst->getDesc().isTerminator()) continue;

    if (VInstrInfo::isBrCndLike(Inst->getOpcode())) {
      if (!isUnpredicatedTerminator(Inst)) return true;

      Terms.push_back(Inst);
    } else // Do not mess up with non-brcnd like terminators, i.e. return
      return true;
  }

  // Mixing branches and return?
  if (Terms.empty() || Terms.size() > 2) return true;

  MachineInstr *FstTerm = Terms[0];

  /// 2. If this block ends with only an unconditional branch, it sets TBB to be
  ///    the destination block.
  if (isUnConditionalBranch(FstTerm)) {
    TBB = FstTerm->getOperand(1).getMBB();
    assert(Terms.size() == 1 && "Expect single fall through edge!");
    return false;
  }

  Cond.push_back(FstTerm->getOperand(0));
  Cond.back().setIsKill(false);
  TBB = FstTerm->getOperand(1).getMBB();
  /// 3. If this block ends with a conditional branch and it falls through to a
  ///    successor block, it sets TBB to be the branch destination block and a
  ///    list of operands that evaluate the condition. These operands can be
  ///    passed to other TargetInstrInfo methods to create new branches.
  if (Terms.size() == 1) return false;

  /// 4. If this block ends with a conditional branch followed by an
  ///    unconditional branch, it returns the 'true' destination in TBB, the
  ///    'false' destination in FBB, and a list of operands that evaluate the
  ///    condition.  These operands can be passed to other TargetInstrInfo
  ///    methods to create new branches.
  MachineInstr *SndTerm = Terms[1];

  // Avoid strange branches.
  MachineOperand &SndPred = SndTerm->getOperand(0);
  if (SndPred.isReg()) {
    unsigned Reg = SndTerm->getOperand(0).getReg();
    if (Reg != 0 && Reg != Cond.front().getReg())
      return true;
  }

  FBB = SndTerm->getOperand(1).getMBB();
  return false;
}

bool VInstrInfo::extractJumpTable(MachineBasicBlock &BB, JT &Table, bool BrOnly)
{
  for (MachineBasicBlock::iterator I = BB.getFirstTerminator(), E = BB.end();
       I != E; ++I) {
    // We can only handle conditional jump.
    if (!VInstrInfo::isBrCndLike(I->getOpcode())) {
      if (BrOnly) return true;
      continue;
    }

    // Do not mess up with the predicated terminator at the moment.
    if (const MachineOperand *Pred = getPredOperand(I))
      if (Pred->isReg() && Pred->getReg() != 0)
        return true;

    MachineBasicBlock *TargetBB = I->getOperand(1).getMBB();
    MachineOperand Cnd = I->getOperand(0);
    bool inserted = Table.insert(std::make_pair(TargetBB, Cnd)).second;
    assert(inserted && "BB with multiple entry in jump table?");
  }

  // Are we fail to extract all jump table entry?
  return Table.size() != BB.succ_size();
}

unsigned VInstrInfo::RemoveBranch(MachineBasicBlock &MBB) const {
  // Do not mess with the scheduled code.
  if (MBB.back().getOpcode() == VTM::EndState)
    return 0;

  // Just a fall through edge, return false and leaving TBB/FBB null.
  if (MBB.getFirstTerminator() == MBB.end()) return true;

  // Collect the branches and remove them.
  SmallVector<MachineInstr*, 4> Terms;
  for (MachineBasicBlock::iterator I = MBB.getFirstTerminator(), E = MBB.end();
       I != E; ++I) {
    MachineInstr *Inst = I;
    if (!Inst->getDesc().isTerminator()) continue;

    if (VInstrInfo::isBrCndLike(Inst->getOpcode()))
      Terms.push_back(Inst);
  }

  unsigned RemovedBranches = 0;
  while (!Terms.empty()) {
    Terms.pop_back_val()->removeFromParent();
    ++RemovedBranches;
  }

  return RemovedBranches;
}

void VInstrInfo::ReversePredicateCondition(MachineOperand &Cond) {
  assert(Cond.isReg() && "Broken predicate condition!");
  Cond.setTargetFlags(Cond.getTargetFlags() ^ VInstrInfo::PredInvertFlag);
}

bool
VInstrInfo::ReverseBranchCondition(SmallVectorImpl<MachineOperand> &Cond) const{
  // Invert the invert condition flag.
  for(unsigned i = 0, e = Cond.size(); i < e; ++i)
    ReversePredicateCondition(Cond[i]);
  return false;
}

unsigned VInstrInfo::InsertBranch(MachineBasicBlock &MBB,
                                  MachineBasicBlock *TBB,
                                  MachineBasicBlock *FBB,
                                  const SmallVectorImpl<MachineOperand> &Cond,
                                  DebugLoc DL) const {
  assert((Cond.size() <= 1) && "Too much conditions!");
  bool isUnconditional = Cond.empty();
  MachineOperand PredOp = isUnconditional ?
                          ucOperand::CreatePredicate() : Cond[0];
  PredOp.setIsKill(false);

  if (FBB == 0) {
    // Insert barrier branch for unconditional branch.
    unsigned Opc = isUnconditional ? VTM::VOpToStateb : VTM::VOpToState;
    BuildMI(&MBB, DL, get(Opc)).addOperand(PredOp).addMBB(TBB)
      .addOperand(ucOperand::CreatePredicate())
      .addOperand(ucOperand::CreateTrace(&MBB));
    return 1;
  }

  // Two-way conditional branch.
  assert(PredOp.isReg() && PredOp.getReg() != 0
         && "Uncondtional predicate with true BB and false BB?");
  // Branch to true BB, with the no-barrier version.
  BuildMI(&MBB, DL, get(VTM::VOpToState)).addOperand(PredOp).addMBB(TBB)
    .addOperand(ucOperand::CreatePredicate())
    .addOperand(ucOperand::CreateTrace(&MBB));
  // Branch to the false BB.
  ReversePredicateCondition(PredOp);
  BuildMI(&MBB, DL, get(VTM::VOpToStateb)).addOperand(PredOp).addMBB(FBB)
    .addOperand(ucOperand::CreatePredicate())
    .addOperand(ucOperand::CreateTrace(&MBB));
   return 2;
}

void VInstrInfo::insertJumpTable(MachineBasicBlock &BB, JT &Table, DebugLoc dl){
  assert(BB.getFirstTerminator() == BB.end() && "Cannot insert jump table!");
  assert(Table.size() == BB.succ_size()&&"Table size and succ_size not match!");

  // Dirty hack: We may not evaluate the predicate to always true at the moment.
  if (Table.size() == 1) {
    BuildMI(&BB, dl, getDesc(VTM::VOpToStateb))
      .addOperand(ucOperand::CreatePredicate()).addMBB(*BB.succ_begin())
      .addOperand(ucOperand::CreatePredicate())
      .addOperand(ucOperand::CreateTrace(&BB));
    return;
  }

  for (JT::iterator I = Table.begin(), E = Table.end(); I != E; ++I) {
    I->second.setIsKill(false);
    BuildMI(&BB, dl, getDesc(VTM::VOpToStateb))
      .addOperand(I->second).addMBB(I->first)
      .addOperand(ucOperand::CreatePredicate())
      .addOperand(ucOperand::CreateTrace(&BB));
  }
}

bool VInstrInfo::DefinesPredicate(MachineInstr *MI,
                                  std::vector<MachineOperand> &Pred) const {
  //MachineRegisterInfo &MRI = MI->getParent()->getParent()->getRegInfo();
  //for (unsigned i = 0, e = MI->getNumOperands(); i < e; ++i) {
  //  MachineOperand &MO =MI->getOperand(i);
  //  if (!MO.isReg() || !MO.isDef()) continue;

  //  if (MRI.getRegClass(MO.getReg()) == VTM::PredRRegisterClass)
  //    Pred.push_back(MO);
  //}

  //return !Pred.empty();
  return false;
}

bool VInstrInfo::isProfitableToIfCvt(MachineBasicBlock &TMBB,
                                     unsigned NumTCycles,
                                     unsigned ExtraTCycles,
                                     MachineBasicBlock &FMBB,
                                     unsigned NumFCycles,
                                     unsigned ExtraFCycles,
                                     float Probability, float Confidence) const{
  return true; // DirtyHack: Everything is profitable.
}

bool VInstrInfo::isProfitableToIfCvt(MachineBasicBlock &MBB, unsigned NumCyles,
                                     unsigned ExtraPredCycles,
                                     float Probability, float Confidence) const{
  return true;
}

bool VInstrInfo::isProfitableToDupForIfCvt(MachineBasicBlock &MBB,
                                           unsigned NumCyles,
                                           float Probability,
                                           float Confidence) const {
  return false; // DirtyHack: Duplicate the BB will break SSA from.
}

MachineInstr *VInstrInfo::PredicatePseudoInstruction(MachineInstr *MI,
                                                     const SmallVectorImpl<MachineOperand> &Pred) {

  // Implicit define do not need to predicate at all.
  if (MI->isImplicitDef()) return MI;

  if (MI->getOpcode() != VTM::COPY) return 0;

  if (MI->isImplicitDef()) return MI;

  if (MI->getOpcode() != VTM::COPY)
    return 0;

  SmallVector<MachineOperand, 2> Ops;
  while (MI->getNumOperands()) {
    unsigned LastOp = MI->getNumOperands() - 1;
    Ops.push_back(MI->getOperand(LastOp));
    MI->RemoveOperand(LastOp);
  }

  MachineBasicBlock::iterator InsertPos = MI;
  InsertPos = VInstrInfo::BuildConditionnalMove(*MI->getParent(), InsertPos,
                                                Ops[1], Pred, Ops[0]);
  MI->eraseFromParent();

  return InsertPos;
}

bool VInstrInfo::PredicateInstruction(MachineInstr *MI,
                                    const SmallVectorImpl<MachineOperand> &Pred)
                                    const {
  assert(Pred.size() == 1 && "Too much conditions!");
  // Can only have 1 predicate at the moment.
  if (!isPredicable(MI) || isPredicated(MI)) return false;

  // Can we get the predicate operand?
  MachineOperand *PredOp = getPredOperand(MI);
  if (PredOp == 0) return false;

  const MachineOperand &NewPred = Pred[0];
  PredOp->setReg(NewPred.getReg());
  PredOp->setTargetFlags(NewPred.getTargetFlags());
  return true;
}

void VInstrInfo::MergeBranches(MachineBasicBlock *PredFBB,
                               SmallVectorImpl<MachineOperand> &Pred,
                               MachineBasicBlock *&CndTBB,
                               MachineBasicBlock *&CndFBB,
                               SmallVectorImpl<MachineOperand> &Cnd,
                               const TargetInstrInfo *TII) {
  assert(Pred.size() <= 1 && "Too many predicate operand!");
  // Nothing to merge, became:
  // br Cnd, CndTBB, CndFBB.
  if (PredFBB == 0) {
    assert(Pred.empty() && "Expected unconditional branch to PredTBB!");
  } else if (Cnd.empty()) {
    // Unconditional branch to CndTBB, becomes:
    // br Pred, CndTBB, PredFBB.
    assert(CndFBB == 0 && "Expected unconditional branch!");
    Cnd.push_back(Pred.front());
    CndFBB = PredFBB;
  } else {
    // Now we have:
    // if (!Pred) br PredFBB,
    // else if (Pred & Cnd) br CndTBB
    // else if (Pred & !Cnd) br CndFBB
    // But PredFBB may equals to CndTBB, otherwise PredFBB equals to CndFBB.
    // Make sure PredFBB == CndFBB so we have:
    // br (Pred & Cnd), CndTBB, CndFBB(PredFBB)
    // Because (Pred & !Cnd) | (!Pred) = !(Pred & Cnd)
    if (PredFBB != CndFBB) {
      TII->ReverseBranchCondition(Cnd);
      std::swap(CndTBB, CndFBB);
    }
    assert(PredFBB == CndFBB && "We have 3 difference BB?");

    Cnd.push_back(Pred.front());
  }

  // Always branch to the same BB.
  if (CndTBB == CndFBB) {
    Cnd.clear();
    CndFBB = 0;
  }
}

bool VInstrInfo::isAlwaysTruePred(MachineOperand &MO){
  assert(MO.isReg() && "Unexpected MO type!");
  if (MO.getReg() == 0) {
    assert(!cast<ucOperand>(MO).isPredicateInverted()&&"Illegal always false!");
    return true;
  }

  return false;
}

static MachineOperand RemoveInvertFlag(MachineOperand MO, MachineRegisterInfo *MRI,
                                       MachineBasicBlock &MBB,
                                       MachineBasicBlock::iterator IP,
                                       const TargetInstrInfo *TII) {
  assert(!VInstrInfo::isAlwaysTruePred(MO) && "Unexpected always false!");
  ucOperand Op(MO);

  if (Op.isPredicateInverted()) {
    Op.clearParent();
    // Remove the invert flag.
    Op.setBitWidth(1);
    // Build the not instruction.
    unsigned DstReg = MRI->createVirtualRegister(VTM::DRRegisterClass);
    ucOperand Dst = MachineOperand::CreateReg(DstReg, true);
    Dst.setBitWidth(1);
    BuildMI(MBB, IP, DebugLoc(), TII->get(VTM::VOpNot))
      .addOperand(Dst).addOperand(Op)
      .addOperand(ucOperand::CreatePredicate())
      .addOperand(ucOperand::CreateTrace(&MBB));
    Dst.setIsDef(false);
    return Dst;
  }

  return Op;
}

MachineOperand VInstrInfo::MergePred(MachineOperand OldCnd,
                                     MachineOperand NewCnd,
                                     MachineBasicBlock &MBB,
                                     MachineBasicBlock::iterator IP,
                                     MachineRegisterInfo *MRI,
                                     const TargetInstrInfo *TII,
                                     unsigned MergeOpC) {
  if (isAlwaysTruePred(OldCnd)) {
    OldCnd = MachineOperand::CreateImm(1);
    OldCnd.setTargetFlags(1);
  } else {
    OldCnd.clearParent();
    MRI->clearKillFlags(OldCnd.getReg());
    OldCnd = RemoveInvertFlag(OldCnd, MRI, MBB, IP, TII);
  }

  if (isAlwaysTruePred(NewCnd)) {
    NewCnd = MachineOperand::CreateImm(1);
    NewCnd.setTargetFlags(1);
  } else {
    NewCnd.clearParent();
    MRI->clearKillFlags(NewCnd.getReg());
    NewCnd = RemoveInvertFlag(NewCnd, MRI, MBB, IP, TII);
  }

  unsigned DstReg = MRI->createVirtualRegister(VTM::DRRegisterClass);
  ucOperand Dst = MachineOperand::CreateReg(DstReg, true);
  Dst.setBitWidth(1);

  BuildMI(MBB, IP, DebugLoc(), TII->get(MergeOpC))
    .addOperand(Dst).addOperand(NewCnd).addOperand(OldCnd)
    .addOperand(ucOperand::CreatePredicate())
    .addOperand(ucOperand::CreateTrace(&MBB));
  Dst.setIsDef(false);
  return Dst;
}

MachineInstr &VInstrInfo::BuildSelect(MachineBasicBlock *MBB,
                                      MachineOperand &Result,
                                      MachineOperand Pred,
                                      MachineOperand IfTrueVal,
                                      MachineOperand IfFalseVal) {
  assert(!isAlwaysTruePred(Pred) && "Selection op with always true condition?");
  // create the result register if necessary.
  if (!Result.getReg()) {
    MachineRegisterInfo &MRI = MBB->getParent()->getRegInfo();
    const TargetRegisterClass *RC = MRI.getRegClass(IfTrueVal.getReg());
    assert(MRI.getRegClass(IfFalseVal.getReg()) == RC
      && "Register class dose not match!");
    Result.setReg(MRI.createVirtualRegister(RC));
  }

  MachineOperand ResDef(Result);
  ResDef.setIsDef();

  // Build and insert the select instruction at the end of the BB.
  return *BuildMI(*MBB, MBB->getFirstTerminator(), DebugLoc(),
                  getDesc(VTM::VOpSel))
            .addOperand(ResDef).addOperand(Pred)
            .addOperand(IfTrueVal).addOperand(IfFalseVal)
            .addOperand(ucOperand::CreatePredicate())
            .addOperand(ucOperand::CreateTrace(MBB));
}

MachineInstr &VInstrInfo::BuildSelect(MachineBasicBlock *MBB, MachineOperand &Result,
                                      const SmallVectorImpl<MachineOperand> &Pred,
                                      MachineOperand IfTrueVal,
                                      MachineOperand IfFalseVal){
  // Build and insert the select instruction at the end of the BB.
  assert(Pred.size() == 1 && "Cannot select value!");
  return BuildSelect(MBB, Result, Pred[0], IfTrueVal, IfFalseVal);
}

MachineInstr &
VInstrInfo::BuildConditionnalMove(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator IP,
                                  MachineOperand &Res,
                                  const SmallVectorImpl<MachineOperand> &Pred,
                                  MachineOperand IfTrueVal) {
  if (!Res.getReg()) {
    MachineRegisterInfo &MRI = MBB.getParent()->getRegInfo();
    const TargetRegisterClass *RC = MRI.getRegClass(IfTrueVal.getReg());
    Res.setReg(MRI.createVirtualRegister(RC));
  }

  MachineOperand ResDef(Res);
  ResDef.setIsDef();

  return *BuildMI(MBB, IP, DebugLoc(), getDesc(VTM::VOpMove))
            .addOperand(ResDef).addOperand(IfTrueVal).addOperand(Pred[0]);
}

// Add Source to PHINode, if PHINod only have 1 source value, replace the PHI by
// a copy, adjust and return true
static bool AddSrcValToPHI(MachineOperand SrcVal, MachineBasicBlock *SrcBB,
                           MachineInstr *PN, MachineRegisterInfo &MRI) {
    if (PN->getNumOperands() != 1) {
      PN->addOperand(SrcVal);
      PN->addOperand(MachineOperand::CreateMBB(SrcBB));
      return false;
    }

    // A redundant PHI have only 1 incoming value after SrcVal added.
    MRI.replaceRegWith(PN->getOperand(0).getReg(), SrcVal.getReg());
    PN->eraseFromParent();
    return true;
}

void
VInstrInfo::mergePHISrc(MachineBasicBlock *Succ, MachineBasicBlock *FromBB,
                        MachineBasicBlock *ToBB, MachineRegisterInfo &MRI,
                        const SmallVectorImpl<MachineOperand> &FromBBCnd) {
  SmallVector<std::pair<MachineOperand, MachineBasicBlock*>, 2> SrcVals;
  SmallVector<MachineInstr*, 8> PHIs;

  // Fix up any PHI nodes in the successor.
  for (MachineBasicBlock::iterator MI = Succ->begin(), ME = Succ->end();
       MI != ME && MI->isPHI(); ++MI)
    PHIs.push_back(MI);

  while (!PHIs.empty()) {
    MachineInstr *MI = PHIs.pop_back_val();
    unsigned Idx = 1;
    while (Idx < MI->getNumOperands()) {
      MachineBasicBlock *SrcBB = MI->getOperand(Idx + 1).getMBB();
      if (SrcBB != FromBB && SrcBB != ToBB ) {
        Idx += 2;
        continue;
      }
      // Take the operand away.
      SrcVals.push_back(std::make_pair(MI->getOperand(Idx), SrcBB));
      MI->RemoveOperand(Idx);
      MI->RemoveOperand(Idx);
    }

    // If only 1 value comes from BB, re-add it to the PHI.
    if (SrcVals.size() == 1) {
      AddSrcValToPHI(SrcVals.pop_back_val().first, ToBB, MI, MRI);
      continue;
    }

    assert(SrcVals.size() == 2 && "Too many edges!");

    // Read the same register?
    if (SrcVals[0].first.getReg() == SrcVals[1].first.getReg()) {
      SrcVals.pop_back();
      AddSrcValToPHI(SrcVals.pop_back_val().first, ToBB, MI, MRI);
      continue;
    }

    // Make sure value from FromBB in SrcVals[1].
    if (SrcVals.back().second != FromBB)
      std::swap(SrcVals[0], SrcVals[1]);

    assert(SrcVals.back().second == FromBB
      && "Cannot build select for value!");
    assert(!FromBBCnd.empty()
      && "Do not know how to select without condition!");
    // Merge the value with select instruction.
    MachineOperand Result = MachineOperand::CreateReg(0, false);
    Result.setTargetFlags(MI->getOperand(0).getTargetFlags());
    MachineOperand FromBBIncomingVal = SrcVals.pop_back_val().first;
    MachineOperand ToBBIncomingVal = SrcVals.pop_back_val().first;
    VInstrInfo::BuildSelect(ToBB, Result, FromBBCnd,
                            FromBBIncomingVal, // Value from FromBB
                            ToBBIncomingVal // Value from ToBB
                            );
    AddSrcValToPHI(Result, ToBB, MI, MRI);
  }
}

bool VInstrInfo::isPrebound(unsigned Opcode) {
  return Opcode == VTM::VOpMemTrans || Opcode == VTM::VOpBRam
         || Opcode == VTM::VOpInternalCall;
}

bool VInstrInfo::isCopyLike(unsigned Opcode) {
  return Opcode == VTM::COPY
         || Opcode == VTM::PHI
         || Opcode == VTM::VOpMove
         || Opcode == VTM::VOpMoveArg
         || Opcode == VTM::VOpDstMux
         || Opcode == VTM::VOpReadReturn
         || Opcode == VTM::VOpReadFU
         || Opcode == VTM::VOpMvPhi;
}

bool VInstrInfo::isBrCndLike(unsigned Opcode) {
  return Opcode == VTM::VOpToState
         || Opcode == VTM::VOpToStateb
         || Opcode == VTM::VOpToState_nt;
}

bool VInstrInfo::isWriteUntilFinish(unsigned OpC) {
  const MCInstrDesc &TID = getDesc(OpC);
  return (TID.TSFlags & (WriteUntilFinishMask << WriteUntilFinishShiftAmount))
         || VInstrInfo::isCopyLike(OpC);
}

bool VInstrInfo::isDatapath(unsigned OpC) {
  // All pseudo instructions are control operations.
  return //OpC > TargetOpcode::COPY Not need because the bit is clean by default
         getDesc(OpC).TSFlags & (DatapathMask << DatapathShiftAmount);
}

VFUs::FUTypes VInstrInfo::getFUType(unsigned OpC) {
  return (VFUs::FUTypes)
    ((getDesc(OpC).TSFlags >> ResTypeShiftAmount) & ResTypeMask);
}

bool VInstrInfo::isReadAtEmit(unsigned OpC) {
  return (getDesc(OpC).TSFlags & (ReadAtEmitMask << ReadAtEmitShiftAmount))
         || isCopyLike(OpC);
}

static unsigned ComputeOperandSizeInByteLog2Ceil(unsigned SizeInBits) {
  return std::max(Log2_32_Ceil(SizeInBits), 3u) - 3;
}

template<int Idx>
static float LookupLatency(const float *Table, const MachineInstr *MI){
  unsigned SizeInBits = cast<ucOperand>(MI->getOperand(Idx)).getBitWidth();

  unsigned i = ComputeOperandSizeInByteLog2Ceil(SizeInBits);
  float latency = Table[i];
  unsigned SizeRoundUpToByteInBits = 8 << i;
  // Scale the latency accoriding to the actually width.
  latency = latency / float(SizeRoundUpToByteInBits) * float(SizeInBits);
  return latency;
}

float VInstrInfo::getOperandLatency(const MachineInstr *MI, unsigned MOIdx) {
  unsigned OpCode = MI->getOpcode();

  switch (OpCode) {
  case VTM::VOpDstMux:
    // Get the prebound mux size.
    return VFUs::getMuxLatency(MI->getOperand(3).getImm());
  }

  return 0.0f;
}

FuncUnitId VInstrInfo::getPreboundFUId(const MachineInstr *MI) {
  // Dirty Hack: Bind all memory access to channel 0 at this moment.
  switch(MI->getOpcode()) {
  case VTM::VOpDisableFU:
    return FuncUnitId(uint16_t(MI->getOperand(1).getImm()));
  case VTM::VOpReadFU:
    return FuncUnitId(uint16_t(MI->getOperand(2).getImm()));
  case VTM::VOpMemTrans:
    return FuncUnitId(VFUs::MemoryBus, 0);
  case VTM::VOpBRam: {
    unsigned Id = MI->getOperand(5).getImm();
    return FuncUnitId(VFUs::BRam, Id);
  }
  case VTM::VOpInternalCall: {
    unsigned Id = MI->getOperand(1).getTargetFlags();
    return FuncUnitId(VFUs::CalleeFN, Id);
  }
  case VTM::VOpDstMux: {
    unsigned Id = MI->getOperand(2).getImm();
    return FuncUnitId(VFUs::Mux, Id);
  }
  default:
    return FuncUnitId();
  }
}

bool VInstrInfo::mayLoad(const MachineInstr *MI) {
  switch (MI->getOpcode()) {
  default: return false;
    // There is a "isLoad" flag in memory access operation.
  case VTM::VOpMemTrans: return !MI->getOperand(3).getImm();
  }
}

bool VInstrInfo::mayStore(const MachineInstr *MI) {
  switch (MI->getOpcode()) {
  default: return false;
    // There is a "isLoad" flag in memory access operation.
  case VTM::VOpMemTrans: return MI->getOperand(3).getImm();
  }
}

BitWidthAnnotator::BitWidthAnnotator(MachineInstr &MI)
  : MO(&MI.getOperand(MI.getNumOperands() - 2)) {
  assert(hasBitWidthInfo() && "Bitwidth not available!");
  BitWidths = MO->getImm();
}

void BitWidthAnnotator::updateBitWidth() {
  assert(MO && hasBitWidthInfo() && "Cannot update bit width!");
  MO->setImm(BitWidths);
}

bool BitWidthAnnotator::hasBitWidthInfo() const {
  assert(MO && "MachineOperand not available!");
  return MO->isImm();
}

void BitWidthAnnotator::changeToDefaultPred() {
  MachineInstr *Parent = MO->getParent();
  unsigned MOIdx = MO - &Parent->getOperand(0);
  unsigned PredIdx = Parent->getDesc().NumOperands - 2;
  if (MOIdx != PredIdx) {
    MachineOperand PredMO[2] = {*(MO + 1), *MO};
    Parent->RemoveOperand(MOIdx);
    Parent->RemoveOperand(MOIdx);

    SmallVector<MachineOperand, 8> Ops;
    while(--MOIdx != 0) {
      Ops.push_back(Parent->getOperand(MOIdx));
      Parent->RemoveOperand(MOIdx);
    }

    // Insert the predicate operand to the operand list.
    Ops.insert(Ops.begin() + (Ops.size() - PredIdx) + 1,
               PredMO, array_endof(PredMO));

    while (!Ops.empty())
      Parent->addOperand(Ops.pop_back_val());
    MO = &Parent->getOperand(PredIdx);
  }

  MO->ChangeToRegister(0, false);
  MO->setTargetFlags(1);
  // Dirty Hack: Also fix the trace number.
  ++MO;
  MO->setTargetFlags(4);
}

const MachineInstr *const DetialLatencyInfo::EntryMarker =
  reinterpret_cast<const MachineInstr *const>(-1);

typedef DetialLatencyInfo::DepLatInfoTy DepLatInfoTy;
typedef DepLatInfoTy::mapped_type LatInfoTy;

static void updateLatency(DepLatInfoTy &CurLatInfo, const MachineInstr *SrcMI,
                          float MSBLatency, float LSBLatency) {
  // Latency from a control operation is simply the latency of the control
  // operation.
  // We may have dependency like:
  //  other op
  //    |   \
  //    |   other op
  //    |   /
  // current op
  // We should update the latency if we get a bigger latency.
  DepLatInfoTy::mapped_type &V = CurLatInfo[SrcMI];
  float &OldLSBLatency = V.second;
  OldLSBLatency = std::max(OldLSBLatency, LSBLatency);
  //assert(LSBLatency <= MSBLatency && "Broken latency pair!");
  float &OldMSBLatency = V.first;
  OldMSBLatency = std::max(OldMSBLatency, MSBLatency);
}

static LatInfoTy getLSB2MSBLatency(float SrcMSBLatency, float SrcLSBLatency,
                                   float TotalLatency, float PerBitLatency) {
  float MSBLatency = std::max(TotalLatency + SrcLSBLatency,
                              PerBitLatency + SrcMSBLatency);
  float LSBLatency = PerBitLatency + SrcLSBLatency;
  return std::make_pair(MSBLatency, LSBLatency);
}

static LatInfoTy getMSB2LSBLatency(float SrcMSBLatency, float SrcLSBLatency,
                                   float TotalLatency, float PerBitLatency) {
  float MSBLatency = PerBitLatency + SrcMSBLatency;
  float LSBLatency = std::max(PerBitLatency + SrcLSBLatency,
                              TotalLatency + SrcMSBLatency);
  return std::make_pair(MSBLatency, LSBLatency);
}

static LatInfoTy getWorstLatency(float SrcMSBLatency, float SrcLSBLatency,
                                 float TotalLatency, float /*PerBitLatency*/) {
  float MSBLatency = TotalLatency + SrcMSBLatency;
  float LSBLatency = TotalLatency + SrcLSBLatency;
  float WorstLatency = std::max(MSBLatency, LSBLatency);
  return std::make_pair(WorstLatency, WorstLatency);
}

static LatInfoTy getParallelLatency(float SrcMSBLatency, float SrcLSBLatency,
                                    float TotalLatency, float /*PerBitLatency*/) {
  float MSBLatency = TotalLatency + SrcMSBLatency;
  float LSBLatency = TotalLatency + SrcLSBLatency;
  return std::make_pair(MSBLatency, LSBLatency);
}

namespace {
struct BitSliceLatencyFN {
  unsigned OperandSize, UB, LB;

  BitSliceLatencyFN(const MachineInstr *BitSliceOp)
    : OperandSize(cast<ucOperand>(BitSliceOp->getOperand(1)).getBitWidth()),
      UB(BitSliceOp->getOperand(2).getImm()),
      LB(BitSliceOp->getOperand(3).getImm()) {
    assert(BitSliceOp->getOpcode() == VTM::VOpBitSlice && "Not a bitslice!");
  }

  BitSliceLatencyFN(unsigned operandSize, unsigned ub)
    : OperandSize(operandSize), UB(ub), LB(0) {}

  LatInfoTy operator()(float SrcMSBLatency, float SrcLSBLatency,
                       float /*TotalLatency*/, float /*PerBitLatency*/) {
    return getBitSliceLatency(OperandSize, UB, LB, SrcMSBLatency, SrcLSBLatency);
  }

  static
  LatInfoTy getBitSliceLatency(unsigned OperandSize, unsigned UB, unsigned LB,
                               float SrcMSBLatency, float SrcLSBLatency) {
    assert(OperandSize && "Unexpected zero size operand!");
    // Time difference between MSB and LSB.
    float MSB2LSBDelta = SrcMSBLatency - SrcLSBLatency;
    float DeltaPerBit = MSB2LSBDelta / OperandSize;
    // Compute the latency of LSB/MSB by assuming the latency is increasing linear
    float MSBLatency = SrcLSBLatency + UB * DeltaPerBit,
          LSBLatency = SrcLSBLatency + LB * DeltaPerBit;
    return std::make_pair(MSBLatency, LSBLatency);
  }

  static float getBitSliceLatency(unsigned OperandSize, unsigned UB,
                                  float SrcMSBLatency) {
    assert(OperandSize && "Unexpected zero size operand!");
    // Compute the latency of MSB by assuming the latency is increasing linear
    float DeltaPerBit = SrcMSBLatency / OperandSize;
    return UB * DeltaPerBit;
  }
};
}

template<unsigned IdxStart, unsigned IdxEnd>
static unsigned countNumRegOperands(const MachineInstr *MI) {
  unsigned NumRegs = 0;
  for (unsigned i = IdxStart; i < IdxEnd; ++i)
    if (MI->getOperand(i).isReg() && MI->getOperand(i).getReg())
      ++NumRegs;

  return NumRegs;
}

static unsigned countNumRegOperands(const MachineInstr *MI) {
  unsigned NumRegs = 0;
  for (unsigned i = 0, e = MI->getNumOperands(); i < e; ++i)
    if (MI->getOperand(i).isReg() && MI->getOperand(i).getReg() &&
        !MI->getOperand(i).isDef())
      ++NumRegs;

  return NumRegs;
}

// Get the latency of a machineinstr in cycle ratio.
float DetialLatencyInfo::getDetialLatency(const MachineInstr *MI) {
  unsigned OpC = MI->getOpcode();

  switch (OpC) {
    // TODO: Bitrepeat.
  case VTM::VOpICmp_c:
  case VTM::VOpICmp:
    return LookupLatency<3>(VFUs::CmpLatencies, MI);
  // Retrieve the FU bit width from its operand bit width
  case VTM::VOpAdd_c:
  case VTM::VOpAdd:
    return LookupLatency<1>(VFUs::AdderLatencies, MI);

  case VTM::VOpMultLoHi_c:
  case VTM::VOpMult_c:
  case VTM::VOpMultLoHi:
  case VTM::VOpMult:
    return LookupLatency<0>(VFUs::MultLatencies, MI);

  case VTM::VOpSRA_c:
  case VTM::VOpSRL_c:
  case VTM::VOpSHL_c:
  case VTM::VOpSRA:
  case VTM::VOpSRL:
  case VTM::VOpSHL:
    return LookupLatency<0>(VFUs::ShiftLatencies, MI);

  case VTM::VOpMemTrans:    return VFUs::MemBusLatency;

  // Can be fitted into LUT.
  case VTM::VOpSel:         return VFUs::LutLatency;

  // Ignore the trivial logic operation latency at the moment.
  case VTM::VOpLUT:
  case VTM::VOpAnd:
  case VTM::VOpOr:
  case VTM::VOpXor:
  case VTM::VOpNot:         return VFUs::LutLatency;

  case VTM::VOpROr:
  case VTM::VOpRAnd:
  case VTM::VOpRXor:{
    unsigned size = cast<ucOperand>(MI->getOperand(1)).getBitWidth();
    return VFUs::getReductionLatency(size);
  }
  case VTM::VOpBRam:        return VFUs::BRamLatency;

  case VTM::VOpInternalCall:  return 1.0f;

  default:                  break;
  }

  return 0.0f;
}

float DetialLatencyInfo::computeLatencyFor(const MachineInstr *MI) {
  float TotalLatency = getDetialLatency(MI);
  // Remember the latency from all MI's dependence leaves.
  CachedLatencies.insert(std::make_pair(MI, TotalLatency));
  return TotalLatency;
}

static float adjustChainingLatency(float Latency, const MachineInstr *SrcInstr,
                                   const MachineInstr *DstInstr) {
  assert(DstInstr && SrcInstr && "Dst and Src Instr should not be null!");
  assert(SrcInstr != DstInstr && "Computing latency of self loop?");
  const MCInstrDesc &DstTID = DstInstr->getDesc();
  unsigned DstOpC = DstTID.getOpcode();
  const MCInstrDesc &SrcTID = SrcInstr->getDesc();
  unsigned SrcOpC = SrcTID.getOpcode();

  bool SrcWriteUntilFInish = VInstrInfo::isWriteUntilFinish(SrcOpC);
  bool DstReadAtEmit = VInstrInfo::isReadAtEmit(DstOpC);

  float Delta = DetialLatencyInfo::DeltaLatency;

  if (DstReadAtEmit && SrcWriteUntilFInish) {
    if (SrcOpC == VTM::VOpMvPhi) {
      assert((DstOpC == TargetOpcode::PHI || DstOpC == VTM::VOpMvPhi
              || VInstrInfo::getDesc(DstOpC).isTerminator())
             && "VOpMvPhi should only used by PHIs or terminators!!");
      // The latency from VOpMvPhi to PHI is exactly 0, because the VOpMvPhi is
      // simply identical to the PHI at next iteration.
      return 0.0f;
    } else
      // If the edge is reg->reg, the result is ready after the clock edge, add
      // a delta to make sure DstInstr not schedule to the moment right at the
      // SrcInstr finish
      return ceil(Latency) + Delta;
  }

  // If the value is written to register, it has a delta latency
  if (SrcWriteUntilFInish) return Latency + Delta;

  // Chain the operations if dst not read value at the edge of the clock.
  return std::max(0.0f, Latency - Delta);
}
template<typename FuncTy>
static void accumulateDatapathLatency(DepLatInfoTy &CurLatInfo,
                                      const DepLatInfoTy *SrcLatInfo,
                                      float SrcMSBLatency, float PerBitLatency,
                                      FuncTy F) {
  typedef DepLatInfoTy::const_iterator src_it;
  // Compute minimal delay for all possible pathes.
  for (src_it I = SrcLatInfo->begin(), E = SrcLatInfo->end(); I != E; ++I) {
    float MSBLatency, LSBLatency;
    tie(MSBLatency, LSBLatency) = F(I->second.first, I->second.second,
                                    SrcMSBLatency, PerBitLatency);
    updateLatency(CurLatInfo, I->first, MSBLatency, LSBLatency);
  }
}

template<bool IsCtrlDep>
bool DetialLatencyInfo::buildDepLatInfo(const MachineInstr *SrcMI,
                                        const MachineInstr *DstMI,// Not needed
                                        DepLatInfoTy &CurLatInfo,
                                        unsigned OperandWidth,
                                        float OperandDelay) {
  const DepLatInfoTy *SrcLatInfo = getDepLatInfo(SrcMI);
  // Latency information not available, the SrcMI maybe in others BB, no need
  // to compute cross BB latency.
  if (SrcLatInfo == 0) return false;

  float SrcMSBLatency = getCachedLatencyResult(SrcMI);
  if (!IsCtrlDep) {
    SrcMSBLatency = adjustChainingLatency(SrcMSBLatency, SrcMI, DstMI);
    if (OperandWidth) {
      unsigned SrcSize = cast<ucOperand>(SrcMI->getOperand(0)).getBitWidth();
      // DirtyHack: Ignore the invert flag.
      if (OperandWidth != SrcSize && SrcSize != 1 && OperandWidth != 3) {
        assert(OperandWidth < SrcSize && "Bad implicit bitslice!");
        SrcMSBLatency =
          BitSliceLatencyFN::getBitSliceLatency(SrcSize, OperandWidth,
                                                SrcMSBLatency);
      }
    }
  } else // IsCtrlDep
    SrcMSBLatency = std::max(0.0f, SrcMSBLatency - DetialLatencyInfo::DeltaLatency);

  // Try to compute the per-bit latency.
  float PerBitLatency = 0.0f;
  if (OperandWidth)
    PerBitLatency = std::max(SrcMSBLatency / OperandWidth, VFUs::LutLatency);

  unsigned Opcode = SrcMI->getOpcode();
  switch (Opcode) {
  default:
    if (VInstrInfo::isDatapath(Opcode))
      accumulateDatapathLatency(CurLatInfo, SrcLatInfo, SrcMSBLatency,
                                PerBitLatency, getWorstLatency);
    else
      updateLatency(CurLatInfo, SrcMI, SrcMSBLatency, SrcMSBLatency);
    break;
    // Result bits are computed from LSB to MSB.
  case VTM::VOpAdd_c:
  case VTM::VOpMultLoHi_c:
  case VTM::VOpMult_c:
    accumulateDatapathLatency(CurLatInfo, SrcLatInfo, SrcMSBLatency,
                              PerBitLatency, getLSB2MSBLatency);
    break;
  case VTM::VOpAdd:
  case VTM::VOpMult:
  case VTM::VOpMultLoHi:
    updateLatency(CurLatInfo, SrcMI, SrcMSBLatency, PerBitLatency);
    break;
    // Each bits are compute independently.
  case VTM::VOpLUT:
  case VTM::VOpAnd:
  case VTM::VOpOr:
  case VTM::VOpXor:
  case VTM::VOpNot:
  case VTM::VOpBitCat:
    accumulateDatapathLatency(CurLatInfo, SrcLatInfo, SrcMSBLatency,
                              PerBitLatency, getParallelLatency);
    break;
  case VTM::VOpBitSlice:
    accumulateDatapathLatency(CurLatInfo, SrcLatInfo, SrcMSBLatency,
                              PerBitLatency, BitSliceLatencyFN(SrcMI));
    break;
  case VTM::VOpICmp_c:
    accumulateDatapathLatency(CurLatInfo, SrcLatInfo, SrcMSBLatency,
                              PerBitLatency, getMSB2LSBLatency);
    break;
  case VTM::VOpICmp:
    // Result bits are computed from MSB to LSB.
    updateLatency(CurLatInfo, SrcMI, PerBitLatency, SrcMSBLatency);
    break;
  }

  return true;
}

const DetialLatencyInfo::DepLatInfoTy &
DetialLatencyInfo::addInstrInternal(const MachineInstr *MI, bool IgnorePHISrc) {
  DepLatInfoTy &CurLatInfo = LatencyMap[MI];

  const MCInstrDesc &TID = MI->getDesc();
  // Iterate from use to define.
  for (unsigned i = 0, e = MI->getNumOperands(); i != e; ++i) {
    const ucOperand &MO = cast<ucOperand>(MI->getOperand(i));

    // Only care about a use register.
    if (!MO.isReg() || MO.isDef() || MO.getReg() == 0)
      continue;

    unsigned SrcReg = MO.getReg();
    MachineInstr *SrcMI = MRI.getVRegDef(SrcReg);
    assert(SrcMI && "Virtual register use without define!");

    // Do we ignore phi as dependence?
    if (SrcMI->isPHI() && IgnorePHISrc) continue;

    unsigned OpSize = MO.getBitWidth();
    float OpDelay = 0.0f;
    if (i < TID.getNumOperands() && TID.OpInfo[i].isPredicate()) {
      OpDelay = VFUs::ClkEnSelLatency;
    } else
      OpDelay = VInstrInfo::getOperandLatency(MI, i);

    if (buildDepLatInfo<false>(SrcMI, MI, CurLatInfo, OpSize, OpDelay))
      // If we build the Latency Info for SrcMI sucessfully, that means SrcMI
      // have user now.
      ExitMIs.erase(SrcMI);
  }

  // Align the compute the latency of MI.
  float Latency = computeLatencyFor(MI);

  // Assume MI do not have any user in the same BB, if it has, it will be
  // deleted later.
  ExitMIs.insert(MI);
  // We will not get any latency information if a datapath operation do not
  // depends any control operation in the same BB
  // Dirty Hack: Use a marker machine instruction to mark it depend on entry of
  // the BB.
  if (CurLatInfo.empty() && VInstrInfo::isDatapath(MI->getOpcode())) {
    float latency = std::max(Latency, DetialLatencyInfo::DeltaLatency);
    CurLatInfo.insert(std::make_pair(EntryMarker,
                                     std::make_pair(latency, latency)));
  }

  return CurLatInfo;
}

void DetialLatencyInfo::buildExitMIInfo(const MachineInstr *ExitMI,
                                        DepLatInfoTy &Info) {
  typedef std::set<const MachineInstr*>::const_iterator exit_it;
  for (exit_it I = ExitMIs.begin(), E = ExitMIs.end(); I != E; ++I)
    buildDepLatInfo<true>(*I, ExitMI, Info, 0, 0.0);
}

const float DetialLatencyInfo::DeltaLatency = FLT_EPSILON * 8.0f;

unsigned DetialLatencyInfo::getStepsFromEntry(const MachineInstr *DstInstr) {
  assert(DstInstr && "DstInstr should not be null!");
  const MCInstrDesc &DstTID = DstInstr->getDesc();
  unsigned DstOpC = DstTID.getOpcode();

  //// Set latency of Control operation and entry root to 1, so we can prevent
  //// scheduling control operation to the first slot.
  //// Do not worry about PHI Nodes, their will be eliminated at the register
  //// allocation pass.
  if (DstInstr->getOpcode() == VTM::PHI) return 0;

  // Schedule datapath operation right after the first control slot.
  if (VInstrInfo::isDatapath(DstOpC)) return 0;

  // Do not schedule function unit operation to the first state at the moment
  // there may be potential resource conflict.
  if (countNumRegOperands(DstInstr)) return 1;

  return 0;
}


float DetialLatencyInfo::getChainingLatency(const MachineInstr *SrcInstr,
                                            const MachineInstr *DstInstr) const{
  // Compute the latency correspond to detail slot.
  float latency = getMaxLatency(SrcInstr);
  return adjustChainingLatency(latency, SrcInstr, DstInstr);
}

unsigned CycleLatencyInfo::computeLatency(MachineBasicBlock &MBB, bool Reset) {
  if (Reset) reset();

  unsigned TotalLatency = 1;
  for (MachineBasicBlock::iterator I = MBB.begin(), E = MBB.end(); I != E; ++I){
    MachineInstr *MI = I;

    FuncUnitId FU = VInstrInfo::getPreboundFUId(MI);
    bool hasPrebindFU = FU.isBound();

    unsigned L = 0;

    const DepLatInfoTy &LatInfo = addInstrInternal(MI, false);
    typedef DepLatInfoTy::const_iterator dep_it;
    for (dep_it I = LatInfo.begin(), E = LatInfo.end(); I != E; ++I) {
      const MachineInstr *DepMI = I->first;
      // Get the delay (from entry) of DepMI.
      DepLatencyMap::iterator at = DepInfo.find(DepMI);
      assert(at != DepInfo.end() && "Dependence missed?");
      // Compute the delay of thie MI by accumulating the edge delay to the
      // delay of DepMI.
      L = at->second + std::ceil(I->second.first);
      if (DepMI == DetialLatencyInfo::EntryMarker) DepMI = 0;
      L = std::max(L, getCtrlStepBetween<true>(DepMI, MI));
    }

    if (hasPrebindFU) L = updateFULatency(FU.getData(), L, MI);

    // Remember the delay (from entry) of this MI.
    DepInfo[MI] = L;

    TotalLatency = std::max(TotalLatency, L);
  }

  return TotalLatency;
}

unsigned CycleLatencyInfo::updateFULatency(unsigned FUId, unsigned Latency,
                                           MachineInstr *MI) {
  std::pair<const MachineInstr*, unsigned> &LI = FUInfo[FUId];
  unsigned &FULatency = LI.second;
  const MachineInstr *&LastMI = LI.first;
  FuncUnitId ID(FUId);

  unsigned EdgeLatency = getCtrlStepBetween<false>(LastMI, MI);

  // Update the FU latency information.
  FULatency = std::max(FULatency + EdgeLatency, Latency);
  LastMI = MI;
  return FULatency;
}
