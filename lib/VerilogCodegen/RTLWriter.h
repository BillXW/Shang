//===------------- RTLWriter.h - HWAtom to RTL verilog  ---------*- C++ -*-===//
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
// This file define the RTLWriter pass, which write out HWAtom into RTL
// verilog form.
//
//===----------------------------------------------------------------------===//
#ifndef VBE_RTL_WRITER_H
#define VBE_RTL_WRITER_H

#include "HWAtom.h"
#include "VLang.h"
#include "vbe/ResourceConfig.h"

#include "llvm/Pass.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/Constants.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Module.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Target/Mangler.h"

using namespace llvm;


namespace esyn {
class HWAtomInfo;

class RTLWriter : public FunctionPass {
  raw_ostream &Out;
  TargetData *TD;
  VLang *vlang;
  HWAtomInfo *HI;
  ResourceConfig *RC;

  VModule *VM;

  unsigned TotalFSMStatesBit, CurFSMStateNum;

  // Mapping used resouces to the using atoms
  typedef std::vector<HWAOpFU*> HWAPreBindVecTy;
  typedef std::map<HWFUnit*,HWAPreBindVecTy> ResourceMapType;

  ResourceMapType ResourceMap;

  void emitFunctionSignature(Function &F);
  void emitCommonPort();
  void emitBasicBlock(BasicBlock &BB);

  // Resource
  void emitResources();

  template<class ResType>
  void emitResource(HWAPreBindVecTy &Atoms);

  template<class ResType>
  void emitResourceDecl(HWFUnit *FU);
  void emitResourceDeclForBinOpRes(HWFUnit *FU,
                                   const std::string &OpPrefix,
                                   const std::string &Operator);

  template<class ResType>
  void emitResourceOp(HWAOpFU *A);
  void emitResourceOpForBinOpRes(HWAOpFU *A, const std::string &OpPrefix);

  template<class ResType>
  void emitResourceDefaultOp(HWFUnit *FU);
  void emitResourceDefaultOpForBinOpRes(HWFUnit *FU, const std::string &OpPrefix);

  // Atoms
  void emitAtom(HWAtom *A);
  void emitOpFU(HWAOpFU *OF);
  void emitWrReg(HWAWrReg *DR);
  void emitLIReg(HWALIReg *DR);

  std::map<unsigned, const HWRegister*> UsedRegs;

  void emitAllRegisters();

  void clear();
  
  std::string getAsOperand(Value *V, const std::string &postfix = "");
  std::string getAsOperand(HWEdge &E);
  std::string getAsOperand(HWAtom *A);
  std::string getAsOperand(const HWRegister *R);
  static std::string getRegPrefix(HWResType::Types T);

  // Create the enable registers for mirco states.
  std::string getMircoStateEnableName(FSMState *State, bool InFSMBlock);
  std::string getMircoStateEnable(FSMState *State, unsigned Slot, bool InFSMBlock);
  void createMircoStateEnable(FSMState *State);
  void emitNextFSMState(raw_ostream &ss, BasicBlock &BB);
  void emitNextMicroState(raw_ostream &ss, BasicBlock &BB,
                          const std::string &NewState);
  std::string computeSelfLoopEnable(FSMState *State);

  /// @name InstVisitor interface
  //{
  void visitReturnInst(HWAOpFU &A);
  void visitBranchInst(HWAOpFU &A);
  void visitSwitchInst(HWAOpFU &A){}
  void visitIndirectBrInst(HWAOpFU &A){}
  void visitInvokeInst(HWAOpFU &A) {
    llvm_unreachable("Lowerinvoke pass didn't work!");
  }

  void visitUnwindInst(HWAOpFU &A) {
    llvm_unreachable("Lowerinvoke pass didn't work!");
  }
  void visitUnreachableInst(HWAOpFU &A){}

  void visitPHINode(HWAOpFU &A) {}

  void visitBinaryOperator(HWAOpFU &A);
  void visitICmpInst(HWAOpFU &A);
  void visitFCmpInst(HWAOpFU &A){}

  void visitTruncInst(HWAOpFU &A);

  void visitExtInst (HWAOpFU &A);
  void visitZExtInst(HWAOpFU &A)      { visitExtInst(A); }
  void visitSExtInst(HWAOpFU &A)      { visitExtInst(A); }

  //
  void visitFPTruncInst(HWAOpFU &A)   { }
  void visitFPExtInst(HWAOpFU &A)     { }
  void visitFPToUIInst(HWAOpFU &A)    { }
  void visitFPToSIInst(HWAOpFU &A)    { }
  void visitUIToFPInst(HWAOpFU &A)    { }
  void visitSIToFPInst(HWAOpFU &A)    { }

  void visitIntCastInst(HWAOpFU &A);
  void visitPtrToIntInst(HWAOpFU &A)  { visitIntCastInst(A); }
  void visitIntToPtrInst(HWAOpFU &A)  { visitIntCastInst(A); }
  void visitBitCastInst(HWAOpFU &A)   { visitIntCastInst(A); }

  void visitSelectInst(HWAOpFU &A);
  void visitCallInst (HWAOpFU &A){}
  void visitInlineAsm(HWAOpFU &A){}
  bool visitBuiltinCall(CallInst &I, Intrinsic::ID ID, bool &WroteCallee) {
    return false;
  }

  void visitAllocaInst(HWAOpFU &A) {}
  void visitLoadInst  (HWAOpFU &A){}
  void visitStoreInst (HWAOpFU &A){}
  void visitGetElementPtrInst(HWAOpFU &A);
  void visitVAArgInst (HWAOpFU &A){}

  void visitInsertElementInst(HWAOpFU &A){}
  void visitExtractElementInst(HWAOpFU &A){}
  void visitShuffleVectorInst(HWAOpFU &A){}

  void visitInsertValueInst(HWAOpFU &A){}
  void visitExtractValueInst(HWAOpFU &A){}

  void visitInstruction(HWAOpFU &A) {
    llvm_unreachable("Unknown instruction!");
  }


#define HANDLE_INST(NUM, OPCODE, CLASS) \
  void visit##OPCODE(HWAOpFU &A) { visit##CLASS(A); }
#include "llvm/Instruction.def"

  void visit(HWAOpFU &A) {
    switch (A.getOpcode()) {
    default: llvm_unreachable("Unknown instruction type encountered!");
      // Build the switch statement using the Instruction.def file...
#define HANDLE_INST(NUM, OPCODE, CLASS) \
    case Instruction::OPCODE: return visit##OPCODE(A);
#include "llvm/Instruction.def"
    }
  }
  //}

public:
  /// @name FunctionPass interface
  //{
  static char ID;
  explicit RTLWriter(raw_ostream &O = nulls())
    : FunctionPass(ID), Out(O), TD(0), vlang(0), HI(0), RC(0), VM(0),
    TotalFSMStatesBit(0), CurFSMStateNum(0) {
  }
  ~RTLWriter();

  VModule *getVerilogModule() const { return VM; }

  bool runOnFunction(Function &F);
  void releaseMemory() { clear(); }
  void getAnalysisUsage(AnalysisUsage &AU) const;
  virtual void print(raw_ostream &O, const Module *M) const;
  //}
};

} //end of namespace
#endif // h guard
