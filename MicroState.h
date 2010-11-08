//===----- BundleTokens.h - Tokens for operation in a FSM state  -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//
//
//===----------------------------------------------------------------------===//


#ifndef BUNDLE_TOKENS_H
#define BUNDLE_TOKENS_H

#include "VTM.h"
#include "VTMConfig.h"

#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {

class BundleToken {
protected:
  const MDNode *TokenNode;

  StringRef getStringField(unsigned Elt) const;
  unsigned getUnsignedField(unsigned Elt) const {
    return (unsigned)getUInt64Field(Elt);
  }
  uint64_t getUInt64Field(unsigned Elt) const;

public:
  enum TokenType {
    // Define a wire, defwire wire_num, source_value_token
    tokenDefWire,
    // Read a wire, readwire wire_num
    tokenReadWire,
    // Excute an instruction, instr ResourceType, ResourceID, operands ...
    tokenInstr,
    // Write register, WriteReg register, source value
    tokenWriteReg = VTM::INSTRUCTION_LIST_END
  };

  explicit BundleToken() : TokenNode(0) {}
  explicit BundleToken(const MDNode *N) : TokenNode(N) {}

  bool Verify() const { return TokenNode != 0; }

  operator MDNode *() const { return const_cast<MDNode*>(TokenNode); }
  MDNode *operator ->() const { return const_cast<MDNode*>(TokenNode); }

  unsigned getTag() const {  return getUnsignedField(0); }
  unsigned getId() const { return getUnsignedField(1); }

  bool isDefWire() const;
  bool isReadWire() const;
  bool isInstr() const;
  bool isDefReg() const;

  uint64_t getWireNum() const {
    assert((isDefWire() || isReadWire() || isDefReg()) && "Bad token type!");
    return getUInt64Field(2);
  }
  uint64_t getBitWidth() const {
    assert(isDefWire() && "Bad token type!");
    return getUInt64Field(3);
  }

  VInstrInfo::FUTypes getResType() const {
    assert(isInstr() && "Bad token type!");
    return (VInstrInfo::FUTypes)getUInt64Field(1);
  }

  unsigned getOpcode() const {
    assert(isInstr() && "Bad token type!");
    return getUInt64Field(3);
  }

  void print(raw_ostream &OS) const;
  void dump() const;
};

class  ucOpIterator;

class ucOp {
public:
  typedef MachineInstr::mop_iterator op_iterator;
private:
  BundleToken Token;
  // iterator op begin and op end.
  op_iterator rangeBegin, rangeEnd;

  // op begin and op end
  ucOp(op_iterator range_begin, op_iterator range_end)
    : Token((*range_begin).getMetadata()), 
    rangeBegin(range_begin + 1), rangeEnd(range_end) {
      assert((Token.isInstr() || Token.isDefReg()) && "Bad leading token!");
  }
  
  friend class ucOpIterator;
public:
  op_iterator op_begin() const { return rangeBegin; }
  op_iterator op_end() const { return rangeEnd; }

  MachineOperand &getOperand(unsigned i) const {
    op_iterator I = op_begin() + i;
    assert(I < rangeEnd && "index out of range!");
    return *I;
  }

  inline unsigned getOpCode() const {
    if (Token.isInstr())
      return Token.getOpcode();
  }

  bool haveDataPath() const;

  void print(raw_ostream &OS) const;
  void dump() const;
};

class ucOpIterator : public std::iterator<std::forward_iterator_tag,
                                             ucOp, ptrdiff_t> {
  MachineInstr::mop_iterator CurIt, EndIt;

  MachineInstr::mop_iterator getNextIt() const;

  /// Create the begin iterator from a machine instruction.
  inline ucOpIterator(MachineInstr &MI)
    : CurIt(MI.operands_begin() + 1), EndIt(MI.operands_end()){
    assert(MI.getOperand(0).isImm() && "Bad bundle!");
  }

  /// Create the begin iterator from a machine instruction.
  inline ucOpIterator(MachineInstr &MI, bool) : CurIt(MI.operands_end()),
    EndIt(MI.operands_end()) {
    assert(MI.getOperand(0).isImm() && "Bad bundle!");
  }

public:
  inline bool operator==(const ucOpIterator& x) const {
    return CurIt == x.CurIt;
  }

  inline bool operator!=(const ucOpIterator& x) const {
    return !operator==(x);
  }
  
  inline ucOp operator*() const {
    assert(CurIt != EndIt && "Iterator out of range!");
    return ucOp(CurIt, getNextIt());
  }

  //inline ucOp *operator->() const {
  //  return &operator *();
  //}

  inline ucOpIterator& operator++() {
    assert(CurIt != EndIt && "Can not increase!");
    CurIt = getNextIt();
    return *this;
  }

  inline ucOpIterator operator++(int) {
    ucOpIterator tmp = *this;
    ++*this;
    return tmp;
  }

  inline const ucOpIterator &operator=(const ucOpIterator &I) {
    CurIt = I.CurIt;
    EndIt = I.EndIt;
    return *this;
  }

  static inline ucOpIterator begin(MachineInstr &MI) {
    return ucOpIterator(MI);
  }

  static inline ucOpIterator end(MachineInstr &MI) {
    return ucOpIterator(MI, false);
  }
};


}

#endif
