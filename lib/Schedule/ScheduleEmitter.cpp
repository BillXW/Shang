//===----- ScheduleEmitter.cpp - Emit the schedule  -------------*- C++ -*-===//
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
// This file implement the Schedule emitter class, which build the state
// instructions from the scheduled schedule unit.
//
//===----------------------------------------------------------------------===//

#include "VSUnit.h"

#include "vtm/VerilogBackendMCTargetDesc.h"
#include "vtm/VFInfo.h"
#include "vtm/VInstrInfo.h"

#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBundle.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/MachineSSAUpdater.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#define DEBUG_TYPE "vtm-schedule-emitter"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(DanglingDatapath, "Number of dangling data-path operations");

namespace {
class OpSlot {
  int SlotNum;
  OpSlot(int S) : SlotNum(S) {}
  enum SlotType { Control, Datapath };
public:
  OpSlot() : SlotNum(0) {}
  OpSlot(int Slot, bool isCtrl) {
    SlotType T = isCtrl ? Control : Datapath;
    SlotNum = (Slot << 0x1) | (0x1 & T);
  }

  SlotType getSlotType() const {
    return (SlotType)(SlotNum & 0x1);
  }

  bool isControl() const {
    return getSlotType() == OpSlot::Control;
  }

  bool isDatapath() const {
    return getSlotType() == OpSlot::Datapath;
  }

  int getSlot() const { return SlotNum / 2; }

  inline bool operator==(OpSlot S) const {
    return SlotNum == S.SlotNum;
  }

  inline bool operator!=(OpSlot S) const {
    return SlotNum != S.SlotNum;
  }

  inline bool operator<(OpSlot S) const {
    return SlotNum < S.SlotNum;
  }

  inline bool operator<=(OpSlot S) const {
    return SlotNum <= S.SlotNum;
  }

  inline bool operator>(OpSlot S) const {
    return SlotNum > S.SlotNum;
  }
  inline bool operator>=(OpSlot S) const {
    return SlotNum >= S.SlotNum;
  }

  inline OpSlot &setSlot(unsigned RHS) {
    unsigned T = SlotNum & 0x1;
    SlotNum = (RHS << 0x1) | T;
    return *this;
  }

  void setType(bool isCtrl) {
    SlotType T = isCtrl ? Control : Datapath;
    SlotNum = (getSlot() << 0x1) | (0x1 & T);
  }

  inline OpSlot operator+(int RHS) const {
    return OpSlot(getSlot() + RHS, isControl());
  }

  inline OpSlot operator-(int RHS) const {
    return OpSlot(getSlot() - RHS, isControl());
  }

  inline OpSlot &operator+=(int RHS) {
    SlotNum += RHS * 2;
    return *this;
  }

  inline OpSlot &operator++() { return operator+=(1); }

  inline OpSlot operator++(int) {
    OpSlot Temp = *this;
    SlotNum += 2;
    return Temp;
  }

  OpSlot getNextSlot() const { return OpSlot(SlotNum + 1); }
  OpSlot getPrevSlot() const { return OpSlot(SlotNum - 1); }

  int getDetailStep() const { return SlotNum; }

  static OpSlot detailStepCeil(int S, bool isDatapath) {
    //OpSlot s(S);

    //// If the type not match, get the next slot.
    //if (s.isControl() != isCtrl)
    //  return s.getNextSlot();

    //return s;
    bool SIsDataPath = S & 0x1;
    bool TypeNotMatch = SIsDataPath != isDatapath;
    return OpSlot(S + TypeNotMatch);
  }

  static OpSlot detailStepFloor(int S, bool isDatapath) {
    //OpSlot s(S);

    //// If the type not match, get the next slot.
    //if (s.isControl() != isCtrl)
    //  return s.getPrevSlot();

    //return s;
    bool SIsDataPath = S & 0x1;
    bool TypeNotMatch = SIsDataPath != isDatapath;
    return OpSlot(S - TypeNotMatch);
  }
};

// Helper class to build Micro state.
struct MicroStateBuilder {
  MicroStateBuilder(const MicroStateBuilder&);     // DO NOT IMPLEMENT
  void operator=(const MicroStateBuilder&); // DO NOT IMPLEMENT

  VSchedGraph &State;
  MachineBasicBlock &MBB;
  const unsigned ScheduleStartSlot, ScheduleLoopOpSlot, ScheduleEndSlot, II,
                 StartSlot;
  const bool isMBBPipelined;
  typedef MachineInstr *InsertPosTy;
  InsertPosTy InsertPos;

  const TargetInstrInfo &TII;
  MachineRegisterInfo &MRI;
  VFInfo &VFI;

  SmallVector<VSUnit*, 8> SUnitsToEmit;

  struct WireDef {
    unsigned WireNum;
    MachineOperand Pred;
    MachineOperand Op;
    OpSlot DefSlot;
    OpSlot CopySlot;
    OpSlot LoopBoundary;

    WireDef(unsigned wireNum, MachineOperand pred, MachineOperand op,
            OpSlot defSlot, OpSlot copySlot, OpSlot loopBoundary)
      : WireNum(wireNum), Pred(pred), Op(op), DefSlot(defSlot),
      CopySlot(copySlot), LoopBoundary(loopBoundary) {}

    // Do not define a register twice by copying it self.
    bool shouldBeCopied() const {
      return (!Op.isReg() || WireNum != Op.getReg());
    }

    MachineOperand getOperand() const { return Op; }

    MachineOperand createOperand() const {
      return VInstrInfo::CreateReg(WireNum, VInstrInfo::getBitWidth(Op));
    }
  };

  inline WireDef createWireDef(unsigned WireNum, MachineOperand MO,
                               MachineOperand Pred, OpSlot defSlot,
                               OpSlot copySlot/*, bool isPHI = false*/){
    assert(copySlot.isControl() && "Can only copy at control!");
    // Compute the loop boundary, the last slot before starting a new loop,
    // which is at the same time with the first slot of next iteration.
    unsigned LoopBoundarySlot = copySlot.getSlot();
    if (!isMBBPipelined)
      LoopBoundarySlot = ScheduleLoopOpSlot;
    else {
      // For a PHI node, we do not need to insert new PHI for those read simply
      // wrap around to preserve SSA form. But we need a copy to pipeline the
      // value to preserve the dependence.
      // if (isPHI) LoopBoundarySlot += II;
      // else {
        // Otherwise, we need to insert a PHI node to preserve SSA form, by
        // avoiding use before define, which occur if the read is wrap around.
        LoopBoundarySlot -= ScheduleStartSlot;
        LoopBoundarySlot = RoundUpToAlignment(LoopBoundarySlot, II);
        LoopBoundarySlot += ScheduleStartSlot;
      //}
    }
    
    assert((LoopBoundarySlot > ScheduleStartSlot || defSlot == copySlot)
           && LoopBoundarySlot >= unsigned(copySlot.getSlot())
           && "LoopBoundary should bigger than start slot and copyt slot!");
    return WireDef(WireNum, Pred, MO, defSlot, copySlot, OpSlot(LoopBoundarySlot, true));
  }
  
  typedef std::vector<WireDef*> DefVector;
  
  typedef SmallVector<InsertPosTy, 32> IPVector;
  IPVector CtrlIPs, DataPathIPs;

  // register number -> wire define.
  typedef std::map<unsigned, WireDef> SWDMapTy;
  SWDMapTy StateWireDefs;

  MicroStateBuilder(VSchedGraph &S, MachineBasicBlock *MBB, unsigned StartSlot)
  : State(S), MBB(*MBB), ScheduleStartSlot(S.getStartSlot(MBB)),
    ScheduleLoopOpSlot(S.getLoopOpSlot(MBB)), ScheduleEndSlot(S.getEndSlot(MBB)),
    II(S.getII(MBB)), StartSlot(StartSlot), isMBBPipelined(S.isPipelined(MBB)),
    InsertPos(MBB->end()),
    TII(*MBB->getParent()->getTarget().getInstrInfo()),
    MRI(MBB->getParent()->getRegInfo()),
    VFI(*MBB->getParent()->getInfo<VFInfo>())
  {
    // Build the instructions for mirco-states.
    unsigned EndSlot = StartSlot + II;
    MachineInstr *Start =
      BuildMI(*MBB, InsertPos, DebugLoc(), TII.get(VTM::CtrlStart))
        .addImm(StartSlot).addImm(0).addImm(0);
    MachineInstr *End =
      BuildMI(*MBB, InsertPos, DebugLoc(), TII.get(VTM::CtrlEnd))
       .addImm(StartSlot).addImm(0).addImm(0);
    // Control Ops are inserted between Ctrl-Starts and Ctrl-Ends
    CtrlIPs.push_back(End);

    for (unsigned i = StartSlot + 1, e = EndSlot; i <= e; ++i) {
      // Build the header for datapath from in slot.
      BuildMI(*MBB, InsertPos, DebugLoc(), TII.get(VTM::Datapath))
        .addImm(i - 1).addImm(0).addImm(0);
      Start = BuildMI(*MBB, InsertPos, DebugLoc(), TII.get(VTM::CtrlStart))
                .addImm(i).addImm(0).addImm(0);
      End = BuildMI(*MBB, InsertPos, DebugLoc(), TII.get(VTM::CtrlEnd))
              .addImm(i).addImm(0).addImm(0);
      // Datapath Ops are inserted between Ctrl-Ends and Ctrl-Starts
      DataPathIPs.push_back(Start);
      CtrlIPs.push_back(End);
    }

    // Build Datapath bundle for dangling data-paths.
    BuildMI(*MBB, InsertPos, DebugLoc(), TII.get(VTM::Datapath))
      .addImm(EndSlot).addImm(0).addImm(0);
  }

  unsigned getModuloSlot(OpSlot S) const {
    unsigned Slot = S.getSlot();
    bool IsControl = S.isControl();

    assert(Slot >= ScheduleStartSlot && "Bad slot!");
    unsigned Idx = Slot -  ScheduleStartSlot;
    if (isMBBPipelined) {
      Idx %= II;
      // Move the entry of non-first stage to the last slot, so
      // Stage 0: Entry,    state1,        ... state(II - 1),
      // Stage 1: stateII,  state(II + 1), ... state(2II - 1),
      // Stage 2: state2II,  state(2II + 1), ... state(3II - 1),
      // become:
      // Stage 0: Entry,    state1,        ... state(II - 1),   stateII,
      // Stage 1:           state(II + 1), ... state(2II - 1),  state2II,
      // Stage 2:           state(2II + 1), ... state(3II - 1),
      if (IsControl && Idx == 0 && Slot >= ScheduleLoopOpSlot)
        Idx = II;
    }

    return Idx;
  }

  bool isReadWrapAround(OpSlot ReadSlot, WireDef &WD) const {
    return WD.LoopBoundary < ReadSlot;
  }

  InsertPosTy getStateCtrlAt(OpSlot CtrlSlot) {
    unsigned Idx = getModuloSlot(CtrlSlot);
    // Retrieve the instruction at specific slot. 
    MachineInstr *Ret = CtrlIPs[Idx];
    assert(Ret && "Unexpected NULL instruction!");
    return Ret;
  }

  InsertPosTy getStateDatapathAt(OpSlot DatapathSlot) {
    unsigned Idx = getModuloSlot(DatapathSlot);
    // Retrieve the instruction at specific slot.
    MachineInstr *Ret = DataPathIPs[Idx];
    assert(Ret && "Unexpected NULL instruction!");
    return Ret;
  }

  InsertPosTy getMIAt(OpSlot Slot) {
    if (Slot.isControl()) return getStateCtrlAt(Slot);
    else                  return getStateDatapathAt(Slot);
  }

  MachineBasicBlock::iterator getInsertPos() { return InsertPos; }

  // Builder interface.
  void emitSUnit(VSUnit *A) { SUnitsToEmit.push_back(A); }
  bool emitQueueEmpty() const { return SUnitsToEmit.empty(); }

  // Main state building function.
  MachineInstr *buildMicroState(unsigned Slot);

  // Fuse instructions in a bundle.
  void fuseInstr(MachineInstr &Inst, OpSlot SchedSlot, FuncUnitId FUId);

  // Build the machine operand that use at a specified slot.
  MachineOperand getRegUseOperand(MachineOperand MO, OpSlot ReadSlot) {
    unsigned Reg = MO.getReg();
    // Else this is a use.
    SWDMapTy::iterator at = StateWireDefs.find(Reg);
    // Using register from previous state.
    if (at == StateWireDefs.end()) {
      // Do not need to worry about if the new loop overwrite the the loop
      // invariants.
      return MO;
    }

    WireDef &WDef = at->second;

    // We need the value after it is written to register.
    if (WDef.CopySlot < ReadSlot) {
      //assert(((!IsCtrl && ReadSlot == EmitSlot + 1)
      //        || (IsCtrl && ReadSlot == EmitSlot))
      //        && "Assumption of Slots broken!");
      MachineOperand Ret = getRegUseOperand(WDef, ReadSlot, MO);
      if (!MO.isImplicit())
        VInstrInfo::setBitWidth(Ret, VInstrInfo::getBitWidth(MO));
      return Ret;
    }

    assert(WDef.DefSlot <= ReadSlot && "Dependencies broken!");
    // No need a implicit use, because the implicit operand is used explicitly
    // at the same slot.
    // Dirty Hack: Just return something meaningless.
    if (MO.isImplicit()) return MachineOperand::CreateReg(0, false);

    MachineOperand Ret = WDef.createOperand();
    VInstrInfo::setBitWidth(Ret, VInstrInfo::getBitWidth(MO));
    return Ret;
  }

  // Build the machine operand that read the wire definition at a specified slot.
  MachineOperand getRegUseOperand(WireDef &WD, OpSlot ReadSlot, MachineOperand MO);
 
  void emitPHICopy(MachineInstr *PN, unsigned Slot) {
    for (unsigned i = 1; i != PN->getNumOperands(); i += 2) {
      if (PN->getOperand(i + 1).getMBB() != &MBB) continue;

      MachineOperand &SrcMO = PN->getOperand(i);

      OpSlot CopySlot(Slot, true);
      unsigned DstReg = MRI.createVirtualRegister(&VTM::DRRegClass);

      BuildMI(MBB, getStateCtrlAt(CopySlot), DebugLoc(), TII.get(VTM::VOpMove))
        .addOperand(VInstrInfo::CreateReg(DstReg, VInstrInfo::getBitWidth(SrcMO),
                                         true))
        .addOperand(getRegUseOperand(SrcMO, CopySlot))
        .addOperand(VInstrInfo::CreatePredicate())
        .addImm(translateToSlotRegNum(Slot));

      SrcMO.ChangeToRegister(DstReg, false);
    }
  }

  void emitPHIDef(MachineInstr *PN) {
    // FIXME: Place the PHI define at the right slot to avoid the live interval
    // of registers in the PHI overlap. 
    unsigned InsertSlot = /*Slot ? Slot :*/ ScheduleStartSlot;
    MachineOperand &MO = PN->getOperand(0);
    unsigned PHIReg = MO.getReg();
    assert(MRI.getRegClass(PHIReg) == &VTM::DRRegClass
           && "Bad register class for PHI!");
    unsigned NewReg = MRI.createVirtualRegister(&VTM::DRRegClass);

    DebugLoc dl;
    InsertPosTy IP = getStateCtrlAt(OpSlot(InsertSlot, true));
    unsigned BitWidth = VInstrInfo::getBitWidth(MO);
    BuildMI(MBB, IP, dl, TII.get(VTM::VOpDefPhi))
      .addOperand(MO).addOperand(VInstrInfo::CreateReg(NewReg, BitWidth, false))
      .addOperand(VInstrInfo::CreatePredicate())
      .addImm(translateToSlotRegNum(InsertSlot));

    // Update the MO of the Original PHI.
    MO.ChangeToRegister(NewReg, true);
  }

  // Translate the global slot number to unique slot register number.
  unsigned translateToSlotRegNum(unsigned ScheduleSlot) {
    return ScheduleSlot - ScheduleStartSlot + StartSlot;
  }

  void setInstrSlotNum(MachineInstr * MI, unsigned ScheduleSlot) {
    unsigned Slot = translateToSlotRegNum(ScheduleSlot);
    VInstrInfo::setInstrSlotNum(MI, Slot);
  }

  // Build PHI nodes to preserve anti-dependence for pipelined BB.
  unsigned createPHI(unsigned RegNo, unsigned SizeInBits, unsigned WriteSlot,
                     bool WrapOnly) {
    SmallVector<MachineInstr*, 4> InsertedPHIs;

    // PHI node needed.
    // TODO: Move to constructor?
    MachineSSAUpdater SSAUpdate(*MBB.getParent(), &InsertedPHIs);
    SSAUpdate.Initialize(RegNo);
    SSAUpdate.AddAvailableValue(&MBB, RegNo);

    // 1. add an initialize value.
    for (MachineBasicBlock::pred_iterator I = MBB.pred_begin(),
         E = MBB.pred_end();I != E; ++I) {
      MachineBasicBlock *PredBB = *I;
      if (PredBB == &MBB) continue;

      // The register to hold initialize value.
      unsigned InitReg = MRI.createVirtualRegister(&VTM::DRRegClass);
      MachineOperand InitOp = MachineOperand::CreateReg(InitReg, true);
      VInstrInfo::setBitWidth(InitOp, SizeInBits);

      MachineBasicBlock::iterator IP = PredBB->getFirstTerminator();
      // Insert the imp_def before the PHI incoming copies.
      while (llvm::prior(IP)->getOpcode() == VTM::VOpMvPhi)
        --IP;

      BuildMI(*PredBB, IP, DebugLoc(), TII.get(VTM::IMPLICIT_DEF))
        .addOperand(InitOp);

      SSAUpdate.AddAvailableValue(PredBB, InitReg);
    }

    unsigned NewReg = SSAUpdate.GetValueInMiddleOfBlock(&MBB);

    // Update the bitwidth for newly inserted PHIs, insert it into the
    // First ucSate.
    while (!InsertedPHIs.empty()) {
      MachineInstr *PN = InsertedPHIs.pop_back_val();
      MachineOperand &Op = PN->getOperand(0);
      VInstrInfo::setBitWidth(Op, SizeInBits);

      for (unsigned i = 1; i != PN->getNumOperands(); i += 2) {
        MachineOperand &SrcOp = PN->getOperand(i);
        VInstrInfo::setBitWidth(SrcOp, SizeInBits);
      }

      if (!WrapOnly) {
        // If the PHI need copy, set the its register class to DR.
        MRI.setRegClass(Op.getReg(), &VTM::DRRegClass);
        // Emit instructions to preserve the dependence about PHINodes.
        emitPHIDef(PN);
        emitPHICopy(PN, WriteSlot);
      }
    }

    return NewReg;
  }

  // Increase the slot counter and emit all pending schedule units.
  unsigned advanceToSlot(unsigned CurSlot, unsigned TargetSlot) {
    assert(TargetSlot > CurSlot && "Bad target slot!");
    buildMicroState(CurSlot);
    SUnitsToEmit.clear();
    // Advance current slot.
    ++CurSlot;

    // Some states may not emit any atoms, but it may read the result from
    // previous atoms.
    // Note that SUnitsToEmit is empty now, so we do not emitting any new
    // atoms.
    while (CurSlot < TargetSlot && CurSlot < ScheduleEndSlot)
      buildMicroState(CurSlot++);

    return CurSlot;
  }
};
}

//===----------------------------------------------------------------------===//
typedef std::pair<MachineInstr*, OpSlot> InSUInstInfo;
template<typename T>
static inline bool sort_intra_latency(const T &LHS, const T &RHS) {
  return LHS.second < RHS.second;
}

MachineInstr* MicroStateBuilder::buildMicroState(unsigned Slot) {
  for (SmallVectorImpl<VSUnit*>::iterator I = SUnitsToEmit.begin(),
       E = SUnitsToEmit.end(); I !=E; ++I) {
    VSUnit *A = *I;

    SmallVector<InSUInstInfo, 8> Insts;

    for (unsigned i = 0, e = A->num_instrs(); i < e; ++i) {
      MachineInstr *Inst = A->getPtrAt(i);
      // Ignore the entry node marker (null) and implicit define.
      if (Inst && !Inst->isImplicitDef()) {
        if (Inst->isPHI())
          emitPHIDef(Inst);
        else {
          unsigned S = A->getSlot() +  A->getLatencyAt(i);
          bool IsCtrl = VInstrInfo::isControl(Inst->getOpcode());
          Insts.push_back(std::make_pair(Inst, OpSlot(S, IsCtrl)));
        }
      }
    }

    // Sort the instructions, so we can emit them in order.
    std::sort(Insts.begin(), Insts.end(), sort_intra_latency<InSUInstInfo>);

    bool IsDangling = A->isDangling();

    typedef SmallVector<InSUInstInfo, 8>::iterator it;
    for (it I = Insts.begin(), E = Insts.end(); I != E; ++I) {
      MachineInstr *MI = I->first;

      // Simply place the dangling node at the end.
      if (IsDangling){
        assert(VInstrInfo::isDatapath(MI->getOpcode())
               && "Unexpected dangling control-path operation!");
        ++DanglingDatapath;
        VInstrInfo::setInstrSlotNum(MI, 0);
        MI->removeFromParent();
        MBB.push_back(MI);
        continue;
      }

      OpSlot S = I->second;
      fuseInstr(*MI, S, VInstrInfo::getPreboundFUId(MI));
    }
  }

  return 0;
}

void MicroStateBuilder::fuseInstr(MachineInstr &Inst, OpSlot SchedSlot,
                                  FuncUnitId FUId) {
  bool IsCtrl = VInstrInfo::isControl(Inst.getOpcode());
  bool IsCtrlSlot = SchedSlot.isControl();
  assert(IsCtrlSlot == IsCtrl && "Wrong slot type.");
  bool isCopyLike = VInstrInfo::isCopyLike(Inst.getOpcode());
  bool isWriteUntilFinish = VInstrInfo::isWriteUntilFinish(Inst.getOpcode());
  // Compute the slots.
  OpSlot ReadSlot = SchedSlot;

  unsigned StepDelay = State.getStepsToFinish(&Inst);
  unsigned FinSlot = SchedSlot.getSlot() + StepDelay;
  OpSlot CopySlot(FinSlot, true);
  // We can not write the value to a register at the same moment we emit it.
  // Unless we read at emit.
  // FIXME: Introduce "Write at emit."
  if (CopySlot < SchedSlot) ++CopySlot;
  // Write to register operation need to wait one more slot if the result is
  // written at the moment (clock event) that the atom finish.
  //if (VInstrInfo::isWriteUntilFinish(Inst.getOpcode())) ++CopySlot;

  unsigned Opc = Inst.getOpcode();
  // SchedSlot is supposed to strictly smaller than CopySlot, if this not hold
  // then slots is wrapped.
  int WrappedDistance = getModuloSlot(CopySlot) - getModuloSlot(SchedSlot);
  int Distance = CopySlot.getSlot() - SchedSlot.getSlot();
  bool WrappedAround = (WrappedDistance != Distance);
  assert((!WrappedAround || isMBBPipelined)
         && "Live intervals are only wrapped in pipelined block d!");
  // FIX the opcode of terminators.
  if (Inst.isTerminator()) {
    if (VInstrInfo::isBrCndLike(Opc)) Inst.setDesc(TII.get(VTM::VOpToState_nt));
    else                              Inst.setDesc(TII.get(VTM::VOpRet_nt));
  }

  // Handle the predicate operand.
  MachineOperand Pred = *VInstrInfo::getPredOperand(&Inst);
  assert(Pred.isReg() && "Cannot handle predicate operand!");
  
  // Do not copy instruction that is write until finish, which is already taken
  // care by VOpPipelineStage.
  bool NeedCopy = !isWriteUntilFinish;

  // The value defined by this instruction.
  DefVector Defs;
  // Adjust the operand by the timing.
  for (unsigned i = 0 , e = Inst.getNumOperands(); i != e; ++i) {
    MachineOperand &MO = Inst.getOperand(i);

    // Ignore the non-register operand (also ignore reg0 which means nothing).
    if (!MO.isReg() || !MO.getReg())
      continue;

    const unsigned RegNo = MO.getReg();

    // Remember the defines.
    // DiryHack: Do not emit write define for copy since copy is write at
    // control block.

    if (MO.isDef()) {
      if (MRI.use_empty(RegNo)) {
        MO.ChangeToRegister(0, true);
        continue;
      }

      unsigned BitWidth = VInstrInfo::getBitWidth(MO);
      // Do not emit write to register unless it not killed in the current state.
      // FIXME: Emit the wire only if the value is not read in a function unit port.
      // if (!NewDef->isSymbol()) {
      // Define wire for operations.
      MachineOperand NewOp = MO;
      unsigned WireNum = NewOp.getReg();

      // Define wire for trivial operation, otherwise, the result of function
      // unit should be wire, and there must be a copy follow up.
      if (!VRegisterInfo::IsWire(RegNo, &MRI) && NeedCopy) {
        assert(CopySlot != SchedSlot
               && "Copy should already set the right RC up!");
        WireNum =
          MRI.createVirtualRegister(VRegisterInfo::getRepRegisterClass(Opc));
        NewOp = VInstrInfo::CreateReg(WireNum, BitWidth, true);
      }

      // If the wire define and the copy wrap around?
      if (WrappedAround)
        // In fact, there is a BUG:
        // Supposed we have an instruction (operation) Op0, which defines
        // virtual register A, and there are the users of A, say Op1, Op2, where
        // Op1 is not wrap around but Op2 does:
        // Op2 use A
        // ...
        // A = Op0
        // ...
        // Op1 use A
        // To preserve the SSA from, we need to insert a PHI: 
        // A' = PHI A, Same BB, Undef, Other BBs
        // Op2 use A'
        // ...
        // A = Op0
        // ...
        // Op1 use A
        // In fact, the generated code is looks like this:
        // A' = PHI A, Same BB, Undef, Other BBs
        // Op2 use A'
        // ...
        // A = Op0
        // ...
        // Op1 use A' <----------- Use the wrong value.
        // Fortunately, this bug is hidden by the PHIElimination and
        // AdjustLIForBundles pass.
        // This bug can be fixed by emitting the PHIs lazily.
        WireNum = createPHI(WireNum, BitWidth, SchedSlot.getSlot(), true);

      WireDef WDef = createWireDef(WireNum, MO, Pred, SchedSlot, CopySlot);

      SWDMapTy::iterator mapIt;
      bool inserted;
      tie(mapIt, inserted) = StateWireDefs.insert(std::make_pair(RegNo, WDef));

      assert(inserted && "Instructions not in SSA form!");
      WireDef *NewDef = &mapIt->second;
      // Remember to emit this wire define if necessary.
      Defs.push_back(NewDef);

      // Update the operand.
      MO.ChangeToRegister(NewOp.getReg(), NewOp.isDef(), NewOp.isImplicit());
      MO.setTargetFlags(NewOp.getTargetFlags());
      // }
    } else if (!MO.isDef()) {
      MachineOperand NewOp = getRegUseOperand(MO, ReadSlot);
      // Update the operand.
      MO.ChangeToRegister(NewOp.getReg(), NewOp.isDef(), NewOp.isImplicit());
      MO.setTargetFlags(NewOp.getTargetFlags());
    }
  }

  // Set the scheduled slot of the instruction. Set the schedule slot of
  // data-path operations to 0, because the slot of data-path dose not make sense,
  // but preventing us from merging identical data-path operations.
  unsigned InstrSlot = IsCtrl ? SchedSlot.getSlot() : 0;
  // Also set the slot of datapath if we need.
  DEBUG_WITH_TYPE("vtm-debug-datapath-slot", InstrSlot = SchedSlot.getSlot());

  setInstrSlotNum(&Inst, InstrSlot);
  // Move the instruction to the right place.
  InsertPosTy IP = getMIAt(SchedSlot);
  Inst.removeFromParent();
  MBB.insert((MachineBasicBlock::iterator)IP, &Inst);

  if (!NeedCopy || Defs.empty()) {
    return;
  }

  // Emit the exported registers at current slot.
  IP = getStateCtrlAt(CopySlot);
  while (!Defs.empty()) {
    WireDef *WD = Defs.back();
    Defs.pop_back();

    MachineOperand MO = WD->getOperand();
    // Do not copy to a wire.
    if (VRegisterInfo::IsWire(MO.getReg(), &MRI)) continue;

    if (WD->shouldBeCopied() && !isCopyLike) {
      llvm_unreachable("VOpReadFU should had been already inserted!");
      unsigned Slot = CopySlot.getSlot();
      // Export the register.
      MachineInstrBuilder Builder = BuildMI(MBB, IP, DebugLoc(),
                                            TII.get(VTM::VOpReadFU));
      MachineOperand Src = WD->createOperand();
      // Do not define MO if we have a implicit use.
      MO.setIsDef();
      Builder.addOperand(MO);

      Builder.addOperand(Src);

      // Also remember the function unit id.
      Builder.addImm(FUId.getData());

      // Get the predicate operand at current slot.
      Builder.addOperand(getRegUseOperand(WD->Pred, CopySlot));

      // Add the operand to hold the schedule.
      Builder.addImm(0);
      // Set the slot number.
      setInstrSlotNum(Builder, Slot);
    }
  }
}

MachineOperand MicroStateBuilder::getRegUseOperand(WireDef &WD, OpSlot ReadSlot,
                                                   MachineOperand MO) {
  bool isImplicit = MO.isImplicit();
  unsigned RegNo = WD.getOperand().getReg();
  unsigned PredReg = WD.Pred.getReg();
  unsigned SizeInBits = VInstrInfo::getBitWidth(WD.Op);

  // Move the value to a new register otherwise the it will be overwritten.
  // If read before write in machine code, insert a phi node.
  while (isReadWrapAround(ReadSlot, WD)) {
    // Because the result of wireops will be copied to register at loop boundary
    // only extend the live interval of its operand to the first loop boundary.
    if (isImplicit && WD.LoopBoundary > WD.DefSlot + II)
      break;

    // Emit the PHI at loop boundary, but do not emit the copy if the wire is
    // defined at loop boundary, otherwise we will get the value from previous
    // iteration which is not we want.
    RegNo = createPHI(RegNo, SizeInBits, WD.LoopBoundary.getSlot(),
                      WD.LoopBoundary == WD.DefSlot);
    MO = VInstrInfo::CreateReg(RegNo, SizeInBits, false);
    WD.Op = MO;

    if (PredReg) {
      PredReg = createPHI(PredReg, 1, WD.LoopBoundary.getSlot(), false);
      WD.Pred = VInstrInfo::CreatePredicate(PredReg);
    }

    // Update the register.
    WD.CopySlot = WD.LoopBoundary;
    WD.LoopBoundary += II;
    assert(WD.CopySlot <= ReadSlot && "Broken PHI Slot!");
  }

  // Return the up to date machine operand
  MO = WD.Op;
  MO.setIsUse();
  MO.setImplicit(isImplicit);
  return MO;
}

void VSchedGraph::insertDelayBlock(MachineBasicBlock *From,
                                   MachineBasicBlock *To, unsigned Latency) {
  MachineInstr *BRInstr = 0;
  typedef MachineBasicBlock::reverse_instr_iterator reverse_iterator;

  for (reverse_iterator I = From->instr_rbegin(), E = From->instr_rend();
       I != E && I->isTerminator(); ++I) {
    if (!VInstrInfo::isBrCndLike(I->getOpcode())) continue;

    if (I->getOperand(1).getMBB() != To) continue;

    BRInstr = &*I;
    break;
  }

  assert(BRInstr && "The corresponding branch instruction not found!");

  MachineFunction *MF = From->getParent();
  MachineBasicBlock *DelayBlock = MF->CreateMachineBasicBlock();
  MF->push_back(DelayBlock);

  // Redirect the target block of the branch instruction to the newly created
  // block, and modify the CFG.
  BRInstr->getOperand(1).setMBB(DelayBlock);
  From->replaceSuccessor(To, DelayBlock);
  DelayBlock->addSuccessor(To);

  // Also fix the PHIs in the original sink block.
  typedef MachineBasicBlock::instr_iterator iterator;
  for (iterator I = To->instr_begin(), E = To->instr_end();
       I != E && I->isPHI(); ++I) {
    for (unsigned i = 1, e = I->getNumOperands(); i != e; i += 2)
      if (I->getOperand(i + 1).getMBB() == From) {
        I->getOperand(i + 1).setMBB(DelayBlock);
        break;
      }
  }

  // Build the entry of the delay block.
  VSUnit *EntrySU = createVSUnit(DelayBlock);
  EntrySU->scheduledTo(EntrySlot);
  EntrySU->setIsDangling(false);

  // And the terminator.
  MachineInstr *DelayBR
    = BuildMI(DelayBlock, DebugLoc(), VInstrInfo::getDesc(VTM::VOpToState))
       .addOperand(VInstrInfo::CreatePredicate()).addMBB(To)
       .addOperand(VInstrInfo::CreatePredicate())
       .addOperand(VInstrInfo::CreateTrace());

  VSUnit *U = createTerminator(DelayBlock);
  mapMI2SU(DelayBR, U, 0);
  U->scheduledTo(EntrySlot + Latency);
  U->setIsDangling(false);

  addDummyLatencyEntry(DelayBR);

  // TODO: Fix the dependencies edges.
}

void VSchedGraph::insertDelayBlock(const BBInfo &Info) {
  MachineBasicBlock *MBB = Info.Entry->getParentBB();

  typedef VSUnit::dep_iterator dep_it;
  for (dep_it I = cp_begin(Info.Entry), E = cp_end(Info.Entry); I != E; ++I) {
    VSUnit *PredTerminator = *I;
    //DEBUG(dbgs() << "MBB#" << PredTerminator->getParentBB()->getNumber()
    //             << "->MBB#" << BBEntry->getParentBB()->getNumber() << " slack: "
    //             << (BBEntry->getSlot() - PredTerminator->getSlot()) << '\n');

    MachineBasicBlock *PredBB = PredTerminator->getParentBB();
    const BBInfo &PredInfo = getBBInfo(PredBB);
    if (int ExtraLatency = Info.getExtraLatencyFrom(PredInfo))
      insertDelayBlock(PredBB, MBB, ExtraLatency);
  }
}

void VSchedGraph::insertDelayBlocks() {
  // Because we will push new BBInfo to BBInfoMap during inserting delay blocks,
  // we should use the index to iterate over the exiting BBInfos, and the newly
  // pushed BBInfos will not be visited.
  for (unsigned i = 0, e = BBInfoMap.size(); i != e; ++i)
    if (BBInfoMap[i].MiniInterBBSlack < 0) insertDelayBlock(BBInfoMap[i]);
}

namespace {
struct ValDef {
  unsigned RegNum;
  unsigned ChainStart;
  unsigned FinishSlot;
  MachineInstr &MI;
  bool IsChainedWithFU;
  ValDef *Next;

  explicit ValDef(MachineInstr &MI, unsigned SchedSlot, unsigned FinishSlot)
    : RegNum(0), ChainStart(SchedSlot), FinishSlot(FinishSlot), MI(MI),
      IsChainedWithFU(!VInstrInfo::getPreboundFUId(&MI).isTrivial()
                       // Dirty Hack: Ignore the copy like FU operations, i.e. ReadFU,
                       // and VOpDstMux.
                       && !VInstrInfo::isCopyLike(MI.getOpcode())),
      Next(0) {}

  MachineBasicBlock *getParentBB() const { return MI.getParent(); }
  unsigned getParentBBNum() const { return getParentBB()->getNumber(); }

  bool isCopy() const { return FinishSlot == ChainStart; }
};

struct ChainBreaker {
  VSchedGraph &G;
  MachineRegisterInfo &MRI;

  SpecificBumpPtrAllocator<ValDef> Allocator;
  typedef std::map<unsigned, ValDef*> ValDefMapTy;
  ValDefMapTy ValDefs;

  void addValDef(ValDef *Def) {
    bool succ = ValDefs.insert(std::make_pair(Def->RegNum, Def)).second;
    assert(succ && "ValDef already existed?");
    (void) succ;
  }

  ValDef *lookupValDef(unsigned RegNum) {
    ValDefMapTy::iterator at = ValDefs.find(RegNum);
    assert(at != ValDefs.end() && "ValDef not found!");
    return at->second;
  }

  unsigned getFinishAtCurBB(const ValDef &Def) {
    return Def.getParentBB() == MBB ? Def.FinishSlot : StartSlot;
  }

  unsigned getChainStartAtCurBB(const ValDef &Def) {
    return Def.getParentBB() == MBB ? Def.ChainStart : StartSlot;
  }

  int getSlack(unsigned UseSlot, unsigned UseBBNum,
               unsigned DefSlot, unsigned DefBBNum) const {
    int Slack = UseSlot - DefSlot - G.getInterBBSlack(DefBBNum, UseBBNum);

    return Slack;
  }

  int getSlackFromChainStart(unsigned UseSlot, unsigned UseBBNum,
                              const ValDef &Def) const {
    return getSlack(UseSlot, UseBBNum, Def.ChainStart, Def.getParentBBNum());
  }

  int getSlackFromFinish(unsigned UseSlot, unsigned UseBBNum,
                         const ValDef &Def) const {
    return getSlack(UseSlot, UseBBNum, Def.FinishSlot, Def.getParentBBNum());
  }

  MachineBasicBlock *MBB;
  unsigned CurII, StartSlot;
  bool IsPipelined;

  explicit ChainBreaker(VSchedGraph *G)
    : G(*G), MRI(*G->DLInfo.MRI), MBB(0), CurII(0), StartSlot(0),
      IsPipelined(false) {}

  void updateMBB(MachineBasicBlock *NewMBB) {
    MBB = NewMBB;
    StartSlot = G.getStartSlot(MBB);
    CurII = G.getII(MBB);
    IsPipelined = G.isPipelined(MBB);
  }

  MachineInstr *buildReadFU(MachineInstr *MI, VSUnit *U, unsigned Offset,
                            FuncUnitId Id = FuncUnitId(), bool Dead = false);

  ValDef *getValInReg(ValDef *Def, unsigned Slot);
  ValDef *insertCopyAtSlot(ValDef &SrcDef, unsigned Slot);
  ValDef *breakChainForAntiDep(ValDef *SrcDef, unsigned ChainEndSlot,
                               unsigned ReadSlot, unsigned II);

  void buildFUCtrl(VSUnit *U);

  void visitUse(MachineInstr *MI, ValDef &Def, bool IsDangling,
                unsigned LatestChainEnd);

  void visitDef(MachineInstr *MI, ValDef &Def);

  void visit(VSUnit *U);
};
}

MachineInstr *ChainBreaker::buildReadFU(MachineInstr *MI, VSUnit *U,
                                        unsigned Offset, FuncUnitId Id,
                                        bool Dead) {
  unsigned RegWidth = VInstrInfo::getBitWidth(MI->getOperand(0));
  unsigned ResultWire = MI->getOperand(0).getReg();
  unsigned ResultReg = Dead ? 0 : MRI.createVirtualRegister(&VTM::DRRegClass);

  DebugLoc dl = MI->getDebugLoc();
  MachineBasicBlock::instr_iterator IP = MI;
  ++IP;

  assert((MI->isPseudo()
          || VInstrInfo::isAlwaysTruePred(*VInstrInfo::getPredOperand(MI)))
         && "Unexpected predicated MI!");

  MachineInstr *ReadFU
    = BuildMI(*MI->getParent(), IP, dl, VInstrInfo::getDesc(VTM::VOpReadFU))
      .addOperand(VInstrInfo::CreateReg(ResultReg, RegWidth, true))
      .addOperand(VInstrInfo::CreateReg(ResultWire, RegWidth, false))
      .addImm(Id.getData()).addOperand(VInstrInfo::CreatePredicate())
      .addOperand(VInstrInfo::CreateTrace());
  G.mapMI2SU(ReadFU, U, Offset, true);
  G.addDummyLatencyEntry(ReadFU);

  return ReadFU;
}

void ChainBreaker::buildFUCtrl(VSUnit *U) {
  MachineInstr *MI = U->getRepresentativePtr();
  // The machine instruction that define the output value.
  assert(MI && "Unexpected BBEntry SU!");
  const TargetRegisterClass *RC
    = VRegisterInfo::getRepRegisterClass(MI->getOpcode());

  DebugLoc dl = MI->getDebugLoc();
  FuncUnitId Id = VInstrInfo::getPreboundFUId(MI);
  MachineBasicBlock::instr_iterator IP = MI;
  ++IP;

  // Insert pseudo copy to model write until finish.
  // FIXME: Insert for others MI rather than Representative MI?
  if (U->getLatency() && VInstrInfo::isWriteUntilFinish(U->getOpcode())) {
    assert(U->getOpcode() == VTM::VOpBRAMTrans &&
           "Only support BRAMTrans at the moment.");
    MachineOperand &MO = MI->getOperand(0);
    unsigned BRAMOpResult = MO.getReg();
    unsigned BRAMPortReg = MRI.createVirtualRegister(RC);
    // Change to the newly allocated register, and kill the new register
    // with VOpPipelineStage.
    MO.ChangeToRegister(BRAMPortReg, true);
    unsigned ResultWidth = VInstrInfo::getBitWidth(MO);
    MachineInstr *PipeStage =
      BuildMI(*MBB, IP, dl, VInstrInfo::getDesc(VTM::VOpPipelineStage))
        .addOperand(VInstrInfo::CreateReg(BRAMOpResult, ResultWidth, true))
        .addOperand(VInstrInfo::CreateReg(BRAMPortReg, ResultWidth)).addImm(Id.getData())
        .addOperand(*VInstrInfo::getPredOperand(MI))
        .addOperand(*VInstrInfo::getTraceOperand(MI));
    assert(U->isControl() && "Only control operation write until finish!");
    G.mapMI2SU(PipeStage, U, U->getLatency() - 1, true);

    // Copy the result 1 cycle later after the value is finished, note that
    // the PipeStage is emit to a data path slot, to delay the VOpReadFU 1
    // cycle later, we need to set its delay to 2, other wise the copy will
    // emit right after the RepLI finish, instead of 1 cycle after the RepLI
    // finish. FIXME: Set the right latency.
    G.addDummyLatencyEntry(PipeStage, 2.0f);
    // The result is not provided by PipeStage.
    // Copy the result of the pipe stage to register if it has any user.
    if (!MRI.use_empty(BRAMOpResult))
      buildReadFU(PipeStage, U, G.getStepsToFinish(PipeStage), Id);
  } else if (!Id.isTrivial()) {
    // The result of InternalCall is hold by the ReadReturn operation, so we
    // do not need to create a new register to hold the result. Otherwise, a
    // new register is needed.
    bool IsWaitOnly = Id.getFUType() == VFUs::CalleeFN;
    buildReadFU(MI, U, G.getStepsToFinish(MI), Id, IsWaitOnly);
    MRI.setRegClass(MI->getOperand(0).getReg(), RC);
  }

  // We also need to disable the FU.
  if (Id.isBound()) {
    assert(Id.getFUType() != VFUs::Mux && "Unexpected FU type!");
    MachineOperand FU = MI->getOperand(0);
    FU.clearParent();
    FU.setIsDef(false);
    MachineInstr *DisableMI =
      BuildMI(*MBB, IP, dl, VInstrInfo::getDesc(VTM::VOpDisableFU))
      .addOperand(FU).addImm(Id.getData())
      .addOperand(*VInstrInfo::getPredOperand(MI))
      .addOperand(*VInstrInfo::getTraceOperand(MI));
    // Add the instruction into the emit list, disable the FU 1 clock later.
    G.mapMI2SU(DisableMI, U, 1);
    G.addDummyLatencyEntry(DisableMI);
  }
}

ValDef *ChainBreaker::getValInReg(ValDef *Def, unsigned Slot) {
  assert(Slot >= Def->FinishSlot && "Read before write detected!");
  // Only return the value at then end of the BB, do not perform cross BB copy.
  if (Def->getParentBB() != MBB) Slot = Def->FinishSlot;

  // Get the value just before ReadSlot.
  while (Def->Next && Def->Next->ChainStart <= Slot)
    Def = Def->Next;

  if (Def->ChainStart != Slot) {
    // Insert the copy operation.
    VSUnit *DefSU = G.lookupSUnit(&Def->MI);
    assert(!DefSU->isDangling() && "Cannot insert Copy for dangling SU!");

    unsigned CopyOffset = Slot - DefSU->getSlot();
    MachineInstr *ReadFU = buildReadFU(&Def->MI, DefSU, CopyOffset);

    // Remember the value definition for the copy.
    ValDef *NewVal = Allocator.Allocate();
    new (NewVal) ValDef(*ReadFU, Slot, Slot);
    NewVal->RegNum = ReadFU->getOperand(0).getReg();
    addValDef(NewVal);

    NewVal->Next = Def->Next;
    Def->Next = NewVal;
    Def = Def->Next;
  }

  return Def;
}

ValDef *ChainBreaker::insertCopyAtSlot(ValDef &SrcDef, unsigned Slot) {
  assert(SrcDef.ChainStart < Slot && SrcDef.Next == 0 && "Cannot insert copy!");

  // Create the copy.
  MachineInstr *MI = &SrcDef.MI;

  unsigned RegWidth = VInstrInfo::getBitWidth(MI->getOperand(0));
  unsigned ResultWire = MI->getOperand(0).getReg();
  unsigned ResultReg = MRI.createVirtualRegister(&VTM::DRRegClass);

  DebugLoc dl = MI->getDebugLoc();
  bool InSameBB = MI->getParent() == MBB;
  MachineBasicBlock::iterator IP = InSameBB ?
                                   llvm::next(MachineBasicBlock::iterator(MI)) :
                                   MBB->getFirstNonPHI();
  // FIXME: Predicate the copy with the codition of the user.
  MachineInstr *ReadFU
    = BuildMI(*MBB, IP, dl, VInstrInfo::getDesc(VTM::VOpReadFU))
    .addOperand(VInstrInfo::CreateReg(ResultReg, RegWidth, true))
    .addOperand(VInstrInfo::CreateReg(ResultWire, RegWidth, false))
    .addImm(FuncUnitId().getData()).addOperand(VInstrInfo::CreatePredicate())
    .addOperand(VInstrInfo::CreateTrace());

  VSUnit *U = InSameBB ? G.lookupSUnit(MI) : G.lookupSUnit(MBB);
  assert(Slot >= U->getSlot() && "Unexpected negative offset!");
  G.mapMI2SU(ReadFU, U, Slot - U->getSlot(), true);
  G.addDummyLatencyEntry(ReadFU);

  // Remember the value definition for the copy.
  ValDef *NewDef = Allocator.Allocate();
  new (NewDef) ValDef(*ReadFU, Slot, Slot);
  NewDef->RegNum = ResultReg;
  addValDef(NewDef);

  return NewDef;
}

ValDef *ChainBreaker::breakChainForAntiDep(ValDef *SrcDef, unsigned ChainEndSlot,
                                           unsigned ReadSlot, unsigned II) {
  // Break the chain to preserve anti-dependencies.
  // FIXME: Calculate the first slot available for inserting the copy.
  unsigned ChainStartSlot = getChainStartAtCurBB(*SrcDef);
  int ChainLength =  ChainEndSlot - ChainStartSlot;

  while (ChainLength > int(II)) {
    assert(SrcDef->FinishSlot <= ReadSlot
           && "Cannot break the chain to preserve anti-dependency!");
    // Try to get the next value define, which hold the value of UseVal in
    // another register in later slots.
    if (!SrcDef->Next || SrcDef->Next->FinishSlot > ReadSlot) {
      unsigned CopySlot = std::min(ChainStartSlot + II, ReadSlot);
      ValDef *SrcNext = SrcDef->Next;
      // FIXME: The may exists next value, but the copy is schedule to a slot
      // which is too late, we may reuse this value by reschedule it.
      SrcDef->Next = 0;
      ValDef *NewVal = insertCopyAtSlot(*SrcDef, CopySlot);
      // Insert the new value into the list.
      NewVal->Next = SrcNext;
      SrcDef->Next = NewVal;
    }

    SrcDef = SrcDef->Next;
    ChainStartSlot = getChainStartAtCurBB(*SrcDef);
    ChainLength =  ChainEndSlot - ChainStartSlot;
  }

  return SrcDef;
}

void ChainBreaker::visitUse(MachineInstr *MI, ValDef &Def, bool IsDangling,
                            unsigned LatestChainEnd) {
  if (MI->isPseudo()) return;

  unsigned CurBBNum = MBB->getNumber();
  bool IsDatapath = VInstrInfo::isDatapath(MI->getOpcode());
  bool IsPipeStage = MI->getOpcode() == VTM::VOpPipelineStage;
  // The control-path operation can read the old value just before the new
  // value come out, calculate the latest slot at which the value can be read
  // by current operation.
  const unsigned ReadSlot = IsDatapath ? Def.ChainStart : Def.ChainStart - 1;
  for (unsigned i = 0, e = MI->getNumOperands(); i != e; ++i) {
    MachineOperand &MO = MI->getOperand(i);

    if (!MO.isReg() || !MO.isUse() || !MO.getReg()) continue;

    ValDef *SrcVal = lookupValDef(MO.getReg());
    bool InSameBB = SrcVal->getParentBB() == MBB;

    // Try to break the chain to shorten the live-interval of the FU.
    // FIXME: Calculate the slot the break the chain, and break the chain lazily.
    int ChainBreakingSlack = getSlackFromFinish(ReadSlot, CurBBNum, *SrcVal);
    if (ChainBreakingSlack >= 0) {
      if (SrcVal->IsChainedWithFU)
        SrcVal = getValInReg(SrcVal, SrcVal->FinishSlot);

      if (IsPipelined && InSameBB
          && LatestChainEnd - SrcVal->ChainStart > CurII
          && LatestChainEnd - ReadSlot < CurII) {

        // Insert the copy to break the chain.
        SrcVal = getValInReg(SrcVal, ReadSlot);
      }
    }

    // If the current MI is dangling, read the latest value.
    if (IsDangling) {
      while (SrcVal->Next) {
        SrcVal = SrcVal->Next;
        assert(getSlackFromFinish(ReadSlot, CurBBNum, *SrcVal) >= 0
               && "Unexpected finish after dangling node start!");
      }
    }

    // Try to break the chain to preserve the anti-dependencies in pipelined
    // block, but ignore the value from other BB, which are invariants.
    if (InSameBB) {
      // The dangling MI is not read by the MI in the same BB, hence there is
      // no anti-dependencies to preserve.
      if (IsPipelined && !IsDangling) {
        unsigned ChainEndSlot = IsDatapath ? Def.FinishSlot : Def.ChainStart;
        // DIRTY HACK: The PipeStage is actually a copy operation, and copy the
        // value 1 slot after its schedule slot.
        if (IsPipeStage) ChainEndSlot = ReadSlot + 1;

        // Break the chain to preserve anti-dependencies.
        SrcVal = breakChainForAntiDep(SrcVal, ChainEndSlot, ReadSlot, CurII);
      }

      // Find the longest chain.
      if (IsDatapath && !IsPipeStage) {
        Def.ChainStart = std::min(Def.ChainStart, SrcVal->ChainStart);
        // FIXME: The finish slot calculation should be improve.
        Def.FinishSlot = std::max(Def.FinishSlot, SrcVal->FinishSlot);
      }

      Def.IsChainedWithFU |= SrcVal->IsChainedWithFU && IsDatapath;
    }

    assert((SrcVal->getParentBB() == MBB || !SrcVal->IsChainedWithFU)
           && "Unexpected cross BB chain.");

    // Update the used register.
    if (MO.getReg() != SrcVal->RegNum)
      MO.ChangeToRegister(SrcVal->RegNum, false);
  }
}

void ChainBreaker::visitDef(MachineInstr *MI, ValDef &Def) {
  for (unsigned i = 0, e = MI->getNumOperands(); i != e; ++i) {
    MachineOperand &MO = MI->getOperand(i);

    if (!MO.isReg() || !MO.isDef() || !MO.getReg()) break;

    Def.RegNum = MO.getReg();
  }
}

static inline bool canForwardSrcVal(unsigned Opcode) {
  // Try to forward the copied value. However, do not forward the VOpDstMux,
  // otherwise we may extend the live-interval of the VOpDstMux and prevent
  // it from binding to the specific Mux.
  return VInstrInfo::isCopyLike(Opcode) && Opcode != VTM::VOpPipelineStage &&
         Opcode != VTM::PHI && Opcode != VTM::VOpDstMux;
}

void ChainBreaker::visit(VSUnit *U) {
  if (U->getFUId().isBound()) {
    assert(U->isControl() && "Unexpected data-path operation!");
    buildFUCtrl(U);
  }

  // Try to break the chain aggressively, to preserve the anti-dependencies in
  // pipelined block.
  unsigned LatestChainEnd = U->getFinSlot();
  if (IsPipelined && U->isDatapath() && !U->isDangling()) {
    unsigned USlot = U->getSlot();

    typedef VSUnit::use_iterator use_it;
    for (use_it I = duse_begin(U), E = duse_end(U); I != E; ++I) {
      VSUnit *User = *I;
      // There is no anti-dependencies from these U to these user.
      if (User->getParentBB() != MBB || User->isDangling() || User->isDatapath())
        continue;

      unsigned ChainEnd = User->getSlot();
      LatestChainEnd = std::max(LatestChainEnd, ChainEnd);
    }
  }

  SmallVector<std::pair<MachineInstr*, int>, 8> Insts;

  for (unsigned i = 0, e = U->num_instrs(); i != e; ++i)
    if (MachineInstr *MI = U->getPtrAt(i)) {
      bool IsDatapath = VInstrInfo::isDatapath(MI->getOpcode());
      int Offset = U->getLatencyAt(i) * 2 + (IsDatapath ? 1 : 0);
      Insts.push_back(std::make_pair(MI, Offset));
    }

  std::sort(Insts.begin(), Insts.end(),
            sort_intra_latency<std::pair<MachineInstr*, int> >);

  typedef SmallVectorImpl<std::pair<MachineInstr*, int> >::iterator iterator;
  for (iterator I = Insts.begin(), E = Insts.end(); I != E; ++I) {
    MachineInstr *MI = I->first;
    unsigned Opcode = MI->getOpcode();
    unsigned SchedSlot = U->getSlot() + I->second / 2;
    unsigned Latency = G.getStepsToFinish(MI);
    bool IsDangling = U->isDangling();
    ValDef Def(*MI, SchedSlot, SchedSlot + Latency);

    visitUse(MI, Def, IsDangling, LatestChainEnd);
    // Do not export the define of VOpMvPhi, which is used by the PHI only.
    if (Opcode == VTM::VOpMvPhi) continue;

    visitDef(MI, Def);

    // This MI define nothing, do not remember the value define.
    if (Def.RegNum == 0) continue;

    if (VInstrInfo::isDatapath(Opcode)) {
      // Fix the register class for the result of data-path operation.
      MRI.setRegClass(Def.RegNum, &VTM::WireRegClass);

      // The result of data-path operation is available 1 slot after the
      // operation start.
      Latency = std::max(1u, Latency);
      Def.FinishSlot = std::max(SchedSlot + Latency, Def.FinishSlot);

    }

    ValDef *NewDef = new (Allocator.Allocate()) ValDef(Def);
    addValDef(NewDef);

    if (!canForwardSrcVal(Opcode)) continue;

    // Link the source and the destinate value define of copy operation together.
    const MachineOperand &CopiedMO = MI->getOperand(1);
    if (CopiedMO.isReg() && CopiedMO.getReg()) {
      ValDef *CopiedDef = lookupValDef(CopiedMO.getReg());
      assert(CopiedDef->Next == 0 && "Value had already copied!");
      CopiedDef->Next = NewDef;
    }
  }
}

void VSchedGraph::insertFUCtrlAndCopy() {
  ChainBreaker Breaker(this);

  for (iterator I = CPSUs.begin(), E = CPSUs.end(); I != E; ++I) {
    VSUnit *U = *I;

    if (U->isBBEntry())
      Breaker.updateMBB(U->getRepresentativePtr().get_mbb());

    Breaker.visit(U);
  }
}

static inline bool top_sort_slot(const VSUnit* LHS, const VSUnit* RHS) {
  if (LHS->getSlot() != RHS->getSlot()) return LHS->getSlot() < RHS->getSlot();

  // In the same slot, control-path operations are ahead of data-path operations.
  if (LHS->isControl() != RHS->isControl()) return LHS->isControl();

  return LHS->getIdx() < RHS->getIdx();
}

static inline bool top_sort_bb_and_slot(const VSUnit* LHS, const VSUnit* RHS) {
  if (LHS->getParentBB() != RHS->getParentBB())
    return LHS->getParentBB()->getNumber() < RHS->getParentBB()->getNumber();

  return top_sort_slot(LHS, RHS);
}

unsigned VSchedGraph::emitSchedule() {
  // Erase the virtual exit root right now, so that we can avoid the special
  // code to handle it.
  assert(CPSUs.back() == getExitRoot() && "ExitRoot at an unexpected position!");
  CPSUs.resize(CPSUs.size() - 1);
  // Insert the delay blocks to fix the inter-bb-latencies.
  insertDelayBlocks();

  // Break the multi-cycles chains to expose more FU sharing opportunities.
  for (iterator I = cp_begin(this), E = cp_end(this); I != E; ++I)
    clearDanglingFlagForTree(*I);

  for (iterator I = dp_begin(this), E = dp_end(this); I != E; ++I) {
    VSUnit *U = *I;

    if (U->isDangling()) {
      MachineBasicBlock *ParentBB = U->getParentBB();
      if (U->getSlot() < getEndSlot(ParentBB)) U->setIsDangling(false);
      else  U->scheduledTo(getEndSlot(U->getParentBB()));
    }

    fixChainedDatapathRC(U);
  }

  // Merge the data-path SU vector to the control-path SU vector.
  CPSUs.insert(CPSUs.end(), DPSUs.begin(), DPSUs.end());
  DPSUs.clear();

  // Sort the SUs by parent BB and its schedule.
  std::sort(CPSUs.begin(), CPSUs.end(), top_sort_bb_and_slot);

  // Break the chains.
  insertFUCtrlAndCopy();

  unsigned MBBStartSlot = EntrySlot;
  iterator to_emit_begin = CPSUs.begin();
  MachineBasicBlock *PrevBB = (*to_emit_begin)->getParentBB();
  for (iterator I = to_emit_begin, E = CPSUs.end(); I != E; ++I) {
    MachineBasicBlock *CurBB = (*I)->getParentBB();
    if (CurBB == PrevBB) continue;

    // If we are entering a new BB, emit the SUs in the previous bb.
    MBBStartSlot = emitSchedule(to_emit_begin, I, MBBStartSlot, PrevBB);
    to_emit_begin = I;
    PrevBB = CurBB;
  }

  // Dont forget the SUs in last BB.
  MBBStartSlot = emitSchedule(to_emit_begin, CPSUs.end(), MBBStartSlot, PrevBB);

  // Re-add the exit roots into the SU vector.
  CPSUs.push_back(getExitRoot());

  return MBBStartSlot;
}

unsigned VSchedGraph::emitSchedule(iterator su_begin, iterator su_end,
                                   unsigned StartSlot, MachineBasicBlock *MBB) {
  unsigned CurSlot = getStartSlot(MBB), EndSlot = getEndSlot(MBB);
  MachineFunction *MF = MBB->getParent();
  VFInfo *VFI = MF->getInfo<VFInfo>();

  // Build bundle from schedule units.
  MicroStateBuilder StateBuilder(*this, MBB, StartSlot);
  DEBUG(dbgs() << "\nEmitting schedule in MBB#" << MBB->getNumber() << '\n';
        dbgs() << "Sorted AllSUs:\n";
              for (iterator I = su_begin, E = su_end; I != E; ++I)
                (*I)->dump();
        );

  for (iterator I = su_begin, E = su_end; I != E; ++I) {
    VSUnit *A = *I;
    DEBUG(dbgs() << "Going to emit: "; A->dump());
    if (A->getSlot() != CurSlot && CurSlot < EndSlot)
      CurSlot = StateBuilder.advanceToSlot(CurSlot, A->getSlot());

    StateBuilder.emitSUnit(A);
  }
  // Build last state.
  assert(!StateBuilder.emitQueueEmpty() && "Expect atoms for last state!");
  StateBuilder.advanceToSlot(CurSlot, CurSlot + 1);
  // Build the dummy terminator.
  BuildMI(MBB, DebugLoc(), MF->getTarget().getInstrInfo()->get(VTM::EndState))
    .addImm(0).addImm(0);

  DEBUG(dbgs() << "After schedule emitted:\n");
  DEBUG(dump());
  DEBUG(dbgs() << '\n');
  // Remember the schedule information.
  unsigned LoopOpSlot = StartSlot + getLoopOpSlot(MBB) - getStartSlot(MBB);
  VFI->rememberTotalSlot(MBB, StartSlot, getTotalSlot(MBB), LoopOpSlot);
  // Advance 1 slots after the endslot.
  return StartSlot + getTotalSlot(MBB) + 1;
}
