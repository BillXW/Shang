//===----- ScalarStreamization.cpp - Translate register to FIFO -*- C++ -*-===//
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
// This file implement a pass that translate scalar registers to fifos, to 
// eliminate the anti dependencies of scalar in modulo scheduled loop.
//
//===----------------------------------------------------------------------===//


#include "HWAtomPasses.h"
#include "HWAtomInfo.h"

#define DEBUG_TYPE "vbe-ss"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace esyn;

namespace {
struct ScalarStreamization : public BasicBlockPass {
  static char ID;
  ScalarStreamization() : BasicBlockPass(&ID) {}
  bool runOnBasicBlock(BasicBlock &BB);
  void getAnalysisUsage(AnalysisUsage &AU) const;
};
}

bool ScalarStreamization::runOnBasicBlock(BasicBlock &BB) {
  HWAtomInfo &HI = getAnalysis<HWAtomInfo>();
  FSMState *State = HI.getStateFor(BB);
  HWAOpFU *Exit = State->getExitRoot();

  // No need to SS.
  if (!State->isPipelined())
    return false;

  unsigned II = State->getII();

  DEBUG(dbgs() << "Find Self Loop :" << BB.getName() << " II: " << II << '\n');
  std::vector<HWAtom*> WorkList;
  for (FSMState::iterator I = State->begin(), E = State->end();
      I != E; ++I)
    if (HWAWrReg *WrReg = dyn_cast<HWAWrReg>(*I)) {
      if (!WrReg->writeFUReg())
        WorkList.push_back(WrReg);
    } else if (HWALIReg *LIReg = dyn_cast<HWALIReg>(*I)) {
      if (LIReg->isPHINode())
        WorkList.push_back(LIReg);
    }

  while (!WorkList.empty()) {
    HWAtom *Src = WorkList.back();
    const Type *Ty = Src->getValue().getType();
    WorkList.pop_back();

    std::vector<HWAtom *> RegUsers(Src->use_begin(), Src->use_end());
    HWAWrReg *NewWrReg = 0;
    while (!RegUsers.empty()) {
      HWAtom *Dst = RegUsers.back();
      RegUsers.pop_back();

      // We need to pipeline the pred
      // Because there may be anti dependence across iterations.
      if (Dst == Exit
          && (Exit->getInstNumOps() == 0 || Exit->getOperand(0) != Src))
        continue;

      HWEdge *E = Dst->getEdgeFrom(Src);
      if (E->isBackEdge())
        continue;
      // Ignore the PHI edge.
      else if (HWValDep *VD = dyn_cast<HWValDep>(E))
        if (VD->getDepType() == HWValDep::PHI)
          continue;

      // Anti dependency occur because new value will write to the original
      // register before it read.
      DEBUG(
        dbgs() << "Src at Slot: " << Src->getSlot() << " ";
        Src->dump();
        dbgs() << "Dst at Slot: " << Dst->getSlot() << " ";
        Dst->dump();
        dbgs() << "Interval: " << (Dst->getSlot() - Src->getSlot()) << "\n\n";
      );
      // The new value will come at slot II, and we must copy the old value out
      // before this moment, that means we need to emit the copy at
      // II - latancy of register assignment in FSM slot.
      // Dst->getSlot() - Src->getSlot() > II or
      // Dst->getSlot() - Src->getSlot() == II and  Exit->getOperand(0) != Src
      if (Dst->getSlot() - Src->getSlot() > II ||
          Dst->getSlot() - Src->getSlot() == II &&  Exit->getOperand(0) != Src){
        DEBUG(dbgs() << "Anti dependency found:\n");
        if (NewWrReg == 0) {
          unsigned StartSlot = Src->getSlot() + II;
          HWRegister *NewReg = HI.allocaRegister(Ty);
          NewWrReg = HI.getWrReg(Src, NewReg, StartSlot);
        }
        DEBUG(dbgs() << "---------------->Insert SS Reg ");
        DEBUG(NewWrReg->dump());
        DEBUG(dbgs() << "before ");
        DEBUG(Dst->dump());
        Dst->replaceDep(Src, NewWrReg);
      }
    } // End foreach RegUsers.

    // Add new node to list so we could check it.
    if (NewWrReg)
      WorkList.push_back(NewWrReg);
  }

  return false;
}

void ScalarStreamization::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<HWAtomInfo>();
  AU.setPreservesAll();
}

char ScalarStreamization::ID = 0;

Pass *esyn::createScalarStreamizationPass() {
  return new ScalarStreamization();
}
