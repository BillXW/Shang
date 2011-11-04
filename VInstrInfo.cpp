//===---------- VInstrInfo.cpp - VTM Instruction Information -----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the VTM implementation of the TargetInstrInfo class.
//
//===----------------------------------------------------------------------===//

#include "VTargetMachine.h"

#include "vtm/VFInfo.h"
#include "vtm/VInstrInfo.h"
#include "vtm/VTM.h"
#include "vtm/MicroState.h"

#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Debug.h"

#include "VGenInstrInfo.inc"
using namespace llvm;

const MachineOperand *VInstrInfo::getPredOperand(const MachineInstr *MI) {
  if (MI->getOpcode() <= VTM::COPY) return 0;

  unsigned Idx = MI->getDesc().NumOperands - 2;
  assert(MI->getDesc().OpInfo[Idx].isPredicate() && "Cannot get PredOperand!");
  return &MI->getOperand(Idx);
}

MachineOperand *VInstrInfo::getPredOperand(MachineInstr *MI) {
  return const_cast<MachineOperand*>(getPredOperand((const MachineInstr*)MI));
}

VInstrInfo::VInstrInfo(const TargetData &TD, const TargetLowering &TLI)
  : TargetInstrInfoImpl(VTMInsts, array_lengthof(VTMInsts)), RI(*this, TD, TLI){
}


bool VInstrInfo::isReallyTriviallyReMaterializable(const MachineInstr *MI,
                                                   AliasAnalysis *AA) const {
  VIDesc Desc(MI->getDesc());
  return !Desc->isBarrier() && Desc.hasTrivialFU();
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

bool VInstrInfo::isUnpredicatedTerminator(const MachineInstr *MI) const{
  const TargetInstrDesc &TID = MI->getDesc();
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
    }
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

bool VInstrInfo::extractJumpTable(MachineBasicBlock &BB, JT &Table) {
  for (MachineBasicBlock::iterator I = BB.getFirstTerminator(), E = BB.end();
       I != E; ++I) {
    // We can only handle conditional jump.
    if (!VInstrInfo::isBrCndLike(I->getOpcode())) return true;

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

  for (JT::iterator I = Table.begin(), E = Table.end(); I != E; ++I) {
    BuildMI(&BB, dl, VTMInsts[VTM::VOpToStateb])
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
  } else
    OldCnd = RemoveInvertFlag(OldCnd, MRI, MBB, IP, TII);

  if (isAlwaysTruePred(NewCnd)) {
    NewCnd = MachineOperand::CreateImm(1);
    NewCnd.setTargetFlags(1);
  } else
    NewCnd = RemoveInvertFlag(NewCnd, MRI, MBB, IP, TII);

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

unsigned VInstrInfo::createPHIIncomingReg(unsigned DestReg,
                                          MachineRegisterInfo *MRI) const {
  const TargetRegisterClass *PHIRC = VTM::PHIRRegisterClass;
  return MRI->createVirtualRegister(PHIRC);
}

typedef MachineBasicBlock::iterator mbb_it;

MachineInstr *VInstrInfo::insertPHIImpDef(MachineBasicBlock &MBB,
                                          mbb_it InsertPos,
                                          MachineInstr *PN) const {
  return TargetInstrInfo::insertPHIImpDef(MBB, InsertPos, PN);
}

MachineInstr *VInstrInfo::insertPHIIcomingCopy(MachineBasicBlock &MBB,
                                               mbb_it InsertPos,
                                               MachineInstr *PN,
                                               unsigned IncomingReg) const {
  ucOperand &DefOp = cast<ucOperand>(PN->getOperand(0));
  // Skip all implicit defines at the beginning of the MBB.
  while (InsertPos->isImplicitDef())
    ++InsertPos;

  ucState Ctrl(InsertPos);
  assert(Ctrl->getOpcode() == VTM::Control && "Unexpected instruction type!");
  // Simply build the copy in the first control slot.
  MachineInstrBuilder Builder(InsertPos);
  Builder.addOperand(ucOperand::CreateOpcode(VTM::VOpDefPhi, Ctrl.getSlot()));
  Builder.addOperand(ucOperand::CreatePredicate());
  // Not need to insert trace for scheduled MBB.
  // Builder.addOperand(ucOperand::CreateTrace(&MBB));
  ucOperand Dst = MachineOperand::CreateReg(DefOp.getReg(), true);
  Dst.setBitWidth(DefOp.getBitWidth());
  Dst.setIsWire(DefOp.isWire());
  Builder.addOperand(Dst);
  ucOperand Src = MachineOperand::CreateReg(IncomingReg, false);
  Src.setBitWidth(DefOp.getBitWidth());
  if (VRegisterInfo::IsWire(IncomingReg, &MBB.getParent()->getRegInfo()))
    Src.setIsWire();
  Builder.addOperand(Src);
  return &*Builder;
}

static MachineInstr *addOperandsToMI(MachineInstr *MI,
                                     SmallVectorImpl<MachineOperand> &Ops) {
  for (SmallVectorImpl<MachineOperand>::iterator I = Ops.begin(), E = Ops.end();
    I != E; ++I)
    MI->addOperand(*I);

  return MI;
}

MachineInstr *VInstrInfo::insertPHICopySrc(MachineBasicBlock &MBB,
                                           mbb_it InsertPos, MachineInstr *PN,
                                           unsigned IncomingReg,
                                           unsigned SrcReg, unsigned SrcSubReg)
                                           const {
  ucOperand &DefOp = cast<ucOperand>(PN->getOperand(0));
  // Get the last slot.
  while ((--InsertPos)->getOpcode() == VTM::IMPLICIT_DEF)
    ;

  VFInfo *VFI = MBB.getParent()->getInfo<VFInfo>();

  // Look up the slot of PHI and the parent MBB of PHI.
  int SSlot;
  const MachineBasicBlock *TargetBB;
  tie(SSlot, TargetBB) = VFI->lookupPHISlot(PN);

  bool isPipeline = SSlot < 0;
  unsigned Slot = isPipeline ? -SSlot : SSlot;

  unsigned StartSlot = VFI->getStartSlotFor(&MBB);
  unsigned EndSlot = VFI->getEndSlotFor(&MBB);
  // If the phi scheduled into this MBB, insert the copy to the right control
  // slot.
  //if (Slot > StartSlot && Slot <= EndSlot) {
  if (Slot <= StartSlot || Slot > EndSlot)
    // Else we are issuing the copy at the end of the BB.
    Slot = EndSlot;

  unsigned II = VFI->getIIFor(&MBB);
  unsigned ModuloSlot = (Slot - StartSlot) % II + StartSlot;
  // If modulo slot is 0, insert the copy in the last control slot.
  // Otherwise, iterate over the BB to find the match slot.
  if (ModuloSlot != StartSlot) {
    while(ucState(InsertPos).getSlot() != ModuloSlot) {
      --InsertPos; // Skip the current control slot.
      --InsertPos; // Skip the current datapath slot.
    }
  }

  ucState Ctrl(InsertPos);
  assert(Ctrl->getOpcode() == VTM::Control && "Unexpected instruction type!");
  SmallVector<MachineOperand, 8> Ops;
  MachineRegisterInfo &MRI = MBB.getParent()->getRegInfo();

  // Build the instruction in form of:
  // IncomingReg = Opc SrcReg, TargetMBB ...
  unsigned Opc = isPipeline ? VTM::VOpMvPipe : VTM::VOpMvPhi;
  Ops.push_back(ucOperand::CreateOpcode(Opc, Slot));
  Ops.push_back(ucOperand::CreatePredicate());
  // Not need to insert trace for scheduled MBB.
  // Builder.addOperand(ucOperand::CreateTrace(&MBB));
  MachineOperand &Pred = Ops.back();
  ucOperand Dst = MachineOperand::CreateReg(IncomingReg, true);
  Dst.setBitWidth(DefOp.getBitWidth());
  if (VRegisterInfo::IsWire(IncomingReg, &MRI))
    Dst.setIsWire();
  Ops.push_back(Dst);

  MachineRegisterInfo::def_iterator DI = MRI.def_begin(SrcReg);

  DEBUG(dbgs() << "Copying " << TargetRegisterInfo::virtReg2Index(SrcReg)
               << " in\n";
    ucState(*DI).dump();
  );

  ucOperand Src = MachineOperand::CreateReg(SrcReg, false);
  Src.setSubReg(SrcSubReg);
  Src.setBitWidth(DefOp.getBitWidth());
  if (VRegisterInfo::IsWire(SrcReg, &MRI))
    Src.setIsWire();

  // Try to forward the source value.
  if (DI->getOpcode() != VTM::PHI) {
    assert (++MRI.def_begin(SrcReg) == MRI.def_end() && "Not in SSA From!");
    ucOp WriteOp = ucOp::getParent(DI);

    // Do not copy the implicit define.
    if (WriteOp->getOpcode() == VTM::IMPLICIT_DEF)
      return InsertPos;

    // We need to forward the wire copy if the source register is written
    // in the same slot, otherwise we will read a out of date value.
    if (WriteOp->getParent() == &*Ctrl && WriteOp->getPredSlot() == Slot) {
      // Copy the predicate operand.
      MachineOperand &WritePred = WriteOp.getPredicate();
      Pred.ChangeToRegister(WritePred.getReg(), false);
      Pred.setTargetFlags(WritePred.getTargetFlags());

      // FIXME: Handle others case.
      if (isCopyLike(WriteOp->getOpcode())) {
        MachineOperand ForwardedVal = WriteOp.getOperand(1);
        Ops.push_back(ForwardedVal);
        MachineOperand TargetBBMO =
          MachineOperand::CreateMBB(const_cast<MachineBasicBlock*>(TargetBB));
        Ops.push_back(TargetBBMO);
        // Make sure the state reads this register, otherwise PHIElimination
        // will complain about that.
        Ops.push_back(Src);
        return addOperandsToMI(InsertPos, Ops);
      }
    }

    Ops.push_back(Src);
    MachineOperand TargetBBMO =
      MachineOperand::CreateMBB(const_cast<MachineBasicBlock*>(TargetBB));
    Ops.push_back(TargetBBMO);

    // Try to forward the imp-uses.
    if (Src.isWire())
      for (ucOp::op_iterator I = WriteOp.op_begin(), E = WriteOp.op_end(); I != E;
           ++I) {
          MachineOperand &Op = *I;
          if (Op.isReg() && Op.isImplicit()) Ops.push_back(Op);
      }

    return addOperandsToMI(InsertPos, Ops);
  }

  // Trivial case, just copy src to dst.
  Ops.push_back(Src);
  MachineOperand TargetBBMO =
    MachineOperand::CreateMBB(const_cast<MachineBasicBlock*>(TargetBB));
  Ops.push_back(TargetBBMO);

  return addOperandsToMI(InsertPos, Ops);
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
                  VTMInsts[VTM::VOpSel])
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

  return *BuildMI(MBB, IP, DebugLoc(), VTMInsts[VTM::VOpMove_rr])
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

bool VInstrInfo::isCopyLike(unsigned Opcode) {
  return Opcode == VTM::COPY
         || Opcode == VTM::VOpMove_ri
         || Opcode == VTM::VOpMove_rw
         || Opcode == VTM::VOpSel || Opcode == VTM::VOpCase
         || Opcode == VTM::VOpReadReturn
         || Opcode == VTM::VOpReadFU;
}

bool VInstrInfo::isBrCndLike(unsigned Opcode) {
  return Opcode == VTM::VOpToState
         || Opcode == VTM::VOpToStateb;
}

bool VInstrInfo::isWireOp(const TargetInstrDesc &TID) {
  VIDesc VID(TID);

  // When a instruction has datapath and only has trivial FU, they will not
  // share the FU with others instructions, and its result will only be written
  // once, this means we can simply put the value to a wire and do not need to
  // register the value.
  return VID->getOpcode() != VTM::VOpReadReturn
         && VID.hasDatapath() && VID.hasTrivialFU();
}

bool VInstrInfo::isCmdSeq(unsigned Cmd) {
  return Cmd >= VFUMemBus::CmdFirstNoLoadStore;
}

bool VInstrInfo::isInSameCmdSeq(const MachineInstr *PrevMI,
                                const MachineInstr *MI) {
  assert(MI->getOpcode() == VTM::VOpCmdSeq
         && PrevMI->getOpcode() == VTM::VOpCmdSeq
         && "Bad opcode!");
  assert(PrevMI->getOperand(3).getImm() == MI->getOperand(3).getImm()
         && "Bad command sequence!");
  return !isCmdSeqBegin(MI);
}

bool VInstrInfo::isCmdSeqBegin(const MachineInstr *MI) {
  return MI->getOpcode() == VTM::VOpCmdSeq
         && MI->getOperand(4).getImm() == VFUMemBus::SeqBegin;
}

bool VInstrInfo::isCmdSeqEnd(const MachineInstr *MI) {
  return MI->getOpcode() == VTM::VOpCmdSeq
         && MI->getOperand(4).getImm() == VFUMemBus::SeqEnd;
}

unsigned VInstrInfo::computeLatency(const MachineInstr *SrcInstr,
                                    const MachineInstr *DstInstr) {
  assert(DstInstr && "DstInstr should not be null!");
  assert(SrcInstr != DstInstr && "Computing latency of self loop?");
  VIDesc DstTID = *DstInstr;

  if (SrcInstr == 0) {
    //// Set latency of Control operation and entry root to 1, so we can prevent
    //// scheduling control operation to the first slot.
    //// Do not worry about PHI Nodes, their will be eliminated at the register
    //// allocation pass.
    if (DstInstr->getOpcode() == VTM::PHI)
      return 0;

    // 1 slot from control to datapath.
    if (DstTID.hasDatapath()) return 1;

    // Now DstInstr is control.
    if (DstTID.hasTrivialFU() && !DstTID->isTerminator() && !DstTID->isReturn()
        // Dirty Hack: Also do not schedule return value to the entry slot of
        // the state.
        && DstTID->getOpcode() != VTM::VOpRetVal)
      return 0;

    // Do not schedule function unit operation to the first state at the moment
    // there may be potential resource conflict.
    return 2;
  }

  VIDesc SrcTID = *SrcInstr;
  // Compute the latency correspond to detail slot.
  unsigned latency = SrcTID.getLatency() * 2;

  if (DstTID.isReadAtEmit()) {
    // If the edge is reg->reg, increase the latency by 1, because the result
    // is ready after the clock edge.
    if (SrcTID.isWriteUntilFinish())
      return latency + 1;

    // Else if the edge is wire->reg, decrease the latency by 1, becaue the
    // result is ready before the clock edge and let the register can
    // capture the result.
    if(latency > 0)
      latency -= 1;

    // If the Src and Dst have difference slot type, it needs 1 extra slot to
    // adjust the slot type.
    return latency + 1;
  }

  // When DstInst dose not read at emit, it hopefully a datapath operation.
  // If the Src and Dst have difference slot type, it needs 1 extra slot to
  // adjust the slot type.
  if (DstTID.hasDatapath() && !SrcTID.hasDatapath())
    return latency + 1;

  return latency;
}

FuncUnitId VIDesc::getPrebindFUId()  const {
  // Dirty Hack: Bind all memory access to channel 0 at this moment.
  switch(getTID().Opcode) {
  case VTM::VOpCmdSeq:
  case VTM::VOpMemTrans:
    return FuncUnitId(VFUs::MemoryBus, 0);
  case VTM::VOpBRam: {
    unsigned Id = get().getOperand(5).getImm();
    return FuncUnitId(VFUs::BRam, Id);
  }
  case VTM::VOpInternalCall: {
    unsigned Id = get().getOperand(1).getTargetFlags();
    return FuncUnitId(VFUs::CalleeFN, Id);
  }
  default:
    return FuncUnitId();
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

bool VIDesc::mayLoad() const {
  switch (getTID().Opcode) {
  default: return false;
  // There is a "isLoad" flag in memory access operation.
  case VTM::VOpMemTrans: return !get().getOperand(3).getImm();
  // Dirty Hack: Command sequence reads memory.
  case VTM::VOpCmdSeq:   return true;
  }
}

bool VIDesc::mayStore() const {
  switch (getTID().Opcode) {
  default: return false;
  // There is a "isLoad" flag in memory access operation.
  case VTM::VOpMemTrans: return get().getOperand(3).getImm();
  // Dirty Hack: Command sequence write memory.
  case VTM::VOpCmdSeq:   return true;
  }
}

bool VIDesc::canCopyBeFused() const {
  const MachineInstr &I = get();
  assert(I.isCopy() && "canCopyBeFused called on the wrong instruction!");
  if (I.getOperand(1).isImm()) return true;

  assert(I.getParent() && "Expected instruction embedded in machine function!");
  const MachineRegisterInfo &MRI = I.getParent()->getParent()->getRegInfo();
  unsigned DstReg = I.getOperand(0).getReg(),
           SrcReg = I.getOperand(1).getReg();

  // Later pass can not eliminate the non-trivial copy, so it should be fused.
  return MRI.getRegClass(DstReg) != MRI.getRegClass(SrcReg);
}

// Out of line virtual function to provide home for the class.
void VIDesc::anchor() {}
