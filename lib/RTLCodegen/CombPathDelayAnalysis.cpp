//CombPathDelayAnalysis.cpp- Analysis the Path delay between registers- C++ -=//
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
// This pass analysis the slack between two registers.
//
// The "Slack" in VAST means the extra cycles that after data appear in
// the output pin of the src register before the dst register read the data.
// i.e. if we assign reg0 at cycle 1, and the data will appear at the output
// pin of reg0 at cycle 2, and now reg1 can read the data. In this case
// becasue the data appear at cycle 2 and we read the data at the same cycle,
// the slack is 0. But if we read the data at cycle 3, the slack is 1.
//
//===----------------------------------------------------------------------===//

#include "RtlSSAAnalysis.h"

#include "vtm/VerilogAST.h"
#include "vtm/Passes.h"
#include "vtm/VFInfo.h"
#include "vtm/Utilities.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/Target/TargetData.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "vtm-comb-path-delay"
#include "llvm/Support/Debug.h"
using namespace llvm;

static cl::opt<bool>
DisableTimingScriptGeneration("vtm-disable-timing-script",
                              cl::desc("Disable timing script generation"),
                              cl::init(false));

namespace{
struct TimingPath {
  int Delay;
  VASTValue **Path;
  unsigned PathSize;

  void bindPath2ScriptEngine();
};

struct CombPathDelayAnalysis : public MachineFunctionPass {
  RtlSSAAnalysis *RtlSSA;
  VASTModule *VM;
  BumpPtrAllocator Allocator;

  static char ID;

  void getAnalysisUsage(AnalysisUsage &AU) const {
    MachineFunctionPass::getAnalysisUsage(AU);
    AU.addRequired<TargetData>();
    AU.addRequired<RtlSSAAnalysis>();
    AU.setPreservesAll();
  }

  void writeConstraintsForDstReg(VASTRegister *DstReg);

  void extractTimingPaths(ValueAtSlot *DstVAS, VASTValue *DepTree,
                          SmallVectorImpl<TimingPath*> &Paths);

  bool runOnMachineFunction(MachineFunction &MF);

  bool doInitialization(Module &) {
    SMDiagnostic Err;
    // Get the script from script engine.
    const char *HeaderScriptPath[] = { "Misc",
                                       "TimingConstraintsHeaderScript" };
    if (!runScriptStr(getStrValueFromEngine(HeaderScriptPath), Err))
      report_fatal_error("Error occur while running timing header script:\n"
                         + Err.getMessage());
    return false;
  }

  CombPathDelayAnalysis() : MachineFunctionPass(ID), RtlSSA(0), VM(0) {
    initializeCombPathDelayAnalysisPass(*PassRegistry::getPassRegistry());
  }

  TimingPath *createTimingPath(ValueAtSlot *Dst, ArrayRef<VASTValue*> Path);
};
}

// The first node of the path is the use node and the last node of the path is
// the define node.
void TimingPath::bindPath2ScriptEngine() {
  assert(PathSize >= 2 && "Path vector have less than 2 nodes!");
  // Path table:
  // Datapath: {
  //  unsigned Slack,
  //  table NodesInPath
  // }
  SMDiagnostic Err;

  if (!runScriptStr("RTLDatapath = {}\n", Err))
    llvm_unreachable("Cannot create RTLDatapath table in scripting pass!");

  std::string Script;
  raw_string_ostream SS(Script);
  SS << "RTLDatapath.Slack = " << Delay;
  SS.flush();
  if (!runScriptStr(Script, Err))
    llvm_unreachable("Cannot create slack of RTLDatapath!");

  Script.clear();

  SS << "RTLDatapath.Nodes = {'" << cast<VASTNamedValue>(Path[0])->getName();
  for (unsigned i = 1; i < PathSize; ++i) {
    // Skip the unnamed nodes.
    if (VASTNamedValue *NV = dyn_cast<VASTNamedValue>(Path[i])) {
      const char *Name = NV->getName();
      if (Name) SS << "', '" << Name;
    }
  }
  SS << "'}";

  SS.flush();
  if (!runScriptStr(Script, Err))
    llvm_unreachable("Cannot create node table of RTLDatapath!");

  // Get the script from script engine.
  const char *DatapathScriptPath[] = { "Misc", "DatapathScript" };
  if (!runScriptStr(getStrValueFromEngine(DatapathScriptPath), Err))
    report_fatal_error("Error occur while running datapath script:\n"
                       + Err.getMessage());
}

// Helper class
struct TimgPathBuilder {
  CombPathDelayAnalysis &A;
  ValueAtSlot *DstVAS;
  SmallVectorImpl<TimingPath*> &Paths;

  TimgPathBuilder(CombPathDelayAnalysis &a, ValueAtSlot *V,
                  SmallVectorImpl<TimingPath*> &P) : A(a), DstVAS(V), Paths(P){}

  void operator() (ArrayRef<VASTValue*> PathArray) {
    VASTValue *SrcUse = PathArray.back();
    if (isa<VASTRegister>(SrcUse)){
      TimingPath *P = A.createTimingPath(DstVAS, PathArray);
      // Ignore the false path.
      if (P) Paths.push_back(P);
    }
  }
};

TimingPath *CombPathDelayAnalysis::createTimingPath(ValueAtSlot *Dst,
                                                    ArrayRef<VASTValue*> Path) {
  // Add the end slots.
  VASTRegister *SrcReg = cast<VASTRegister>(Path.back());

  unsigned PathDelay = -1;

  typedef VASTRegister::assign_itertor assign_it;
  for (assign_it I = SrcReg->assign_begin(), E = SrcReg->assign_end();
       I != E; ++I) {
    VASTSlot *SrcSlot = VM->getSlot(I->first->getSlotNum());
    ValueAtSlot *SrcVAS = RtlSSA->getValueASlot(SrcReg, SrcSlot);

    // Update the PathDelay if the source VAS reaches DstSlot.
    if (unsigned Distance = Dst->getCyclesFromDef(SrcVAS))
      PathDelay = std::min(PathDelay, Distance);
  }

  // The path {SrcReg -> DstReg} maybe a false path, i.e. SrcReg never reaches
  // DstSlot.
  if ((int)PathDelay == -1) return 0;

  TimingPath *P = new (Allocator.Allocate<TimingPath>()) TimingPath();

  // The Path should include the Dst.
  P->PathSize = Path.size() + 1;
  P->Path = Allocator.Allocate<VASTValue*>(P->PathSize);

  unsigned ExtraDelay = 0;

  P->Path[0] = Dst->getValue();
  for (unsigned i = 0; i < Path.size(); ++i) {
    VASTValue *V = Path[i];
    P->Path[i + 1] = V;

    // Accumulates the block box latency.
    if (VASTWire *W = dyn_cast<VASTWire>(V))
      ExtraDelay += W->getExtraDelayIfAny();
  }

  assert((ExtraDelay == 0 || PathDelay == 1)
         && "Unexpected multi-cycles path with extra delay!");
  // The path delay is the sum of minimum distance between source/destinate slot
  // and the delay of block boxes.
  P->Delay = std::max(PathDelay, ExtraDelay);

  return P;
}

bool CombPathDelayAnalysis::runOnMachineFunction(MachineFunction &MF) {
  // No need to write timing script at all.
  if (DisableTimingScriptGeneration) return false;

  bindFunctionInfoToScriptEngine(MF, getAnalysis<TargetData>());
  VM = MF.getInfo<VFInfo>()->getRtlMod();
  RtlSSA = &getAnalysis<RtlSSAAnalysis>();

  //Write the timing constraints.
  typedef VASTModule::reg_iterator reg_it;
  for (reg_it I = VM->reg_begin(), E = VM->reg_end(); I != E; ++I)
    writeConstraintsForDstReg(*I);

  return false;
}

static bool path_delay_is_bigger(const TimingPath *LHS, const TimingPath *RHS) {
  return LHS->Delay > RHS->Delay;
}

void CombPathDelayAnalysis::writeConstraintsForDstReg(VASTRegister *DstReg) {
  SmallVector<TimingPath*, 8> Paths;

  typedef VASTRegister::assign_itertor assign_it;
  for (assign_it I = DstReg->assign_begin(), E = DstReg->assign_end();
       I != E; ++I) {
    VASTSlot *S = VM->getSlot(I->first->getSlotNum());
    ValueAtSlot *DstVAS = RtlSSA->getValueASlot(DstReg, S);

    // Paths for the assigning value
    extractTimingPaths(DstVAS, I->first, Paths);
    // Paths for the condition.
    extractTimingPaths(DstVAS, *I->second, Paths);
  }

  // Sort the delay so we write big delay constraints first, if the loose
  // constraint is not applied, we may always apply the tighter constraint,
  // so the design can synthesized correctly.
  std::sort(Paths.begin(), Paths.end(), path_delay_is_bigger);

  typedef SmallVector<TimingPath*, 8>::iterator path_it;
  for (path_it DI = Paths.begin(), DE = Paths.end(); DI != DE; ++DI)
    (*DI)->bindPath2ScriptEngine();

  Allocator.Reset();
}

void CombPathDelayAnalysis::extractTimingPaths(ValueAtSlot *DstVAS,
                                               VASTValue *DepTree,
                                               SmallVectorImpl<TimingPath*>
                                               &Paths) {
  VASTValue *SrcValue = DepTree;

  // If Define Value is immediate or symbol, skip it.
  if (!SrcValue) return;

  // Trivial case: register to register path.
  if (VASTRegister *SrcReg = dyn_cast<VASTRegister>(SrcValue)){
    VASTValue *Path[] = { SrcReg };
    TimingPath *P = createTimingPath(DstVAS, Path);
    assert(P && "A trivial path is a false path?");
    Paths.push_back(P);
    return;
  }

  TimgPathBuilder B(*this, DstVAS, Paths);
  DepthFirstTraverseDepTree(DepTree, B);
}

char CombPathDelayAnalysis::ID = 0;
INITIALIZE_PASS_BEGIN(CombPathDelayAnalysis, "CombPathDelayAnalysis",
                      "CombPathDelayAnalysis", false, false)
INITIALIZE_PASS_END(CombPathDelayAnalysis, "CombPathDelayAnalysis",
                    "CombPathDelayAnalysis", false, false)
Pass *llvm::createCombPathDelayAnalysisPass() {
  return new CombPathDelayAnalysis();
}
