//===------------ VSUnit.cpp - Translate LLVM IR to VSUnit  -----*- C++ -*-===//
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
// Compute the detail ctrlop to ctrlop latency (in cycle ratio) information.
//
//===----------------------------------------------------------------------===//
#include "vtm/DetailLatencyInfo.h"
#include "llvm/ADT/PostOrderIterator.h"

using namespace llvm;

static cl::opt<bool>
EnableBLC("vtm-enable-blc",
          cl::desc("Enable bit-level chaining"),
          cl::init(true));

INITIALIZE_PASS_BEGIN(DetialLatencyInfo, "detail-latency-info",
                      "Calculating the latency of instructions",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(MachineBasicBlockTopOrder)
INITIALIZE_PASS_END(DetialLatencyInfo, "detail-latency-info",
                    "Calculating the latency of instructions",
                    false, true)

char DetialLatencyInfo::ID = 0;
const float DetialLatencyInfo::DeltaLatency = FLT_EPSILON * 8.0f;

DetialLatencyInfo::DetialLatencyInfo() : MachineFunctionPass(ID), MRI(0) {
  initializeDetialLatencyInfoPass(*PassRegistry::getPassRegistry());
}

Pass *llvm::createDetialLatencyInfoPass() {
  return new DetialLatencyInfo();
}

void DetialLatencyInfo::getAnalysisUsage(AnalysisUsage &AU) const {
  MachineFunctionPass::getAnalysisUsage(AU);
  AU.addRequiredID(MachineBasicBlockTopOrderID);
  AU.setPreservesAll();
}

typedef DetialLatencyInfo::DepLatInfoTy DepLatInfoTy;
typedef DepLatInfoTy::mapped_type LatInfoTy;

static void updateLatency(DepLatInfoTy &CurLatInfo, InstPtrTy Src,
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
  DepLatInfoTy::mapped_type &V = CurLatInfo[Src];
  float &OldLSBLatency = V.second;
  OldLSBLatency = std::max(OldLSBLatency, LSBLatency);
  //assert(LSBLatency <= MSBLatency && "Broken latency pair!");
  float &OldMSBLatency = V.first;
  OldMSBLatency = std::max(OldMSBLatency, MSBLatency);
}

static LatInfoTy getMSB2LSBLatency(float SrcMSBLatency, float SrcLSBLatency,
                                   float TotalLatency, float PerBitLatency) {
  float MSBLatency = PerBitLatency + SrcMSBLatency;
  float LSBLatency = std::max(PerBitLatency + SrcLSBLatency,
                              TotalLatency + SrcMSBLatency);
  return std::make_pair(MSBLatency, LSBLatency);
}

static LatInfoTy getCmpLatency(float SrcMSBLatency, float SrcLSBLatency,
                               float TotalLatency, float PerBitLatency) {
  LatInfoTy LatInfo = getMSB2LSBLatency(SrcMSBLatency, SrcLSBLatency,
                                        TotalLatency, PerBitLatency);
  // We need to get the worst delay because the cmps only have 1 bit output.
  float WorstLat = std::max(LatInfo.first, LatInfo.second);
  return std::make_pair(WorstLat, WorstLat);
}

static LatInfoTy getLSB2MSBLatency(float SrcMSBLatency, float SrcLSBLatency,
                                   float TotalLatency, float PerBitLatency) {
  float MSBLatency = std::max(TotalLatency + SrcLSBLatency,
                              PerBitLatency + SrcMSBLatency);
  float LSBLatency = PerBitLatency + SrcLSBLatency;
  return std::make_pair(MSBLatency, LSBLatency);
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

static bool NeedExtraStepToLatchResult(const MachineInstr *MI,
                                       const MachineRegisterInfo &MRI,
                                       float Latency) {
  if (MI->getNumOperands() == 0) return false;

  const MachineOperand &MO = MI->getOperand(0);
  if (!MO.isReg() || !MO.isDef()) return false;

  assert(MO.getReg() && "Broken instruction defining register 0!");
  return Latency != 0.0f && VInstrInfo::isWriteUntilFinish(MI->getOpcode())
         && !MRI.use_empty(MO.getReg());
}

namespace {
struct BitSliceLatencyFN {
  unsigned OperandSize, UB, LB;

  BitSliceLatencyFN(const MachineInstr *BitSliceOp)
    : OperandSize(VInstrInfo::getBitWidth(BitSliceOp->getOperand(1))),
    UB(BitSliceOp->getOperand(2).getImm()),
    LB(BitSliceOp->getOperand(3).getImm()) {
      assert(BitSliceOp->getOpcode() == VTM::VOpBitSlice && "Not a bitslice!");
  }

  BitSliceLatencyFN(unsigned operandSize, unsigned ub)
    : OperandSize(operandSize), UB(ub), LB(0) {}

  LatInfoTy operator()(float SrcMSBLatency, float SrcLSBLatency,
                       float /*TotalLatency*/, float /*PerBitLatency*/) {
      return getBitSliceLatency(OperandSize, UB, LB, SrcMSBLatency, 
                                SrcLSBLatency);
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

static LatInfoTy getBitSliceLatency(unsigned OperandSize,
                                    unsigned UB, unsigned LB,
                                    float SrcMSBLatency, float SrcLSBLatency) {
  return BitSliceLatencyFN::getBitSliceLatency(OperandSize, UB, LB,
                                               SrcMSBLatency, SrcLSBLatency);
}

static LatInfoTy getMSBAndLSBLatency(float SrcMSBLatency, float SrcLSBLatency,
                                     float MSBInc, float LSBInc) {
  return std::make_pair(SrcMSBLatency + MSBInc, SrcLSBLatency + LSBInc);
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

static float adjustChainingLatency(float Latency, unsigned SrcOpcode,
                                   unsigned DstOpcode) {
  bool SrcWriteUntilFInish = VInstrInfo::isWriteUntilFinish(SrcOpcode);
  bool DstReadAtEmit = VInstrInfo::isReadAtEmit(DstOpcode);

  float Delta = DetialLatencyInfo::DeltaLatency;

  if (DstReadAtEmit && SrcWriteUntilFInish) {
    if (SrcOpcode == VTM::VOpMvPhi) {
      assert((DstOpcode == TargetOpcode::PHI || DstOpcode == VTM::VOpMvPhi
              || VInstrInfo::getDesc(DstOpcode).isTerminator())
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

float DetialLatencyInfo::computeLatencyFor(const MachineInstr *MI) {
  float TotalLatency = VInstrInfo::getDetialLatency(MI);
  // Remember the latency from all MI's dependence leaves.
  CachedLatencies.insert(std::make_pair(MI, TotalLatency));
  return TotalLatency;
}

bool DetialLatencyInfo::propagateFromLSB2MSB(unsigned Opcode) {
  switch (Opcode) {
  default: break;
  case VTM::VOpAdd_c:
  case VTM::VOpMultLoHi_c:
  case VTM::VOpMult_c:
  case VTM::VOpAdd:
  case VTM::VOpMult:
  case VTM::VOpMultLoHi:
    return true;
  }

  return false;
}

template<bool IsCtrlDep>
LatInfoTy DetialLatencyInfo::getLatencyToDst(const MachineInstr *SrcMI,
                                             unsigned DstOpcode,
                                             unsigned UB, unsigned LB) {
  float SrcMSBLatency = getCachedLatencyResult(SrcMI);
  float SrcLSBLatency = SrcMSBLatency;
  if (!IsCtrlDep || NeedExtraStepToLatchResult(SrcMI, *MRI, SrcMSBLatency)) {
    SrcMSBLatency = adjustChainingLatency(SrcMSBLatency, SrcMI->getOpcode(),
      DstOpcode);
    // If we are only reading the lower part of the result of SrcMI, and the
    // LSB of the result of SrcMI are available before SrcMI completely finish,
    // we can read the subword before SrcMI finish.
    if (UB && propagateFromLSB2MSB(SrcMI->getOpcode())) {
      unsigned SrcSize = VInstrInfo::getBitWidth(SrcMI->getOperand(0));
      // DirtyHack: Ignore the invert flag.
      if (SrcSize != 1 && UB != 3) {
        assert(UB <= SrcSize && UB > LB  && "Bad bitslice!");
        SrcLSBLatency = std::max(SrcMSBLatency / SrcSize, VFUs::LutLatency);
        tie(SrcMSBLatency, SrcLSBLatency) =
          getBitSliceLatency(SrcSize, UB, LB, SrcMSBLatency, SrcLSBLatency);
      }
    }
  } else {
    // IsCtrlDep
    SrcMSBLatency = std::max(0.0f, SrcMSBLatency - DetialLatencyInfo::DeltaLatency);
    SrcLSBLatency = SrcMSBLatency;
  }

  return std::make_pair(SrcMSBLatency, SrcLSBLatency);
}

template<bool IsCtrlDep>
bool DetialLatencyInfo::buildDepLatInfo(const MachineInstr *SrcMI,
                                        DepLatInfoTy &CurLatInfo,
                                        unsigned OperandWidth,
                                        float OperandDelay, unsigned DstOpcode){
  const DepLatInfoTy *SrcLatInfo = getDepLatInfo(SrcMI);
  // Latency information not available, the SrcMI maybe in others BB, no need
  // to compute cross BB latency.
  if (SrcLatInfo == 0) return false;

  float SrcMSBLatency
    = getLatencyToDst<IsCtrlDep>(SrcMI, DstOpcode, OperandWidth, 0).first;

  // Try to compute the per-bit latency.
  float PerBitLatency = 0.0f;
  if (OperandWidth)
    PerBitLatency = std::max(SrcMSBLatency / OperandWidth, VFUs::LutLatency);

  unsigned Opcode = VTM::INSTRUCTION_LIST_END;
  bool isCtrl = VInstrInfo::isControl(SrcMI->getOpcode());
  if (EnableBLC) Opcode = SrcMI->getOpcode();

  switch (Opcode) {
  default:
    if (isCtrl)
      updateLatency(CurLatInfo, SrcMI, SrcMSBLatency, SrcMSBLatency);
    else
      accumulateDatapathLatency(CurLatInfo, SrcLatInfo, SrcMSBLatency,
                                PerBitLatency, getWorstLatency);
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
  case VTM::VOpBitSlice: {
    // Forward the latency from the source of the bitslice, and increase the
    // MSBLatency and LSBLatency according to the upper bound and lowerbound
    // of the bitslice.
    float SrcLSBLatency = SrcMSBLatency;
    if (SrcMI->getOperand(1).isReg()) {
      MachineInstr *BitSliceSrc = MRI->getVRegDef(SrcMI->getOperand(1).getReg());
      assert(BitSliceSrc && "The source MachineInstr for BitSlice not found!");
      const DepLatInfoTy *BitSliceSrcLatInfo = getDepLatInfo(BitSliceSrc);
      if (BitSliceSrcLatInfo) {
        // Forward the SrcLatInfo of the source of the bitslice.
        SrcLatInfo = BitSliceSrcLatInfo;
        // Update SrcMSBLatency and SrcLSBLatency according to the upper bound
        // and the lower bound of the bitslice.
        unsigned BitSliceUB = SrcMI->getOperand(2).getImm(),
                 BitSliceLB = SrcMI->getOperand(3).getImm();
        tie(SrcMSBLatency, SrcLSBLatency)
          = getLatencyToDst<IsCtrlDep>(BitSliceSrc, DstOpcode,
                                       BitSliceUB, BitSliceLB);
      }
    }

    accumulateDatapathLatency(CurLatInfo, SrcLatInfo, SrcMSBLatency,
                              SrcLSBLatency, getMSBAndLSBLatency);
    break;
  case VTM::VOpICmp_c:
    accumulateDatapathLatency(CurLatInfo, SrcLatInfo, SrcMSBLatency,
                              PerBitLatency, getCmpLatency);
    break;
  case VTM::VOpICmp:
    // Result bits are computed from MSB to LSB.
    updateLatency(CurLatInfo, SrcMI, PerBitLatency, SrcMSBLatency);
    break;
  }

  return true;
}

const DetialLatencyInfo::DepLatInfoTy &
DetialLatencyInfo::addInstrInternal(const MachineInstr *MI,
                                    DepLatInfoTy &CurLatInfo) {
  const MachineBasicBlock *CurMBB = MI->getParent();

  const MCInstrDesc &TID = MI->getDesc();
  bool IsControl = VInstrInfo::isControl(TID.getOpcode());

  // Iterate from use to define, ignore the the incoming value of PHINodes.
  // Because the incoming value may be not visited yet.
  for (unsigned i = 0, e = MI->isPHI() ? 1 : MI->getNumOperands(); i != e; ++i){
    const MachineOperand &MO = MI->getOperand(i);

    // Only care about a use register.
    if (!MO.isReg() || MO.isDef() || MO.getReg() == 0)
      continue;

    unsigned SrcReg = MO.getReg();
    MachineInstr *SrcMI = MRI->getVRegDef(SrcReg);
    assert(SrcMI && "Virtual register use without define!");

    // Do we ignore phi as dependence? Also ignore self loop.
    if (SrcMI == MI) continue;
    unsigned OpSize = VInstrInfo::getBitWidth(MO);

    float OpDelay = 0.0f;
    if (i < TID.getNumOperands() && TID.OpInfo[i].isPredicate()) {
      OpDelay = VFUs::ClkEnSelLatency;
    } else
      OpDelay = VInstrInfo::getOperandLatency(MI, i);

    if (!buildDepLatInfo<false>(SrcMI, CurLatInfo, OpSize, OpDelay,
                                MI->getOpcode()))
      continue;

    // If we build the Latency Info for SrcMI successfully, that means SrcMI
    // have user now.
    if (CurMBB != SrcMI->getParent()) continue;
  }

  // Compute the latency of MI.
  float Latency = computeLatencyFor(MI);

  // We will not get any latency information if a datapath operation do not
  // depends any control operation in the same BB.
  if (CurLatInfo.empty() && !IsControl) {
    float latency = std::max(Latency, DetialLatencyInfo::DeltaLatency);
    CurLatInfo.insert(std::make_pair(CurMBB, std::make_pair(latency, latency)));
  }

  return CurLatInfo;
}

void DetialLatencyInfo::buildExitMIInfo(const MachineInstr *ExitMI,
                                        DepLatInfoTy &Info,
                                        MISetTy &MIsToWait, MISetTy &MIsToRead){
  typedef MISetTy::const_iterator exit_it;
  // Exiting directly, no need to read the result fore fore exting.
  for (exit_it I = MIsToWait.begin(), E = MIsToWait.end(); I != E; ++I)
    buildDepLatInfo<true>(*I, Info, 0, 0.0, ExitMI->getOpcode());

  // Exiting via data-path operation, the value need to be read before exiting.
  for (exit_it I = MIsToRead.begin(), E = MIsToRead.end(); I != E; ++I)
    buildDepLatInfo<false>(*I, Info, 0, 0.0, ExitMI->getOpcode());
}

float DetialLatencyInfo::getChainingLatency(const MachineInstr *SrcInstr,
                                            const MachineInstr *DstInstr) const{
  // Compute the latency correspond to detail slot.
  float latency = getMaxLatency(SrcInstr);
  return adjustChainingLatency(latency, SrcInstr->getOpcode(),
                               DstInstr->getOpcode());
}

bool DetialLatencyInfo::runOnMachineFunction(MachineFunction &MF) {
  MRI = &MF.getRegInfo();

  typedef MachineFunction::iterator iterator;
  typedef MachineBasicBlock::instr_iterator instr_iterator;
  for (iterator BI = MF.begin(), BE = MF.end(); BI != BE; ++BI)
    for (instr_iterator I = BI->instr_begin(), E = BI->instr_end(); I != E; ++I)
      addInstrInternal(I,  LatencyMap[I]);

  return false;
}
