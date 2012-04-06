//===--- HSLInliner.cpp ---Perform HLS specific function inlining ---------===//
//
// Copyright: 2011 by SYSU EDA Group. all rights reserved.
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
//
//===----------------------------------------------------------------------===//

#include "vtm/Passes.h"
#include "llvm/CallingConv.h"
#include "llvm/Instructions.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/Module.h"
#include "llvm/Type.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/InlinerPass.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/raw_ostream.h"
#define DEBUG_TYPE "AlwaysInlineFunction"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {

// AlwaysInliner only inlines functions that are mark as "always inline".
class HLSInliner : public Inliner {
  // Functions that are never inlined
  SmallPtrSet<const Function*, 16> NeverInline;
  InlineCostAnalyzer CA;
public:
  // Use extremely low threshold.
  HLSInliner() : Inliner(ID) {
    initializeHLSInlinerPass(*PassRegistry::getPassRegistry());
  }
  static char ID; // Pass identification, replacement for typeid
  InlineCost getInlineCost(CallSite CS) {
    return CA.getInlineCost(CS, -1);
  }
  virtual bool doInitialization(CallGraph &CG);
};
}

char HLSInliner::ID = 0;
INITIALIZE_PASS_BEGIN(HLSInliner, "hls-inline",
                "Inliner HLS specific algorithm", false, false)
INITIALIZE_AG_DEPENDENCY(CallGraph)
INITIALIZE_PASS_END(HLSInliner, "hls-inline",
                "Inliner HLS specific algorithm", false, false)

Pass *llvm::createHLSInlinerPass() {
  return new HLSInliner();
}

// doInitialization - Initializes the vector of functions that have not
// been annotated with the "always inline" attribute.
bool HLSInliner::doInitialization(CallGraph &CG) {
  Module &M = CG.getModule();
  bool Changed = false;

  for (Module::iterator I = M.begin(), E = M.end();
       I != E; ++I) {
    Function *F = I;

    if (F->isDeclaration() || F->hasFnAttr(Attribute::NoInline)) {
      NeverInline.insert(F);
      DEBUG(dbgs() << "No inline " << F->getName() << '\n');
      continue;
    }

    CallGraphNode *CGN = CG[F];

    // Inlining the functions with only 1 caller will never increase
    // the module size.
    if (CGN->getNumReferences() == 1)
      F->addAttribute(~0, Attribute::AlwaysInline);
  }

  return Changed;
}
