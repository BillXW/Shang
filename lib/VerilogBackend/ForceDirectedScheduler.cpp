//===- ForceDirectedScheduler.cpp - The ForceDirected Scheduler  -*- C++ -*-===//
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
// This file implement the Force Direct Scheduler pass describe in
// Force-Directed Scheduling for the Behavioral Synthesis of ASIC's
//
//===----------------------------------------------------------------------===//

#include "ForceDirectedInfo.h"
#include "ModuloScheduleInfo.h"
#include "HWAtomPasses.h"

#include "llvm/ADT/PriorityQueue.h"

#define DEBUG_TYPE "vbe-fd-sched"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace esyn;

namespace {
struct fds_sort {
  ForceDirectedInfo *Info;
  fds_sort(ForceDirectedInfo *s) : Info(s) {}
  bool operator() (const HWAOpInst* LHS, const HWAOpInst* RHS) const;
};

struct FDLScheduler : public BasicBlockPass {
  HWAtomInfo *HI;
  ResourceConfig *RC;
  ForceDirectedInfo *FDInfo;
  ModuloScheduleInfo *MSInfo;
  FSMState *CurState;
  HWAOpInst *Exit;

  unsigned MII;

  enum SchedResult {
    SchedSucc, SchedFailCricitalPath, SchedFailII
  };

  /// @name PriorityQueue
  //{
  typedef PriorityQueue<HWAOpInst*, std::vector<HWAOpInst*>, fds_sort> AtomQueueType;
  
  // Find the Node the do not have dependency in this Queue.
  template<class It>
  HWAtom *findFirstNode(It begin, It end);

  // Fill the priorityQueue, ignore FirstNode.
  template<class It>
  void fillQueue(AtomQueueType &Queue, It begin, It end, HWAtom *FirstNode = 0);

  typedef ModuloScheduleInfo::rec_iterator rec_iterator;
  typedef ModuloScheduleInfo::scc_vector scc_vector;

  SchedResult scheduleCriticalPath();

  SchedResult scheduleAtom(HWAtom *A);
  SchedResult scheduleQueue(AtomQueueType &Queue);
  SchedResult scheduleAtII();
  //}

  unsigned findBestStep(HWAOpInst *A);

  void clear();

  void FDListSchedule();
  void FDModuloSchedule();

  /// @name Common pass interface
  //{
  static char ID;
  FDLScheduler() : BasicBlockPass(&ID) {}
  bool runOnBasicBlock(BasicBlock &BB);
  void releaseMemory();
  void getAnalysisUsage(AnalysisUsage &AU) const;
  void print(raw_ostream &O, const Module *M) const;
  //}
};
} //end namespace

//===----------------------------------------------------------------------===//
bool fds_sort::operator()(const HWAOpInst* LHS, const HWAOpInst* RHS) const {
  HWFUnit LFU = LHS->getFunUnit(), RFU = RHS->getFunUnit();
  // Schedule the atom with less available function unit first.
  if (LFU.getTotalFUs() > RFU.getTotalFUs())
    return true;
  else if (LFU.getTotalFUs() < RFU.getTotalFUs())
    return false;

  // Schedule the low mobility nodes first.
  if (Info->getTimeFrame(LHS) > Info->getTimeFrame(RHS))
    return true; // Place RHS first.
  else if (Info->getTimeFrame(LHS) < Info->getTimeFrame(RHS))
    return false;

  //unsigned LHSLatency = FDS->getASAPStep(LHS);
  //unsigned RHSLatency = FDS->getASAPStep(RHS);
  //// Schedule as soon as possible?
  //if (LHSLatency < RHSLatency) return true;
  //if (LHSLatency > RHSLatency) return false;

  return LHS->getIdx() > RHS->getIdx();
}

//===----------------------------------------------------------------------===//
char FDLScheduler::ID = 0;

void FDLScheduler::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<HWAtomInfo>();
  AU.addRequired<ResourceConfig>();
  AU.addRequired<ForceDirectedInfo>();
  AU.addRequired<ModuloScheduleInfo>();
  AU.setPreservesAll();
}

template<class It>
void FDLScheduler::fillQueue(AtomQueueType &Queue, It begin, It end,
                             HWAtom *FirstNode) {
  for (It I = begin, E = end; I != E; ++I) {
    HWAtom *A = *I;

    // Do not push the FirstNode into the queue.
    if (A == FirstNode || A->isScheduled())
      continue;
    
    if (HWAOpInst *OI = dyn_cast<HWAOpInst>(A))    
      Queue.push(OI);
  }
  //
  Queue.reheapify();
}

template<class It>
HWAtom *FDLScheduler::findFirstNode(It begin, It end) {
  if (begin == end) return 0;

  std::pair<HWAtom*, unsigned> Ret =
    std::make_pair(*begin, FDInfo->getASAPStep(*begin));

  while (++begin != end) {
    HWAtom *A = *begin;

    unsigned ASAP = FDInfo->getASAPStep(A);
    if (ASAP < Ret.second)
      Ret = std::make_pair(A, ASAP);
  }

  return Ret.first;
}

bool FDLScheduler::runOnBasicBlock(BasicBlock &BB) {
  DEBUG(dbgs() << "==================== " << BB.getName() << '\n');
  HI = &getAnalysis<HWAtomInfo>();
  FDInfo = &getAnalysis<ForceDirectedInfo>();
  MSInfo = &getAnalysis<ModuloScheduleInfo>();

  CurState = HI->getStateFor(BB);

  if (MSInfo->isModuloSchedulable(*CurState)) {
    unsigned RecMII = MSInfo->computeRecMII(*CurState);
    unsigned ResMII = MSInfo->computeResMII(*CurState);
    MII = std::max(RecMII, ResMII);
  } else
    MII = 0;

  if (MSInfo->isModuloSchedulable(*CurState))
    FDModuloSchedule();
  else
    FDListSchedule();

  HI->setTotalCycle(CurState->getEndSlot() + 1);

  // Do not forget to schedule the delay atom;
  for (FSMState::iterator I = CurState->begin(), E = CurState->end();
       I != E; ++I) {
    HWAtom *A = *I;
    if (!A->isScheduled()) {
      assert(isa<HWADelay>(A) && "Some atom not scheduled!");
      A->scheduledTo(FDInfo->getASAPStep(A));
    }
  }
  
  return false;
}

void FDLScheduler::FDModuloSchedule() {
  unsigned StartStep = HI->getTotalCycle();
  for(;;) {
    CurState->resetSchedule();
    CurState->scheduledTo(StartStep);

    // Set up Resource table
    FDInfo->reset();
    FDInfo->setMII(0);
    FDInfo->buildFDInfo();
    DEBUG(FDInfo->dumpTimeFrame());
    // TODO: check if we could ever schedule these node without breaking the
    // resource constrain by check the DG.
    // If the resource average DG is bigger than the total available resource
    // we can never schedule the nodes without breaking the resource constrain.
    if (scheduleCriticalPath() != FDLScheduler::SchedSucc) {
      FDInfo->lengthenCriticalPath();
      continue;
    }

    CurState->resetSchedule();
    CurState->scheduledTo(StartStep);

    FDInfo->setMII(MII);
    FDInfo->buildFDInfo();
    if (scheduleCriticalPath() != FDLScheduler::SchedSucc) {
      FDInfo->lengthenMII();
      continue;
    }

    switch (scheduleAtII()) {
    case FDLScheduler::SchedSucc:
      DEBUG(FDInfo->dumpTimeFrame());
      DEBUG(FDInfo->dumpDG());
      // Set up the initial interval.
      CurState->setII(FDInfo->getMII());
      return;
    case FDLScheduler::SchedFailII:
        FDInfo->lengthenMII();
        MII = FDInfo->getMII();
        continue;
    case FDLScheduler::SchedFailCricitalPath:
        FDInfo->lengthenCriticalPath();
        continue;
    }
  }
}

FDLScheduler::SchedResult FDLScheduler::scheduleAtII() {
  fds_sort s(FDInfo);
  AtomQueueType AQueue(s);
  // Schedule all SCCs.
  for (unsigned i = FDInfo->getMII(); i > 0; --i) {
    for (rec_iterator I = MSInfo->rec_begin(i), E = MSInfo->rec_end(i);
        I != E; ++I) {
      scc_vector &SCC = I->second;
      AQueue.clear();
      // FIXME: schedule all first nodes and then schedule other nodes.
      // First of all Schedule the node that do not have any predecessor in
      // the SCC (Schedule First Node).
      // First Node = ...
      // And schedule rest nodes in SCC, but the max latency of the SCC
      // should not exceed II.
      HWAtom *FirstNode = findFirstNode(SCC.begin(), SCC.end());
      DEBUG(dbgs() << " Schedule First Node:-------------------\n");
      SchedResult Result = scheduleAtom(FirstNode);
      if (Result != FDLScheduler::SchedSucc)
        return Result;

      DEBUG(FDInfo->dumpTimeFrame());
      if (scheduleCriticalPath() != FDLScheduler::SchedSucc)
        return FDLScheduler::SchedFailII;
      

      //fillQueue(AQueue, SCC.begin(), SCC.end(), FirstNode);
      //// schedule other nodes.
      //Result = scheduleQueue(AQueue);
      //DEBUG(dbgs() << " Schedule SCC at II: " << FDInfo->getMII()
      //             << "-------------------\n");
      //DEBUG(FDInfo->dumpTimeFrame());
      //if (Result != FDLScheduler::SchedSucc)
      //  return Result;
    }
  }

  // The nodes not in any SCC.
  AQueue.clear();
  fillQueue(AQueue, CurState->begin(), CurState->end());

  return scheduleQueue(AQueue);
}

void FDLScheduler::FDListSchedule() {
  unsigned StartStep = HI->getTotalCycle();
  for(;;) {
    CurState->resetSchedule();
    CurState->scheduledTo(StartStep);

    FDInfo->reset();
    FDInfo->buildFDInfo();

    fds_sort s(FDInfo);
    AtomQueueType AQueue(s);

    fillQueue(AQueue, CurState->begin(), CurState->end());

    if (scheduleQueue(AQueue) != FDLScheduler::SchedSucc)
      FDInfo->lengthenCriticalPath();
    else // Break the loop if we schedule successful.
      break;
  }
  // Set the Initial Interval to the total slot, so we can generate the correct
  // control logic for loop if MS is disable.
  if (CurState->haveSelfLoop())
    CurState->setII(CurState->getTotalSlot());
  DEBUG(FDInfo->dumpTimeFrame());
}


unsigned FDLScheduler::findBestStep(HWAOpInst *A) {
  std::pair<unsigned, double> BestStep = std::make_pair(0, 1e32);
  DEBUG(dbgs() << "\tScan for best step:\n");
  // For each possible step:
  for (unsigned i = FDInfo->getASAPStep(A), e = FDInfo->getALAPStep(A) + 1;
      i != e; ++i) {
    DEBUG(dbgs() << "At Step " << i << "\n");
    // Check if we can schedule the node at this step because the resource
    // is not enough.
    if (!FDInfo->isFUAvailalbe(i, A->getFunUnit()))
      continue;

    // Compute the forces.
    double SelfForce = FDInfo->computeSelfForceAt(A, i);
    // Force update time frame
    A->scheduledTo(i);
    // Recover the time frame by force rebuild
    FDInfo->buildASAPStep(); 
    FDInfo->buildALAPStep();

    // The follow function will invalid the time frame.
    DEBUG(dbgs() << " Self Force: " << SelfForce);
    double PredForce = FDInfo->computePredForceAt(A, i);
    DEBUG(dbgs() << " Pred Force: " << PredForce);
    double SuccForce = FDInfo->computeSuccForceAt(A, i);
    DEBUG(dbgs() << " Succ Force: " << SuccForce);
    double Force = SelfForce + PredForce + SuccForce;
    DEBUG(dbgs() << " Force: " << Force);
    if (Force < BestStep.second)
      BestStep = std::make_pair(i, Force);

    DEBUG(dbgs() << '\n');
  }
  return BestStep.first;
}

void FDLScheduler::releaseMemory() {
  clear();
}

void FDLScheduler::clear() {
  CurState = 0;
  FDInfo->reset();
  MSInfo->clear();
}

void FDLScheduler::print(raw_ostream &O, const Module *M) const { }

FDLScheduler::SchedResult FDLScheduler::scheduleCriticalPath() {
  for (FSMState::iterator I = CurState->begin(), E = CurState->end();
      I != E; ++I) {
    HWAtom *A = *I;
  
    if (A->isScheduled() || FDInfo->getTimeFrame(A) != 1)
      continue;

    unsigned step = FDInfo->getASAPStep(A);
    if (HWAOpInst *OI = dyn_cast<HWAOpInst>(A)) {
      if (!FDInfo->isFUAvailalbe(step, OI->getFunUnit())) {
        DEBUG(OI->dump());
        DEBUG(dbgs() << " Can not scheduled in critical path!\n\n");
        // FIXME: Return the right reason!
        return FDLScheduler::SchedFailCricitalPath;
      }
    }
    // Schedule to the best step.
    A->scheduledTo(step);
    FDInfo->presevesFUForAtom(A);
  }
  return FDLScheduler::SchedSucc;
}

FDLScheduler::SchedResult FDLScheduler::scheduleAtom(HWAtom *A) {
  DEBUG(A->print(dbgs()));
  unsigned step = FDInfo->getASAPStep(A);
  if (FDInfo->getTimeFrame(A) != 1) {
    HWAOpInst *OI = cast<HWAOpInst>(A);
    bool ConstrainByMII = FDInfo->constrainByMII(OI);
    step = findBestStep(OI);
    DEBUG(dbgs() << "\n\nbest step: " << step << "\n");
    // If we can not schedule A.
    if (step == 0) {
      DEBUG(dbgs() << " Can not find avaliable step!\n\n");
      return ConstrainByMII ? FDLScheduler::SchedFailII :
                              FDLScheduler::SchedFailCricitalPath;
    }

    A->scheduledTo(step);
    FDInfo->presevesFUForAtom(A);
    // Dirty Hack: we are not always apply MII.
    FDInfo->buildFDInfo();
  } else { //if(!A->isScheduled())
    if (HWAOpInst *OI = dyn_cast<HWAOpInst>(A)) {
      if (!FDInfo->isFUAvailalbe(step, OI->getFunUnit())) {
        DEBUG(dbgs() << " No avaliable step!\n\n");
        return FDLScheduler::SchedFailCricitalPath;   
      }
    }
    // Schedule to the best step.
    A->scheduledTo(step);
    FDInfo->presevesFUForAtom(A);
    DEBUG(dbgs() << "\n\nasap step: " << step << "\n");
  }

  return FDLScheduler::SchedSucc;
}

FDLScheduler::SchedResult FDLScheduler::scheduleQueue(AtomQueueType &Queue) {
  while (!Queue.empty()) {
    // TODO: Short the list
    HWAOpInst *A = Queue.top();
    Queue.pop();

    DEBUG(dbgs() << " Schedule Node:-------------------\n");
    SchedResult Result = scheduleAtom(A);
    Queue.reheapify();
    if (Result != FDLScheduler::SchedSucc)
      return Result;

  }

  return FDLScheduler::SchedSucc;
}

Pass *esyn::createFDLSchedulePass() {
  return new FDLScheduler();
}
