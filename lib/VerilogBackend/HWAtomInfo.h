//===------------- HWAtom.h - Translate LLVM IR to HWAtom  -------*- C++ -*-===//
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
// This file also define the HWAtomInfo class, which construction the HWAtom
// represent from LLVM IR.
//
//===----------------------------------------------------------------------===//
//

#ifndef VBE_HARDWARE_ATOMINFO_H
#define VBE_HARDWARE_ATOMINFO_H

#include "llvm/Support/MathExtras.h"

#include "vbe/HWAtom.h"

namespace llvm {
  class LoopInfo;
}
using namespace llvm;

namespace esyn {
/// @brief Hardware atom construction pass
///
class HWAtomInfo : public FunctionPass, public InstVisitor<HWAtomInfo> {

  /// @name InstVisitor interface
  //{
  friend class InstVisitor<HWAtomInfo>;
  void visitTerminatorInst(TerminatorInst &I);

  void visitPHINode(PHINode &I);
  void visitBinaryOperator(Instruction &I);
  void visitICmpInst(ICmpInst &I);
  void visitFCmpInst(FCmpInst &I){
    llvm_unreachable("Instruction not support yet!");
  }

  void visitCastInst (CastInst &I);
  void visitSelectInst(SelectInst &I);
  void visitCallInst (CallInst &I){
    llvm_unreachable("Instruction not support yet!");}
  void visitInlineAsm(CallInst &I){
    llvm_unreachable("Instruction not support yet!");
  }
  bool visitBuiltinCall(CallInst &I, Intrinsic::ID ID, bool &WroteCallee) {
    llvm_unreachable("Instruction not support yet!");
  }

  void visitAllocaInst(AllocaInst &I) {
    llvm_unreachable("Instruction not support yet!");
  }
  void visitLoadInst  (LoadInst   &I);
  void visitStoreInst (StoreInst  &I);
  void visitGetElementPtrInst(GetElementPtrInst &I);
  void visitVAArgInst (VAArgInst &I){
    llvm_unreachable("Instruction not support yet!");
  }

  void visitInsertElementInst(InsertElementInst &I){
    llvm_unreachable("Instruction not support yet!");
  }
  void visitExtractElementInst(ExtractElementInst &I){
    llvm_unreachable("Instruction not support yet!");
  }
  void visitShuffleVectorInst(ShuffleVectorInst &SVI){
    llvm_unreachable("Instruction not support yet!");
  }

  void visitInsertValueInst(InsertValueInst &I){
    llvm_unreachable("Instruction not support yet!");
  }
  void visitExtractValueInst(ExtractValueInst &I){
    llvm_unreachable("Instruction not support yet!");
  }

  void visitInstruction(Instruction &I) {
#ifndef NDEBUG
    errs() << "HWAtomInfo does not know about " << I;
#endif
    llvm_unreachable(0);
  }
  //}

  // Allocator
  BumpPtrAllocator HWAtomAllocator;

  // 
  FoldingSet<HWAtom> UniqiueHWAtoms;

  HWAPreBind *getPreBind(Instruction &I, SmallVectorImpl<HWEdge*> &Deps,
                     size_t OpNum, enum HWResource::ResTypes OpClass,
                     unsigned latency, unsigned ResInst = 0);
  HWAPreBind *getPreBind(Instruction &I, SmallVectorImpl<HWEdge*> &Deps,
                     enum HWResource::ResTypes OpClass,  unsigned latency, 
                     unsigned ResInst = 0) {
    return getPreBind(I, Deps, I.getNumOperands(), OpClass, latency, ResInst);
  }

  HWAPostBind *getPostBind(Instruction &I, SmallVectorImpl<HWEdge*> &Deps,
    size_t OpNum, unsigned latency, enum HWResource::ResTypes OpClass);

  HWAPostBind *getPostBind(Instruction &I, SmallVectorImpl<HWEdge*> &Deps,
                       unsigned latency, enum HWResource::ResTypes OpClass) {
    return getPostBind(I, Deps, I.getNumOperands(), latency, OpClass);
  }

  HWAVRoot *getEntryRoot(BasicBlock *BB);

  typedef DenseMap<const Value*, HWAtom*> AtomMapType;
  AtomMapType ValueToHWAtoms;

  typedef DenseMap<const BasicBlock*, FSMState*> StateMapType;
  StateMapType BBToStates;
  HWAtom *ControlRoot;

  void SetControlRoot(HWAtom *NewRoot) {
    ControlRoot = NewRoot;
  }

  HWAtom *getControlRoot() {
    return ControlRoot;
  }

  HWValDep *getValDepEdge(HWAtom *Src, HWReg *R, bool isSigned = 0) {
    return new (HWAtomAllocator) HWValDep(Src, isSigned, R);
  }

  HWValDep *getValDepEdge(HWAtom *Src, Constant *C, bool isSigned = 0) {
    return new (HWAtomAllocator) HWValDep(Src, isSigned, C);
  }

  HWCtrlDep *getCtrlDepEdge(HWAtom *Src) {
    return new (HWAtomAllocator) HWCtrlDep(Src);
  }

  HWValDep *getValDepInState(Value &V, BasicBlock *BB, bool isSigned = false) {
    // Is this not a instruction?
    if (isa<Argument>(V) || isa<PHINode>(V))
      return getValDepEdge(getEntryRoot(BB), getRegNumForLiveVal(V), isSigned);
    else if (Constant *C = dyn_cast<Constant>(&V))
      return getValDepEdge(getEntryRoot(BB), C, isSigned);

    // Now it is an Instruction
    Instruction &Inst = cast<Instruction>(V);
    if (BB == Inst.getParent())
      // Create a wire dep for atoms in the same state.
      return getValDepEdge(getAtomFor(Inst), (HWReg*)0, isSigned);
    else
      // Otherwise we need a register.
      return getValDepEdge(getEntryRoot(BB), getRegNumForLiveVal(V), isSigned);
  }
  void addOperandDeps(Instruction &I, SmallVectorImpl<HWEdge*> &Deps) {
    BasicBlock *ParentBB = I.getParent();
    for (ReturnInst::op_iterator OI = I.op_begin(), OE = I.op_end();
        OI != OE; ++OI)
      if(!isa<BasicBlock>(OI)) // Ignore the basic Block.
        Deps.push_back(getValDepInState(**OI, ParentBB));
  }

  void clear();

  unsigned NumRegs;
  // Mapping Phi node to registers
  std::map<Value*, HWReg*> LiveValueReg;

  void setRegNum(Value &V, HWReg *R) {
    R->addValue(V);
    LiveValueReg.insert(std::make_pair(&V, R));
  }

  // The loop Info
  LoopInfo *LI;
  ResourceConfig *RC;
  // Total states
  unsigned totalCycle;

public:

  /// @name FunctionPass interface
  //{
  static char ID;
  HWAtomInfo() : FunctionPass(&ID), ControlRoot(0), LI(0),
    totalCycle(1), NumRegs(1) {}

  bool runOnFunction(Function &F);
  void releaseMemory();
  void getAnalysisUsage(AnalysisUsage &AU) const;
  virtual void print(raw_ostream &O, const Module *M) const;
  //}

  HWAPreBind *bindToResource(HWAPostBind &PostBind, unsigned Instance);

  FSMState &getStateFor(BasicBlock &BB) const {
    StateMapType::const_iterator At = BBToStates.find(&BB);
    assert(At != BBToStates.end() && "Can not get the State!");
    return  *(At->second);
  }

  HWReg *getRegNumForLiveVal(Value &V) {
    if (isa<Constant>(V))
      return 0;

    std::map<Value*, HWReg*>::iterator at = LiveValueReg.find(&V);
    if (at != LiveValueReg.end())
      return at->second;

    HWReg *R = new (HWAtomAllocator) HWReg(++NumRegs, V);

    LiveValueReg.insert(std::make_pair(&V, R));
    return R;
  }

  HWADrvReg *getDrvReg(HWAtom *Src, HWReg *Reg);

  HWAtom *getAtomFor(Value &V) const {
    AtomMapType::const_iterator At = ValueToHWAtoms.find(&V);
    assert(At != ValueToHWAtoms.end() && "Atom can not be found!");
    return  At->second;    
  }

  void updateAtomMap(Value &V, HWAtom *A) {
    ValueToHWAtoms[&V] = A;
  }

  unsigned getTotalCycle() const {
    return totalCycle;
  }


  void setTotalCycle(unsigned Cyc) {
    totalCycle = Cyc;
  }

  unsigned getTotalCycleBitWidth() const {
    return Log2_32_Ceil(totalCycle);
  }

  void incTotalCycle() { ++totalCycle; }
};
} // end namespace
#endif
