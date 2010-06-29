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

#include "llvm/Support/Debug.h"


using namespace llvm;
using namespace esyn;

//===----------------------------------------------------------------------===//
static bool isAtomFinish(const HWAtom &Atom, unsigned CurSlot) {
  if (const HWAOpInst *Op = dyn_cast<HWAOpInst>(&Atom))
    return Op->getSlot() + Op->getLatency() <= CurSlot;

  if (isa<HWAInline>(Atom))
    return Atom.getSlot() <= CurSlot;

  // Constant is always finish
  // Entry root is always finish
  return true;
}

bool SchedAtom::isOperationFinish(unsigned CurSlot) const {
  return isAtomFinish(*Atom, CurSlot);
}

bool SchedAtom::isAllDepsOpFin(unsigned CurSlot) const {
  for (const_dep_iterator I = Atom->dep_begin(), E = Atom->dep_end();
      I != E; ++I) {
    const HWAtom &A = **I;  
    if (!isAtomFinish(A, CurSlot))
      return false;
  }
  return true;
}

//===----------------------------------------------------------------------===//
void Scheduler::clear() {
  ScheduleAtoms.clear();
  ResCycMap.clear();
}

Scheduler::~Scheduler() {
  clear();
}

unsigned Scheduler::getReadyCycle(HWResource::ResIdType ResId) {
  return ResCycMap[ResId];
}

void Scheduler::rememberReadyCycle(HWResource::ResIdType ResId,
                                   unsigned ReadyCycle) {
  ResCycMap[ResId] = ReadyCycle;
}

SchedAtom *Scheduler::getReadyAtoms(unsigned Cycle) {
  for (SchedAtomVec::iterator I = ScheduleAtoms.begin(),
      E = ScheduleAtoms.end(); I != E; ++I) {
    SchedAtom *atom = *I;
    if (atom->isAllDepsOpFin(Cycle)) {
      DEBUG((*atom)->print(dbgs()));
      DEBUG(dbgs() << " is Ready\n");
      return atom;
    }
  }
  return 0;
}

void Scheduler::removeFromList(SchedAtom *Atom) {
  SchedAtomVec::iterator at = std::find(ScheduleAtoms.begin(),
                                        ScheduleAtoms.end(), Atom);
  ScheduleAtoms.erase(at);
}

void Scheduler::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<HWAtomInfo>();
  AU.addRequired<ResourceConfig>();
}

bool Scheduler::runOnBasicBlock(BasicBlock &BB) {
  HI = &getAnalysis<HWAtomInfo>();
  RC = &getAnalysis<ResourceConfig>();

  ExecStage &State = HI->getStateFor(BB);
  // Buidl the schedule atom list
  for (ExecStage::iterator I = State.begin(), E = State.end(); I != E; ++I)
    ScheduleAtoms.push_back(new SchedAtom(**I));
  
  scheduleBasicBlock(State);
  return false;
}

void Scheduler::releaseMemory() {
  clear();
  releaseContext();
}
