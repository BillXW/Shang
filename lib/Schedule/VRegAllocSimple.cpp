//===- VRegAllocSimple.cpp - Simple Register Allocation ---------*- C++ -*-===//
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
// This file defines a simple register allocation pass for Verilog target
// machine.
//
//===----------------------------------------------------------------------===//

#include "CompGraphTraits.h"
#include "CompGraph.h"
#include "CompGraphDOT.h"

#include "vtm/VFInfo.h"
#include "vtm/VRegisterInfo.h"
#include "vtm/MicroState.h"
#include "vtm/Passes.h"
#include "vtm/VInstrInfo.h"

//Dirty Hack:
#include "llvm/../../lib/CodeGen/VirtRegMap.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Function.h"
#include "llvm/PassAnalysisSupport.h"
#include "llvm/CodeGen/CalcSpillWeights.h"
#include "llvm/CodeGen/EdgeBundles.h"
#include "llvm/CodeGen/LiveIntervalAnalysis.h"
#include "llvm/CodeGen/LiveStackAnalysis.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineLoopRanges.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#define DEBUG_TYPE "vtm-regalloc"
#include "llvm/Support/Debug.h"

using namespace llvm;

STATISTIC(LIMerged,
          "Number of live intervals merged in resource binding pass");
static cl::opt<bool> DisableFUSharing("vtm-disable-fu-sharing",
                                      cl::desc("Disable function unit sharing"),
                                      cl::init(false));

namespace {
struct VRASimple : public MachineFunctionPass {
  // Context.
  MachineFunction *MF;
  VFInfo *VFI;
  MachineRegisterInfo *MRI;
  const TargetInstrInfo *TII;
  VRegisterInfo *TRI;
  // Analysis
  VirtRegMap *VRM;
  LiveIntervals *LIS;

  VRASimple();
  void init(VirtRegMap &vrm, LiveIntervals &lis);

  static char ID;

  void getAnalysisUsage(AnalysisUsage &AU) const;
  void releaseMemory();

  void assign(LiveInterval &VirtReg, unsigned PhysReg) {
    assert(!VRM->hasPhys(VirtReg.reg) && "Duplicate VirtReg assignment");
    VRM->assignVirt2Phys(VirtReg.reg, PhysReg);
  }

  LiveInterval *getInterval(unsigned RegNum) {
    if (MRI->reg_nodbg_empty(RegNum) || !LIS->hasInterval(RegNum)) {
      return 0;
    }

    LiveInterval &LI = LIS->getInterval(RegNum);
    if (LI.empty()) {
      LIS->removeInterval(RegNum);
      return 0;
    }

    return &LI;
  }

  unsigned getBitWidthOf(unsigned RegNum) {
    unsigned BitWidth = 0;
    for (MachineRegisterInfo::def_iterator I = MRI->def_begin(RegNum);
         I != MachineRegisterInfo::def_end(); ++I) {
      ucOperand &DefOp = cast<ucOperand>(I.getOperand());
      BitWidth = std::max(BitWidth, DefOp.getBitWidth());
    }

    assert(BitWidth && "Unexpected empty define register");
    return BitWidth;
  }

  // Run the functor on each definition of the register, and return true
  // if any functor return true (this means we meet some unexpected situation),
  // return false otherwise.
  template<class DefFctor>
  bool iterateUseDefChain(unsigned Reg, DefFctor &F) const {
    for (MachineRegisterInfo::reg_iterator I = MRI->reg_begin(Reg),
         E = MRI->reg_end(); I != E; ++I)
      if (F(I)) return true;

    return false;
  }

  void joinPHINodeIntervals();
  void mergeLI(LiveInterval *FromLI, LiveInterval *ToLI,
               bool AllowOverlap = false);

  // Put all datapath op that using the corresponding register of the LI to
  // the user vector.
  typedef DenseMap<MachineInstr*, unsigned, MachineInstrExpressionTrait>
    DatapathOpMap;
  void mergeIdenticalDatapath(LiveInterval *LI);
  typedef EquivalenceClasses<unsigned> EquRegClasses;
  void mergeEquLIs(EquRegClasses &LIs);

  // Pre-bound function unit binding functions.
  void bindMemoryBus();
  void bindBlockRam();
  void bindCalleeFN();
  void bindDstMux();
  unsigned allocateCalleeFNPorts(unsigned RegNum);

  typedef const std::vector<unsigned> VRegVec;
  // Register/FU Compatibility Graph.
  typedef CompGraph<LiveInterval*> LICGraph;
  typedef CompGraphNode<LiveInterval*> LICGraphNode;

  // Compatibility Graph building.
  void buildCompGraph(LICGraph &G);

  template<class CompEdgeWeight>
  bool reduceCompGraph(LICGraph &G, CompEdgeWeight &C);

  void bindCompGraph(LICGraph &G);
  void bindICmps(LICGraph &G);

  bool runOnMachineFunction(MachineFunction &F);
  void addMBBLiveIns(MachineFunction *MF);

  const char *getPassName() const {
    return "Verilog Backend Resource Binding Pass";
  }
};

struct WidthChecker {
  // The Maximum bit width of current register.
  unsigned CurMaxWidth;

  WidthChecker() : CurMaxWidth(0) {}

  void resetWidth() { CurMaxWidth = 0; }

  bool checkWidth(unsigned CurWidth) {
    //CurMaxWidth = std::max(CurMaxWidth, CurWidth);
    if (!CurMaxWidth)
      CurMaxWidth = CurWidth;
    else if (CurMaxWidth != CurWidth)
      // Do not merge regisrters with difference width.
      return false;

    return true;
  }

  unsigned getWidth() const { return CurMaxWidth; }
};

// Implement this with some hash function?
static uint64_t getRegKey(uint32_t Reg, uint32_t OpIdx) {
  union {
    uint64_t Data;
    struct {
      uint32_t RegNum;
      uint32_t OpIdx;
    } S;
  } U;

  U.S.RegNum = Reg;
  U.S.OpIdx = OpIdx;

  return U.Data;
}

//static uint64_t getRegKey(MachineOperand &MO, uint8_t OpIdx = 0) {
//  return getRegKey(MO.getReg(), OpIdx, MO.getSubReg());
//}

template<int NUMSRC>
struct FaninChecker {
  // TODO Explain sources and fanins
  // All copy source both fu of the edge.
  SmallSet<uint64_t, 4> MergedFanins[NUMSRC];
  unsigned Fanins[2][NUMSRC];
  unsigned CurSrc;
  unsigned ExtraCost;
  // TODO: Timing cost.
  // The more predicate value we have, the bigger latency will get to select
  // a value from the mux value.
  unsigned PredNum;
  SmallSet<uint64_t, 4> Preds;

  FaninChecker() {}

  void resetSrcs() {
    ExtraCost = 0;
    CurSrc = 0;
    for (unsigned i = 0; i < NUMSRC; ++i) {
      MergedFanins[i].clear();
      Fanins[0][i] = 0;
      Fanins[1][i] = 0;
    }
  }

  // Notify the source checker that we are switching to next source.
  void nextSrc() { ++CurSrc; }

  template<int N>
  void addFanin(ucOperand &FanInOp) {
    if (FanInOp.isReg()) {
      // Igore the operand index.
      MergedFanins[N].insert(getRegKey(FanInOp.getReg(), 0));
      ++Fanins[CurSrc][N];
    } else
      ExtraCost += /*LUT Cost*/ VFUs::LUTCost;
  }
  template<int N>
  void addFanin(MachineOperand &SrcOp) {
    FaninChecker<NUMSRC>::addFanin<N>(cast<ucOperand>(SrcOp));
  }

  template<int N>
  void removeSrcReg(unsigned Reg) {
    MergedFanins[N].erase(Reg);
  }

  template<int N>
  int getMergedSrcMuxSize() const {
    return MergedFanins[N].size();
  }

  template<int N>
  int getSrcNum(int SrcIndex) const {
    return Fanins[SrcIndex][N];
  }

  template<int N>
  int getSavedSrcMuxCost(int BitWidth){
    return (VFUs::getMuxCost(getSrcNum<N>(0)) + VFUs::getMuxCost(getSrcNum<N>(1))
            - VFUs::getMuxCost(getMergedSrcMuxSize<N>())) *BitWidth;
  }

  int getExtraCost() const { return ExtraCost; }
  //// Total cost.
  int getTotalSavedSrcMuxCost(int BitWidth);
  //int getTotalSrcMuxCost(int BitWidth);
  int getMaxMergedSrcMuxSize();
};

  template<>
  int FaninChecker<1>::getTotalSavedSrcMuxCost(int BitWidth){
    return getSavedSrcMuxCost<0>(BitWidth);
  }

  template<>
  int FaninChecker<2>::getTotalSavedSrcMuxCost(int BitWidth){
    return getSavedSrcMuxCost<0>(BitWidth) + getSavedSrcMuxCost<1>(BitWidth);
  }

  template<>
  int FaninChecker<1>::getMaxMergedSrcMuxSize() {
  return getMergedSrcMuxSize<0>();
  }

  template<>
  int FaninChecker<2>::getMaxMergedSrcMuxSize() {
  return std::max(getMergedSrcMuxSize<0>(), getMergedSrcMuxSize<1>());
  }

struct FanoutChecker {
  // All copy source both fu of the edge.
  SmallSet<uint64_t, 8> mergedFanouts;
  int NumFanouts;
  void resetFanouts() {
    mergedFanouts.clear();
    NumFanouts = 0;
  }

  unsigned addFanout(MachineRegisterInfo::reg_iterator I) {
    MachineInstr *MI = &*I;

    // Ignore the datapath at the moment.
    if (!isCtrlBundle(MI)) return 0;
    ++NumFanouts;
    if (MI->getNumOperands() == 0) {
      assert(MI->getOpcode() == VTM::VOpRet && "Unexpected empty operand op!");
      mergedFanouts.insert(getRegKey(1, -1));
      return 0;
    }

    MachineOperand &DefMO = MI->getOperand(0);
    if (!DefMO.isReg()) {
      if (MI->getOpcode() == VTM::VOpRetVal)
        mergedFanouts.insert(getRegKey(0, -2));

      return 0;
    }

    unsigned DstReg = DefMO.getReg();
    mergedFanouts.insert(getRegKey(DstReg, I.getOperandNo()));
    return DstReg;
  }

  int getNumMergedFanouts() {
    return mergedFanouts.size();
  }

  int getSavedFanoutsCost(int BitWidth) {
    return (VFUs::getMuxCost(NumFanouts) - VFUs::getMuxCost(getNumMergedFanouts()))
            * BitWidth;
  }

};

template<int NUMSRC>
struct CompEdgeWeightBase : public FaninChecker<NUMSRC>, public FanoutChecker,
                            public WidthChecker {
  VRASimple *VRA;
  // The pre-bit cost of this kind of function unit.
  unsigned *const Cost;

  CompEdgeWeightBase(VRASimple *V, unsigned cost[]) : VRA(V), Cost(cost) {}

  void reset() {
    resetFanouts();
    FaninChecker<NUMSRC>::resetSrcs();
    resetWidth();
  }

  int computeWeight(unsigned FanInWidth, unsigned FanOutWidth) {
    // Only merge the register if the mux size not exceed the max allowed size.
    if (FaninChecker<NUMSRC>::getMaxMergedSrcMuxSize() > int(VFUs::MaxAllowedMuxSize))
      return CompGraphWeights::HUGE_NEG_VAL;

    int Weight = 0;
    // We can save some register if we merge these two registers.
    unsigned Index = std::min(FanInWidth, 64u);
    Weight += /*FU Cost*/ Cost[Index];
    Weight += FaninChecker<NUMSRC>::getTotalSavedSrcMuxCost(FanInWidth);
    Weight += getSavedFanoutsCost(FanOutWidth);
    return Weight;
  }

  int computeWeight(int BitWidth) { return computeWeight(BitWidth, BitWidth); }
};

// Weight computation functor for register Compatibility Graph.
struct CompRegEdgeWeight : public CompEdgeWeightBase<1> {
  // Context
  // Do not try to merge PHI copies, it is conditional and we cannot handle
  // them correctly at the moment.
  bool hasPHICopy;
  unsigned DstReg;

  typedef CompEdgeWeightBase<1> Base;
  CompRegEdgeWeight(VRASimple *V, unsigned cost[]) : Base(V, cost) {}

  void reset(unsigned DstR) {
    DstReg = DstR;
    hasPHICopy = false;
    Base::reset();
  }

  bool visitUse(MachineRegisterInfo::reg_iterator I) {
    unsigned DefReg = addFanout(I);
    if (I->getOpcode() == VTM::VOpMvPhi || I->getOpcode() == VTM::VOpSel) {
      // We cannot handle these ops correctly after their src and dst merged.
      if (DefReg == DstReg) return true;
    }

    return false;
  }

  bool visitDef(MachineInstr *MI) {
    switch (MI->getOpcode()) {
    case VTM::VOpMvPhi:
      // FIXME: Merging VOpMvPhi break adpcm_main_IMS_ASAP.
      return true;
    case VTM::VOpMvPipe:
    case VTM::VOpMove:
    case VTM::VOpMoveArg:
    case VTM::VOpReadFU:
      addFanin<0>(MI->getOperand(1));
      break;
    case VTM::VOpReadReturn:
      addFanin<0>(MI->getOperand(2));
      break;
    default:
#ifndef NDEBUG
      MI->dump();
      llvm_unreachable("Unexpected opcode in CompRegEdgeWeight!");
#endif
    }
    return false;
  }

  // Run on the definition of a register to collect information about the live
  // interval.
  bool operator()(MachineRegisterInfo::reg_iterator I) {
    MachineOperand &MO = I.getOperand();
    if (MO.isDef()) {
      // 1. Get the bit width information.
      if (!checkWidth(cast<ucOperand>(I.getOperand()).getBitWidth()))
        return true;

      // 2. Analyze the definition op.
      return visitDef(&*I);
    }

    if (!MO.isImplicit()) return visitUse(I);

    return false;
  }

  // Run on the edge of the Compatibility Graph and return the weight of the
  // edge.
  int operator()(LiveInterval *Src, LiveInterval *Dst) {
    assert(Dst && Src && "Unexpected null li!");
    reset(Dst->reg);

    if (VRA->iterateUseDefChain(Src->reg, *this))
      return CompGraphWeights::HUGE_NEG_VAL;

    // Go on check next source.
    nextSrc();

    if (VRA->iterateUseDefChain(Dst->reg, *this))
      return CompGraphWeights::HUGE_NEG_VAL;

    if (hasPHICopy) return CompGraphWeights::HUGE_NEG_VAL;
    // Src register appear in the src of mux do not cost anything.
    removeSrcReg<0>(Src->reg);

    return Cost[0] * getWidth();
  }
};

// Weight computation functor for commutable binary operation Compatibility
// Graph.
template<unsigned OpCode, unsigned OpIdx>
struct CompBinOpEdgeWeight : public CompEdgeWeightBase<2> {
  // Is there a copy between the src and dst of the edge?
  //bool hasCopy;

  void reset() {
    //hasCopy = 0;
    Base::reset();
  }

  template<unsigned Offset>
  void visitOperand(MachineInstr *MI) {
    addFanin<Offset>(MI->getOperand(OpIdx + Offset));
  }

  typedef CompEdgeWeightBase<2> Base;
  CompBinOpEdgeWeight(VRASimple *V, unsigned cost[]) : Base(V, cost) {}

  // Run on the use-def chain of a FU to collect information about the live
  // interval.
  bool operator()(MachineRegisterInfo::reg_iterator I) {
    MachineOperand &MO = I.getOperand();
    if (I.getOperand().isDef()) {
      // 1. Get the bit width information.
      if (!checkWidth(cast<ucOperand>(MO).getBitWidth()))
        return true;
      // 2. Analyze the definition op.
      MachineInstr *MI = &*I;
      assert(MI->getOpcode() == OpCode && "Unexpected Opcode!");

      visitOperand<0>(MI);
      visitOperand<1>(MI);
    }

    if (!MO.isImplicit()) addFanout(I);

    return false;
  }

  int operator()(LiveInterval *Src, LiveInterval *Dst) {
    assert(Dst && Src && "Unexpected null li!");
    reset();

    if (VRA->iterateUseDefChain(Src->reg, *this))
      return CompGraphWeights::HUGE_NEG_VAL;

    // Go on check next source.
    nextSrc();

    if (VRA->iterateUseDefChain(Dst->reg, *this))
      return CompGraphWeights::HUGE_NEG_VAL;

    return computeWeight(getWidth());
  }
};

struct CompICmpEdgeWeight : public CompBinOpEdgeWeight<VTM::VOpICmp, 1> {
  bool hasSignedCC, hasUnsignedCC;

  typedef CompBinOpEdgeWeight<VTM::VOpICmp, 1> Base;
  CompICmpEdgeWeight(VRASimple *V, unsigned cost[]) : Base(V, cost) {}

  void reset() {
    hasSignedCC = false;
    hasUnsignedCC = false;
    //hasCopy = 0;
    Base::reset();
  }

  bool hasInCompatibleCC(unsigned CC) {
    if (CC == VFUs::CmpSigned) {
      hasSignedCC = true;
      return hasUnsignedCC;
    }

    if (CC == VFUs::CmpUnsigned) {
      hasUnsignedCC = true;
      return hasSignedCC;
    }

    return false;
  }

  // Run on the use-def chain of a FU to collect information about the live
  // interval.
  bool operator()(MachineRegisterInfo::reg_iterator I) {
    MachineOperand &MO = I.getOperand();
    if (I.getOperand().isDef()) {
      // 2. Analyze the definition op.
      MachineInstr *MI = &*I;
      assert(MI->getOpcode() == VTM::VOpICmp && "Unexpected Opcode!");

      ucOperand &CondCode = cast<ucOperand>(MI->getOperand(3));
      // Get the bit width information.
      if (!checkWidth(CondCode.getBitWidth()))
        return true;
      // Are these CC compatible?
      if (hasInCompatibleCC(CondCode.getImm()))
        return true;

      visitOperand<0>(MI);
      visitOperand<1>(MI);
    }

    if (!MO.isImplicit()) addFanout(I);

    return false;
  }

  int operator()(LiveInterval *Src, LiveInterval *Dst) {
    assert(Dst && Src && "Unexpected null li!");
    reset();

    if (VRA->iterateUseDefChain(Src->reg, *this))
      return CompGraphWeights::HUGE_NEG_VAL;

    if (VRA->iterateUseDefChain(Dst->reg, *this))
      return CompGraphWeights::HUGE_NEG_VAL;

    return computeWeight(getWidth(), 1);
  }
};
}

namespace llvm {
  // Specialized For liveInterval.
template<> struct CompGraphTraits<LiveInterval*> {
  static bool isEarlier(LiveInterval *LHS, LiveInterval *RHS) {
    assert(LHS && RHS && "Unexpected virtual node!");

    return *LHS < *RHS;
  }

  static bool compatible(LiveInterval *LHS, LiveInterval *RHS) {
    if (LHS == 0 || RHS == 0) return true;

    // A LiveInterval may has multiple live ranges for example:
    // LI0 = [0, 4), [32, 35)
    // Suppose we have another LiveInterval LI1 and LI2:
    // LI1 = [5, 9)
    // LI2 = [26, 33)
    // If we make LiveIntervals compatible if they are not overlap, we may
    // have a compatible graph for LI0, LI1 and LI2 like this:
    // LI0 -> LI1 -> LI2
    // The compatibility in a compatible graph suppose to be transitive,
    // but this not true in the above graph because LI0 is overlap with LI2.
    // This means two live interval may not mark as "compatible" even if they
    // are not overlap.

    // LHS and RHS is compatible if RHS end before LHS begin and vice versa.
    return LHS->beginIndex() >= RHS->endIndex()
            || RHS->beginIndex() >= LHS->endIndex();
  }
};
}

//===----------------------------------------------------------------------===//

FunctionPass *llvm::createSimpleRegisterAllocator() {
  return new VRASimple();
}

char VRASimple::ID = 0;

VRASimple::VRASimple() : MachineFunctionPass(ID) {
  initializeLiveIntervalsPass(*PassRegistry::getPassRegistry());
  initializeSlotIndexesPass(*PassRegistry::getPassRegistry());
  initializeCalculateSpillWeightsPass(*PassRegistry::getPassRegistry());
  initializeLiveStacksPass(*PassRegistry::getPassRegistry());
  initializeMachineDominatorTreePass(*PassRegistry::getPassRegistry());
  initializeMachineLoopInfoPass(*PassRegistry::getPassRegistry());
  initializeVirtRegMapPass(*PassRegistry::getPassRegistry());
  initializeBitLevelInfoPass(*PassRegistry::getPassRegistry());
}

void VRASimple::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<AliasAnalysis>();
  AU.addPreserved<AliasAnalysis>();
  AU.addRequired<LiveIntervals>();
  AU.addPreserved<SlotIndexes>();
  AU.addRequired<CalculateSpillWeights>();
  AU.addRequired<LiveStacks>();
  AU.addPreserved<LiveStacks>();
  AU.addRequiredID(MachineDominatorsID);
  AU.addPreservedID(MachineDominatorsID);
  AU.addRequired<MachineLoopInfo>();
  AU.addPreserved<MachineLoopInfo>();
  AU.addRequired<VirtRegMap>();
  AU.addPreserved<VirtRegMap>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

void VRASimple::releaseMemory() {
  //RegAllocBase::releaseMemory();
}

void VRASimple::init(VirtRegMap &vrm, LiveIntervals &lis) {
  TargetRegisterInfo *RegInfo
    = const_cast<TargetRegisterInfo*>(&vrm.getTargetRegInfo());
  TRI = reinterpret_cast<VRegisterInfo*>(RegInfo);
  MRI = &vrm.getRegInfo();
  VRM = &vrm;
  LIS = &lis;
  // Reset the physics register allocation information before register allocation.
  TRI->resetPhyRegAllocation();
  // FIXME: Init the PhysReg2LiveUnion right before we start to bind the physics
  // registers.
  // PhysReg2LiveUnion.init(UnionAllocator, MRI->getNumVirtRegs() + 1);
  // Cache an interferece query for each physical reg
  // Queries.reset(new LiveIntervalUnion::Query[PhysReg2LiveUnion.numRegs()]);
}

bool VRASimple::runOnMachineFunction(MachineFunction &F) {
  MF = &F;
  VFI = F.getInfo<VFInfo>();

  init(getAnalysis<VirtRegMap>(), getAnalysis<LiveIntervals>());

  DEBUG(dbgs() << "Before simple register allocation:\n";F.dump());
  
  joinPHINodeIntervals();

  // Bind the pre-bind function units.
  bindMemoryBus();
  bindBlockRam();
  bindCalleeFN();
  bindDstMux();

  //Build the Compatibility Graphs
  LICGraph RCG(VTM::DRRegClassID),
           AdderCG(VTM::RADDRegClassID),
           ICmpCG(VTM::RUCMPRegClassID),
           MulCG(VTM::RMULRegClassID),
           MulLHCG(VTM::RMULLHRegClassID),
           AsrCG(VTM::RASRRegClassID),
           LsrCG(VTM::RLSRRegClassID),
           ShlCG(VTM::RSHLRegClassID);
  buildCompGraph(RCG);
  buildCompGraph(AdderCG);
  buildCompGraph(ICmpCG);
  buildCompGraph(MulCG);
  buildCompGraph(MulLHCG);
  buildCompGraph(AsrCG);
  buildCompGraph(LsrCG);
  buildCompGraph(ShlCG);

  CompRegEdgeWeight RegWeight(this, &VFUs::RegCost);
  CompBinOpEdgeWeight<VTM::VOpAdd, 1> AddWeight(this, VFUs::AddCost);
  CompICmpEdgeWeight ICmpWeight(this, VFUs::ICmpCost);
  CompBinOpEdgeWeight<VTM::VOpMult, 1> MulWeiht(this, VFUs::MulCost);
  CompBinOpEdgeWeight<VTM::VOpMultLoHi, 1> MulLHWeiht(this, VFUs::MulCost);
  CompBinOpEdgeWeight<VTM::VOpSRA, 1> SRAWeight(this, VFUs::ShiftCost);
  CompBinOpEdgeWeight<VTM::VOpSRL, 1> SRLWeight(this, VFUs::ShiftCost);
  CompBinOpEdgeWeight<VTM::VOpSHL, 1> SHLWeight(this, VFUs::ShiftCost);

  bool SomethingBound = !DisableFUSharing;
  // Reduce the Compatibility Graphs
  while (SomethingBound) {
    DEBUG(dbgs() << "Going to reduce CompGraphs\n");
    SomethingBound = reduceCompGraph(AsrCG, SRAWeight)
                  || reduceCompGraph(LsrCG, SRLWeight)
                  || reduceCompGraph(ShlCG, SHLWeight)
                  || reduceCompGraph(MulCG, MulWeiht)
                  || reduceCompGraph(MulLHCG, MulLHWeiht)
                  || reduceCompGraph(AdderCG, AddWeight)
                  || reduceCompGraph(ICmpCG, ICmpWeight)
                  || reduceCompGraph(RCG, RegWeight);
  }

  // Bind the Compatibility Graphs
  bindCompGraph(RCG);
  bindCompGraph(AdderCG);
  bindICmps(ICmpCG);
  bindCompGraph(MulCG);
  bindCompGraph(MulLHCG);
  bindCompGraph(AsrCG);
  bindCompGraph(LsrCG);
  bindCompGraph(ShlCG);

  addMBBLiveIns(MF);
  LIS->addKillFlags();

  // FIXME: Verification currently must run before VirtRegRewriter. We should
  // make the rewriter a separate pass and override verifyAnalysis instead. When
  // that happens, verification naturally falls under VerifyMachineCode.
#ifndef NDEBUG
  //if (VerifyEnabled) {
  //  // Verify accuracy of LiveIntervals. The standard machine code verifier
  //  // ensures that each LiveIntervals covers all uses of the virtual reg.

  //  // FIXME: MachineVerifier is badly broken when using the standard
  //  // spiller. Always use -spiller=inline with -verify-regalloc. Even with the
  //  // inline spiller, some tests fail to verify because the coalescer does not
  //  // always generate verifiable code.
  //  MF->verify(this, "In RABasic::verify");

  //  // Verify that LiveIntervals are partitioned into unions and disjoint within
  //  // the unions.
  //  verify();
  //}
#endif // !NDEBUG

  // Run rewriter
  VRM->rewrite(LIS->getSlotIndexes());

  releaseMemory();

  DEBUG(dbgs() << "After simple register allocation:\n";
        //printVMF(dbgs(), F);
  );
  MRI->leaveSSA();
  MRI->invalidateLiveness();
  return true;
}

void VRASimple::addMBBLiveIns(MachineFunction *MF) {
  typedef SmallVector<MachineBasicBlock*, 8> MBBVec;
  MBBVec liveInMBBs;
  MachineBasicBlock &entryMBB = *MF->begin();

  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    unsigned Reg = TargetRegisterInfo::index2VirtReg(i);
    unsigned PhysReg = VRM->getPhys(Reg);
    // Virtual register not bound.
    if (PhysReg == VirtRegMap::NO_PHYS_REG) continue;

    LiveInterval &LI = LIS->getInterval(Reg);

    for (LiveInterval::iterator RI = LI.begin(), RE = LI.end(); RI != RE; ++RI){
      // Find the set of basic blocks which this range is live into...
      liveInMBBs.clear();
      if (!LIS->findLiveInMBBs(RI->start, RI->end, liveInMBBs)) continue;

      // And add the physreg for this interval to their live-in sets.
      for (MBBVec::iterator I = liveInMBBs.begin(), E = liveInMBBs.end();
           I != E; ++I) {
        MachineBasicBlock *MBB = *I;
        if (MBB == &entryMBB) continue;
        if (MBB->isLiveIn(PhysReg)) continue;
        MBB->addLiveIn(PhysReg);
      }
    }
  }
}

void VRASimple::joinPHINodeIntervals() {
  typedef MachineFunction::iterator bb_it;
  typedef MachineBasicBlock::instr_iterator instr_it;

  for (bb_it BI = MF->begin(), BE = MF->end(); BI != BE; ++BI) {
    for (instr_it II = BI->instr_begin(),IE = BI->instr_end();II != IE;/*++II*/){
      MachineInstr *PHIDef = II++;

      if (PHIDef->getOpcode() != VTM::VOpDefPhi) continue;
 
      mergeLI(&LIS->getInterval(PHIDef->getOperand(1).getReg()),
              &LIS->getInterval(PHIDef->getOperand(0).getReg()), true);
      BI->erase_instr(PHIDef);
    }
  }
}

void VRASimple::mergeEquLIs(EquRegClasses &EquLIs) {
  // Merge the equivalence liveintervals.
  typedef EquRegClasses::iterator equ_li_it;
  typedef EquRegClasses::member_iterator mem_it;
  for (equ_li_it I = EquLIs.begin(), E = EquLIs.end(); I != E; ++I) {
    if (!I->isLeader()) continue;

    mem_it MI = EquLIs.member_begin(I);
    LiveInterval *LeaderLI = 0;

    while (MI != EquLIs.member_end()) {
      LiveInterval *CurLI = getInterval(*MI++);
      if (!CurLI) continue;

      if (LeaderLI)
        // FIXME: Why the identical datapath ops overlap?
        mergeLI(CurLI, LeaderLI, true);
      else
        LeaderLI = CurLI;
    }
  }
}

void VRASimple::mergeIdenticalDatapath(LiveInterval *LI) {
  typedef MachineRegisterInfo::use_iterator use_it;
  DatapathOpMap Users;
  EquRegClasses EquLIs;

  for (use_it I = MRI->use_begin(LI->reg), E = MRI->use_end(); I != E; ++I) {
    if (I.getOperand().isImplicit()) continue;

    MachineInstr *MI = &*I;
    if (isCtrlBundle(MI)) continue;

    // Datapath op should define and only define its result at operand 0.
    unsigned NewReg = MI->getOperand(0).getReg();
    // Ignore the dead datapath ops.
    if (MRI->use_empty(NewReg)) continue;

    std::pair<DatapathOpMap::iterator, bool> p =
      Users.insert(std::make_pair(MI, NewReg));
    // Merge all identical datapath ops.
    if (!p.second && NewReg != p.first->second)
      EquLIs.unionSets(NewReg, p.first->second);
  }

  // Merge the equivalence liveintervals.
  mergeEquLIs(EquLIs);
}

void VRASimple::mergeLI(LiveInterval *FromLI, LiveInterval *ToLI,
                        bool AllowOverlap) {
  assert(FromLI != ToLI && "Why we merge the same LI?");
  assert((AllowOverlap || !ToLI->overlaps(*FromLI)) && "Cannot merge LI!");
  assert(getBitWidthOf(FromLI->reg) == getBitWidthOf(ToLI->reg)
         && "Cannot merge LIs difference with bit width!");
  // Merge two live intervals.
  ToLI->MergeRangesInAsValue(*FromLI, ToLI->getValNumInfo(0));
  MRI->replaceRegWith(FromLI->reg, ToLI->reg);
  ++LIMerged;
  // Merge the identical datapath ops.
  mergeIdenticalDatapath(ToLI);
}

void VRASimple::bindMemoryBus() {
  LiveInterval *MemBusLI = 0;

  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    unsigned RegNum = TargetRegisterInfo::index2VirtReg(i);
    if (MRI->getRegClass(RegNum) != VTM::RINFRegisterClass)
      continue;

    if (LiveInterval *LI = getInterval(RegNum)) {
      if (MemBusLI == 0) {
        assign(*LI, TRI->allocateFN(VTM::RINFRegClassID));
        MemBusLI = LI;
        continue;
      }

      // Merge all others LI to MemBusLI.
      mergeLI(LI, MemBusLI);
    }
  }
}

void VRASimple::bindDstMux() {
  std::map<unsigned, LiveInterval*> RepLIs;

  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    unsigned RegNum = TargetRegisterInfo::index2VirtReg(i);
    if (MRI->getRegClass(RegNum) != VTM::RMUXRegisterClass)
      continue;

    if (LiveInterval *LI = getInterval(RegNum)) {
      MachineInstr *MI = MRI->getVRegDef(RegNum);
      assert(MI && MI->getOpcode() == VTM::VOpDstMux && "Unexpected opcode!");
      unsigned MuxNum = VInstrInfo::getPreboundFUId(MI).getFUNum();

      // Merge to the representative live interval.
      LiveInterval *RepLI = RepLIs[MuxNum];
      // Had we allocate a register for this bram?
      if (RepLI == 0) {
        unsigned BitWidth = cast<ucOperand>(MI->getOperand(0)).getBitWidth();
        unsigned PhyReg = TRI->allocateFN(VTM::RMUXRegClassID, BitWidth);
        RepLIs[MuxNum] = LI;
        assign(*LI, PhyReg);
        continue;
      }

      // Merge to the representative live interval.
      // DirtyHack: Now bram is write until finish, it is ok to overlap the
      // live interval of bram for 1 control step(2 indexes in SlotIndex).
      //SlotIndex NextStart = LI->beginIndex().getNextIndex().getNextIndex();
      //assert(!RepLI->overlaps(NextStart, LI->endIndex())
        //&& "Unexpected bram overlap!");
      mergeLI(LI, RepLI);
    }
  }
}

void VRASimple::bindBlockRam() {
  assert(VInstrInfo::isWriteUntilFinish(VTM::VOpBRam)
         && "Expected block ram write until finish!");
  std::map<unsigned, LiveInterval*> RepLIs;

  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    unsigned RegNum = TargetRegisterInfo::index2VirtReg(i);
    if (MRI->getRegClass(RegNum) != VTM::RBRMRegisterClass)
      continue;

    if (LiveInterval *LI = getInterval(RegNum)) {
      MachineInstr *MI = MRI->getVRegDef(RegNum);
      assert(MI->getOpcode() == VTM::VOpBRam && "Unexpected opcode!");
      unsigned BRamNum = VInstrInfo::getPreboundFUId(MI).getFUNum();

      VFInfo::BRamInfo &Info = VFI->getBRamInfo(BRamNum);
      unsigned &PhyReg = Info.PhyRegNum;

      // Had we allocate a register for this bram?
      if (PhyReg == 0) {
        unsigned BitWidth = Info.ElemSizeInBytes * 8;
        PhyReg = TRI->allocateFN(VTM::RBRMRegClassID, BitWidth);
        RepLIs[PhyReg] = LI;
        assign(*LI, PhyReg);
        continue;
      }

      // Merge to the representative live interval.
      LiveInterval *RepLI = RepLIs[PhyReg];
      // DirtyHack: Now bram is write until finish, it is ok to overlap the
      // live interval of bram for 1 control step(2 indexes in SlotIndex).
      SlotIndex NextStart = LI->beginIndex().getNextIndex().getNextIndex();
      assert(!RepLI->overlaps(NextStart, LI->endIndex())
             && "Unexpected bram overlap!");
      mergeLI(LI, RepLI, true);
    }
  }
}

unsigned VRASimple::allocateCalleeFNPorts(unsigned RegNum) {
  unsigned RetPortSize = 0;
  typedef MachineRegisterInfo::use_iterator use_it;
  for (use_it I = MRI->use_begin(RegNum); I != MRI->use_end(); ++I) {
    MachineInstr *MI = &*I;
    if (MI->getOpcode() == VTM::VOpReadFU ||
        MI->getOpcode() == VTM::VOpDisableFU)
      continue;

    assert(MI->getOpcode() == VTM::VOpReadReturn && "Unexpected callee user!");
    assert((RetPortSize == 0 ||
            RetPortSize == cast<ucOperand>(MI->getOperand(0)).getBitWidth())
            && "Return port has multiple size?");
    RetPortSize = cast<ucOperand>(MI->getOperand(0)).getBitWidth();
  }

  return TRI->allocateFN(VTM::RCFNRegClassID, RetPortSize);
}

void VRASimple::bindCalleeFN() {
  std::map<unsigned, LiveInterval*> RepLIs;

  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    unsigned RegNum = TargetRegisterInfo::index2VirtReg(i);
    if (MRI->getRegClass(RegNum) != VTM::RCFNRegisterClass)
      continue;

    if (LiveInterval *LI = getInterval(RegNum)) {
      MachineInstr *MI = MRI->getVRegDef(RegNum);
      assert(MI && MI->getOpcode() == VTM::VOpInternalCall
             && "Unexpected define op of CaleeFN!");
      unsigned FNNum = VInstrInfo::getPreboundFUId(MI).getFUNum();
      LiveInterval *&RepLI = RepLIs[FNNum];

      if (RepLI == 0) {
        // This is the first live interval bound to this callee function.
        // Use this live interval to represent the live interval of this callee
        // function.
        RepLI = LI;
        // Get the return ports of this callee FN.
        unsigned NewFNNum = allocateCalleeFNPorts(RegNum);
        // Also allocate the enable port.
        TRI->getSubRegOf(NewFNNum, 1, 0);
        if (NewFNNum != FNNum)
          VFI->remapCallee(MI->getOperand(1).getSymbolName(), NewFNNum);
        
        assign(*LI, NewFNNum);
        continue;
      }

      // Merge to the representative live interval.
      mergeLI(LI, RepLI);
    }
  }
}

void VRASimple::buildCompGraph(LICGraph &G) {
  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    unsigned RegNum = TargetRegisterInfo::index2VirtReg(i);
    if (MRI->getRegClass(RegNum)->getID() != G.ID)
      continue;

    if (LiveInterval *LI = getInterval(RegNum))
      G.GetOrCreateNode(LI);
  }
}

template<class CompEdgeWeight>
bool VRASimple::reduceCompGraph(LICGraph &G, CompEdgeWeight &C) {
  SmallVector<LiveInterval*, 8> LongestPath, MergedLIs;
  G.updateEdgeWeight(C);

  bool AnyReduced = false;

  for (int PathWeight = G.findLongestPath(LongestPath, true); PathWeight > 0;
       PathWeight = G.findLongestPath(LongestPath, true)) {
    DEBUG(dbgs() << "// longest path in graph: {"
      << TRI->getRegClass(G.ID)->getName() << "} weight:" << PathWeight << "\n";
    for (unsigned i = 0; i < LongestPath.size(); ++i) {
      LiveInterval *LI = LongestPath[i];
      dbgs() << *LI << " bitwidth:" << getBitWidthOf(LI->reg) << '\n';
    });

    LiveInterval *RepLI = LongestPath.pop_back_val();

    // Merge the other LIs in the path to the first LI.
    while (!LongestPath.empty())
      mergeLI(LongestPath.pop_back_val(), RepLI);
    // Add the merged LI back to the graph later.
    MergedLIs.push_back(RepLI);
    AnyReduced = true;
  }

  // Re-add the merged LI to the graph.
  while (!MergedLIs.empty())
    G.GetOrCreateNode(MergedLIs.pop_back_val());

  return AnyReduced;
}

void VRASimple::bindCompGraph(LICGraph &G) {
  unsigned RC = G.ID;
  for (LICGraph::iterator I = G.begin(), E = G.end(); I != E; ++I) {
    LiveInterval *LI = (*I)->get();
    assign(*LI, TRI->allocatePhyReg(RC, getBitWidthOf(LI->reg)));
  }
}

void VRASimple::bindICmps(LICGraph &G) {
  CompICmpEdgeWeight ICmpChecker(this, 0);

  for (LICGraph::iterator I = G.begin(), E = G.end(); I != E; ++I) {
    LiveInterval *LI = (*I)->get();
    // Run the checker on LI to collect the signedness and bitwidth information.
    ICmpChecker.reset();
    bool succ = iterateUseDefChain(LI->reg, ICmpChecker);
    assert(!succ && "Something went wrong while checking LI!");
    (void) succ;

    unsigned FUType = ICmpChecker.hasSignedCC ? VTM::RSCMPRegClassID
                                              : VTM::RUCMPRegClassID;
    unsigned CmpFU = TRI->allocateFN(FUType, ICmpChecker.CurMaxWidth);
    assign(*LI, CmpFU);
  }
}
