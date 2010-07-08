//===--- vbe/SchedulerBase.h - The BaseClass of all scheduler  ----*- C++ -*-===//
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
// This file define the common interface of a scheduler
//
//===----------------------------------------------------------------------===//
//
#ifndef VBE_SCHEDULER_BASE_H
#define VBE_SCHEDULER_BASE_H

#include "vbe/ResourceConfig.h"
#include <list>

using namespace llvm;

namespace esyn {
class HWAtomInfo;
class HWAtom;
class HWAOpInst;
class FSMState;

class Scheduler {
  // {instance, next available cycle}
  typedef std::map<HWResource::ResIdType, unsigned> ResCycMapType;

  ResCycMapType ResCycMap;

protected:
  typedef std::list<HWAOpInst*> SchedAtomVec;
  typedef std::list<HWAOpInst*>::iterator ListIt;
  SchedAtomVec ScheduleAtoms;

  FSMState *CurStage;

  void clearSchedulerBase();

  //
  static bool isOperationFinish(const HWAtom *Atom, unsigned CurSlot);
  static bool isAllDepsOpFin(const HWAOpInst *Atom, unsigned CurSlot);
  static bool isAllDepsScheduled(const HWAOpInst *Atom);
  // Get the ready cycle of the given resource.
  unsigned getReadyCycle(HWResource::ResIdType ResId);

  // Remember the ready cycle of the given resource.
  void rememberReadyCycle(HWResource::ResIdType ResId, unsigned ReadyCycle);

  // Get Any ready atom.
  ListIt getReadyAtoms(unsigned Cycle);

  ListIt list_begin() { return ScheduleAtoms.begin(); }
  ListIt list_end() { return ScheduleAtoms.end(); }
  bool isListEmpty() const { return ScheduleAtoms.empty(); }
  void removeFromList(ListIt It);

  void createAtomList();
public:
  Scheduler() : CurStage(0) {}
  virtual ~Scheduler();

};

} // end namespace

#endif
