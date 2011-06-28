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

static const unsigned MoveOpcodes[] = {
    0, //DRRegClassID = 0,
    0, //PHIRRegClassID = 1,
    VTM::VOpMove_ra, //RADDRegClassID = 2,
    VTM::VOpMove_rm, //RMULRegClassID = 3,
    VTM::VOpMove_rs, //RSHTRegClassID = 4,
    VTM::VOpMove_rw, //WireRegClassID = 5
};

const MachineOperand *VInstrInfo::getPredOperand(const MachineInstr *MI) {
  if (MI->getOpcode() <= VTM::COPY) return 0;

  unsigned Idx = MI->getDesc().NumOperands - 1;
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

    if (VInstrInfo::isBrCndLike(Inst->getOpcode()))
      Terms.push_back(Inst);
  }

  // Mixing branches and return?
  if (Terms.empty() || Terms.size() > 2) return true;

  MachineInstr *FstTerm = Terms[0];

  /// 2. If this block ends with only an unconditional branch, it sets TBB to be
  ///    the destination block.
  if (isUnpredicatedTerminator(FstTerm)) {
    TBB = FstTerm->getOperand(0).getMBB();
    assert(Terms.size() == 1 && "Expect single fall through edge!");
    return false;
  }

  Cond.push_back(*getPredOperand(FstTerm));
  Cond.back().setIsKill(false);
  TBB = FstTerm->getOperand(0).getMBB();
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
  FBB = SndTerm->getOperand(0).getMBB();
  return false;
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
    BuildMI(&MBB, DL, get(Opc)).addMBB(TBB).addOperand(PredOp);
    return 1;
  }

  // Two-way conditional branch.
  assert(PredOp.isReg() && PredOp.getReg() != 0
         && "Uncondtional predicate with true BB and false BB?");
  // Branch to true BB, with the no-barrier version.
  BuildMI(&MBB, DL, get(VTM::VOpToState)).addMBB(TBB).addOperand(PredOp);
  // Branch the false BB.
  ReversePredicateCondition(PredOp);
  BuildMI(&MBB, DL, get(VTM::VOpToStateb))
      .addMBB(TBB).addOperand(PredOp);
   return 2;
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
  return true; // DirtyHack: Everything is profitable.
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
  ucState Ctrl(InsertPos);
  assert(Ctrl->getOpcode() == VTM::Control && "Unexpected instruction type!");
  // Simply build the copy in the first control slot.
  MachineInstrBuilder Builder(InsertPos);
  Builder.addOperand(ucOperand::CreateOpcode(VTM::VOpDefPhi, Ctrl.getSlot()));
  Builder.addOperand(ucOperand::CreatePredicate());
  ucOperand Dst = MachineOperand::CreateReg(DefOp.getReg(), true);
  Dst.setBitWidth(DefOp.getBitWidth());
  Builder.addOperand(Dst);
  ucOperand Src = MachineOperand::CreateReg(IncomingReg, false);
  Src.setBitWidth(DefOp.getBitWidth());
  if (VRegisterInfo::IsWire(IncomingReg, &MBB.getParent()->getRegInfo()))
    Src.setIsWire();
  Builder.addOperand(Src);
  return &*Builder;
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
  unsigned Slot = VFI->lookupPHISlot(PN);
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

  MachineRegisterInfo &MRI = MBB.getParent()->getRegInfo();
  MachineInstrBuilder Builder(InsertPos);
  Builder.addOperand(ucOperand::CreateOpcode(VTM::VOpMvPhi, Slot));
  Builder.addOperand(ucOperand::CreatePredicate());
  ucOperand Dst = MachineOperand::CreateReg(IncomingReg, true);
  Dst.setBitWidth(DefOp.getBitWidth());
  if (VRegisterInfo::IsWire(DefOp.getReg(), &MRI))
    Dst.setIsWire();
  Builder.addOperand(Dst);

  MachineRegisterInfo::def_iterator DI = MRI.def_begin(SrcReg);

  DEBUG(dbgs() << "Copying " << TargetRegisterInfo::virtReg2Index(SrcReg)
               << " in\n";
    ucState(*DI).dump();
  );

  // Try to forward the source value.
  if (DI->getOpcode() != VTM::PHI) {
    assert (++MRI.def_begin(SrcReg) == MRI.def_end() && "Not in SSA From!");
    ucOp WriteOp = ucOp::getParent(DI);
    // We need to forward the wire copy if the source register is written
    // in the same slot, otherwise we will read a out of date value.
    if (WriteOp->getParent() == &*Ctrl && WriteOp->getPredSlot() == Slot) {
      // FIXME: Handle others case.
      assert(WriteOp.getPredicate().getReg() == 0
             && "Can not handle predicated ucop!");

      if (isCopyLike(WriteOp->getOpcode())) {
        MachineOperand ForwardedVal = WriteOp.getOperand(1);
        Builder.addOperand(ForwardedVal);
      }
    }
  }

  ucOperand Src = MachineOperand::CreateReg(SrcReg, false);
  Src.setSubReg(SrcSubReg);
  Src.setBitWidth(DefOp.getBitWidth());
  if (VRegisterInfo::IsWire(SrcReg, &MRI))
    Src.setIsWire();
  Builder.addOperand(Src);
  return &*Builder;
}

MachineInstr &VInstrInfo::BuildSelect(MachineBasicBlock *MBB,
                                      MachineOperand &Result,
                                      MachineOperand Pred,
                                      MachineOperand IfTrueVal,
                                      MachineOperand IfFalseVal,
                                      const TargetInstrInfo *TII) {
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
                  TII->get(VTM::VOpSel))
            .addOperand(ResDef).addOperand(Pred)
            .addOperand(IfTrueVal).addOperand(IfFalseVal)
            .addOperand(ucOperand::CreatePredicate());
}

MachineInstr &VInstrInfo::BuildSelect(MachineBasicBlock *MBB, MachineOperand &Result,
                                      const SmallVectorImpl<MachineOperand> &Pred,
                                      MachineOperand IfTrueVal,
                                      MachineOperand IfFalseVal,
                                      const TargetInstrInfo *TII){
  // Build and insert the select instruction at the end of the BB.
  assert(Pred.size() == 1 && "Cannot select value!");
  return BuildSelect(MBB, Result, Pred[0], IfTrueVal, IfFalseVal, TII);
}

MachineInstr &
VInstrInfo::BuildConditionnalMove(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator IP,
                                  MachineOperand &Res,
                                  const SmallVectorImpl<MachineOperand> &Pred,
                                  MachineOperand IfTrueVal,
                                  const TargetInstrInfo *TII) {
  MachineRegisterInfo &MRI = MBB.getParent()->getRegInfo();
  if (!Res.getReg()) {
    MachineRegisterInfo &MRI = MBB.getParent()->getRegInfo();
    const TargetRegisterClass *RC = MRI.getRegClass(IfTrueVal.getReg());
    Res.setReg(MRI.createVirtualRegister(RC));
  }

  MachineOperand ResDef(Res);
  ResDef.setIsDef();

  unsigned Opcode = MoveOpcodes[MRI.getRegClass(IfTrueVal.getReg())->getID()];
  assert(Opcode && "Unsupported move!");

  return *BuildMI(MBB, IP, DebugLoc(), TII->get(Opcode))
            .addOperand(ResDef).addOperand(IfTrueVal).addOperand(Pred[0]);
}

bool VInstrInfo::isCopyLike(unsigned Opcode, bool IncludeMoveImm) {
  return Opcode == VTM::COPY
         || Opcode == VTM::VOpMove_ra
         || (Opcode == VTM::VOpMove_ri && IncludeMoveImm)
         || Opcode == VTM::VOpMove_rm
         || Opcode == VTM::VOpMove_rs
         || Opcode == VTM::VOpMove_rw;
}

bool VInstrInfo::isBrCndLike(unsigned Opcode) {
  return Opcode == VTM::VOpToState
         || Opcode == VTM::VOpToStateb;
}

bool VInstrInfo::isWireOp(unsigned Opcode) {
  return Opcode == VTM::VOpBitCat
         || Opcode == VTM::VOpBitRepeat
         || Opcode == VTM::VOpBitSlice;
}

FuncUnitId VIDesc::getPrebindFUId()  const {
  // Dirty Hack: Bind all memory access to channel 0 at this moment.
  switch(getTID().Opcode) {
  case VTM::VOpMemTrans:
    return FuncUnitId(VFUs::MemoryBus, 0);
  case VTM::VOpBRam: {
    unsigned Id = get().getOperand(5).getImm();
    return FuncUnitId(VFUs::BRam, Id);
  }
  case VTM::VOpInternalCall: {
    unsigned Id = get().getOperand(1).getImm();
    return FuncUnitId(VFUs::CalleeFN, Id);
  }
  default:
    return FuncUnitId();
  }
}


BitWidthAnnotator::BitWidthAnnotator(MachineInstr &MI)
  : MO(&MI.getOperand(MI.getNumOperands() - 1)) {
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
  unsigned PredIdx = Parent->getDesc().NumOperands - 1;
  if (MOIdx != PredIdx) {
    MachineOperand PredMO = *MO;
    Parent->RemoveOperand(MOIdx);

    SmallVector<MachineOperand, 8> Ops;
    while(--MOIdx != 0) {
      Ops.push_back(Parent->getOperand(MOIdx));
      Parent->RemoveOperand(MOIdx);
    }

    // Insert the predicate operand to the operand list.
    Ops.insert(Ops.begin() + (Ops.size() - PredIdx) + 1, PredMO);

    while (!Ops.empty())
      Parent->addOperand(Ops.pop_back_val());
    MO = &Parent->getOperand(PredIdx);
  }

  MO->ChangeToRegister(0, false);
  MO->setTargetFlags(1);
}

bool VIDesc::mayLoad() const {
  switch (getTID().Opcode) {
  default: return false;
  // There is a "isLoad" flag in memory access operation.
  case VTM::VOpMemTrans: return !get().getOperand(3).getImm();
  }
}

bool VIDesc::mayStore() const {
  switch (getTID().Opcode) {
  default: return false;
    // There is a "isLoad" flag in memory access operation.
  case VTM::VOpMemTrans: return get().getOperand(3).getImm();
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
