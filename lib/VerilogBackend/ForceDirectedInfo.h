//===- ForceDirectedInfo.h - ForceDirected information analyze --*- C++ -*-===//
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
// This file define the Force Direct information computation pass describe in
// Force-Directed Scheduling for the Behavioral Synthesis of ASIC's
//
//===----------------------------------------------------------------------===//
#ifndef VBE_FORCE_DIRECTED_INFO
#define VBE_FORCE_DIRECTED_INFO

#include "HWAtomInfo.h"

using namespace llvm;

namespace esyn {
class ForceDirectedInfo : public FunctionPass {
  HWAtomInfo *HI;
  ResourceConfig *RC;

  // Time Frame { {asap step, alap step }, isMIIContraint }
  typedef std::pair<unsigned, unsigned> TimeFrame;
  // Mapping hardware atoms to time frames.
  typedef std::map<const HWAtom*, TimeFrame> TimeFrameMapType;
  typedef std::map<const HWAtom*, unsigned> SCCALAPMapType;

  TimeFrameMapType AtomToTF;
  SCCALAPMapType AtomToSCCALAP;

  void buildASAPStep(const HWAVRoot *EntryRoot, unsigned step);
  void buildALAPStep(const HWAOpInst *ExitRoot, unsigned step);

  // The Key of DG, { step, resource type }
  typedef std::map<unsigned, double> DGType;
  DGType DGraph;

  // Resource usage.
  typedef std::map<unsigned, unsigned> ResUsageTabType;
  ResUsageTabType ResUsage;

  unsigned computeStepKey(unsigned step, HWFUnitID FUID) const;

  std::map<const HWAPostBind*, double> AvgDG;

  // MII in modulo schedule.
  unsigned MII, CriticalLength;
public:

  /// @name TimeFrame
  //{
  void buildASAPStep(FSMState *State) {
    const HWAVRoot *Root = &State->getEntryRoot();
    buildASAPStep(Root, getASAPStep(Root));
  }
  unsigned getASAPStep(const HWAtom *A) const {
    assert((isa<HWAOpInst>(A) || isa<HWAVRoot>(A) || isa<HWADelay>(A))
          && "Bad atom type!");
    return const_cast<ForceDirectedInfo*>(this)->AtomToTF[A].first;
  }

  // Build the ALAP step constrain by SCC.
  template<class It>
  bool buildSCCASAPStep(FSMState *State, unsigned FirstStep, unsigned II,
                        It SCCBegin, It SCCEnd) {
      // Build SCC ALAP constrain before we build the ALAP steps.
      for (It I = SCCBegin, E = SCCEnd; I != E; ++I) {
        HWAtom *A = *I;
        unsigned SCCALAP = std::min(getASAPStep(A) + MII - A->getLatency(),
                                    FirstStep + II - A->getLatency());

        if (SCCALAP < getASAPStep(A))
          return false;

        AtomToSCCALAP[A] = SCCALAP;
      }
      return true;
  }

  unsigned getSCCALAPStep(const HWAtom *A) const {
    SCCALAPMapType::const_iterator at = AtomToSCCALAP.find(A);
    if (at != AtomToSCCALAP.end())
      return at->second;
   
    return CriticalLength;
  }

  void buildALAPStep(FSMState *State) {
    const HWAOpInst *Root = &State->getExitRoot();
    buildALAPStep(Root, getALAPStep(Root));
  }
  unsigned getALAPStep(const HWAtom *A) const {
    assert((isa<HWAOpInst>(A) || isa<HWAVRoot>(A) || isa<HWADelay>(A))
          && "Bad atom type!");
    return const_cast<ForceDirectedInfo*>(this)->AtomToTF[A].second;
  }

  unsigned getTimeFrame(const HWAtom *A) const {
    return getALAPStep(A) - getASAPStep(A) + 1;
  }

  // If the TimeFrame Constrains by II.
  bool constrainByMII(const HWAOpInst *A) const {
    return getALAPStep(A) == getSCCALAPStep(A);
  }

  void printTimeFrame(FSMState *State, raw_ostream &OS) const;
  void dumpTimeFrame(FSMState *State) const;
  //}

  /// @name Distribution Graphs
  //{
  void buildDGraph(FSMState *State);
  double getDGraphAt(unsigned step, HWFUnitID FUID) const;
  void accDGraphAt(unsigned step, HWFUnitID FUID, double d);
  void printDG(FSMState *State, raw_ostream &OS) const ;
  void dumpDG(FSMState *State) const ;
  //}

  /// @name Resource usage table
  //{
  /// @brief If the usage of given kind of FU not exceed the maximum available number.
  bool isFUAvailalbe(unsigned step, HWFUnit FU) const;
  //}

  /// @name Force computation
  //{
  void buildAvgDG(FSMState *State);
  double getAvgDG(const HWAPostBind *A) {  return AvgDG[A]; }
  double getRangeDG(const HWAPostBind *A, unsigned start, unsigned end/*included*/);

  double computeSelfForceAt(const HWAOpInst *OpInst, unsigned step);
  /// This function will invalid the asap step of all node in
  /// successor tree
  double computeSuccForceAt(const HWAOpInst *OpInst, unsigned step);
  /// This function will invalid the alap step of all node in
  /// predecessor tree
  double computePredForceAt(const HWAOpInst *OpInst, unsigned step);

  unsigned findBestStep(HWAOpInst *A);
  //}

  unsigned buildFDInfo(FSMState *State, unsigned StartStep,
                       unsigned EndStep);

  void recoverFDInfo(FSMState *State);

  template<class It>
  bool updateFDInfoForSCC(FSMState *State, unsigned FirstStep, unsigned II,
                          It SCCBegin, It SCCEnd) {
    // Build the time frame
    buildASAPStep(State); 
    if (!buildSCCASAPStep(State, FirstStep, II, SCCBegin, SCCEnd))
      return false;

    buildALAPStep(State);
    buildDGraph(State);
    buildAvgDG(State);
    return true;
  }

  void enableModuleFD(unsigned II) { MII = II; }

  void clear();
  /// @name Common pass interface
  //{
  static char ID;
  ForceDirectedInfo() : FunctionPass(&ID), HI(0), RC(0) {}
  bool runOnFunction(Function &F);
  void releaseMemory();
  void getAnalysisUsage(AnalysisUsage &AU) const;
  virtual void print(raw_ostream &O, const Module *M) const {}
  //}
};
} // End namespace.
#endif
