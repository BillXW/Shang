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

class Scheduler {
  // {instance, next available cycle}
  typedef std::map<HWResource::ResIdType, unsigned> ResCycMapType;

  ResCycMapType ResCycMap;

protected:
  typedef std::list<HWAtom*> SchedAtomVec;
  SchedAtomVec ScheduleAtoms;

  void clear();

  //
  static bool isOperationFinish(const HWAtom *Atom, unsigned CurSlot);
  static bool isAllDepsOpFin(const HWAtom *Atom, unsigned CurSlot);
  // Get the ready cycle of the given resource.
  unsigned getReadyCycle(HWResource::ResIdType ResId);

  // Remember the ready cycle of the given resource.
  void rememberReadyCycle(HWResource::ResIdType ResId, unsigned ReadyCycle);

  // Get Any ready atom.
  HWAtom *getReadyAtoms(unsigned Cycle);

  void removeFromList(HWAtom *Atom);

  void createAtomList(HWAtomInfo *HI, BasicBlock &BB);
public:
  Scheduler() {}
  virtual ~Scheduler();

};

} // end namespace

#endif
