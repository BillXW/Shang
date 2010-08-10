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
class MemDepInfo;

/// @brief Hardware atom construction pass
///
class HWAtomInfo : public FunctionPass, public InstVisitor<HWAtomInfo, HWAtom*> {

  /// @name InstVisitor interface
  //{
  friend class InstVisitor<HWAtomInfo, HWAtom*>;
  HWAtom *visitTerminatorInst(TerminatorInst &I);

  HWAtom *visitPHINode(PHINode &I);
  HWAtom *visitBinaryOperator(Instruction &I);
  HWAtom *visitICmpInst(ICmpInst &I);
  HWAtom *visitFCmpInst(FCmpInst &I){
    llvm_unreachable("Instruction not support yet!");
    return 0;
  }

  HWAtom *visitCastInst (CastInst &I);
  HWAtom *visitSelectInst(SelectInst &I);
  HWAtom *visitCallInst (CallInst &I){
    llvm_unreachable("Instruction not support yet!");
    return 0;
  }
  HWAtom *visitInlineAsm(CallInst &I){
    llvm_unreachable("Instruction not support yet!");
    return 0;
  }
  bool visitBuiltinCall(CallInst &I, Intrinsic::ID ID, bool &WroteCallee) {
    llvm_unreachable("Instruction not support yet!");
    return 0;
  }

  HWAtom *visitAllocaInst(AllocaInst &I) {
    llvm_unreachable("Instruction not support yet!");
    return 0;
  }
  HWAtom *visitLoadInst  (LoadInst   &I);
  HWAtom *visitStoreInst (StoreInst  &I);
  HWAtom *visitGetElementPtrInst(GetElementPtrInst &I);
  HWAtom *visitVAArgInst (VAArgInst &I){
    llvm_unreachable("Instruction not support yet!");
    return 0;
  }

  HWAtom *visitInsertElementInst(InsertElementInst &I){
    llvm_unreachable("Instruction not support yet!");
    return 0;
  }
  HWAtom *visitExtractElementInst(ExtractElementInst &I){
    llvm_unreachable("Instruction not support yet!");
    return 0;
  }
  HWAtom *visitShuffleVectorInst(ShuffleVectorInst &SVI){
    llvm_unreachable("Instruction not support yet!");
    return 0;
  }

  HWAtom *visitInsertValueInst(InsertValueInst &I){
    llvm_unreachable("Instruction not support yet!");
    return 0;
  }
  HWAtom *visitExtractValueInst(ExtractValueInst &I){
    llvm_unreachable("Instruction not support yet!");
    return 0;
  }

  HWAtom *visitInstruction(Instruction &I) {
#ifndef NDEBUG
    errs() << "HWAtomInfo does not know about " << I;
#endif
    llvm_unreachable(0);
    return 0;
  }
  //}

  // Allocator
  BumpPtrAllocator HWAtomAllocator;

  // 
  FoldingSet<HWAtom> UniqiueHWAtoms;

  HWAPreBind *getPreBind(Instruction &I, SmallVectorImpl<HWEdge*> &Deps,
                         size_t OpNum, HWFUnit FUID);
  HWAPreBind *getPreBind(Instruction &I, SmallVectorImpl<HWEdge*> &Deps,
                         HWFUnit FUID) {
    return getPreBind(I, Deps, I.getNumOperands(), FUID);
  }

  HWAPostBind *getPostBind(Instruction &I, SmallVectorImpl<HWEdge*> &Deps,
                           size_t OpNum, HWFUnit FUID);

  HWAPostBind *getPostBind(Instruction &I, SmallVectorImpl<HWEdge*> &Deps,
                           HWFUnit FUID) {
    return getPostBind(I, Deps, I.getNumOperands(), FUID);
  }

  FSMState *getState(BasicBlock *BB);

  typedef DenseMap<const Value*, HWAtom*> AtomMapType;
  AtomMapType ValueToHWAtoms;

  HWAtom *ControlRoot;

  void SetControlRoot(HWAtom *NewRoot) {
    ControlRoot = NewRoot;
  }

  HWAtom *getControlRoot() {
    return ControlRoot;
  }

  HWValDep *getValDepEdge(HWAtom *Src, bool isSigned = false,
                          bool isImport = false) {
    return new (HWAtomAllocator) HWValDep(Src, isSigned, isImport);
  }

  HWConst *getConstEdge(HWAtom *Src, Constant *C) {
    return new (HWAtomAllocator) HWConst(Src, C);
  }

  HWCtrlDep *getCtrlDepEdge(HWAtom *Src, bool isExport = false) {
    return new (HWAtomAllocator) HWCtrlDep(Src, isExport);
  }

  HWMemDep *getMemDepEdge(HWAtom *Src, bool isBackEdge,
                          enum HWMemDep::MemDepTypes DepType,
                          unsigned Diff);

  HWEdge *getValDepInState(Value &V, BasicBlock *BB, bool isSigned = false) {
    // Is this not a instruction?
    if (isa<Argument>(V))
      return getValDepEdge(getStateFor(*BB), isSigned, true);
    else if (isa<PHINode>(V)) // PHINode is an import edge.
      return getValDepEdge(getStateFor(*BB), isSigned, true);
    else if (Constant *C = dyn_cast<Constant>(&V))
      return getConstEdge(getStateFor(*BB), C);

    // Now it is an Instruction
    Instruction &Inst = cast<Instruction>(V);
    if (BB == Inst.getParent())
      // Create a wire dep for atoms in the same state.
      return getValDepEdge(getAtomFor(Inst), isSigned);
    else
      // Otherwise this edge is an import edge.
      return getValDepEdge(getStateFor(*BB), isSigned, true);
  }

  void addOperandDeps(Instruction &I, SmallVectorImpl<HWEdge*> &Deps) {
    BasicBlock *ParentBB = I.getParent();
    for (ReturnInst::op_iterator OI = I.op_begin(), OE = I.op_end();
        OI != OE; ++OI)
      if(!isa<BasicBlock>(OI)) // Ignore the basic Block.
        Deps.push_back(getValDepInState(**OI, ParentBB));
  }

  void addPhiDelays(BasicBlock &BB, SmallVectorImpl<HWEdge*> &Deps);

  void clear();

  unsigned NumRegs;
  // Mapping Value to registers
  std::map<const Value*, HWRegister*> RegForValues;

  void addMemDepEdges(std::vector<HWAOpInst*> &MemOps, BasicBlock &BB);

  bool haveSelfLoop(BasicBlock *BB);
  void addLoopPredBackEdge(BasicBlock *BB);

  // The loop Info
  LoopInfo *LI;
  ResourceConfig *RC;
  // Total states
  unsigned short totalCycle;
  unsigned short InstIdx;
  // Analysis for dependence analyzes.
  MemDepInfo *MDA;
public:
  /// @name FunctionPass interface
  //{
  static char ID;
  HWAtomInfo() : FunctionPass(&ID), ControlRoot(0), LI(0), totalCycle(1),
    InstIdx(0), NumRegs(1), MDA(0) {}

  bool runOnFunction(Function &F);
  void releaseMemory();
  void getAnalysisUsage(AnalysisUsage &AU) const;
  void print(raw_ostream &O, const Module *M) const;
  //}

  HWAPreBind *bindToResource(HWAPostBind &PostBind, unsigned Instance);

  HWRegister *getRegForValue(const Value *V, unsigned StartSlot, unsigned EndSlot) {
    std::map<const Value*, HWRegister*>::iterator at = RegForValues.find(V);
    HWRegister *R = 0;
    if (at == RegForValues.end()) {
      R = allocaRegister(V->getType(), StartSlot, EndSlot);
      RegForValues.insert(std::make_pair(V, R));
    } else
      R = at->second;
    
    // TODO: Extend the life time of the register if necessary.

    return R;
  }

  HWRegister *lookupRegForValue(const Value *V) {
    std::map<const Value*, HWRegister*>::iterator at = RegForValues.find(V);
    assert(at != RegForValues.end() && "Register not found!");
    return at->second;
  }

  HWRegister *allocaFURegister(HWAPreBind *A) {
    int RegNum = A->getFunUnitID().getRawData();
    unsigned Slot = A->getFinSlot();
    return new (HWAtomAllocator) HWRegister(-RegNum, A->getValue().getType(),
                                       Slot, Slot);
  }

  HWRegister *allocaRegister(const Type *Ty,
                                  unsigned StartSlot, unsigned EndSlot) {
    return new (HWAtomAllocator) HWRegister(++NumRegs, Ty,
                                                 StartSlot, EndSlot);
  }

  HWAtom *getAtomFor(Value &V) const {
    AtomMapType::const_iterator At = ValueToHWAtoms.find(&V);
    assert(At != ValueToHWAtoms.end() && "Atom can not be found!");
    return  At->second;    
  }

  FSMState *getStateFor(BasicBlock &BB) const {
    return cast<FSMState>(getAtomFor(BB));
  }

  HWADelay *getDelay(HWAtom *Src, unsigned Delay);
  HWAWrReg *getWrReg(HWAtom *Src, HWRegister *Reg);
  HWARdReg *getRdReg(HWAtom *Src, HWRegister *Reg, Value &V);

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
