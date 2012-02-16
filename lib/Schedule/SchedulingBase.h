//===- ForceDirectedSchedulingBase.h - ForceDirected information analyze --*- C++ -*-===//
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

#include "VSUnit.h"

#include "llvm/ADT/PriorityQueue.h"
#include <map>
using namespace llvm;

//Dirty Hack
struct _lprec;
typedef _lprec lprec;

namespace llvm {
class SchedulingBase {
  // MII in modulo schedule.
  unsigned MII, CriticalPathEnd;
  double ExtraResReq;
  // Time Frame {asap step, alap step }
public:
  typedef std::pair<unsigned, unsigned> TimeFrame;
private:
  // Time frames for each schedule unit.
  std::map<const VSUnit*, TimeFrame> SUnitToTF;

  // Step -> resource require number.
  typedef std::map<unsigned, unsigned> UsageMapType;
  // Resource -> resource usage at each step.
  typedef std::map<FuncUnitId, UsageMapType> RTType;
  RTType RT;

protected:
  /// @name PriorityQueue
  //{

  VSchedGraph &State;
  unsigned computeStepKey(unsigned step) const;
  SchedulingBase(VSchedGraph &S)
    : MII(0), CriticalPathEnd(0), ExtraResReq(0.0), State(S) {}

public:
  virtual ~SchedulingBase() {}

  VSchedGraph &getState() const { return State; }

  virtual bool scheduleState() = 0;
  // Return true when resource constraints preserved after citical path
  // scheduled
  bool scheduleCriticalPath(bool refreshFDepHD);
  void schedulePassiveSUnits();
  bool allNodesSchedued() const;

  /// @name TimeFrame
  //{
  void buildASAPStep();
  void buildALAPStep();
  void buildTimeFrame();

  unsigned getASAPStep(const VSUnit *A) const {
    std::map<const VSUnit*, TimeFrame>::const_iterator at = SUnitToTF.find(A);
    assert(at != SUnitToTF.end() && "TimeFrame for SU not exist!");
    return at->second.first;
  }
  unsigned getALAPStep(const VSUnit *A) const {
    std::map<const VSUnit*, TimeFrame>::const_iterator at = SUnitToTF.find(A);
    assert(at != SUnitToTF.end() && "TimeFrame for SU not exist!");
    return at->second.second;
  }

  unsigned getTimeFrame(const VSUnit *A) const {
    return getALAPStep(A) - getASAPStep(A) + 1;
  }

  void printTimeFrame(raw_ostream &OS) const;
  void dumpTimeFrame() const;
  //}

  /// Check the distribution graphs to see if we could schedule the nodes
  /// without breaking the resource constrain.
  void resetRT() {
    // FIXME: Do not clear the RT but set the function unit count in the
    // table to 0.
    RT.clear();
  }

  bool tryTakeResAtStep(VSUnit *U, unsigned step);
  void scheduleSU(VSUnit *U, unsigned step);
  void unscheduleSU(VSUnit *U);

  bool isResourceConstraintPreserved();
  double getExtraResReq() const { return ExtraResReq; }

  unsigned buildFDepHD(bool resetSTF);

  unsigned computeRecMII();
  unsigned computeResMII();
  bool computeMII();

  void setMII(unsigned II) { MII = II; }
  unsigned getMII() const { return MII; }
  void increaseMII() { ++MII; }
  void decreaseMII() { --MII; }
  void lengthenCriticalPath() { ++CriticalPathEnd; }
  void shortenCriticalPath() { --CriticalPathEnd; }
  unsigned getCriticalPathLength() {
    return CriticalPathEnd - State.getStartSlot();
  }
  void setCriticalPathLength(unsigned L) {
    CriticalPathEnd = State.getStartSlot() + L;
  }

  void viewGraph();
};

template <> struct GraphTraits<SchedulingBase*> 
    : public GraphTraits<VSchedGraph*> {
  typedef VSchedGraph::sched_iterator nodes_iterator;
  static nodes_iterator nodes_begin(SchedulingBase *G) {
    return G->getState().sched_begin();
  }
  static nodes_iterator nodes_end(SchedulingBase *G) {
    return G->getState().sched_end();
  }
};

class IterativeModuloScheduling : public SchedulingBase {
  std::map<const VSUnit*, std::set<unsigned> > ExcludeSlots;

  void excludeStep(VSUnit *A, unsigned step);
  bool isStepExcluded(VSUnit *A, unsigned step);
  bool isAllSUnitScheduled();
  VSUnit *findBlockingSUnit(VSUnit *U, unsigned step);
public:
  IterativeModuloScheduling(VSchedGraph &S)
    : SchedulingBase(S) {}

  bool scheduleState();
};

struct ASAPScheduler : public SchedulingBase {
  ASAPScheduler(VSchedGraph &S) : SchedulingBase(S) {}

  bool scheduleState();
};

class ILPScheduler : public SchedulingBase {
  // The index of first step variable of the schedule unit.
  // For a given schedule unit with index i, who's time frame is N, then there
  // will be N step variables:
  // sv_i_0, sv_i_1, sv_i_2, ... , sv_i_(N-1) 
  // sv_i_j set to 1 means the schedule unit is scheduled to asap + j step
  std::map<const VSUnit*, unsigned> SUnitToSV;
  // Total step variables count.
  unsigned NumStepVars;
  // Total Step Variable;
  unsigned StepIdx;
  // Total variables in the model.
  unsigned TotalVariables;

  unsigned TotalRows;

  // Build the first step variable index mapping of schedule units based on
  // their ASAP and ALAP and return the total step variable count.
  //
  unsigned buildSVIdx();

  unsigned getFstSVIdxOf(const VSUnit *U) const {
    assert(U && "Unexpected NULL pointer!");
    std::map<const VSUnit*, unsigned>::const_iterator at = SUnitToSV.find(U);
    assert(at != SUnitToSV.end() && "SVIdx for VSUnit not exist!");
    return at->second;
  }

  typedef std::pair<const VSUnit*, unsigned> SUToIdx;
  typedef SmallVector<SUToIdx, 64> ActiveSUVec;
  typedef std::map<unsigned, ActiveSUVec> ActiveSUMap;
  ActiveSUMap ActiveSUs;

  // Only 1 step variable allowed set to 1.
  void buildOneActiveStepConstraints(lprec *lp);

  // The schedule should satisfy the dependences.
  void buildPrecedenceConstraints(lprec *lp);

  unsigned getIdxOf(VFUs::FUTypes FU) {
    return NumStepVars + FU - VFUs::FirstNonTrivialFUType;
  }

  // Avoid the resources conflict for the function units. 
  void buildFUConstraints(lprec *lp);

  // Set the variables' name in the model.
  void setUpVariables(lprec *lp);

  void buildObject(lprec *lp);

  // Build the schedule form the result of ILP.
  void buildSchedule(lprec *lp);
public:
  ILPScheduler(VSchedGraph &S);

  bool scheduleState();
};

 class SDCScheduler : public SchedulingBase {
 public:

    SDCScheduler(VSchedGraph &S);

    bool scheduleState();

 private:

    lprec *lp;
    // Total step variables count.
    int NumVars;
    // Total instructions count.
    int NumInst;
    unsigned ShedCounter;
    // The table of the constraint row of the schedule unit.
    std::map<VSUnit*, unsigned> SchedTable;
    // The table of the start step variable of the schedule unit.
    std::map<const VSUnit*, unsigned> StartVariableIndex;
    // The table of the end step variable of the schedule unit.
    std::map<const VSUnit*, unsigned> EndVariableIndex;
    typedef std::map<const VSUnit*, unsigned> SUIdxMapType;

    // Set the variables' name in the model.
    void createLPVariables(lprec *lp);

    // The schedule should satisfy the multi-cycle operations.
    void addMulticycleConstraints(lprec *lp);

    // The schedule should satisfy the dependences.
    void addDependencyConstraints(lprec *lp);

    // Avoid the resources conflict for the function units.
    void addResourceConstraints(lprec *lp);

    // Build the schedule object function.
    void buildObject(bool ASAP = true);

    // Build the schedule form the result of ILP.
    void buildSchedule(lprec *lp);

};

} // End namespace.
#endif
