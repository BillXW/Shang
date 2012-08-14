//===- DesignMetrics.cpp - Estimate the metrics of the design ---*- C++ -*-===//
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
// This file implement the DesignMetrics class, which estimate the resource
// usage and speed performance of the design at early stage.
//
//===----------------------------------------------------------------------===//

#include "IR2Datapath.h"
#include "vtm/Passes.h"
#include "vtm/DesignMetrics.h"

#include "llvm/Pass.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Support/CFG.h"
#define DEBUG_TYPE "vtm-design-metrics"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace llvm {
// Wrapper for the external values.
class VASTValueOperand : public VASTValue {
public:
  const Value *const V;

  VASTValueOperand(const Value *V, unsigned Size)
    : VASTValue(VASTNode::vastCustomNode, Size),
      V(V) {}

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTValueOperand *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastCustomNode;
  }

  void printAsOperandImpl(raw_ostream &OS, unsigned UB, unsigned LB) const {
    OS << *V << '[' << UB << ',' << LB << ']';
  }
};

// FIXME: Move the class definition to a header file.
class DesignMetricsImpl : public EarlyDatapathBuilderContext {
  // Data-path container to hold the optimized data-path of the design.
  DatapathContainer DPContainer;
  EarlyDatapathBuilder Builder;

  // The data-path value which are used by control-path operations.
  typedef std::set<VASTValue*> ValSetTy;
  ValSetTy LiveOutedVal;

  ValSetTy AddressBusFanins, DataBusFanins;
  unsigned NumCalls;
  // TODO: Model the control-path, in the control-path, we can focus on the MUX
  // in the control-path, note that the effect of FU allocation&binding
  // algorithm should also be considered when estimating resource usage.

  // TODO: To not perform cycle-accurate speed performance estimation at the IR
  // layer, instead we should only care about the number of memory accesses.
  
  VASTImmediate *getOrCreateImmediate(uint64_t Value, int8_t BitWidth) {
    return DPContainer.getOrCreateImmediate(Value, BitWidth);
  }

  VASTValPtr createExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                        unsigned UB, unsigned LB) {
    return DPContainer.createExpr(Opc, Ops, UB, LB);;
  }

  VASTValPtr getAsOperand(Value *Op, bool GetAsInlineOperand);

  // Visit the expression tree whose root is Root and return the cost of the
  // tree, insert all visited data-path nodes into Visited.
  uint64_t getExprTreeFUCost(VASTValPtr Root, ValSetTy &Visited) const;
  uint64_t getFUCost(VASTValue *V) const;

  // Collect the fanin information of the memory bus.
  void visitLoadInst(LoadInst &I);
  void visitStoreInst(StoreInst &I);
public:
  explicit DesignMetricsImpl(TargetData *TD) : Builder(*this, TD), NumCalls(0){}

  void visit(Instruction &Inst);
  void visit(BasicBlock &BB);
  void visit(Function &F);

  void reset() {
    DPContainer.reset();
    LiveOutedVal.clear();
    AddressBusFanins.clear();
    DataBusFanins.clear();
    NumCalls = 0;
  }

  // Visit all data-path expression and compute the cost.
  uint64_t getDatapathFUCost() const;
  uint64_t getMemBusMuxCost() const;
  unsigned getNumCalls() const { return NumCalls; }
};

struct DesignMetricsPass : public FunctionPass {
  static char ID;

  DesignMetricsPass() : FunctionPass(ID) {}

  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<TargetData>();
    AU.setPreservesAll();
  }

  bool runOnFunction(Function &F);
};
}

VASTValPtr DesignMetricsImpl::getAsOperand(Value *Op, bool GetAsInlineOperand) {
  if (ConstantInt *Int = dyn_cast<ConstantInt>(Op)) {
    unsigned ConstantSize = Int->getType()->getScalarSizeInBits();
    // Do not create the immediate if it is too wide.
    if (ConstantSize <= 64)
      return getOrCreateImmediate(Int->getZExtValue(), ConstantSize);
  }

  if (VASTValPtr V = Builder.lookupExpr(Op)) {
    // Try to inline the operand if user ask to.
    if (GetAsInlineOperand) V = V.getAsInlineOperand();
    return V;
  }

  // Else we need to create a leaf node for the expression tree.
  VASTValueOperand *ValueOp
    = DPContainer.getAllocator()->Allocate<VASTValueOperand>();
    
  new (ValueOp) VASTValueOperand(Op, Builder.getValueSizeInBits(Op));

  // Remember the newly create VASTValueOperand, so that it will not be created
  // again.
  Builder.indexVASTExpr(Op, ValueOp);
  return ValueOp;
}

void DesignMetricsImpl::visitLoadInst(LoadInst &I) {
  Value *Address = I.getPointerOperand();
  if (VASTValPtr V = Builder.lookupExpr(Address))
    AddressBusFanins.insert(V.get());
}

void DesignMetricsImpl::visitStoreInst(StoreInst &I) {
  Value *Address = I.getPointerOperand();
  if (VASTValPtr V = Builder.lookupExpr(Address))
    AddressBusFanins.insert(V.get());

  Value *Data = I.getValueOperand();
  if (VASTValPtr V = Builder.lookupExpr(Data))
    DataBusFanins.insert(V.get());
}

void DesignMetricsImpl::visit(Instruction &Inst) {
  if (VASTValPtr V = Builder.visit(Inst)) {
    Builder.indexVASTExpr(&Inst, V);
    return;
  }

  // Else Inst is a control-path instruction, all its operand are live-outed.
  // A live-outed data-path expression and its children should never be
  // eliminated.
  typedef Instruction::op_iterator op_iterator;
  for (op_iterator I = Inst.op_begin(), E = Inst.op_end(); I != E; ++I) {
    if (VASTValPtr V = Builder.lookupExpr(*I))
      LiveOutedVal.insert(V.get());
  }

  if (LoadInst *LI = dyn_cast<LoadInst>(&Inst))
    visitLoadInst(*LI);
  else if (StoreInst *SI = dyn_cast<StoreInst>(&Inst))
    visitStoreInst(*SI);
  else if (isa<CallInst>(Inst))
    ++NumCalls;
}

void DesignMetricsImpl::visit(BasicBlock &BB) {
  typedef BasicBlock::iterator iterator;
  for (iterator I = BB.begin(), E = BB.end(); I != E; ++I) {
    // PHINodes will be handled in somewhere else.
    if (isa<PHINode>(I)) continue;

    visit(*I);
  }

  // Remember the incoming value from the current BB of the PHINodes in
  // successors as live-outed values.
  for (succ_iterator SI = succ_begin(&BB), SE = succ_end(&BB); SI != SE; ++SI) {
    BasicBlock *SuccBB = *SI;
    for (iterator I = SuccBB->begin(), E = SuccBB->end(); I != E; ++I) {
      PHINode *PN = dyn_cast<PHINode>(I);
      if (PN == 0) break;

      Value *LiveOutedFromBB = PN->DoPHITranslation(SuccBB, &BB);
      if (VASTValPtr V = Builder.lookupExpr(LiveOutedFromBB))
        LiveOutedVal.insert(V.get());
    }
  }

}

void DesignMetricsImpl::visit(Function &F) {
  ReversePostOrderTraversal<BasicBlock*> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator iterator;
  for (iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
    visit(**I);
}

uint64_t DesignMetricsImpl::getFUCost(VASTValue *V) const {
  VASTExpr *Expr = dyn_cast<VASTExpr>(V);
  // We can only estimate the cost of VASTExpr.
  if (!Expr) return 0;

  unsigned ValueSize = std::min(V->getBitWidth(), 64u);

  switch (Expr->getOpcode()) {
  default: break;

  case VASTExpr::dpAdd: return VFUs::AddCost[ValueSize];
  case VASTExpr::dpMul: return VFUs::MulCost[ValueSize];
  case VASTExpr::dpShl:
  case VASTExpr::dpSRA:
  case VASTExpr::dpSRL: return VFUs::ShiftCost[ValueSize];
  }

  return 0;
}

uint64_t DesignMetricsImpl::getExprTreeFUCost(VASTValPtr Root, ValSetTy &Visited)
                                          const {
  uint64_t Cost = 0;
  // FIXME: Provide a generic depth-first search iterator for VASTValue tree.
  typedef VASTValue::dp_dep_it ChildIt;
  std::vector<std::pair<VASTValue*, ChildIt> > WorkStack;

  WorkStack.push_back(std::make_pair(Root.get(),
                                     VASTValue::dp_dep_begin(Root.get())));

  Cost += getFUCost(Root.get());
  
  while (!WorkStack.empty()) {
    VASTValue *N;
    ChildIt It;
    tie(N, It) = WorkStack.back();

    // All children are visited.  
    if (It == VASTValue::dp_dep_end(N)) {
      WorkStack.pop_back();
      continue;
    }

    // Depth first traverse the child of current node.
    VASTValue *ChildNode = It->get().get();
    ++WorkStack.back().second;

    // Had we visited this node?
    if (!Visited.insert(ChildNode).second) continue;

    // Visit the node and compute its FU cost.
    Cost += getFUCost(ChildNode);

    WorkStack.push_back(std::make_pair(ChildNode,
                                       VASTValue::dp_dep_begin(ChildNode)));
  }

  return Cost;
}

uint64_t DesignMetricsImpl::getDatapathFUCost() const {
  uint64_t Cost = 0;
  ValSetTy Visited;

  typedef ValSetTy::const_iterator iterator;
  for (iterator I = LiveOutedVal.begin(), E = LiveOutedVal.end(); I != E; ++I)
    Cost += getExprTreeFUCost(*I, Visited);

  return Cost;
}

uint64_t DesignMetricsImpl::getMemBusMuxCost() const {
  VFUMemBus *Bus = getFUDesc<VFUMemBus>();
  return VFUs::getMuxCost(AddressBusFanins.size()) * Bus->getAddrWidth()
         + VFUs::getMuxCost(DataBusFanins.size()) * Bus->getDataWidth();
}

DesignMetrics::DesignMetrics(TargetData *TD)
  : Impl(new DesignMetricsImpl(TD)) {}

DesignMetrics::~DesignMetrics() { delete Impl; }

void DesignMetrics::visit(Instruction &Inst) {
  Impl->visit(Inst);
}

void DesignMetrics::visit(BasicBlock &BB) {
  Impl->visit(BB);
}

void DesignMetrics::visit(Function &F) {
  Impl->visit(F);
}

unsigned DesignMetrics::getNumCalls() const { return Impl->getNumCalls(); }

uint64_t DesignMetrics::getResourceCost() const {
  return Impl->getDatapathFUCost() + Impl->getMemBusMuxCost();
}

void DesignMetrics::reset() { Impl->reset(); }

char DesignMetricsPass::ID = 0;

bool DesignMetricsPass::runOnFunction(Function &F) {
  DesignMetrics Metrics(&getAnalysis<TargetData>());

  Metrics.visit(F);

  DEBUG(dbgs() << "Data-path cost of function " << F.getName() << ':'
               << Metrics.getResourceCost() << '\n');
  
  return false;
}

FunctionPass *llvm::createDesignMetricsPass() {
  return new DesignMetricsPass();
}
