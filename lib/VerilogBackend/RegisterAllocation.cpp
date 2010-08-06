//===-------- RegisterAllocation.cpp - Allocation registers -----*- C++ -*-===//
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
// This file implement a pass that reduce unnecessary registers
//
//===----------------------------------------------------------------------===//

#include "HWAtomPasses.h"
#include "HWAtomInfo.h"

#define DEBUG_TYPE "vbe-reg-alloca"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace esyn;

namespace {
struct RegAllocation : public FunctionPass {
  static char ID;
  explicit RegAllocation() : FunctionPass(&ID) {}
  bool runOnFunction(Function &F);
  bool runOnBasicBlock(BasicBlock &BB, HWAtomInfo &HI);
  void getAnalysisUsage(AnalysisUsage &AU) const;
};
}

bool RegAllocation::runOnFunction(Function &F) {
  HWAtomInfo &HI = getAnalysis<HWAtomInfo>();

  // Allocate register for argument.
  //for (Function::arg_iterator I = F.arg_begin(), E = F.arg_end();
  //    I != E; ++I) {
  //  Argument *Arg = I;
  //  HI.getRegForValue(Arg, 1, 1);
  //}

  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I)
    runOnBasicBlock(*I, HI);

  return false;
}

bool RegAllocation::runOnBasicBlock(BasicBlock &BB, HWAtomInfo &HI) {
  FSMState &State = HI.getStateFor(BB);
  HWAVRoot *EntryRoot = &State.getEntryRoot();

  // Emit the operand of PHINode.
  for (BasicBlock::iterator II = BB.begin(), IE = BB.getFirstNonPHI();
      II != IE; ++II) {
    PHINode *PN = cast<PHINode>(II);
    for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
      Value *IV = PN->getIncomingValue(i);
      FSMState &IncomingStage = HI.getStateFor(*PN->getIncomingBlock(i));
      if ((isa<Instruction>(IV) || isa<Argument>(IV))
          && !IncomingStage.getLiveOutRegAtTerm(IV)) {
        HWReg *IR = HI.getRegForValue(IV, EntryRoot->getSlot(),
                                      EntryRoot->getSlot());
        IncomingStage.updateLiveOutReg(IV, IR);
      }
    }
  }

  SmallVector<HWAtom*, 32> Worklist(usetree_iterator::begin(EntryRoot),
                                    usetree_iterator::end(EntryRoot));

  while(!Worklist.empty()) {
    HWAOpInst *A = dyn_cast<HWAOpInst>(Worklist.back());
    Worklist.pop_back();
    
    if (A == 0)
      continue;

    DEBUG(A->print(dbgs()));
    DEBUG(dbgs() << " Visited\n");

    for (unsigned i = 0, e = A->getNumDeps(); i != e; ++i) {
      if (HWValDep *VD = dyn_cast<HWValDep>(A->getDep(i))) {
        Value *V = A->getIOperand(i);
        if (VD->isImport()) {
          // Insert the import node.
          HWAVRoot *Root = cast<HWAVRoot>(VD->getDagSrc());
          HWReg *R = HI.getRegForValue(V, Root->getSlot(), A->getSlot());
          // Update the live out value.
          if (!State.getLiveOutRegAtTerm(V))
            State.updateLiveOutReg(V, R);

          HWAImpStg *ImpStg = HI.getImpStg(Root, R, *V);
          A->setDep(i, ImpStg);

        } else if (HWAOpInst *DI = dyn_cast<HWAOpInst>(VD->getDagSrc())) {
          // We need to register the value if the value life through
          // several cycle. Or we need to keep the value until the computation
          // finish.
          if (DI->getFinSlot() != A->getSlot() || A->getLatency() > 0) {
            assert(DI->getFinSlot() <= A->getSlot() && "Bad Schedule!");
            DEBUG(DI->print(dbgs()));
            DEBUG(dbgs() << " Registered\n");
            // Store the value to register.
            HWReg *R = HI.getRegForValue(V, DI->getFinSlot(), A->getSlot());
            HWAWrStg *WR = HI.getWrStg(DI, R);
            A->setDep(i, WR);
          }
        } else if (HWAWrStg *WrStg = dyn_cast<HWAWrStg>(VD->getDagSrc())) {
          // Move the value out of the Function unit register.
          assert(WrStg->getReg()->isFuReg()
                 && "Only Expect function unit register!");
          if (WrStg->getReg()->getEndSlot() < A->getSlot()) {
            DEBUG(WrStg->print(dbgs()));
            DEBUG(dbgs() << " extended\n");
            HWReg *R = HI.getRegForValue(V, WrStg->getFinSlot(), A->getSlot());
            HWAWrStg *WR = HI.getWrStg(WrStg, R);
            A->setDep(i, WR);
          }
        }
      }
    }
  }
  
  // Emit the exported register.
  HWAOpInst *Exit = &State.getExitRoot();
  for (unsigned i = Exit->getInstNumOps(), e = Exit->getNumDeps(); i != e; ++i) {
    HWCtrlDep *CD = dyn_cast<HWCtrlDep>(Exit->getDep(i));
    if (!(CD && CD->isExport())) continue;

    HWAtom *SrcAtom = CD->getDagSrc();

    // If we already emit the register, just skip it.
    if (HWAWrStg *WR = dyn_cast<HWAWrStg>(SrcAtom))
      if (!WR->getReg()->isFuReg())
        continue;

    Value *V = &SrcAtom->getValue();

    DEBUG(SrcAtom->print(dbgs()));
    DEBUG(dbgs() << " Registered for export.\n");
    // Store the value to register.
    HWReg *R = HI.getRegForValue(V, SrcAtom->getFinSlot(), Exit->getSlot());
    HWAWrStg *WR = HI.getWrStg(SrcAtom, R);

    if (!State.getLiveOutRegAtTerm(V))
      State.updateLiveOutReg(V, R);
    Exit->setDep(i, WR);
  }

  return false;
}

void RegAllocation::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<HWAtomInfo>();
  AU.setPreservesAll();
}

char RegAllocation::ID = 0;

Pass *esyn::createRegisterAllocationPass() {
  return new RegAllocation();
}
