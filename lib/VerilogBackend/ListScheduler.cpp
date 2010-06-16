//===------- ScheduleDriver.cpp - The Scheduler driver pass  ----*- C++ -*-===//
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
// This file implement the ScheduleDriver Pass, which run difference schedulers
// on a llvm function to schedule the Hardware atoms.
//
//===----------------------------------------------------------------------===//
//

#include "ScheduleDriver.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace esyn;

char ListScheduler::ID = 0;

void ListScheduler::print(raw_ostream &O, const Module *M) const {

}

void ListScheduler::getAnalysisUsage( AnalysisUsage &AU ) const {
  AU.addRequired<HWAtomInfo>();
  //AU.addRequired<ScheduleDriver>();
  AU.setPreservesAll();
}


bool esyn::ListScheduler::runOnBasicBlock(BasicBlock &BB) {
  HI = &getAnalysis<HWAtomInfo>();

  dbgs() << "At BB: " << BB.getName() << '\n';

  unsigned CurCycle = 0;

  HWAState &State = HI->getStateFor(BB);
  HWAStateEnd *StateEnd = State.getStateEnd();
  assert(StateEnd && "Why StateEnd is null?");

  typedef SmallVector<HWAtom*, 64> HWAtomVec;
  HWAtomVec Atoms(State.begin(), State.end());

  // TODO: sort the atoms

  // Remember the state start, so we can schedule this bb again
  State.scheduledTo(CurCycle);
  ++CurCycle;
  // Schedule StateBegin

  // TODO: Check if the atoms are empty
  while (!StateEnd->isAllDepsOpFin(CurCycle)) {
    dbgs() << "======Cycle " << CurCycle << "\n";
    // Find all ready atoms

    // For each ready atoms
    while(HWAtom *ReadyAtom = getReadyAtoms(Atoms, CurCycle)){
      if (HWAOpRes *OpRes = dyn_cast<HWAOpRes>(ReadyAtom)) {
        HWResource *Res = &OpRes->getUsedResource();
        unsigned ResInstance = OpRes->getAllocatedResourceInstance();
        if (ResInstance == 0) // Not allocate?
          // Try to allocate a resource for it
          // Or just get the "idle instance?"
          ResInstance = Res->getLeastBusyInstance();

        // Is the resource available?
        unsigned readyCyc = getReadyCycle(Res, ResInstance);
        if (readyCyc > CurCycle)
          continue;
        
        //
        rememberReadyCycle(Res, ResInstance, CurCycle + Res->getLatency());
      }
      ReadyAtom->scheduledTo(CurCycle);

      ReadyAtom->print(dbgs());
      dbgs() << " scheduled\n";
      // Remove the exist atom
      HWAtomVec::iterator at = std::find(Atoms.begin(), Atoms.end(), ReadyAtom);
      Atoms.erase(at);
    }

    // Advance the state
    ++CurCycle;
  }
  // schedule the state end;
  StateEnd->scheduledTo(CurCycle);

  State.print(dbgs());
  return false;
}


void ListScheduler::releaseMemory() {
  clear();
}
