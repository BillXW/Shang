//===- ForceDirectedInfo.cpp - ForceDirected information analyze --*- C++ -*-===//
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
// This file implement the Force Direct information computation pass describe in
// Force-Directed Scheduling for the Behavioral Synthesis of ASIC's
//
//===----------------------------------------------------------------------===//

#include "ForceDirectedInfo.h"
#include "HWAtomPasses.h"

#define DEBUG_TYPE "vbe-fd-info"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace esyn;

//===----------------------------------------------------------------------===//
char ForceDirectedInfo::ID = 0;

RegisterPass<ForceDirectedInfo> X("vbe-fd-info",
                           "vbe - Compute necessary information for force"
                           " directed scheduling passes");

void ForceDirectedInfo::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequiredTransitive<HWAtomInfo>();
  AU.setPreservesAll();
}


void ForceDirectedInfo::buildASAPStep(const HWAtom *Root, unsigned step) {
  AtomToTF[Root].first = step;

  FSMState::iterator Start = std::find(State->begin(), State->end(),
                                       Root);

  for (FSMState::iterator I = ++Start, E = State->end(); I != E; ++I) {
    HWAtom *A = *I;
    if (A->isScheduled()) {
      AtomToTF[A].first = A->getSlot();
      continue;
    }

    unsigned NewStep = 0;

    for (HWAtom::dep_iterator DI = A->dep_begin(), DE = A->dep_end();
        DI != DE; ++DI) {
      const HWAtom *Dep = *DI;
      if (!DI.getEdge()->isBackEdge() || (Dep->isScheduled() && MII)) {
        unsigned Step = getASAPStep(Dep) + Dep->getLatency()
                        - MII * DI.getEdge()->getItDst();
        DEBUG(dbgs() << "From ";
              if (DI.getEdge()->isBackEdge())
                dbgs() << "BackEdge ";
              Dep->print(dbgs());
              dbgs() << " Step " << Step << '\n');
        NewStep = std::max(Step, NewStep);
      }
    }

    DEBUG(dbgs() << "Update ASAP step to: " << NewStep << " for \n";
    A->dump();
    dbgs() << "\n\n";);
    AtomToTF[A].first = NewStep;
  }

  HWAOpInst *Exit = State->getExitRoot();
  CriticalPathEnd = std::max(CriticalPathEnd, getASAPStep(Exit));
}

void ForceDirectedInfo::buildALAPStep(const HWAtom *Root, unsigned step) {
  AtomToTF[Root].second = step;

  FSMState::reverse_iterator Start = std::find(State->rbegin(), State->rend(),
                                               Root);

  for (FSMState::reverse_iterator I = ++Start, E = State->rend();
       I != E; ++I) {
    HWAtom *A = *I;
    if (A->isScheduled()) {
      AtomToTF[A].second = A->getSlot();
      continue;
    }

    unsigned NewStep = SCCAtoms.count(A) ? getASAPStep(A) + MII - A->getLatency()
                       : HWAtom::MaxSlot;
 
    for (HWAtom::use_iterator UI = A->use_begin(), UE = A->use_end();
         UI != UE; ++UI) {
      HWEdge *UseEdge = (*UI)->getEdgeFrom(A);
      const HWAtom *Use = *UI;

      if (!UseEdge->isBackEdge()
          || (Use->isScheduled() && MII)) {
        unsigned Step = getALAPStep(Use) - A->getLatency()
                        + MII * UseEdge->getItDst();
        DEBUG(dbgs() << "From ";
              if (UseEdge->isBackEdge())
                dbgs() << "BackEdge ";
              Use->print(dbgs());
              dbgs() << " Step " << Step << '\n');
        NewStep = std::min(Step, NewStep);
      }
    }

    DEBUG(dbgs() << "Update ALAP step to: " << NewStep << " for \n";
          A->dump();
          dbgs() << "\n\n";);
    AtomToTF[A].second = NewStep;

    assert(getALAPStep(A) >= getASAPStep(A)
      && "Broken time frame!");
  }
}


void ForceDirectedInfo::printTimeFrame(raw_ostream &OS) const {
  OS << "Time frame:\n";
  for (FSMState::iterator I = State->begin(), E = State->end();
      I != E; ++I) {
    HWAtom *A = *I;
    A->print(OS);
    OS << " : {" << getASAPStep(A) << "," << getALAPStep(A)
      << "} " <<  getTimeFrame(A) << "\n";
  }
}

void ForceDirectedInfo::dumpTimeFrame() const {
  printTimeFrame(dbgs());
}

void ForceDirectedInfo::buildDGraph() {
  DGraph.clear();
  for (FSMState::iterator I = State->begin(), E = State->end(); I != E; ++I){
    // We only try to balance the post bind resource.
    if (HWAOpInst *OpInst = dyn_cast<HWAOpInst>(*I)) {
      // Ignore the DG for trivial resources.
      if (OpInst->isTrivial()) continue;

      unsigned TimeFrame = getTimeFrame(OpInst);
      unsigned ASAPStep = getASAPStep(OpInst), ALAPStep = getALAPStep(OpInst);

      HWFUnit *FU = OpInst->getFunUnit();
      double Prob = 1.0 / (double) TimeFrame;
      // Including ALAPStep.
      for (unsigned i = ASAPStep, e = ALAPStep + 1; i != e; ++i)
        accDGraphAt(i, FU, Prob);
    }
  }
  DEBUG(printDG(dbgs()));
}


bool ForceDirectedInfo::isResourceConstraintPreserved() {
  // No resource in use.
  if (DGraph.empty()) return true;

  for (DGType::const_iterator I = DGraph.begin(), E = DGraph.end();
       I != E; ++I) {
    DEBUG(dbgs() << "FU " << *I->first << " Average Usage: ");
    double TotalDG = 0;
    unsigned AvailableSteps = 0;
    for (DGStepMapType::const_iterator SI = I->second.begin(),
         SE = I->second.end(); SI != SE; ++SI) {
      ++AvailableSteps;
      TotalDG += SI->second;
    }
    double AverageDG = TotalDG / AvailableSteps;
    DEBUG(dbgs() << AverageDG << '\n');
    if (AverageDG > I->first->getTotalFUs())
      return false;
  }
  return true;
}

void ForceDirectedInfo::printDG(raw_ostream &OS) const {  
  // For each step
  // For each FU.
  for (DGType::const_iterator I = DGraph.begin(), E = DGraph.end();
       I != E; ++I) {
    OS << "FU " << *I->first << ":\n";
    for (DGStepMapType::const_iterator SI = I->second.begin(),
         SE = I->second.end(); SI != SE; ++SI) {
      OS << "@ " << SI->first << ": " << SI->second << '\n';
    }
    OS << '\n';
  }
}

double ForceDirectedInfo::getDGraphAt(unsigned step, HWFUnit *FU) const {
  // Modulo DG for modulo schedule.
  DGType::const_iterator at = DGraph.find(FU);
  if (at != DGraph.end()) {
    DGStepMapType::const_iterator SI = at->second.find(computeStepKey(step));
    if (SI != at->second.end())
      return SI->second;
  }

  return 0.0;
}

unsigned ForceDirectedInfo::computeStepKey(unsigned step) const {
  if (MII != 0) {
#ifndef NDEBUG
    unsigned StartSlot = State->getSlot();
    step = StartSlot + (step - StartSlot) % MII;
#else
    step = step % MII;
#endif
  }

  return step;
}

void ForceDirectedInfo::accDGraphAt(unsigned step, HWFUnit *FU, double d) {
  // Modulo DG for modulo schedule.
  DGraph[FU][computeStepKey(step)] += d;
}

// Including end.
double ForceDirectedInfo::getRangeDG(HWFUnit  *FU, unsigned start, unsigned end) {
  double range = end - start + 1;
  double ret = 0.0;
  for (unsigned i = start, e = end + 1; i != e; ++i)
    ret += getDGraphAt(i, FU);

  ret /= range;
  return ret;
}

double ForceDirectedInfo::computeSelfForceAt(const HWAtom *A, unsigned step) {
  const HWAOpInst *OpInst = dyn_cast<HWAOpInst>(A);
  if (!OpInst) return 0.0;

  HWFUnit *FU = OpInst->getFunUnit();
  double Force = getDGraphAt(step, FU) - getAvgDG(OpInst);

  // Make the atoms taking expensive function unit have bigger force.
  return Force / FU->getTotalFUs();
}

double ForceDirectedInfo::computeRangeForce(const HWAtom *A, unsigned int start,
                                            unsigned int end) {
  const HWAOpInst *OpInst = dyn_cast<HWAOpInst>(A);
  if (!OpInst) return 0.0;

  HWFUnit *FU = OpInst->getFunUnit();
  double Force = getRangeDG(FU, start, end) - getAvgDG(OpInst);
  return Force / FU->getTotalFUs();
}

double ForceDirectedInfo::computeSuccForceAt(const HWAtom *A, unsigned step) {
  // Adjust the time frame.
  buildASAPStep(A, step); 

  double ret = 0.0;
  FSMState::iterator at = std::find(State->begin(), State->end(), A);
  assert(at != State->end() && "Can not find Atom!");

  for (FSMState::iterator I = ++at, E = State->end(); I != E; ++I)
    if (const HWAOpInst *OI = dyn_cast<HWAOpInst>(*I))
      ret += computeRangeForce(OI, getASAPStep(OI), getALAPStep(OI));

  return ret;
}

double ForceDirectedInfo::computePredForceAt(const HWAtom *A, unsigned step) {
  // Adjust the time frame.
  buildALAPStep(A, step);

  double ret = 0;
  FSMState::iterator at = std::find(State->begin(), State->end(), A);
  assert(at != State->end() && "Can not find Atom!");

  for (FSMState::iterator I = State->begin(), E = at; I != E; ++I)
    if (const HWAOpInst *OI = dyn_cast<HWAOpInst>(*I))
      ret += computeRangeForce(OI, getASAPStep(OI), getALAPStep(OI));

  return ret;
}

void ForceDirectedInfo::buildAvgDG() {
  for (FSMState::iterator I = State->begin(), E = State->end();
       I != E; ++I)
    // We only care about the utilization of post bind resource. 
    if (HWAOpInst *A = dyn_cast<HWAOpInst>(*I)) {
      double res = 0.0;
      for (unsigned i = getASAPStep(A), e = getALAPStep(A) + 1; i != e; ++i)
        res += getDGraphAt(i, A->getFunUnit());

      res /= (double) getTimeFrame(A);
      AvgDG[A] = res;
    }
}

void ForceDirectedInfo::reset() {
  AtomToTF.clear();
  SCCAtoms.clear();
  DGraph.clear();
  AvgDG.clear();
}

void ForceDirectedInfo::releaseMemory() {
  reset();
  MII = 0;
  CriticalPathEnd = 0;
}

bool ForceDirectedInfo::runOnBasicBlock(BasicBlock &BB) {
  HWAtomInfo &HI = getAnalysis<HWAtomInfo>();
  State = HI.getStateFor(BB);
  
  return false;
}

unsigned ForceDirectedInfo::buildFDInfo() {
  // Build the time frame
  assert(State->isScheduled() && "Entry must be scheduled first!");
  unsigned FirstStep = State->getSlot();
  buildASAPStep(State, FirstStep);
  buildALAPStep(State->getExitRoot(), CriticalPathEnd);

  DEBUG(dumpTimeFrame());

  buildDGraph();
  buildAvgDG();

  return CriticalPathEnd;
}

void ForceDirectedInfo::dumpDG() const {
  printDG(dbgs());
}
