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

#include "HWAtom.h"
#include "VTMConfig.h"
//#include "ModuloScheduleInfo.h"

#include "llvm/ADT/PriorityQueue.h"

using namespace llvm;

namespace llvm {
class VTMConfig;

class ForceDirectedSchedulingBase {
  const VTMConfig *VST;
  // MII in modulo schedule.
  unsigned MII, CriticalPathEnd;
  double ExtraResReq;
  // Time Frame {asap step, alap step }
public:
  typedef std::pair<unsigned, unsigned> TimeFrame;
private:
  // Mapping hardware atoms to time frames.
  typedef std::map<const HWAtom*, TimeFrame> TimeFrameMapType;

  TimeFrameMapType AtomToTF;
  TimeFrameMapType AtomToSTF;

  typedef std::map<unsigned, double> DGStepMapType;
  typedef std::map<unsigned, std::map<unsigned, double> > DGType;
  DGType DGraph;

  unsigned computeStepKey(unsigned step) const;

  std::map<const HWAtom*, double> AvgDG;

  void resetSTF();

protected:
  FSMState *State;

  ForceDirectedSchedulingBase(FSMState *S)
    : MII(0), CriticalPathEnd(0), ExtraResReq(0.0), State(S) {}
public:
  virtual ~ForceDirectedSchedulingBase() {}

  virtual bool scheduleState() = 0;
  // Return true when resource constraints preserved after citical path
  // scheduled
  bool scheduleCriticalPath(bool refreshFDInfo);
  void schedulePassiveAtoms();

  /// @name TimeFrame
  //{
  void sinkSTF(const HWAtom *A, unsigned ASAP, unsigned ALAP);
  void updateSTF();


  void buildASAPStep(const HWAtom *ClampedAtom = 0,
    unsigned ClampedASAP = 0);
  void buildALAPStep(const HWAtom *ClampedAtom = 0,
    unsigned ClampedALAP = 0);
  void buildTimeFrame(const HWAtom *ClampedAtom = 0,
                      unsigned ClampedASAP = 0,
                      unsigned ClampedALAP = 0);

  unsigned getASAPStep(const HWAtom *A) const {
    return const_cast<ForceDirectedSchedulingBase*>(this)->AtomToTF[A].first;
  }
  unsigned getALAPStep(const HWAtom *A) const {
    return const_cast<ForceDirectedSchedulingBase*>(this)->AtomToTF[A].second;
  }

  unsigned getSTFASAP(const HWAtom *A) const {
    return const_cast<ForceDirectedSchedulingBase*>(this)->AtomToSTF[A].first;
  }
  unsigned getSTFALAP(const HWAtom *A) const {
    return const_cast<ForceDirectedSchedulingBase*>(this)->AtomToSTF[A].second;
  }

  unsigned getTimeFrame(const HWAtom *A) const {
    return getALAPStep(A) - getASAPStep(A) + 1;
  }

  unsigned getScheduleTimeFrame(const HWAtom *A) const {
    return getSTFALAP(A) - getSTFASAP(A) + 1;
  }

  bool isSTFScheduled(const HWAtom *A) const {
    return getScheduleTimeFrame(A) < HWAtom::MaxSlot;
  }

  void printTimeFrame(raw_ostream &OS) const;
  void dumpTimeFrame() const;
  //}

  /// @name Distribution Graphs
  //{
  void buildDGraph();
  double getDGraphAt(unsigned step, unsigned FUClass) const;
  void accDGraphAt(unsigned step, unsigned FUClass, double d);
  void printDG(raw_ostream &OS) const;
  void dumpDG() const;
  /// Check the distribution graphs to see if we could schedule the nodes
  /// without breaking the resource constrain.
  bool isResourceConstraintPreserved();
  double getExtraResReq() const { return ExtraResReq; }
  //}

  /// @name Force computation
  //{
  void buildAvgDG();
  double getAvgDG(const HWAtom *A) {  return AvgDG[A]; }
  double getRangeDG(unsigned FUClass, unsigned start, unsigned end/*included*/);

  double computeRangeForce(const HWAtom *A,
                           unsigned start, unsigned end/*include*/);
  double computeSelfForce(const HWAtom *A,
                          unsigned start, unsigned end/*include*/);

  /// If there are recurrences in the graph,
  /// Define "pred tree" and "succ tree" may intersect.
  /// All we can do is compute "other force", and the time frame of atoms
  /// not in "pred tree" or "succ tree" will not change, and not contribute
  /// to the force.
  double computeOtherForce(const HWAtom *A);

  double computeForce(const HWAtom *A, unsigned ASAP, unsigned ALAP);
  //}

  unsigned buildFDInfo(bool resetSTF);

  void setMII(unsigned II) { MII = II; }
  unsigned getMII() const { return MII; }
  void increaseMII() { ++MII; }
  void decreaseMII() { --MII; }
  void lengthenCriticalPath() { ++CriticalPathEnd; }
  void shortenCriticalPath() { --CriticalPathEnd; }
  unsigned getCriticalPathLength() {
    return CriticalPathEnd - State->getStartSlot();
  }
  void setCriticalPathLength(unsigned L) {
    CriticalPathEnd = State->getStartSlot() + L;
  }
};

class ForceDirectedListScheduler : public ForceDirectedSchedulingBase {
protected:
  /// @name PriorityQueue
  //{
  struct fds_sort {
    ForceDirectedSchedulingBase &Info;
    fds_sort(ForceDirectedSchedulingBase &s) : Info(s) {}
    bool operator() (const HWAtom *LHS, const HWAtom *RHS) const;
  };

  typedef PriorityQueue<HWAtom*, std::vector<HWAtom*>, fds_sort> AtomQueueType;

  // Fill the priorityQueue, ignore FirstNode.
  template<class It>
  void fillQueue(AtomQueueType &Queue, It begin, It end, HWAtom *FirstNode = 0);

  unsigned findBestStep(HWAtom *A);

  bool scheduleAtom(HWAtom *A);
  bool scheduleQueue(AtomQueueType &Queue);
  //}

public:
  ForceDirectedListScheduler(FSMState *S) 
    : ForceDirectedSchedulingBase(S) {}

  bool scheduleState();

};


class IteractiveModuloScheduling : public ForceDirectedListScheduler {
  typedef std::map<unsigned, unsigned> UsageMapType;
  typedef std::map<unsigned, UsageMapType> MRTType;
  MRTType MRT;
  std::map<HWAtom*, std::set<unsigned> > ExcludeSlots;

  bool isResAvailable(unsigned FUClass, unsigned step);
  void excludeStep(HWAtom *A, unsigned step);
  bool isStepExcluded(HWAtom *A, unsigned step);
  bool isAllAtomScheduled();
  HWAtom *findBlockingAtom(unsigned FUClass, unsigned step); 
public:
  IteractiveModuloScheduling(FSMState *S)
    : ForceDirectedListScheduler(S){}

  bool scheduleState();
};

class ForceDirectedScheduler : public ForceDirectedSchedulingBase {
public:
  ForceDirectedScheduler(FSMState *S)
    : ForceDirectedSchedulingBase(S) {}

  bool scheduleState();
  bool findBestSink();
  double trySinkAtom(HWAtom *A, TimeFrame &NewTimeFrame);
};

} // End namespace.
#endif
