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
#include "llvm/Support/raw_ostream.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Target/Mangler.h"

#include "vbe/utils.h"
#include "vbe/ResourceConfig.h"
#include "vbe/HWAtom.h"

#include "VLang.h"

using namespace llvm;


namespace esyn {
class HWAtomInfo;

class RTLWriter : public FunctionPass {
  raw_ostream &Out;
  TargetData *TD;
  VLang *vlang;
  HWAtomInfo *HI;

  // Buffers
  raw_string_ostream  ModDecl, StateDecl, SignalDecl, DataPath,
    ControlBlock, ResetBlock, SeqCompute;

  // Mapping used resouces to the using atoms
  typedef std::vector<HWAOpRes*> HWAOpResVecTy;
  typedef std::map<HWResource::ResIdType,HWAOpResVecTy> ResourceMapType;

  ResourceMapType ResourceMap;

  void emitFunctionSignature(Function &F);
  void emitCommonPort();
  void emitBasicBlock(BasicBlock &BB);

  // Resource
  void emitResources();
  void emitMemBus(HWMemBus &MemBus, HWAOpResVecTy &Atoms);
  void opMemBus(HWAOpRes *OpRes);

  // Atoms
  void emitAtom(HWAtom *A);
  void emitRegister(HWARegister *Register);
  void emitOpRes(HWAOpRes *OpRes);
  void emitSigned(HWASigned *Signed);
  void emitOpInst(HWAOpInst *OpRes);
  // Helper function for state end
  // Copy incoming value for Phi node.
  void emitPHICopiesForSucc(BasicBlock &CurBlock, BasicBlock &Succ,
                            unsigned ind = 0);

  void clear();
  
  std::string getAsOperand(Value *V, const std::string &postfix = "");
  std::string getAsOperand(HWAtom *A);


  raw_ostream &getStateDeclBuffer() {
    return StateDecl.indent(2);
  }

  raw_ostream &getModDeclBuffer() {
    return ModDecl.indent(4);
  }

  raw_ostream &getSignalDeclBuffer() {
    return SignalDecl.indent(2);
  }

  raw_ostream &getResetBlockBuffer() {
    return ResetBlock.indent(6);
  }

  void emitNextState(raw_ostream &ss, BasicBlock &BB, unsigned offset = 0);

  /// @name InstVisitor interface
  //{
  void visitReturnInst(HWAOpInst &A);
  void visitBranchInst(HWAOpInst &A);
  void visitSwitchInst(HWAOpInst &A){}
  void visitIndirectBrInst(HWAOpInst &A){}
  void visitInvokeInst(HWAOpInst &A) {
    llvm_unreachable("Lowerinvoke pass didn't work!");
  }

  void visitUnwindInst(HWAOpInst &A) {
    llvm_unreachable("Lowerinvoke pass didn't work!");
  }
  void visitUnreachableInst(HWAOpInst &A){}

  void visitPHINode(HWAOpInst &A);
  void visitBinaryOperator(HWAOpInst &A);
  void visitICmpInst(HWAOpInst &A);
  void visitFCmpInst(HWAOpInst &A){}

  void visitTruncInst(HWAOpInst &A);

  void visitExtInst (HWAOpInst &A);
  void visitZExtInst(HWAOpInst &A)      { visitExtInst(A); }
  void visitSExtInst(HWAOpInst &A)      { visitExtInst(A); }

  //
  void visitFPTruncInst(HWAOpInst &A)   { }
  void visitFPExtInst(HWAOpInst &A)     { }
  void visitFPToUIInst(HWAOpInst &A)    { }
  void visitFPToSIInst(HWAOpInst &A)    { }
  void visitUIToFPInst(HWAOpInst &A)    { }
  void visitSIToFPInst(HWAOpInst &A)    { }
  void visitPtrToIntInst(HWAOpInst &A)  { }
  void visitIntToPtrInst(HWAOpInst &A)  { }
  void visitBitCastInst(HWAOpInst &A)   { }

  void visitSelectInst(HWAOpInst &A){}
  void visitCallInst (HWAOpInst &A){}
  void visitInlineAsm(HWAOpInst &A){}
  bool visitBuiltinCall(CallInst &I, Intrinsic::ID ID, bool &WroteCallee){}

  void visitAllocaInst(HWAOpInst &A) {}
  void visitLoadInst  (HWAOpInst &A){}
  void visitStoreInst (HWAOpInst &A){}
  void visitGetElementPtrInst(HWAOpInst &A);
  void visitVAArgInst (HWAOpInst &A){}

  void visitInsertElementInst(HWAOpInst &A){}
  void visitExtractElementInst(HWAOpInst &A){}
  void visitShuffleVectorInst(HWAOpInst &A){}

  void visitInsertValueInst(HWAOpInst &A){}
  void visitExtractValueInst(HWAOpInst &A){}

  void visitInstruction(HWAOpInst &A) {
    llvm_unreachable("Unknown instruction!");
  }


#define HANDLE_INST(NUM, OPCODE, CLASS) \
  void visit##OPCODE(HWAOpInst &A) { visit##CLASS(A); }
#include "llvm/Instruction.def"

  void visit(HWAOpInst &A) {
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
  explicit RTLWriter(raw_ostream &O)
    : FunctionPass(&ID), Out(O), TD(0), vlang(0), HI(0),
    ModDecl(*(new std::string())),
    StateDecl(*(new std::string())),
    SignalDecl(*(new std::string())),
    DataPath(*(new std::string())),
    ControlBlock(*(new std::string())),
    ResetBlock(*(new std::string())),
    SeqCompute(*(new std::string())){
  }
  ~RTLWriter();

  bool runOnFunction(Function &F);
  void releaseMemory() { clear(); }
  void getAnalysisUsage(AnalysisUsage &AU) const;
  virtual void print(raw_ostream &O, const Module *M) const;
  //}
};

} //end of namespace
#endif // h guard
