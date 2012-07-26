//===- BitLevelOpt.cpp - Implement bit level optimizations on SelectionDAG -===//
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
// This pass try to lower some arithmetic and logic operations to much more
// cheaper bitwise operation which can be express by the combination of bit
// slice selection, bit concation and bit repeation.
// for example, logic operation shift left:
//   a[31:0] = b[31:0] << 2 => a[31:0] = {b[29:0], 2'b00 }
//
// After lowering to bit level operation, more optimization opportunity may be
// exposed.
//
//===----------------------------------------------------------------------===//

#include "vtm/VISelLowering.h"
#include "vtm/Utilities.h"

#include "llvm/Function.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/PseudoSourceValue.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/CommandLine.h"
using namespace llvm;


using namespace llvm;
cl::opt<bool> EnableArithOpt("vtm-enable-arith-opt",
                             cl::init(true), cl::Hidden);

template<typename Func>
inline static
SDValue commuteAndTryAgain(SDNode *N, TargetLowering::DAGCombinerInfo &DCI,
                           bool Commuted, Func F) {

  if (Commuted) return SDValue();

  // If we not try to exchange the operands, exchange and try again.
  return F(N, DCI, !Commuted);
}

inline static bool isAllOnesValue(uint64_t Val, unsigned SizeInBits) {
  uint64_t AllOnes = getBitSlice64(~uint64_t(0), SizeInBits);
  return getBitSlice64(Val, SizeInBits) == AllOnes;
}

inline static bool isNullValue(uint64_t Val, unsigned SizeInBits) {
  return getBitSlice64(Val, SizeInBits) == 0;
}

inline static unsigned ExtractConstant(SDValue V, uint64_t &Val) {
  if (ConstantSDNode *CSD =dyn_cast<ConstantSDNode>(V)) {
    unsigned SizeInBits = V.getValueSizeInBits();
    Val = getBitSlice64(CSD->getZExtValue(), SizeInBits);
    return SizeInBits;
  }

  if (V.getOpcode() == VTMISD::BitSlice)
    if (ConstantSDNode *CSD =dyn_cast<ConstantSDNode>(V.getOperand(0))) {
      unsigned UB = V.getConstantOperandVal(1), LB = V.getConstantOperandVal(2);
      Val = getBitSlice64(CSD->getZExtValue(), UB, LB);
      return UB - LB;
    }

  return 0;
}

inline static bool IsConstant(SDValue V) {
  uint64_t Val;
  return ExtractConstant(V, Val) != 0;
}

// Lower shifting a constant amount:
//   a[31:0] = b[31:0] << 2 => a[31:0] = {b[29:0], 2'b00 }
static
SDValue PerformShiftImmCombine(SDNode *N, TargetLowering::DAGCombinerInfo &DCI) {
  SelectionDAG &DAG = DCI.DAG;
  SDValue Op = N->getOperand(0);
  SDValue ShiftAmt = N->getOperand(1);
  uint64_t ShiftVal = 0;
  DebugLoc dl = N->getDebugLoc();

  // Limit the shift amount.
  if (!ExtractConstant(ShiftAmt, ShiftVal)) return SDValue();

  unsigned SrcSize = VTargetLowering::computeSizeInBits(Op);
  EVT VT = N->getValueType(0);
  unsigned PaddingSize = ShiftVal;
  EVT PaddingVT = EVT::getIntegerVT(*DAG.getContext(), PaddingSize);
  // Create this padding bits as target constant.
  SDValue PaddingBits;

  switch (N->getOpcode()) {
  default:
    PaddingBits = DAG.getConstant(0, PaddingVT, true);
    break;
  case ISD::SRA:
  case ISD::SRL:
  case ISD::SHL:
    return SDValue();
  case ISD::ROTL:
    PaddingBits = VTargetLowering::getBitSlice(DAG, dl, Op, SrcSize, SrcSize - PaddingSize);
    DCI.AddToWorklist(PaddingBits.getNode());
    break;
  case ISD::ROTR:
    PaddingBits = VTargetLowering::getBitSlice(DAG, dl, Op, PaddingSize, 0);
    DCI.AddToWorklist(PaddingBits.getNode());
    break;
  }

  switch (N->getOpcode()) {
  case ISD::ROTL:
    // Discard the higher bits of src.
    Op = VTargetLowering::getBitSlice(DAG, dl, Op, SrcSize - PaddingSize, 0);
    DCI.AddToWorklist(Op.getNode());
    return DAG.getNode(VTMISD::BitCat, dl, VT, Op, PaddingBits);
  case ISD::ROTR:
    // Discard the lower bits of src.
    Op = VTargetLowering::getBitSlice(DAG, dl, Op, SrcSize, PaddingSize);
    DCI.AddToWorklist(Op.getNode());
    return DAG.getNode(VTMISD::BitCat, dl, VT, PaddingBits, Op);
  default:
    assert(0 && "Bad opcode!");
    return SDValue();
  }
}

static bool ExtractBitMaskInfo(int64_t Val, unsigned SizeInBits,
                               unsigned &UB, unsigned &LB) {
  if (isShiftedMask_64(Val) || isMask_64(Val)) {
    LB = CountTrailingZeros_64(Val);
    UB = std::min(SizeInBits, 64 - CountLeadingZeros_64(Val));
    return true;
  }

  return false;
}

//===--------------------------------------------------------------------===//
// Bit level manipulate function for the BitSlice/BitCat based bit level
// optimization framework.
static void SplitLHSAt(TargetLowering::DAGCombinerInfo &DCI, unsigned SplitBit,
                       SDValue LHS, SDValue &LHSLo, SDValue &LHSHi,
                       SDValue &RHSLo, SDValue &RHSHi)  {
  SelectionDAG &DAG = DCI.DAG;

  LHSLo = VTargetLowering::getBitSlice(DAG, LHS->getDebugLoc(), LHS, SplitBit, 0);
  DCI.AddToWorklist(LHSLo.getNode());
  // Adjust the bitwidth of constant to match LHS's width.
  if (LHSLo.getValueSizeInBits() != RHSLo.getValueSizeInBits()) {
    RHSLo = VTargetLowering::getBitSlice(DAG, RHSLo->getDebugLoc(), RHSLo,
                                         SplitBit, 0,
                                         LHSLo.getValueSizeInBits());
    DCI.AddToWorklist(RHSLo.getNode());
  }

  unsigned LHSSizeInBit = VTargetLowering::computeSizeInBits(LHS);
  LHSHi = VTargetLowering::getBitSlice(DAG, LHS->getDebugLoc(), LHS,
                                       LHSSizeInBit, SplitBit);
  DCI.AddToWorklist(LHSHi.getNode());
  // Adjust the bitwidth of constant to match RHS's width.
  if (LHSHi.getValueSizeInBits() != RHSHi.getValueSizeInBits()) {
    RHSHi = VTargetLowering::getBitSlice(DAG, RHSHi->getDebugLoc(), RHSHi,
                                         LHSSizeInBit - SplitBit, 0,
                                         LHSHi.getValueSizeInBits());
    DCI.AddToWorklist(RHSHi.getNode());
  }
}

static void SplitOpAtRHSConstant(TargetLowering::DAGCombinerInfo &DCI,
                                 unsigned SplitBit,
                                 SDValue LHS, SDValue &LHSLo, SDValue &LHSHi,
                                 SDValue RHS, SDValue &RHSLo, SDValue &RHSHi) {
  SelectionDAG &DAG = DCI.DAG;
  // Build the lower part.
  LHSLo = VTargetLowering::getBitSlice(DAG, LHS->getDebugLoc(), LHS, SplitBit, 0);
  DCI.AddToWorklist(LHSLo.getNode());
  RHSLo = VTargetLowering::getBitSlice(DAG, RHS->getDebugLoc(), RHS, SplitBit, 0,
                                       LHSLo.getValueSizeInBits());
  DCI.AddToWorklist(RHSLo.getNode());
  // And the higher part.
  LHSHi = VTargetLowering::getBitSlice(DAG, LHS->getDebugLoc(), LHS,
                                       VTargetLowering::computeSizeInBits(LHS),
                                       SplitBit);
  DCI.AddToWorklist(LHSHi.getNode());
  RHSHi = VTargetLowering::getBitSlice(DAG, RHS->getDebugLoc(), RHS,
                                       VTargetLowering::computeSizeInBits(LHS),
                                       SplitBit,
                                       LHSHi.getValueSizeInBits());
  DCI.AddToWorklist(RHSHi.getNode());
}

// Simply concat higher part and lower part.
inline static SDValue ConcatBits(TargetLowering::DAGCombinerInfo &DCI,
                                 SDNode *N, SDValue Hi, SDValue Lo) {
  return DCI.DAG.getNode(VTMISD::BitCat, N->getDebugLoc(), N->getVTList(),
                         Hi, Lo);
}

inline static SDValue LogicOpBuildLowPart(TargetLowering::DAGCombinerInfo &DCI,
                                          SDNode *N, SDValue LHS, SDValue RHS,
                                          bool Commuted) {
  SDValue Lo = DCI.DAG.getNode(N->getOpcode(), N->getDebugLoc(),
                               LHS.getValueType(), LHS, RHS);
  DCI.AddToWorklist(Lo.getNode());
  return Lo;
}

inline static SDValue LogicOpBuildHighPart(TargetLowering::DAGCombinerInfo &DCI,
                                           SDNode *N, SDValue LHS, SDValue RHS,
                                           SDValue Lo, bool Commuted) {
  SDValue Hi = DCI.DAG.getNode(N->getOpcode(), N->getDebugLoc(),
                               LHS.getValueType(), LHS, RHS);
  DCI.AddToWorklist(Hi.getNode());
  return Hi;
}

inline static SDValue ExtractBitSlice(TargetLowering::DAGCombinerInfo &DCI,
                                      SDValue Op, unsigned UB, unsigned LB) {
  SDValue V = VTargetLowering::getBitSlice(DCI.DAG, Op->getDebugLoc(),
                                           Op, UB, LB);
  DCI.AddToWorklist(V.getNode());
  return V;
}

inline static SDValue GetZerosBitSlice(TargetLowering::DAGCombinerInfo &DCI,
                                       SDValue Op, unsigned UB, unsigned LB) {
  EVT HiVT = EVT::getIntegerVT(*DCI.DAG.getContext(), UB - LB);
  return DCI.DAG.getTargetConstant(0, HiVT);
}

inline static SDValue GetOnesBitSlice(TargetLowering::DAGCombinerInfo &DCI,
                                      SDValue Op,  unsigned UB, unsigned LB) {
  EVT VT = EVT::getIntegerVT(*DCI.DAG.getContext(), UB - LB);
  return DCI.DAG.getTargetConstant(~uint64_t(0), VT);
}

inline static SDValue FlipBitSlice(TargetLowering::DAGCombinerInfo &DCI,
                                   SDValue Op, unsigned UB, unsigned LB) {
  SDValue V = VTargetLowering::getBitSlice(DCI.DAG, Op->getDebugLoc(),
                                           Op, UB, LB);
  DCI.AddToWorklist(V.getNode());
  V = VTargetLowering::getNot(DCI.DAG, Op->getDebugLoc(), V);
  DCI.AddToWorklist(V.getNode());
  return V;
}

// FIXME: Allow custom bit concation.
template<typename ExtractBitsFunc>
static SDValue ExtractBits(SDValue Op, int64_t Mask,
                           TargetLowering::DAGCombinerInfo &DCI,
                           ExtractBitsFunc ExtractEnabledBits,
                           ExtractBitsFunc ExtractDisabledBits,
                           bool flipped = false) {
  DebugLoc dl = Op->getDebugLoc();
  SelectionDAG &DAG = DCI.DAG;
  EVT VT = Op.getValueType();
  unsigned SizeInBits = VTargetLowering::computeSizeInBits(Op);

  // DirtyHack: ExtractBitMaskInfo cannot handle 0.
  if (Mask == 0)
    return ExtractDisabledBits(DCI, Op, SizeInBits, 0);

  unsigned UB, LB;
  if (ExtractBitMaskInfo(Mask, SizeInBits, UB, LB)) {
    // All enable?
    if (UB - LB == SizeInBits)
      return ExtractEnabledBits(DCI, Op, SizeInBits, 0);

    // Handle and with a bit mask like A & 0xf0, simply extract the
    // corresponding bits.
    // Handle or with a bit mask like A | 0xf0, simply set the corresponding
    // bits to 1.

    SDValue MidBits = ExtractEnabledBits(DCI, Op, UB, LB);
    DCI.AddToWorklist(MidBits.getNode());

    SDValue HiBits;
    if (SizeInBits != UB) {
      HiBits = ExtractDisabledBits(DCI, Op, SizeInBits, UB);
      DCI.AddToWorklist(HiBits.getNode());
    }

    if(LB == 0) {
      assert(SizeInBits != UB && "Unexpected all one value!");
      SDValue Result = DAG.getNode(VTMISD::BitCat, dl, VT, HiBits, MidBits);
      assert(VTargetLowering::computeSizeInBits(Result) == SizeInBits
             && "Bit widht not match!");
      return Result;
    }

    // Build the lower part.
    SDValue LoBits = ExtractDisabledBits(DCI, Op, LB, 0);
    DCI.AddToWorklist(LoBits.getNode());

    SDValue Lo = DAG.getNode(VTMISD::BitCat, dl, VT, MidBits, LoBits);
    if (UB == SizeInBits) {
      assert(VTargetLowering::computeSizeInBits(Lo) == SizeInBits
             && "Bit widht not match!");
      return Lo;
    }

    DCI.AddToWorklist(Lo.getNode());
    SDValue Result = DAG.getNode(VTMISD::BitCat, dl, VT, HiBits, Lo);
    assert(VTargetLowering::computeSizeInBits(Result) == SizeInBits
           && "Bit widht not match!");
    return Result;
  }

  if (flipped) return SDValue();

  // Flip the mask and try again.
  return ExtractBits(Op, ~Mask, DCI,
                     ExtractDisabledBits, ExtractEnabledBits, !flipped);
}

static
unsigned GetBitCatCommonSplitBit(SDNode *N, TargetLowering::DAGCombinerInfo &DCI,
                                 SDValue LHS, SDValue &LHSLo, SDValue &LHSHi,
                                 SDValue RHS, SDValue &RHSLo, SDValue &RHSHi){
  if (LHS->getOpcode() != VTMISD::BitCat || RHS->getOpcode() != VTMISD::BitCat)
    return 0;

  unsigned LHSLoBits = VTargetLowering::computeSizeInBits(LHS->getOperand(1));
  unsigned RHSLoBits = VTargetLowering::computeSizeInBits(RHS->getOperand(1));
  if (LHSLoBits != RHSLoBits) return 0;

  unsigned LHSHiBits = VTargetLowering::computeSizeInBits(LHS->getOperand(0));
  unsigned RHSHiBits = VTargetLowering::computeSizeInBits(RHS->getOperand(0));
  assert(LHSHiBits == RHSHiBits && "Hi size do not match!");

  LHSHi = LHS.getOperand(0);
  RHSHi = RHS.getOperand(0);
  // TODO: Force they have the same type!
  if (LHSHi.getValueType() != RHSHi.getValueType())
    return 0;

  LHSLo = LHS.getOperand(1);
  RHSLo = RHS.getOperand(1);
  // TODO: Force they have the same type!
  if (LHSLo.getValueType() != RHSLo.getValueType())
    return 0;

  return LHSLoBits;
}

static unsigned CountLeadingZerosSplitBit(uint64_t Val, unsigned SizeInBit) {
  unsigned LeadingZerosSplitBit = CountLeadingZeros_64(Val);
  if (LeadingZerosSplitBit == 0) return 0;

  LeadingZerosSplitBit = 64 - LeadingZerosSplitBit;
  if (LeadingZerosSplitBit == SizeInBit) return 0;

  return LeadingZerosSplitBit;
}

static unsigned CountLeadingOnesSplitBit(uint64_t Val, unsigned SizeInBit) {
  Val = SignExtend64(Val, SizeInBit);
  unsigned LeadingOnesSplitBit = CountLeadingOnes_64(Val);
  if (LeadingOnesSplitBit) LeadingOnesSplitBit = 64 - LeadingOnesSplitBit;

  return LeadingOnesSplitBit;
}

static unsigned GetDefaultSplitBit(SDNode *N,
                                   TargetLowering::DAGCombinerInfo &DCI,
                                   SDValue LHS, SDValue &LHSLo, SDValue &LHSHi,
                                   SDValue RHS, SDValue &RHSLo, SDValue &RHSHi){
  if (RHS->getOpcode() == VTMISD::BitCat) {
    unsigned RHSLoBits = VTargetLowering::computeSizeInBits(RHS->getOperand(1));

    RHSHi = RHS.getOperand(0);
    RHSLo = RHS.getOperand(1);

    // Only promote the ADDE when we can drop the lower part.
    uint64_t RHSLoVal = 0, RHSHiVal = 0;
    if (!ExtractConstant(RHSLo, RHSLoVal) && !ExtractConstant(RHSHi, RHSHiVal))
      // If constant not found, is there a common bitcat split bit?
      return GetBitCatCommonSplitBit(N, DCI,
                                     LHS, LHSLo, LHSHi,
                                     RHS, RHSLo, RHSHi);

    //if (!isNullValue(RHSLoVal, RHSLoBits) && !isNullValue(RHSHiVal, RHSHiBits)&&
    //    !isAllOnesValue(RHSLoVal, RHSLoBits) && !isAllOnesValue(RHSHiVal, RHSHiBits))
    //  return 0;

    SplitLHSAt(DCI, RHSLoBits, LHS, LHSLo, LHSHi, RHSLo, RHSHi);

    return RHSLoBits;
  }

  uint64_t RHSVal = 0;
  if (unsigned SizeInBit = ExtractConstant(RHS, RHSVal)) {
    unsigned TrailingZerosSplitBit = CountTrailingZeros_64(RHSVal);
    unsigned LeadingZerosSplitBit = CountLeadingZerosSplitBit(RHSVal, SizeInBit);

    unsigned TrailingOnesSplitBit = CountTrailingOnes_64(RHSVal);
    unsigned LeadingOnesSplitBit = CountLeadingOnesSplitBit(RHSVal, SizeInBit);

    unsigned SplitBit =
      std::max(std::max(TrailingZerosSplitBit, LeadingZerosSplitBit),
               std::max(TrailingOnesSplitBit, LeadingOnesSplitBit));
    assert(SplitBit < SizeInBit && "Bad split bit!");

    SplitOpAtRHSConstant(DCI, SplitBit,
                         LHS, LHSLo, LHSHi,
                         RHS, RHSLo, RHSHi);

    return SplitBit;
  }

  return 0;
}

// Promote bitcat in dag, like {a , b} & {c, d} => {a & c, b &d } or something.
template<typename ConcatBitsFunc,
         typename BuildLowPartFunc,
         typename BuildHighPartFunc,
         typename GetSplitBitFunc>
static
SDValue PromoteBinOpBitCat(SDNode *N, TargetLowering::DAGCombinerInfo &DCI,
                           // For some SDNode, we cannot call getbitslice
                           // on hi and lo.
                           bool GetBitSliceForHiLo,
                           ConcatBitsFunc &ConcatBits,
                           BuildLowPartFunc &BuildLowPart,
                           BuildHighPartFunc &BuildHighPart,
                           GetSplitBitFunc &GetSplitBit,
                           bool Commuted = false) {
  SDValue LHS = N->getOperand(0 ^ Commuted), RHS = N->getOperand(1 ^ Commuted);

  unsigned SizeInBit = VTargetLowering::computeSizeInBits(SDValue(N, 0));
  SDValue LHSLo, RHSLo, LHSHi, RHSHi;
  unsigned LoBitWidth = GetSplitBit(N, DCI,
                                    LHS, LHSLo, LHSHi,
                                    RHS, RHSLo, RHSHi);
  if (LoBitWidth == 0) return SDValue();

  unsigned HiBitWidth = SizeInBit - LoBitWidth;

  SelectionDAG &DAG = DCI.DAG;
  DebugLoc dl = N->getDebugLoc();

  SDValue Lo = BuildLowPart(DCI, N, LHSLo, RHSLo, Commuted);
  if (GetBitSliceForHiLo) {
    Lo = VTargetLowering::getBitSlice(DAG, dl, Lo, LoBitWidth, 0);
    DCI.AddToWorklist(Lo.getNode());
  }

  SDValue Hi = BuildHighPart(DCI, N, LHSHi, RHSHi, Lo, Commuted);
  if (GetBitSliceForHiLo) {
    Hi = VTargetLowering::getBitSlice(DAG, dl, Hi, HiBitWidth, 0);
    DCI.AddToWorklist(Hi.getNode());
  }
  return ConcatBits(DCI, N, Hi, Lo);
}



static
SDValue PerformLogicCombine(SDNode *N, TargetLowering::DAGCombinerInfo &DCI,
                            bool Commuted = false) {
  SDValue LHS = N->getOperand(0 ^ Commuted),
          RHS = N->getOperand(1 ^ Commuted);

  SDValue NewNode;
  uint64_t Mask = 0;
  if (ExtractConstant(RHS, Mask)) {
    switch(N->getOpcode()) {
    case ISD::AND:
      NewNode = ExtractBits(LHS, Mask, DCI, ExtractBitSlice, GetZerosBitSlice);
      break;
    case ISD::OR:
      NewNode = ExtractBits(LHS, Mask, DCI, GetOnesBitSlice, ExtractBitSlice);
      break;
    case ISD::XOR:
      NewNode = ExtractBits(LHS, Mask, DCI, FlipBitSlice, ExtractBitSlice);
      break;
    default:
      llvm_unreachable("Unexpected Logic Node!");
    }
  }

  if (NewNode.getNode()) return NewNode;

  // Try to promote the bitcat after operand commuted.
  NewNode = PromoteBinOpBitCat(N, DCI,
                               // Get bitslice from hi part and lo before
                               // concact them.
                               true,
                               ConcatBits, LogicOpBuildLowPart,
                               LogicOpBuildHighPart, GetDefaultSplitBit);
  if (NewNode.getNode()) return NewNode;

  return commuteAndTryAgain(N, DCI, Commuted, PerformLogicCombine);
}

static
SDValue PerformNotCombine(SDNode *N, TargetLowering::DAGCombinerInfo &DCI) {
  SDValue Op = N->getOperand(0);

  // ~(~A) = A.
  if (Op->getOpcode() == VTMISD::Not) return Op->getOperand(0);

  if (Op->getOpcode() == VTMISD::BitCat) {
    SDValue Hi = Op->getOperand(0), Lo = Op->getOperand(1);

    Hi = VTargetLowering::getNot(DCI.DAG, N->getDebugLoc(), Hi);
    DCI.AddToWorklist(Hi.getNode());
    Lo = VTargetLowering::getNot(DCI.DAG, N->getDebugLoc(), Lo);
    DCI.AddToWorklist(Lo.getNode());
    return DCI.DAG.getNode(VTMISD::BitCat, N->getDebugLoc(), N->getVTList(),
                           Hi, Lo);
  }

  return SDValue();
}

static SDValue CombineConstants(SelectionDAG &DAG, SDValue &Hi, SDValue &Lo,
                                unsigned ResultWidth) {
  uint64_t LoVal = 0;
  unsigned LoSizeInBits = ExtractConstant(Lo, LoVal);
  if (!LoSizeInBits) return SDValue();
  assert(LoSizeInBits <= 64 && "Lower part of constant too large!");

  uint64_t HiVal = 0;
  unsigned HiSizeInBits = ExtractConstant(Hi, HiVal);
  if (!HiSizeInBits) return SDValue();

  unsigned SizeInBits = LoSizeInBits + HiSizeInBits;
  assert(SizeInBits <= 64 && "Constant too large!");
  uint64_t Val = (LoVal) | (HiVal << LoSizeInBits);

  EVT VT =  EVT::getIntegerVT(*DAG.getContext(), SizeInBits);
  // Use BitSlice to match the type if necessary.
  return VTargetLowering::getBitSlice(DAG, Hi.getDebugLoc(),
                                      DAG.getTargetConstant(Val, VT),
                                      SizeInBits, 0, ResultWidth);
}


static
SDValue PerformBitCatCombine(SDNode *N, TargetLowering::DAGCombinerInfo &DCI) {
  SDValue Hi = N->getOperand(0), Lo = N->getOperand(1);
  SelectionDAG &DAG = DCI.DAG;

  // Dose the node looks like {a[UB-1, M], a[M-1, LB]}? If so, combine it to
  // a[UB-1, LB]
  if (Hi->getOpcode() == VTMISD::BitSlice
      && Lo->getOpcode() == VTMISD::BitSlice) {
    SDValue HiSrc = Hi->getOperand(0);

    if (HiSrc.getNode() == Lo->getOperand(0).getNode()
        && Hi->getConstantOperandVal(2) == Lo->getConstantOperandVal(1))
      return VTargetLowering::getBitSlice(DAG, HiSrc->getDebugLoc(),
                                          HiSrc,
                                          Hi->getConstantOperandVal(1),
                                          Lo->getConstantOperandVal(2),
                                          N->getValueSizeInBits(0));

  }

  // Try to merge the constants.
  return CombineConstants(DAG, Hi, Lo, N->getValueSizeInBits(0));
}

static
SDValue PerformBitSliceCombine(SDNode *N, TargetLowering::DAGCombinerInfo &DCI) {
  SDValue Op = N->getOperand(0);
  unsigned UB = N->getConstantOperandVal(1),
           LB = N->getConstantOperandVal(2);
  DebugLoc dl = N->getDebugLoc();

  // Try to flatten the bitslice tree.
  if (Op->getOpcode() == VTMISD::BitSlice) {
    SDValue SrcOp = Op->getOperand(0);
    unsigned Offset = Op->getConstantOperandVal(2);
    assert(UB <= Op->getConstantOperandVal(1) - Offset && "Broken bitslice!");
    return VTargetLowering::getBitSlice(DCI.DAG, dl,
                                        SrcOp, UB + Offset, LB + Offset,
                                        N->getValueSizeInBits(0));
  }

  // If the big range fall into the bit range of one of the BitCat operand,
  // return bitslice of that operand.
  if (Op->getOpcode() == VTMISD::BitCat) {
    SDValue HiOp = Op->getOperand(0), LoOp = Op->getOperand(1);
    unsigned SplitBit = VTargetLowering::computeSizeInBits(LoOp);
    if (UB <= SplitBit)
      return VTargetLowering::getBitSlice(DCI.DAG, dl, LoOp, UB, LB,
                                          N->getValueSizeInBits(0));

    if (LB >= SplitBit)
      return VTargetLowering::getBitSlice(DCI.DAG, dl,
                                          HiOp, UB - SplitBit, LB - SplitBit,
                                          N->getValueSizeInBits(0));

    HiOp = VTargetLowering::getBitSlice(DCI.DAG, dl, HiOp, UB - SplitBit, 0);
    DCI.AddToWorklist(HiOp.getNode());
    LoOp = VTargetLowering::getBitSlice(DCI.DAG, dl, LoOp, SplitBit, LB);
    DCI.AddToWorklist(LoOp.getNode());
    return DCI.DAG.getNode(VTMISD::BitCat, dl, N->getVTList(), HiOp, LoOp);
  }

  return SDValue();
}

static
SDValue PerformReduceCombine(SDNode *N, TargetLowering::DAGCombinerInfo &DCI) {
  SDValue Op = N->getOperand(0);

  // 1 bit value do not need to reduce at all.
  if (VTargetLowering::computeSizeInBits(Op) == 1) return Op;

  SelectionDAG &DAG = DCI.DAG;

  // Try to fold the reduction
  uint64_t Val = 0;
  if (unsigned SizeInBits = ExtractConstant(Op, Val)) {
    switch (N->getOpcode()) {
    case VTMISD::ROr:
      // Only reduce to 0 if all bits are 0.
      if (isNullValue(Val, SizeInBits))
        return DAG.getTargetConstant(0, MVT::i1);
      else
        return DAG.getTargetConstant(1, MVT::i1);
    case VTMISD::RAnd:
      // Only reduce to 1 if all bits are 1.
      if (isAllOnesValue(Val, SizeInBits))
        return DAG.getTargetConstant(1, MVT::i1);
      else
        return DAG.getTargetConstant(0, MVT::i1);
    case VTMISD::RXor:
      // Only reduce to 1 if there are odd 1s.
      if (CountPopulation_64(Val) & 0x1)
        return DAG.getTargetConstant(1, MVT::i1);
      else
        return DAG.getTargetConstant(0, MVT::i1);
      break; // FIXME: Who knows how to evaluate this?
    default:  llvm_unreachable("Unexpected Reduction Node!");
    }
  }

  DebugLoc dl = N->getDebugLoc();
  // Reduce high part and low part respectively.
  if (Op->getOpcode() == VTMISD::BitCat) {
    SDValue Hi = Op->getOperand(0), Lo = Op->getOperand(1);
    Hi = VTargetLowering::getReductionOp(DAG, N->getOpcode(), dl, Hi);
    DCI.AddToWorklist(Hi.getNode());
    Lo = VTargetLowering::getReductionOp(DAG, N->getOpcode(), dl, Lo);
    DCI.AddToWorklist(Lo.getNode());
    unsigned Opc = 0;
    switch (N->getOpcode()) {
    case VTMISD::ROr:   Opc = ISD::OR;  break;
    case VTMISD::RAnd:  Opc = ISD::AND; break;
    case VTMISD::RXor:  Opc = ISD::XOR; break;
    default:  llvm_unreachable("Unexpected Reduction Node!");
    }

    return DAG.getNode(Opc, dl, MVT::i1, Hi, Lo);
  }

  return SDValue();
}

//===--------------------------------------------------------------------===//
// Arithmetic operations.
// Add
static bool isAddEOpBOneBit(SDValue Op, bool Commuted) {
  SDValue OpB = Op->getOperand(1 ^ Commuted);
  uint64_t OpBVal = 0;
  if (!ExtractConstant(OpB, OpBVal)) return false;

  // FIXME: Use Bit mask information.
  return OpBVal == 0 || OpBVal == 1;
}

inline static SDValue ConcatADDEs(TargetLowering::DAGCombinerInfo &DCI,
                                  SDNode *N, SDValue Hi, SDValue Lo) {
  SDValue NewOp = DCI.DAG.getNode(VTMISD::BitCat, N->getDebugLoc(),
                                  N->getValueType(0), Hi, Lo);
  if (Hi->getOpcode() == VTMISD::BitSlice)
    Hi = Hi->getOperand(0);
  DCI.CombineTo(N, NewOp, Hi.getValue(1));
  return SDValue(N, 0);
}

// Pad the operand of ADDE so we can always get the right carry value.
// for example we have:
// i16 sum, i1 c = i12 a + i12 b + i1 0
// in this case the carry value should be sum[12], to put the carry value to c,
// we can pad the higher bits of a with 1 and the higher bits of a with 0:
// i16 sum, i1 c = {i4 f, i12 a} + {i4 0, i12 b} + i1 0
inline void PadADDEOperand(TargetLowering::DAGCombinerInfo &DCI, DebugLoc dl,
                           SDValue &LHS, SDValue &RHS) {
  SelectionDAG &DAG = DCI.DAG;
  unsigned ActualBits = VTargetLowering::computeSizeInBits(LHS);
  assert(VTargetLowering::computeSizeInBits(LHS)
          == VTargetLowering::computeSizeInBits(RHS)
         && "Bitwidth do not match!");
  if (unsigned DiffBits = LHS.getValueSizeInBits() - ActualBits) {
    // Try to keep RHS as constant.
    if (IsConstant(LHS)) std::swap(LHS, RHS);
    EVT PaddingVT = EVT::getIntegerVT(*DAG.getContext(), DiffBits);
    SDValue LHSPadding = DAG.getTargetConstant(~uint64_t(0), PaddingVT);
    LHS = DAG.getNode(VTMISD::BitCat, dl, LHS.getValueType(), LHSPadding, LHS);
    DCI.AddToWorklist(LHS.getNode());
    uint64_t RHSVal;
    if (ExtractConstant(RHS, RHSVal)) {
      RHS = DAG.getTargetConstant(RHSVal, RHS.getValueType());
    } else {
      SDValue RHSPadding = DAG.getTargetConstant(0, PaddingVT);
      RHS = DAG.getNode(VTMISD::BitCat, dl, RHS.getValueType(), RHSPadding, RHS);
      DCI.AddToWorklist(RHS.getNode());
    }
  }
}

inline static SDValue ADDEBuildLowPart(TargetLowering::DAGCombinerInfo &DCI,
                                       SDNode *N, SDValue LHS, SDValue RHS,
                                       bool Commuted) {
  SelectionDAG &DAG = DCI.DAG;
  DebugLoc dl = N->getDebugLoc();

  PadADDEOperand(DCI, dl, LHS, RHS);

  SDVTList VTs = DAG.getVTList(LHS.getValueType(),  MVT::i1);
  SDValue Lo = DAG.getNode(ISD::ADDE, dl, VTs, LHS, RHS, N->getOperand(2));
  DCI.AddToWorklist(Lo.getNode());
  return Lo;
}

inline static SDValue ADDEBuildHighPart(TargetLowering::DAGCombinerInfo &DCI,
                                        SDNode *N, SDValue LHS, SDValue RHS,
                                        SDValue Lo, bool Commuted) {
  SelectionDAG &DAG = DCI.DAG;
  DebugLoc dl = N->getDebugLoc();
  PadADDEOperand(DCI, dl, LHS, RHS);

  SDVTList VTs = DAG.getVTList(LHS.getValueType(),  MVT::i1);

  if (Lo->getOpcode() == VTMISD::BitSlice)
    Lo = Lo->getOperand(0);

  SDValue LoC = Lo.getValue(1);
  SDValue Hi = DAG.getNode(ISD::ADDE, N->getDebugLoc(), VTs, LHS, RHS, LoC);
  DCI.AddToWorklist(Hi.getNode());
  return Hi;
}

static
unsigned GetADDEBitCatSplitBit(SDNode *N, TargetLowering::DAGCombinerInfo &DCI,
                               SDValue LHS, SDValue &LHSLo, SDValue &LHSHi,
                               SDValue RHS, SDValue &RHSLo, SDValue &RHSHi) {
  unsigned RHSLoBits = VTargetLowering::computeSizeInBits(RHS->getOperand(1));
  unsigned RHSHiBits = VTargetLowering::computeSizeInBits(RHS->getOperand(0));

  // FIXME: Can we also optimize this?
  if (RHSLoBits < RHSHiBits) return 0;

  // C and RHSLo must be constant.
  uint64_t CVal = 0;
  SDValue C = N->getOperand(2);
  if (!ExtractConstant(C, CVal)) return 0;

  RHSHi = RHS.getOperand(0);
  RHSLo = RHS.getOperand(1);

  // Only promote the ADDE when we can drop the lower part.
  uint64_t RHSLoVal = 0;
  if (!ExtractConstant(RHSLo, RHSLoVal)) return 0;

  if (!isNullValue(CVal + RHSLoVal, RHSLoBits)) return 0;

  SplitLHSAt(DCI, RHSLoBits, LHS, LHSLo, LHSHi, RHSLo, RHSHi);

  return RHSLoBits;
}

static unsigned GetADDESplitBit(SDNode *N, TargetLowering::DAGCombinerInfo &DCI,
                                SDValue LHS, SDValue &LHSLo, SDValue &LHSHi,
                                SDValue RHS, SDValue &RHSLo, SDValue &RHSHi) {
  if (RHS->getOpcode() == VTMISD::BitCat)
    return GetADDEBitCatSplitBit(N, DCI, LHS, LHSLo, LHSHi, RHS, RHSLo, RHSHi);

  // Try to perform: a + 0x8000 => { a[15:14] + 0x1, a[13:0] }
  uint64_t RHSVal = 0;
  if (unsigned SizeInBit = ExtractConstant(RHS, RHSVal)) {
    unsigned SplitBit = CountTrailingZeros_64(RHSVal);
    // It is not profitable to split if the lower zero part is too small.
    if (SplitBit < SizeInBit / 2) return 0;

    SplitOpAtRHSConstant(DCI, SplitBit, LHS, LHSLo, LHSHi, RHS, RHSLo, RHSHi);

    return SplitBit;
  }

  return 0;
}

static
SDValue PerformAddCombine(SDNode *N, TargetLowering::DAGCombinerInfo &DCI,
                          bool Commuted = false) {
  DebugLoc dl = N->getDebugLoc();
  SelectionDAG &DAG = DCI.DAG;

  uint64_t CVal = 0;

  SDValue OpA = N->getOperand(0 ^ Commuted), OpB = N->getOperand(1 ^ Commuted);
  SDValue C = N->getOperand(2);

  // Expand 1 bit adder to full adder.
  if (VTargetLowering::computeSizeInBits(SDValue(N, 0)) == 1) {
    SDValue AXOrB = DAG.getNode(ISD::XOR, dl, MVT::i1, OpA, OpB);
    SDValue CAndAXOrB = DAG.getNode(ISD::AND, dl, MVT::i1, C, AXOrB);
    SDValue AAndB = DAG.getNode(ISD::AND, dl, MVT::i1, OpA, OpB);
    SDValue NewC = DAG.getNode(ISD::XOR, dl, MVT::i1, AAndB, CAndAXOrB);
    SDValue Sum = DAG.getNode(ISD::XOR, dl, MVT::i1, AXOrB, C);
    DCI.CombineTo(N, Sum, NewC);
    return SDValue(N, 0);
  }

  // Can only combinable if carry is known.
  if (!ExtractConstant(C, CVal)) {
    uint64_t OpAVal = 0, OpBVal = 0;
    // Fold the constant.
    if (unsigned OpASize = ExtractConstant(OpA, OpAVal)) {
      if (unsigned OpBSize = ExtractConstant(OpB, OpBVal)) {
        // 0 + ~0 + carry = {carry, 0}
        if (isNullValue(OpAVal, OpASize) && isAllOnesValue(OpBVal, OpBSize)) {
          DCI.CombineTo(N, DAG.getTargetConstant(0, N->getValueType(0)), C);
          return SDValue(N, 0);
        }
        // TODO: Fold the constant addition.
      }
    }
    return SDValue();
  }

  // A + (B + 1 bit value + 0) + 0 -> A + B + 1'bit value
  if (CVal == 0 && OpB->getOpcode() == ISD::ADDE) {
    uint64_t OpBCVal = 0;
    if (ExtractConstant(OpB->getOperand(2), OpBCVal) && OpBCVal == 0) {
      bool CommuteOpB = false;
      bool isOpBOneBitOnly = false;
      if (!(isOpBOneBitOnly = /*ASSIGNMENT*/ isAddEOpBOneBit(OpB, CommuteOpB))){
        CommuteOpB = true; // Commute and try again.
        isOpBOneBitOnly = isAddEOpBOneBit(OpB, CommuteOpB);
      }

      if (isOpBOneBitOnly) {
        SDValue OpBOpA = OpB->getOperand(0 ^ CommuteOpB),
                OpBOpB = OpB->getOperand(1 ^ CommuteOpB);
        OpBOpB = VTargetLowering::getBitSlice(DAG, dl, OpBOpB, 1, 0);
        DCI.AddToWorklist(OpBOpB.getNode());
        return DAG.getNode(ISD::ADDE, dl, N->getVTList(), OpA, OpBOpA, OpBOpB);
      }
    }
  }

  uint64_t OpBVal = 0;
  if (unsigned OpBSize = ExtractConstant(OpB, OpBVal)) {
    if (CVal == 0)
      if (GlobalAddressSDNode *GSD = dyn_cast<GlobalAddressSDNode>(OpA)) {
        SDValue FoldedGSD = DAG.getGlobalAddress(GSD->getGlobal(), dl,
                                                 GSD->getValueType(0),
                                                 OpBVal + GSD->getOffset());
        DCI.CombineTo(N, FoldedGSD, DAG.getTargetConstant(0, MVT::i1));
        return SDValue(N, 0);
      }
    
    // A + ~0 + 1 => A - 0 => {1, A}
    if (isAllOnesValue(CVal, 1) && isAllOnesValue(OpBVal, OpBSize)) {
      DCI.CombineTo(N, OpA, DAG.getTargetConstant(1, MVT::i1));
      return SDValue(N, 0);
    }

    // A + 0 + 0 => {0, A}
    if (isNullValue(CVal, 1) && isNullValue(OpBVal, OpBSize)){
      DCI.CombineTo(N, OpA, DAG.getTargetConstant(0, MVT::i1));
      return SDValue(N, 0);
    }

    if (CVal) {
      // Fold the constant carry to RHS.
      assert((getBitSlice64(OpBVal, OpBSize) + 1 ==
              getBitSlice64(OpBVal + 1, std::min(64u, OpBSize + 1)))
             && "Unexpected overflow!");
      OpBVal += 1;
      return DAG.getNode(ISD::ADDE, dl, N->getVTList(),
                         OpA,
                         DAG.getTargetConstant(OpBVal, OpB.getValueType()),
                         DAG.getTargetConstant(0, MVT::i1));
    }
  }

  SDValue RV = PromoteBinOpBitCat(N, DCI,
                                  // Get bitslice from hi part and lo before
                                  // concact them.
                                  true,
                                  ConcatADDEs, ADDEBuildLowPart,
                                  ADDEBuildHighPart, GetADDESplitBit,
                                  Commuted);
  if (RV.getNode()) return RV;

  // Try to concat ADDE togethers
  //if (C->getOpcode() == ISD::ADDE && C->hasOneUse()) {
  //  unsigned LowerSize = C->getValueSizeInBits(0);
  //  if (LowerSize <= 32 && LowerSize == N->getValueSizeInBits(0)) {
  //    SDValue LoOpA = C->getOperand(0), LoOpB = C->getOperand(1),
  //                       LoC = C->getOperand(2);
  //    // The concatation is profitable only if some operands are derived from
  //    // the same node.
  //    if (LoOpA.getNode() == OpA.getNode() || LoOpB.getNode() == OpB.getNode()){
  //      LLVMContext &Cntx = *DAG.getContext();
  //      EVT NewVT = VTargetLowering::getRoundIntegerOrBitType(LowerSize * 2, Cntx);

  //      OpA = DAG.getNode(VTMISD::BitCat, OpA->getDebugLoc(), NewVT, OpA, LoOpA);
  //      DCI.AddToWorklist(OpA.getNode());
  //      OpB = DAG.getNode(VTMISD::BitCat, OpB->getDebugLoc(), NewVT, OpB, LoOpB);
  //      DCI.AddToWorklist(OpB.getNode());

  //      SDValue NewAdd = DAG.getNode(ISD::ADDE, dl, DAG.getVTList(NewVT, MVT::i1),
  //                                   OpA, OpB, LoC);
  //      DCI.AddToWorklist(NewAdd.getNode());
  //      DCI.CombineTo(N, VTargetLowering::getBitSlice(DAG, dl, NewAdd,
  //                                                    2 * LowerSize, LowerSize),
  //                        NewAdd.getValue(1));

  //      DCI.CombineTo(C.getNode(), VTargetLowering::getBitSlice(DAG, dl, NewAdd,
  //                                                              LowerSize, 0));
  //      return SDValue(N, 0);
  //    }
  //  }
  //}

  // TODO: Combine with bit mask information.
  return commuteAndTryAgain(N, DCI, Commuted, PerformAddCombine);
}
//----------------------------------------------------------------------------//
static
unsigned GetMULBitCatSplitBit(SDNode *N, TargetLowering::DAGCombinerInfo &DCI,
                              SDValue LHS, SDValue &LHSLo, SDValue &LHSHi,
                              SDValue RHS, SDValue &RHSLo, SDValue &RHSHi) {
  unsigned RHSLoBits = VTargetLowering::computeSizeInBits(RHS->getOperand(1));
  unsigned RHSHiBits = VTargetLowering::computeSizeInBits(RHS->getOperand(0));

  // Only split the mul from the middle, and do not generate small multiplier.
  if (RHSLoBits != RHSHiBits || RHSLoBits < 8) return 0;

  RHSHi = RHS.getOperand(0);
  RHSLo = RHS.getOperand(1);

  // Only promote the ADDE when we can drop the lower part.
  uint64_t RHSLoVal = 0, RHSHiVal = 0;
  if (!ExtractConstant(RHSLo, RHSLoVal) && !ExtractConstant(RHSHi, RHSHiVal))
    return 0;

  if (!isNullValue(RHSLoVal, RHSLoBits) && !isNullValue(RHSHiVal, RHSHiBits))
    return 0;

  SplitLHSAt(DCI, RHSLoBits, LHS, LHSLo, LHSHi, RHSLo, RHSHi);

  return RHSLoBits;
}

static unsigned GetMULSplitBit(SDNode *N, TargetLowering::DAGCombinerInfo &DCI,
                               SDValue LHS, SDValue &LHSLo, SDValue &LHSHi,
                               SDValue RHS, SDValue &RHSLo, SDValue &RHSHi) {
  if (RHS->getOpcode() == VTMISD::BitCat)
    return GetMULBitCatSplitBit(N, DCI, LHS, LHSLo, LHSHi, RHS, RHSLo, RHSHi);

  // Try to perform: a * 0x8000 => { a[15:14] * 0x1, a[13:0] }
  uint64_t RHSVal = 0;
  if (unsigned SizeInBit = ExtractConstant(RHS, RHSVal)) {
    unsigned SplitBit = SizeInBit / 2;
    // Do not generate small multiplier.
    if (SplitBit < 8) return 0;

    unsigned TrailingZeros = CountTrailingZeros_64(RHSVal);
    unsigned LeadingZeros = CountLeadingZeros_64(RHSVal);
    if (LeadingZeros > (64 - SizeInBit)) LeadingZeros -= (64 - SizeInBit);
    else                                 LeadingZeros = 0;

    // It is not profitable to split if the zero part is too small.
    if (TrailingZeros < SplitBit && LeadingZeros < SplitBit)
      return 0;

    SplitOpAtRHSConstant(DCI, SplitBit,
                         LHS, LHSLo, LHSHi,
                         RHS, RHSLo, RHSHi);

    return SplitBit;
  }

  return 0;
}

inline static SDValue MULBuildLowPart(TargetLowering::DAGCombinerInfo &DCI,
                                      SDNode *N, SDValue LHS, SDValue RHS,
                                      bool Commuted) {
  SelectionDAG &DAG = DCI.DAG;
  DebugLoc dl = N->getDebugLoc();
  assert(VTargetLowering::computeSizeInBits(LHS) == LHS.getValueSizeInBits()
         && "Expect aligned bit width in MUL!");
  // We have to check this because the getNode will not check this for UMUL_LOHI
  assert(LHS.getValueSizeInBits() == RHS.getValueSizeInBits()
         && "UMUL_LOHI Operands size not match!");

  SDVTList VTs = DAG.getVTList(LHS.getValueType(),  LHS.getValueType());
  SDValue Lo = DAG.getNode(ISD::UMUL_LOHI, dl, VTs, LHS, RHS);
  DCI.AddToWorklist(Lo.getNode());
  return Lo;
}

inline static SDValue MULBuildHighPart(TargetLowering::DAGCombinerInfo &DCI,
                                       SDNode *N, SDValue LHSHi, SDValue RHSHi,
                                       SDValue Lo, bool Commuted) {
  // Build the result with the same way as ExpandIntRes_MUL.
  SelectionDAG &DAG = DCI.DAG;
  DebugLoc dl = N->getDebugLoc();
  assert(Lo->getOpcode() == ISD::UMUL_LOHI && "Expect Lo is UMUL_LOHI!");
  SDValue LHSLo = Lo->getOperand(0), RHSLo = Lo->getOperand(1);

  // The High part of the UMUL_LOHI.
  EVT HiVT = Lo.getValueType();

  unsigned MULOpcode = N->getOpcode();
  bool isUMUL_LoHi = MULOpcode == ISD::UMUL_LOHI;
  SDVTList MULVTs = isUMUL_LoHi ? DAG.getVTList(HiVT, HiVT)
                                : DAG.getVTList(HiVT);
  // Build the lower part of LL * RH and LH * RL and add them together.
  SDValue MulLLRHLo = DAG.getNode(MULOpcode, dl, MULVTs, LHSLo, RHSHi);
  DCI.AddToWorklist(MulLLRHLo.getNode());
  SDValue MulLHRLLo = DAG.getNode(MULOpcode, dl, MULVTs, LHSHi, RHSLo);
  DCI.AddToWorklist(MulLHRLLo.getNode());

  SDVTList ADDEVTs = DAG.getVTList(HiVT, MVT::i1);
  SDValue LoHi = DAG.getNode(ISD::ADDE, dl, ADDEVTs, MulLLRHLo, MulLHRLLo,
                             DAG.getTargetConstant(0, MVT::i1));
  // Carry bit is need if we build the high part of the multiplication.
  SDValue C0 = LoHi.getValue(1);
  DCI.AddToWorklist(LoHi.getNode());

  // Add the higher part of Lo to the sum of Lo(LL * RH) and Lo(LH * RL)
  LoHi = DAG.getNode(ISD::ADDE, dl, ADDEVTs, LoHi, Lo.getValue(1),
                     DAG.getTargetConstant(0, MVT::i1));
  // Remember the carry bit.
  SDValue C1 = LoHi.getValue(1);
  DCI.AddToWorklist(LoHi.getNode());

  Lo = DAG.getNode(VTMISD::BitCat, N->getDebugLoc(),
                   N->getValueType(0), LoHi, Lo);
  // The lower part of the multiplication is enough.
  if (!isUMUL_LoHi) return Lo;

  // Compute the High part of the result.
  // The lower half of the High Par, add Hi(LL * RH) and Hi(LH * RL) together.
  SDValue HiLo = DAG.getNode(ISD::ADDE, dl, ADDEVTs,
                             MulLLRHLo.getValue(1), MulLHRLLo.getValue(1), C0);
  // Remember the carry bit.
  C0 = HiLo.getValue(1);
  DCI.AddToWorklist(HiLo.getNode());

  // Build LH * RH
  SDValue MulLHRHLo = DAG.getNode(ISD::UMUL_LOHI, dl, MULVTs, LHSHi, RHSHi);
  DCI.AddToWorklist(MulLHRHLo.getNode());

  // Add Lo(LH * RH) to the sum of Hi(LL * RH) and Hi(LH * RL).
  HiLo = DAG.getNode(ISD::ADDE, dl, ADDEVTs, HiLo, MulLHRHLo, C1);
  // Remember the carry bit.
  C1 = HiLo.getValue(1);
  DCI.AddToWorklist(HiLo.getNode());

  // Add Hi(LH * RH) and the carry bit from Lo(LH * RH) + Hi(LL * RH) + Hi(LH * RL)
  SDValue MulLHRHHi = MulLHRHLo.getValue(1);
  SDValue HiHi = DAG.getNode(ISD::ADDE, dl, ADDEVTs,
                             MulLHRHHi, DAG.getTargetConstant(0, HiVT), C0);
  DCI.AddToWorklist(HiHi.getNode());

  HiHi = DAG.getNode(ISD::ADDE, dl, ADDEVTs,
                     HiHi, DAG.getTargetConstant(0, HiVT), C1);
  DCI.AddToWorklist(HiHi.getNode());
  // Build the High part of the UMUL_LOHI.
  SDValue Hi = DAG.getNode(VTMISD::BitCat, N->getDebugLoc(),
                           N->getValueType(0), HiHi, HiLo);
  DCI.CombineTo(N, Lo, Hi);
  return SDValue(N, 0);
}

// We already build the full result in MULBuildHighPart.
inline static SDValue ConcatMUL(TargetLowering::DAGCombinerInfo &DCI,
                                SDNode *N, SDValue Hi, SDValue Lo) {
  return Hi;
}

static
SDValue PerformMulCombine(SDNode *N, TargetLowering::DAGCombinerInfo &DCI,
                                 bool Commuted = false) {
  SDValue OpA = N->getOperand(0 ^ Commuted), OpB = N->getOperand(1 ^ Commuted);
  bool isMUL_LoHi = N->getOpcode() == ISD::UMUL_LOHI;

  uint64_t OpBVal = 0;
  if (unsigned OpBSize = ExtractConstant(OpB, OpBVal)) {
    SelectionDAG &DAG = DCI.DAG;
    LLVMContext &Cntx = *DAG.getContext();
    DebugLoc dl = N->getDebugLoc();
    EVT VT = N->getValueType(0);

    SDValue Lo, Hi = DAG.getTargetConstant(0, VT);
    // A * 1 -> A
    if (OpBVal == 1) Lo = OpA;

    // A * 0 -> 0
    if (OpBVal == 0) Lo = DAG.getTargetConstant(0, VT);

    // TODO: A * -1 -> -A

    // Try to lower mult to shift constant.
    if (isPowerOf2_64(OpBVal)) {
      unsigned PowerOf2 = Log2_64(OpBVal);
      assert(PowerOf2 < OpBSize && "Bad power of 2 in mult!");
      Lo = VTargetLowering::getBitSlice(DAG, dl, OpA, OpBSize - PowerOf2, 0);
      DCI.AddToWorklist(Lo.getNode());
      EVT PaddingVT = EVT::getIntegerVT(Cntx, PowerOf2);
      Lo = DAG.getNode(VTMISD::BitCat, dl, VT,
                       Lo, DAG.getTargetConstant(0, PaddingVT));
      // Build the high part if necessary.
      if (isMUL_LoHi) {
        Hi = VTargetLowering::getBitSlice(DAG, dl, OpA,
                                          OpBSize, OpBSize - PowerOf2);
        DCI.AddToWorklist(Hi.getNode());
        PaddingVT = EVT::getIntegerVT(Cntx, OpBSize - PowerOf2);
        Hi = DAG.getNode(VTMISD::BitCat, dl, VT,
                         DAG.getTargetConstant(0, PaddingVT), Hi);
      }
    }

    // If something combined
    if (Lo.getNode()) {
      if (isMUL_LoHi) {
        DCI.CombineTo(N, Lo, Hi);
        return SDValue(N, 0);
      } else
        return Lo;
    }
  }

  SDValue RV = PromoteBinOpBitCat(N, DCI,
                                  // Get bitslice from hi part and lo before
                                  // concact them.
                                  false,
                                  ConcatMUL, MULBuildLowPart,
                                  MULBuildHighPart, GetMULSplitBit,
                                  Commuted);
  if (RV.getNode()) return RV;

  return commuteAndTryAgain(N, DCI, Commuted, PerformMulCombine);
}

//static void ExpandOperand(TargetLowering::DAGCombinerInfo &DCI, SDValue Op,
//                          SDValue &HiOp, SDValue &LoOp) {
//  unsigned BitWidth = Op.getValueSizeInBits();
//  assert(isPowerOf2_32(BitWidth) && "Cannot handle irregular bitwidth!");
//  unsigned SplitBit = BitWidth / 2;
//  assert(VTargetLowering::computeSizeInBits(Op) > SplitBit
//         && "Cannot expand operand!");
//  HiOp = VTargetLowering::getBitSlice(DCI.DAG, Op.getDebugLoc(), Op,
//                                      BitWidth, SplitBit);
//  DCI.AddToWorklist(HiOp.getNode());
//  LoOp = VTargetLowering::getBitSlice(DCI.DAG, Op.getDebugLoc(), Op,
//                                      SplitBit, 0);
//  DCI.AddToWorklist(LoOp.getNode());
//}
//
//template<typename ConcatBitsFunc,
//         typename BuildLowPartFunc,
//         typename BuildHighPartFunc>
//static SDValue ExpandArithmeticOp(TargetLowering::DAGCombinerInfo &DCI,
//                                  const VTargetLowering &TLI, SDNode *N,
//                                  ConcatBitsFunc ConcatBits,
//                                  BuildLowPartFunc BuildLowPart,
//                                  BuildHighPartFunc BuildHighPart) {
//  SDValue LHS = N->getOperand(0), RHS = N->getOperand(1);
//  SDValue LHSLo, LHSHi;
//  ExpandOperand(DCI, LHS, LHSHi, LHSLo);
//  SDValue RHSLo, RHSHi;
//  ExpandOperand(DCI, RHS, RHSHi, RHSLo);
//  SDValue ADDELo = BuildLowPart(DCI, N, LHSLo, RHSLo);
//  SDValue ADDEHi = BuildHighPart(DCI, N, LHSHi, RHSHi, ADDELo);
//  return ConcatBits(DCI, N, ADDEHi, ADDELo);
//}

//----------------------------------------------------------------------------//
// Function for ICmp combine.
// Split the size of comparison to get smaller latency.
static
unsigned GetICmpBitCatSplitBit(SDNode *N, TargetLowering::DAGCombinerInfo &DCI,
                               SDValue LHS, SDValue &LHSLo, SDValue &LHSHi,
                               SDValue RHS, SDValue &RHSLo, SDValue &RHSHi) {
  unsigned RHSLoBits = VTargetLowering::computeSizeInBits(RHS->getOperand(1));
  unsigned RHSHiBits = VTargetLowering::computeSizeInBits(RHS->getOperand(0));
  RHSHi = RHS.getOperand(0);
  RHSLo = RHS.getOperand(1);

  // Only promote the ICmp when one of them are constant, and we can split the
  // icmp at the middle.
  if (!((IsConstant(RHSLo)|| IsConstant(RHSHi)) && RHSLoBits == RHSHiBits))
    return 0;

  SelectionDAG &DAG = DCI.DAG;
  DebugLoc dl = N->getDebugLoc();

  LHSLo = VTargetLowering::getBitSlice(DAG, dl, LHS, RHSLoBits, 0);
  DCI.AddToWorklist(LHSLo.getNode());
  // Adjust the bitwidth of constant to match LHS's width.
  if (LHSLo.getValueSizeInBits() != RHSLo.getValueSizeInBits()) {
    RHSLo = VTargetLowering::getBitSlice(DAG, dl, RHSLo, RHSLoBits, 0,
                                         LHSLo.getValueSizeInBits());
    DCI.AddToWorklist(RHSLo.getNode());
  }

  LHSHi = VTargetLowering::getBitSlice(DAG, dl, LHS,
                                       RHSLoBits + RHSHiBits, RHSLoBits);
  DCI.AddToWorklist(LHSHi.getNode());

  return RHSLoBits;
}

static unsigned GetICmpRHSConstSplitBit(uint64_t RHSVal, unsigned RHSSize) {
  if (RHSSize <= 8) return 0;

  unsigned SplitBit = RHSSize / 2;
  if (CountTrailingZeros_64(RHSVal) >= SplitBit
      || CountTrailingOnes_64(RHSVal)  >= SplitBit
      || CountLeadingZerosSplitBit(RHSVal, RHSSize) <= SplitBit
      || CountLeadingOnesSplitBit(RHSVal, RHSSize) <= SplitBit)
    return SplitBit;

  return 0;
}

static unsigned GetICmpSplitBit(SDNode *N, TargetLowering::DAGCombinerInfo &DCI,
                                SDValue LHS, SDValue &LHSLo, SDValue &LHSHi,
                                SDValue RHS, SDValue &RHSLo, SDValue &RHSHi) {
  if (RHS->getOpcode() == VTMISD::BitCat)
    return GetICmpBitCatSplitBit(N, DCI, LHS, LHSLo, LHSHi, RHS, RHSLo, RHSHi);

  uint64_t RHSVal = 0;
  unsigned SizeInBit = ExtractConstant(RHS, RHSVal);
  // Only split if RHS is constant.
  if (SizeInBit == 0) return 0;

  // Try lower some comparison like: a > 0x8000 or a > 0x00ff
  unsigned SplitBit = GetICmpRHSConstSplitBit(RHSVal, SizeInBit);
  if (SplitBit  == 0) return 0;

  DebugLoc dl = N->getDebugLoc();
  SelectionDAG &DAG = DCI.DAG;
  // Build the lower part.
  LHSLo = VTargetLowering::getBitSlice(DAG, dl, LHS, SplitBit, 0);
  DCI.AddToWorklist(LHSLo.getNode());
  RHSLo = VTargetLowering::getBitSlice(DAG, dl, RHS, SplitBit, 0,
                                        LHSLo.getValueSizeInBits());
  DCI.AddToWorklist(RHSLo.getNode());
  // And the higher part.
  LHSHi = VTargetLowering::getBitSlice(DAG, dl, LHS, SizeInBit, SplitBit);
  DCI.AddToWorklist(LHSHi.getNode());
  RHSHi = VTargetLowering::getBitSlice(DAG, dl, RHS, SizeInBit, SplitBit,
                                        LHSHi.getValueSizeInBits());
  DCI.AddToWorklist(RHSHi.getNode());

  return SplitBit;
}

// Pad the operand of ICmp so we can always get the right result on signed
// comparision for example we have:
// i1 c = icmp i12 a, i12 b signed_cc
// we can pad the higher bits of a with 1 and the higher bits of a with 0:
// i16 sum, i1 c = signed_ext_to_i16(a) + signed_ext_to_i16(b)
static void PadICmpOperand(TargetLowering::DAGCombinerInfo &DCI, DebugLoc dl,
                           SDValue &LHS, SDValue &RHS, ISD::CondCode CC) {
  SelectionDAG &DAG = DCI.DAG;
  unsigned ActualBits = VTargetLowering::computeSizeInBits(LHS);
  assert(VTargetLowering::computeSizeInBits(LHS)
          == VTargetLowering::computeSizeInBits(RHS)
         && "Bitwidth do not match!");

  EVT VT = VTargetLowering::getRoundIntegerOrBitType(ActualBits,
                                                     *DAG.getContext());
  if (VT.getSizeInBits() > ActualBits) {
    LHS = VTargetLowering::getExtend(DAG, dl, LHS, VT.getSizeInBits(),
                                     ISD::isSignedIntSetCC(CC));
    DCI.AddToWorklist(LHS.getNode());
    RHS = VTargetLowering::getExtend(DAG, dl, RHS, VT.getSizeInBits(),
                                     ISD::isSignedIntSetCC(CC));
    DCI.AddToWorklist(RHS.getNode());
  }
}

inline static SDValue BuildICmpLowPart(TargetLowering::DAGCombinerInfo &DCI,
                                       SDNode *N, SDValue LHS, SDValue RHS,
                                       bool Commuted) {
  SelectionDAG &DAG = DCI.DAG;
  DebugLoc dl = N->getDebugLoc();
  CondCodeSDNode *CCNode = cast<CondCodeSDNode>(N->getOperand(2));
  ISD::CondCode CC = CCNode->get();
  if (Commuted) CC = ISD::getSetCCSwappedOperands(CC);

  PadICmpOperand(DCI, dl, LHS, RHS, CC);

  // Lower part of ICmp is always unsigned because only the signed bit make
  // signed compare difference from unsigned.
  switch (CC) {
  default: llvm_unreachable("Unknown integer setcc!");
  case ISD::SETEQ:
  case ISD::SETNE:  /*do nothing*/ break;
  case ISD::SETLT:
  case ISD::SETULT: CC = ISD::SETULT; break;
  case ISD::SETGT:
  case ISD::SETUGT: CC = ISD::SETUGT; break;
  case ISD::SETLE:
  case ISD::SETULE: CC = ISD::SETULE; break;
  case ISD::SETGE:
  case ISD::SETUGE: CC = ISD::SETUGE; break;
  }

  SDValue Lo = DAG.getNode(VTMISD::ICmp, dl, MVT::i1, LHS, RHS,
                           DAG.getCondCode(CC));
  DCI.AddToWorklist(Lo.getNode());
  return Lo;
}

inline static SDValue BuildICmpHighPart(TargetLowering::DAGCombinerInfo &DCI,
                                        SDNode *N, SDValue LHS, SDValue RHS,
                                        SDValue Lo, bool Commuted) {
  SelectionDAG &DAG = DCI.DAG;
  DebugLoc dl = N->getDebugLoc();
  CondCodeSDNode *CCNode = cast<CondCodeSDNode>(N->getOperand(2));
  ISD::CondCode CC = CCNode->get();
  if (Commuted) CC = ISD::getSetCCSwappedOperands(CC);

  PadICmpOperand(DCI, dl, LHS, RHS, CC);

  SDValue Hi = DAG.getNode(VTMISD::ICmp, dl, MVT::i1, LHS, RHS,
                           DAG.getCondCode(CC));
  DCI.AddToWorklist(Hi.getNode());

  return Hi;
}

static SDValue ConcatICmps(TargetLowering::DAGCombinerInfo &DCI,
                           SDNode *N, SDValue Hi, SDValue Lo) {
  SelectionDAG &DAG = DCI.DAG;
  DebugLoc dl = N->getDebugLoc();

  SDValue HiLHS = Hi->getOperand(0), HiRHS = Hi->getOperand(1);
  SDValue HiEq = DAG.getNode(VTMISD::ICmp, dl, MVT::i1, HiLHS, HiRHS,
                             DAG.getCondCode(ISD::SETEQ));
  DCI.AddToWorklist(HiEq.getNode());

  // Return Hi(LHS) == Hi(RHS) ? LoCmp : HiCmp;
  return DAG.getNode(ISD::OR, dl, MVT::i1,
                              DAG.getNode(ISD::AND, dl, MVT::i1, HiEq, Lo),
                              DAG.getNode(ISD::AND, dl, MVT::i1,
                                          VTargetLowering::getNot(DAG, dl, HiEq),
                                          Hi));
}

#define GETLHSNOT(WHAT) GetLHSNot##WHAT

#define DEF_GETLHSNOT(WHAT) \
static SDValue GETLHSNOT(WHAT)(SDValue LHS, EVT RHSVT, SelectionDAG &DAG,\
                               TargetLowering::DAGCombinerInfo &DCI) {\
  SDValue LHSWHAT = GetLHS##WHAT(LHS, RHSVT, DAG, DCI);\
  DCI.AddToWorklist(LHSWHAT.getNode());\
  return VTargetLowering::getNot(DAG, LHS->getDebugLoc(), LHSWHAT);\
}

#define GETLHSBINOP(OP, LHS, RHS) GetLHS##LHS##OP##RHS
#define DEF_GETLHSBINOP(OP, LHS, RHS) \
static SDValue GETLHSBINOP(OP, LHS, RHS)(SDValue LHS, EVT RHSVT,\
                                         SelectionDAG &DAG,\
                                         TargetLowering::DAGCombinerInfo &DCI) {\
  /*Work around for GCC does not accept something like "ISD::##OP"*/\
  unsigned OpCOR = ISD::OR;\
  (void) OpCOR;\
  unsigned OpCAND = ISD::AND;\
  (void) OpCAND;\
  SDValue OPLHS = GetLHS##LHS(LHS, RHSVT, DAG, DCI);\
  DCI.AddToWorklist(OPLHS.getNode());\
  SDValue OPRHS = GetLHS##RHS(LHS, RHSVT, DAG, DCI);\
  DCI.AddToWorklist(OPRHS.getNode());\
  return DAG.getNode(OpC##OP, LHS->getDebugLoc(), MVT::i1, OPLHS, OPRHS);\
}

static SDValue GetLHSNotZero(SDValue LHS, EVT RHSVT, SelectionDAG &DAG,
                             TargetLowering::DAGCombinerInfo &DCI) {
  // A value is not zero, if it has some bit set.
  return VTargetLowering::getReductionOp(DAG, VTMISD::ROr, LHS->getDebugLoc(), LHS);
}

static SDValue GetLHSAllOnes(SDValue LHS, EVT RHSVT, SelectionDAG &DAG,
                             TargetLowering::DAGCombinerInfo &DCI) {
  // A value is all ones, if it has all bit set.
  return VTargetLowering::getReductionOp(DAG, VTMISD::RAnd, LHS->getDebugLoc(), LHS);
}

static SDValue GetLHSNegative(SDValue LHS, EVT RHSVT, SelectionDAG &DAG,
                              TargetLowering::DAGCombinerInfo &DCI) {
  return VTargetLowering::getSignBit(DAG, LHS->getDebugLoc(), LHS);
}

DEF_GETLHSNOT(AllOnes)
DEF_GETLHSNOT(NotZero)
DEF_GETLHSNOT(Negative)
DEF_GETLHSBINOP(OR, AllOnes, NotNegative)
DEF_GETLHSBINOP(OR, NotNotZero, Negative)
DEF_GETLHSBINOP(AND, NotZero, NotNegative)
DEF_GETLHSBINOP(AND, NotAllOnes, Negative)


static SDValue GetAlwaysFalse(SDValue LHS, EVT RHSVT, SelectionDAG &DAG,
                              TargetLowering::DAGCombinerInfo &DCI) {
  return DAG.getTargetConstant(0, MVT::i1);
}

static SDValue GetAlwaysTrue(SDValue LHS, EVT RHSVT, SelectionDAG &DAG,
                              TargetLowering::DAGCombinerInfo &DCI) {
  return DAG.getTargetConstant(1, MVT::i1);
}

template<typename OnRHSZeroFunc, typename OnRHSAllOnesFunc>
static
SDValue PerfromRHSConstantGenericCombine(SDValue LHS, uint64_t RHSVal,
                                         EVT RHSVT, SelectionDAG &DAG,
                                         DebugLoc dl,
                                         TargetLowering::DAGCombinerInfo &DCI,
                                         OnRHSZeroFunc &OnRHSZero,
                                         OnRHSAllOnesFunc &OnRHSAllOnes) {
  if (isNullValue(RHSVal, RHSVT.getSizeInBits()))
    return OnRHSZero(LHS, RHSVT, DAG, DCI);
  if (isAllOnesValue(RHSVal, RHSVT.getSizeInBits()))
    return OnRHSAllOnes(LHS, RHSVT, DAG, DCI);

  return SDValue();
}

static
SDValue PerfromICmpRHSConstantCombine(SDNode *N, TargetLowering::DAGCombinerInfo &DCI,
                                      bool Commuted = false) {
  DebugLoc dl = N->getDebugLoc();
  SelectionDAG &DAG = DCI.DAG;
  SDValue LHS = N->getOperand(0 ^ Commuted), RHS = N->getOperand(1 ^ Commuted);
  CondCodeSDNode *CCNode = cast<CondCodeSDNode>(N->getOperand(2));
  ISD::CondCode CC = CCNode->get();
  if (Commuted) CC = ISD::getSetCCSwappedOperands(CC);

  uint64_t RHSVal;

  unsigned RHSSize = ExtractConstant(RHS, RHSVal);
  if (RHSSize) {
    LLVMContext &Cntx = *DAG.getContext();
    EVT RHSVT = EVT::getIntegerVT(Cntx, RHSSize);

    switch (CC) {
    case ISD::SETUGT:
      return PerfromRHSConstantGenericCombine(LHS, RHSVal, RHSVT, DAG, dl, DCI,
          // Lower a > 0 to a != 0 for unsigned greater than.
                                   GetLHSNotZero,
          // We never have an unsigned value greater than ~0
                                   GetAlwaysFalse);
    case ISD::SETUGE:
      return PerfromRHSConstantGenericCombine(LHS, RHSVal, RHSVT, DAG, dl, DCI,
          // All unsigned value is greater than or equal to 0.
                                   GetAlwaysTrue,
          // An unsigned value is greater than or equal to ~0 only it is ~0
                                   GetLHSNotAllOnes);
    case ISD::SETULT:
      return PerfromRHSConstantGenericCombine(LHS, RHSVal, RHSVT, DAG, dl, DCI,
          // We never have an unsigned value less than 0
                                   GetAlwaysFalse,
          // An unsigned value is less than ~0 only it is not ~0
                                   GetLHSNotAllOnes);
    case ISD::SETULE:
      return PerfromRHSConstantGenericCombine(LHS, RHSVal, RHSVT, DAG, dl, DCI,
          // An unsigned value is less than or equal to 0 only is 0.
                                   GetLHSNotNotZero,
          // All unsigned value is less than or equal to ~0
                                   GetAlwaysTrue);
    case ISD::SETGT:
      return PerfromRHSConstantGenericCombine(LHS, RHSVal, RHSVT, DAG, dl, DCI,
          // An signed value is greater than 0
          // if its signed bit not set and Not zero.
                                   GetLHSNotZeroANDNotNegative,
          // An signed value is greater than -1
          // if its signed bit not set.
                                   GetLHSNotNegative);
    case ISD::SETGE:
      return PerfromRHSConstantGenericCombine(LHS, RHSVal, RHSVT, DAG, dl, DCI,
          // An signed value is greater than or equal to 0
          // if its signed bit not set.
                                   GetLHSNotNegative,
          // An signed value is greater than or equal to -1
          // if its -1 or signed bit not set.
                                   GetLHSAllOnesORNotNegative);
    case ISD::SETLT:
      return PerfromRHSConstantGenericCombine(LHS, RHSVal, RHSVT, DAG, dl, DCI,
          // An signed value is less than  0 if its signed bit set.
                                   GetLHSNegative,
          // An signed value is less than -1 its signed bit set and not equal to -1
                                   GetLHSNotAllOnesANDNegative);
    case ISD::SETLE:
      return PerfromRHSConstantGenericCombine(LHS, RHSVal, RHSVT, DAG, dl, DCI,
          // An signed value is less than or equal to 0
          // if its signed bit set or it is 0.
                                   GetLHSNotNotZeroORNegative,
          // An signed value is less than  or equal to -1 its signed bit set
                                   GetLHSNegative);
    default: break;
    }
  }

  return commuteAndTryAgain(N, DCI, Commuted, PerfromICmpRHSConstantCombine);
}

static
SDValue PerformICmpSplit(SDNode *N, TargetLowering::DAGCombinerInfo &DCI,
                         bool Commuted = false) {
  // Can we find optimization opportunity by spliting the comparison?
  SDValue RV = PromoteBinOpBitCat(N, DCI,
                                  // No need to get bitslice from hi part and lo before
                                  // concact them.
                                  false,
                                  ConcatICmps, BuildICmpLowPart,
                                  BuildICmpHighPart, GetICmpSplitBit,
                                  Commuted);
  if (RV.getNode()) return RV;

  return commuteAndTryAgain(N, DCI, Commuted, PerformICmpSplit);
}

static
SDValue PerfromICmpCombine(SDNode *N, TargetLowering::DAGCombinerInfo &DCI) {
  DebugLoc dl = N->getDebugLoc();
  SelectionDAG &DAG = DCI.DAG;
  LLVMContext &Cntx = *DAG.getContext();
  SDValue LHS = N->getOperand(0), RHS = N->getOperand(1);

  CondCodeSDNode *CCNode = dyn_cast<CondCodeSDNode>(N->getOperand(2));

  // The node is already Lowered for ISel.
  if (CCNode == 0) return SDValue();

  ISD::CondCode CC = CCNode->get();

  uint64_t LHSVal, RHSVal;
  unsigned LHSSize = ExtractConstant(LHS, LHSVal);

  unsigned RHSSize = ExtractConstant(RHS, RHSVal);
  // Do we got a constant comparison?
  if (RHSSize && LHSSize) {
    EVT LHSVT = EVT::getIntegerVT(Cntx, LHSSize);
    EVT RHSVT = EVT::getIntegerVT(Cntx, RHSSize);
    return DAG.FoldSetCC(MVT::i1, DAG.getTargetConstant(LHSVal, LHSVT),
                                  DAG.getTargetConstant(LHSVal, RHSVT),
                                  CC, dl);
  }

  // Lower SETNE and SETEQ
  if (CC == ISD::SETNE || CC == ISD::SETEQ) {

    unsigned CmpWidth = VTargetLowering::computeSizeInBits(LHS);
    assert(CmpWidth == VTargetLowering::computeSizeInBits(RHS)
      && "Compare operand with difference width!");

    EVT OperandVT = VTargetLowering::getRoundIntegerOrBitType(CmpWidth, Cntx);

    SDValue NE = DAG.getNode(ISD::XOR, dl, OperandVT, LHS, RHS);
    DCI.AddToWorklist(NE.getNode());
    NE = VTargetLowering::getReductionOp(DAG, VTMISD::ROr, dl, NE);
    if (CC == ISD::SETNE) return NE;

    // Else it is a SETEQ, just get it from not(SETNE);
    DCI.AddToWorklist(NE.getNode());
    return VTargetLowering::getNot(DAG, dl, NE);
  }

  SDValue RV = PerfromICmpRHSConstantCombine(N, DCI);
  if (RV.getNode()) return RV;

  return PerformICmpSplit(N, DCI);
}

SDValue VTargetLowering::PerformDAGCombine(SDNode *N,
                                           TargetLowering::DAGCombinerInfo &DCI)
                                           const {
  if (DCI.isBeforeLegalize()) return SDValue();

  switch (N->getOpcode()) {
  case VTMISD::BitCat:
    return PerformBitCatCombine(N, DCI);
  case VTMISD::BitSlice:
    return PerformBitSliceCombine(N, DCI);
  case ISD::ADDE:
    if(EnableArithOpt)  return PerformAddCombine(N, DCI);
    break;
  case ISD::UMUL_LOHI:
  case ISD::MUL:
    if(EnableArithOpt)  return PerformMulCombine(N, DCI);
    break;
  case VTMISD::ICmp:
    if(EnableArithOpt)  return PerfromICmpCombine(N, DCI);
    break;
  case ISD::ROTL:
  case ISD::ROTR:
  case ISD::SHL:
  case ISD::SRA:
  case ISD::SRL:
    return PerformShiftImmCombine(N, DCI);
  case ISD::AND:
  case ISD::OR:
  case ISD::XOR:
    return PerformLogicCombine(N, DCI);
  case VTMISD::Not:
    return PerformNotCombine(N, DCI);
  case VTMISD::RAnd:
  case VTMISD::ROr:
  case VTMISD::RXor:
    return PerformReduceCombine(N, DCI);
  }

  return SDValue();
}
