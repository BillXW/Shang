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

    unsigned NewStep = HWAtom::MaxSlot;
 
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

      HWFUnit FU = OpInst->getFunUnit();
      double Prob = 1.0 / (double) TimeFrame;
      // Including ALAPStep.
      for (unsigned i = ASAPStep, e = ALAPStep + 1; i != e; ++i)
        accDGraphAt(i, FU.getFUnitID(), Prob);
    }
  }
  DEBUG(printDG(dbgs()));
}


bool ForceDirectedInfo::isResourceConstraintPreserved() {
  // No resource in use.
  if (DGraph.empty()) return true;
  
  HWFUnitID FUID;
  unsigned DummyStep;
  decompseStepKey(DGraph.begin()->first, DummyStep, FUID);

  double TotalDG = 0;
  unsigned AvailableSteps = 0;
  for (DGType::const_iterator I = DGraph.begin(), E = DGraph.end(); I != E; ++I) {
    unsigned Step = 0;
 
    HWFUnitID CurID;
    decompseStepKey(I->first, Step, CurID);
    // We iterate to a new function unit?
    if (CurID != FUID) {
      // Compute the average usage of the old function unit.
      double AverageDG = TotalDG / AvailableSteps;
      if (AverageDG > LocalAvailabeRes[FUID]) {
        DEBUG(dbgs() << "Bad resource usage: Average " << AverageDG
                     << " Available: " << LocalAvailabeRes[FUID] << '\n');
        return false;
      }

      // Update data to count a new FU.
      FUID = CurID;
      AvailableSteps = 0;
      TotalDG = 0.0;
    }
    ++AvailableSteps;
    TotalDG += I->second;
  }
  // Do not forget the last one.
  double AverageDG = TotalDG / AvailableSteps;
  DEBUG(
    if (AverageDG > LocalAvailabeRes[FUID])
      dbgs() << "Bad resource usage: Average " << AverageDG
             << " Available: " << LocalAvailabeRes[FUID] << '\n';
    );
  // NOTE: we do not use <= because "==" of float point number dose not make sence. 
  return  !(AverageDG > LocalAvailabeRes[FUID]);
}

void ForceDirectedInfo::printDG(raw_ostream &OS) const {  
  // For each step
  for (DGType::const_iterator I = DGraph.begin(), E = DGraph.end(); I != E; ++I) {
    unsigned Key = I->first;
    double V = I->second;

    unsigned Step;
    HWFUnitID FUID;
    decompseStepKey(Key, Step, FUID);
    OS << '[' << FUID << "] @ " << Step << ": " << V << '\n';
  }
}

unsigned
ForceDirectedInfo::computeStepKey(unsigned step, HWFUnitID FUID) const {
  if (MII != 0) {
#ifndef NDEBUG
    unsigned StartSlot = State->getSlot();
    step = StartSlot + (step - StartSlot) % MII;
#else
    step = step % MII;
#endif
  }

  union {
    struct {
      unsigned Step     : 16;
      unsigned ResData  : 16;
    } S;
    unsigned Data;
  } U;

  U.S.Step = step;
  U.S.ResData = FUID.getRawData();
  return U.Data;
}

void ForceDirectedInfo::decompseStepKey(unsigned key, unsigned &step,
                                        HWFUnitID &FUID) {
  union {
    struct {
      unsigned Step     : 16;
      unsigned ResData  : 16;
    } S;
    unsigned Data;
  } U;

  U.Data = key;

  step = U.S.Step;
  FUID = U.S.ResData;
}

double ForceDirectedInfo::getDGraphAt(unsigned step, HWFUnitID FUID) const {
  // Modulo DG for modulo schedule.
  DGType::const_iterator at = DGraph.find(computeStepKey(step , FUID));
  
  if (at != DGraph.end()) return at->second;  

  return 0.0;
}

void ForceDirectedInfo::accDGraphAt(unsigned step, HWFUnitID FUID, double d) {
  // Modulo DG for modulo schedule.
  DGraph[computeStepKey(step , FUID)] += d;
}

double ForceDirectedInfo::getRangeDG(HWFUnitID FUID, unsigned start,
                                     unsigned end/*included*/) {
  double range = end - start + 1;
  double ret = 0.0;
  for (unsigned i = start, e = end + 1; i != e; ++i)
    ret += getDGraphAt(i, FUID);

  ret /= range;
  return ret;
}

double ForceDirectedInfo::computeSelfForceAt(const HWAtom *A, unsigned step) {
  const HWAOpInst *OpInst = dyn_cast<HWAOpInst>(A);
  if (!OpInst) return 0.0;

  double Force = getDGraphAt(step, OpInst->getFunUnitID()) - getAvgDG(OpInst);
  HWFUnit FU = OpInst->getFunUnit();
  // Make the atoms taking expensive function unit have bigger force.
  return Force / FU.getTotalFUs();
}

double ForceDirectedInfo::computeRangeForce(const HWAtom *A, unsigned int start,
                                            unsigned int end) {
  const HWAOpInst *OpInst = dyn_cast<HWAOpInst>(A);
  if (!OpInst) return 0.0;

  HWFUnit FU = OpInst->getFunUnit();
  double Force = getRangeDG(FU.getFUnitID(), start, end) - getAvgDG(OpInst);
  return Force / FU.getTotalFUs();
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
        res += getDGraphAt(i, A->getFunUnitID());

      res /= (double) getTimeFrame(A);
      AvgDG[A] = res;
    }
}

void ForceDirectedInfo::reset() {
  AtomToTF.clear();
  DGraph.clear();
  ResUsage.clear();
  AvgDG.clear();
}

void ForceDirectedInfo::releaseMemory() {
  reset();
  LocalAvailabeRes.clear();
  MII = 0;
  CriticalPathEnd = 0;
}

bool ForceDirectedInfo::runOnBasicBlock(BasicBlock &BB) {
  HWAtomInfo &HI = getAnalysis<HWAtomInfo>();
  State = HI.getStateFor(BB);
  for (FSMState::iterator I = State->begin(), E = State->end(); I != E; ++I)
    if (HWAOpInst *A = dyn_cast<HWAOpInst>(*I))
      LocalAvailabeRes[A->getFunUnitID()] = A->getFunUnit().getTotalFUs();
  
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
