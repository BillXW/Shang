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
// This file define the HWAtom class, which represent the basic atom operation
// in hardware.
//
//===----------------------------------------------------------------------===//
//

#ifndef VBE_HARDWARE_ATOM_H
#define VBE_HARDWARE_ATOM_H

#include "llvm/Pass.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/Intrinsics.h"
#include "llvm/Assembly/Writer.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/InstVisitor.h"
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Support/ErrorHandling.h"

#include "vbe/ResourceConfig.h"

#include <list>

using namespace llvm;

namespace esyn {

enum HWAtomTypes {
  atomDrvReg,
  atomPreBind,      // Operate on pre bind resource
  atomPostBind,     // Operate on post binding resource
  atomVRoot       // Virtual Root
};

enum HWEdgeTypes {
  edgeValDep,          // The constant Atom
  edgeMemDep,
  edgeCtrlDep,
};

class HWAtom;
class HWAOpInst;

/// @brief Inline operation
class HWEdge {
  const unsigned short EdgeType;
  HWAtom *Src;
  // Iterate distance.
  unsigned ItDst;

  HWEdge(const HWEdge &);            // DO NOT IMPLEMENT
  void operator=(const HWEdge &);  // DO NOT IMPLEMENT

protected:
  HWEdge(enum HWEdgeTypes T, HWAtom *src,
    unsigned Dst) : EdgeType(T), Src(src), ItDst(Dst) {}
public:
  unsigned getEdgeType() const { return EdgeType; }
  // The referenced value.
  HWAtom *getDagSrc() const { return Src; }
  void setDagSrc(HWAtom *NewSrc) { Src = NewSrc; }

  unsigned getItDst() const { return ItDst; }

  virtual HWAtom *getSCCSrc() const { return Src; }
  bool isBackEdge() const { return getDagSrc() != getSCCSrc(); }

  virtual void print(raw_ostream &OS) const = 0;
};

class HWReg {
  std::set<Value*> Vals;
  const Type *Ty;
  unsigned Num;

public:
  explicit HWReg(unsigned num, Value &V)
    : Ty(V.getType()), Num(num) {
      Vals.insert(&V);
  }

  const Type *getType() const { return Ty; }

  void addValue(Value &V) {
    assert(Ty == V.getType() && "Can merge difference type!");
    Vals.insert(&V);
  }

  unsigned getRegNum() const { return Num; }

  typedef std::set<Value*>::iterator iterator;
  typedef std::set<Value*>::const_iterator const_iterator;

  iterator begin() { return Vals.begin(); }
  const_iterator begin() const { return Vals.begin(); }

  iterator end() { return Vals.begin(); }
  const_iterator end() const { return Vals.begin(); }
}; 

/// @brief Constant node
class HWValDep : public HWEdge {
  const bool Signed;
  PointerUnion<HWReg*, Constant*> Data;
public:
  HWValDep(HWAtom *Src, bool isSigned, HWReg *Reg)
    : HWEdge(edgeValDep, Src, 0), Signed(isSigned), Data(Reg) {}

  HWValDep(HWAtom *Src, bool isSigned, Constant *C)
    : HWEdge(edgeValDep, Src, 0), Signed(isSigned), Data(C) {}

  HWReg *getReg() const { return Data.get<HWReg*>(); }
  Constant *getConstant() const { return Data.get<Constant*>(); }
  bool isConstant() const { return Data.is<Constant*>(); }

  bool isWire() const { return Data.isNull(); }
  
  bool isSigned() const { return Signed; }

  void assignRegister(HWReg *R) { Data = R; }

  void print(raw_ostream &OS) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const HWValDep *A) { return true; }
  static inline bool classof(const HWEdge *A) {
    return A->getEdgeType() == edgeValDep;
  }
};

class HWCtrlDep : public HWEdge {
public:
  HWCtrlDep(HWAtom *Src) : HWEdge(edgeCtrlDep, Src, 0) {}

  void print(raw_ostream &OS) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const HWCtrlDep *A) { return true; }
  static inline bool classof(const HWEdge *A) {
    return A->getEdgeType() == edgeCtrlDep;
  }
};

class HWMemDep : public HWEdge {
public:
  enum MemDepTypes {
    TrueDep, AntiDep, OutputDep, NoDep
  };
private:
  PointerIntPair<HWAOpInst*, 2, enum MemDepTypes> Data;
public:
  HWMemDep(HWAtom *Src, HWAOpInst *DepSrc, enum MemDepTypes DT,
          unsigned Distance) : HWEdge(edgeMemDep, Src, Distance),
          Data(DepSrc, DT) {}

  enum MemDepTypes getDepType() const { return Data.getInt(); }

  HWAtom *getSCCSrc() const;

  void print(raw_ostream &OS) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const HWCtrlDep *A) { return true; }
  static inline bool classof(const HWEdge *A) {
    return A->getEdgeType() == edgeMemDep;
  }
};

template<class IteratorType, class NodeType>
class HWAtomDepIterator : public std::iterator<std::forward_iterator_tag,
                                               NodeType*, ptrdiff_t> {
  IteratorType I;   // std::vector<MSchedGraphEdge>::iterator or const_iterator
public:
  HWAtomDepIterator(IteratorType i) : I(i) {}

  bool operator==(const HWAtomDepIterator RHS) const { return I == RHS.I; }
  bool operator!=(const HWAtomDepIterator RHS) const { return I != RHS.I; }

  const HWAtomDepIterator &operator=(const HWAtomDepIterator &RHS) {
    I = RHS.I;
    return *this;
  }

  NodeType* operator*() const {
    return (*I)->getDagSrc();
  }
  NodeType* operator->() const { return operator*(); }

  HWAtomDepIterator& operator++() {                // Preincrement
    ++I;
    return *this;
  }
  HWAtomDepIterator operator++(int) { // Postincrement
    HWAtomDepIterator tmp = *this;
    ++*this;
    return tmp; 
  }

  HWEdge *getEdge() { return *I; }
  const HWEdge *getEdge() const { return *I; }
};

/// @brief Base Class of all hardware atom. 
class HWAtom : public FoldingSetNode {
  /// FastID - A reference to an Interned FoldingSetNodeID for this node.
  /// The ScalarEvolution's BumpPtrAllocator holds the data.
  FoldingSetNodeIDRef FastID;

  // The HWAtom baseclass this node corresponds to
  const unsigned short HWAtomType;

  /// First of all, we schedule all atom base on dependence
  SmallVector<HWEdge*, 4> Deps;

  // The atoms that using this atom.
  std::list<HWAtom*> UseList;

  void addToUseList(HWAtom *User) {
    UseList.push_back(User);
  }

  HWAtom(const HWAtom &);            // DO NOT IMPLEMENT
  void operator=(const HWAtom &);  // DO NOT IMPLEMENT

protected:
  // The corresponding LLVM Instruction
  Value &Val;

  // The time slot that this atom scheduled to.
  unsigned SchedSlot;

  virtual ~HWAtom();

public:
  template <class It>
  HWAtom(const FoldingSetNodeIDRef ID, unsigned HWAtomTy, Value &V,
    It depbegin, It depend)  : FastID(ID), HWAtomType(HWAtomTy),
    Deps(depbegin, depend), Val(V), SchedSlot(0) {
    for (dep_iterator I = dep_begin(), E = dep_end(); I != E; ++I) {
      //Deps.push_back(*I);
      (*I)->addToUseList(this);
    }
  }

  // Add a new depencence edge to the atom.
  void addDep(HWEdge *E) {
    E->getDagSrc()->addToUseList(this);
    Deps.push_back(E);
  }

  HWAtom(const FoldingSetNodeIDRef ID, unsigned HWAtomTy, Value &V);

  HWAtom(const FoldingSetNodeIDRef ID, unsigned HWAtomTy, Value &V, HWEdge *Dep0);

  unsigned getHWAtomType() const { return HWAtomType; }

  SmallVectorImpl<HWEdge*>::iterator edge_begin() { return Deps.begin(); }
  SmallVectorImpl<HWEdge*>::iterator edge_end() { return Deps.end(); }

  SmallVectorImpl<HWEdge*>::const_iterator edge_begin() const { return Deps.begin(); }
  SmallVectorImpl<HWEdge*>::const_iterator edge_end() const { return Deps.end(); }

  /// Profile - FoldingSet support.
  void Profile(FoldingSetNodeID& ID) { ID = FastID; }

  Value &getValue() const { return Val; }

  /// @name Operands
  //{
  HWEdge *getDep(unsigned i) const {
    return Deps[i];
  }

  typedef HWAtomDepIterator<SmallVectorImpl<HWEdge*>::iterator, HWAtom>
    dep_iterator;
  typedef HWAtomDepIterator<SmallVectorImpl<HWEdge*>::const_iterator, const HWAtom>
    const_dep_iterator;
  dep_iterator dep_begin() { return Deps.begin(); }
  dep_iterator dep_end() { return Deps.end(); }
  const_dep_iterator dep_begin() const { return Deps.begin(); }
  const_dep_iterator dep_end() const { return Deps.end(); }

  size_t getNumDeps() const { return Deps.size(); }

  // If the current atom depend on A?
  bool isDepOn(const HWAtom *A) const { return getDepIt(A) != dep_end(); }

  void setDep(dep_iterator I, HWAtom *NewDep) {
    assert(I != dep_end() && "I out of range!");
    I->removeFromList(this);
    NewDep->addToUseList(this);
    // Setup the dependence list.
    I.getEdge()->setDagSrc(NewDep);
  }

  void setDep(unsigned idx, HWAtom *NewDep) {
    // Update use list
    Deps[idx]->getDagSrc()->removeFromList(this);
    NewDep->addToUseList(this);
    // Setup the dependence list.
    Deps[idx]->setDagSrc(NewDep);
  }

  // If this Depend on A? return the position if found, return dep_end otherwise.
  const_dep_iterator getDepIt(const HWAtom *A) const {
    for (const_dep_iterator I = dep_begin(), E = dep_end(); I != E; ++I)
      if ((*I) == A)
        return I;

    return dep_end();
  }
  dep_iterator getDepIt(const HWAtom *A) {
    for (dep_iterator I = dep_begin(), E = dep_end(); I != E; ++I)
      if ((*I) == A)
        return I;
    
    return dep_end();
  }

  HWEdge *getDepEdge(const HWAtom *A) {
    assert(isDepOn(A) && "Current atom not depend on A!");
    return getDepIt(A).getEdge();
  }
  HWEdge *getDepEdge(const HWAtom *A) const {
    assert(isDepOn(A) && "Current atom not depend on A!");
    return getDepIt(A).getEdge();
  }

  //}

  /// @name Use
  //{
  typedef std::list<HWAtom*>::iterator use_iterator;
  typedef std::list<HWAtom*>::const_iterator const_use_iterator;
  use_iterator use_begin() { return UseList.begin(); }
  const_use_iterator use_begin() const { return UseList.begin(); }
  use_iterator use_end() { return UseList.end(); }
  const_use_iterator use_end() const { return UseList.end(); }

  HWAtom *use_back() { return UseList.back(); }
  HWAtom *use_back() const { return UseList.back(); }

  void removeFromList(HWAtom *User) {
    std::list<HWAtom*>::iterator at = std::find(UseList.begin(), UseList.end(),
      User);
    assert(at != UseList.end() && "Not in use list!");
    UseList.erase(at);
  }

  void replaceAllUseBy(HWAtom *A);

  bool use_empty() { return UseList.empty(); }
  size_t getNumUses() const { return UseList.size(); }
  //}

  void resetSchedule() { SchedSlot = 0; }
  unsigned getSlot() const { return SchedSlot; }
  bool isScheduled() const { return SchedSlot != 0; }
  void scheduledTo(unsigned slot);

  // Get the latency of this atom
  virtual unsigned getLatency() const { return 0; }

  /// print - Print out the internal representation of this atom to the
  /// specified stream.  This should really only be used for debugging
  /// purposes.
  virtual void print(raw_ostream &OS) const = 0;

  /// dump - This method is used for debugging.
  ///
  void dump() const;
};

}

namespace llvm {

template<> struct GraphTraits<Inverse<esyn::HWAtom*> > {
  typedef esyn::HWAtom NodeType;
  typedef esyn::HWAtom::dep_iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->dep_begin();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->dep_end();
  }
};
template<> struct GraphTraits<Inverse<const esyn::HWAtom*> > {
  typedef const esyn::HWAtom NodeType;
  typedef esyn::HWAtom::const_dep_iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->dep_begin();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->dep_end();
  }
};
template<> struct GraphTraits<esyn::HWAtom*> {
  typedef esyn::HWAtom NodeType;
  typedef esyn::HWAtom::use_iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->use_begin();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->use_end();
  }
};
template<> struct GraphTraits<const esyn::HWAtom*> {
  typedef const esyn::HWAtom NodeType;
  typedef esyn::HWAtom::const_use_iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->use_begin();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->use_end();
  }
};
}

// FIXME: Move to a seperate header.
namespace esyn {

// Use tree iterator.
typedef df_iterator<HWAtom*, SmallPtrSet<HWAtom*, 8>, false,
  GraphTraits<HWAtom*> > usetree_iterator;
typedef df_iterator<const HWAtom*, SmallPtrSet<const HWAtom*, 8>, false,
  GraphTraits<const HWAtom*> > const_usetree_iterator;

// Predecessor tree iterator, travel the tree from exit node.
typedef df_iterator<HWAtom*, SmallPtrSet<HWAtom*, 8>, false,
  GraphTraits<Inverse<HWAtom*> > > deptree_iterator;

typedef df_iterator<const HWAtom*, SmallPtrSet<const HWAtom*, 8>, false,
  GraphTraits<Inverse<const HWAtom*> > > const_deptree_iterator;

// Drive register
class HWADrvReg : public HWAtom {
public:
  HWADrvReg(const FoldingSetNodeIDRef ID, HWEdge *Edge)
    : HWAtom(ID, atomDrvReg, Edge->getDagSrc()->getValue(), Edge) {
    scheduledTo(Edge->getDagSrc()->getSlot() + Edge->getDagSrc()->getLatency());
  }

  const HWReg *getReg() const {
    return cast<HWValDep>(getDep(0))->getReg();
  }

  static inline bool classof(const HWADrvReg *A) { return true; }
  static inline bool classof(const HWAtom *A) {
    return A->getHWAtomType() == atomDrvReg;
  }

  void print(raw_ostream &OS) const;
};

// Virtual Root
class HWAVRoot : public HWAtom {
public:
  HWAVRoot(const FoldingSetNodeIDRef ID, BasicBlock &BB)
    : HWAtom(ID, atomVRoot, BB) {}

  BasicBlock &getBasicBlock() { return cast<BasicBlock>(getValue()); }


  usetree_iterator begin() {
    return usetree_iterator::begin(this);
  }
  usetree_iterator end() {
    return usetree_iterator::end(this);
  }

  const_usetree_iterator begin() const { 
    return const_usetree_iterator::begin(this);
  }
  const_usetree_iterator end() const {
    return const_usetree_iterator::end(this);
  }

  void print(raw_ostream &OS) const;

  static inline bool classof(const HWAVRoot *A) { return true; }
  static inline bool classof(const HWAtom *A) {
    return A->getHWAtomType() == atomVRoot;
  }
};

// Create the ative atom class for register, and operate on instruction

/// @brief The Schedulable Hardware Atom
class HWAOpInst : public HWAtom {
  // The latency of this atom
  unsigned Latency;
  unsigned NumOps;
protected:
  unsigned SubClassData;

  template <class It>
  HWAOpInst(const FoldingSetNodeIDRef ID, enum HWAtomTypes T,
    Instruction &Inst, unsigned latency, It depbegin, It depend,
    size_t OpNum, unsigned subClassData = 0)
    : HWAtom(ID, T, Inst, depbegin, depend), Latency(latency), NumOps(OpNum),
    SubClassData(subClassData) {}
public:
  // Get the latency of this atom
  unsigned getLatency() const {
    return Latency;
  }

  template<class InstTy>
  InstTy &getInst() { return cast<InstTy>(getValue()); }

  template<class InstTy>
  const InstTy &getInst() const { return cast<InstTy>(getValue()); }

  Value *getIOperand(unsigned idx) {
    return getInst<Instruction>().getOperand(idx);
  }

  Value *getIOperand(unsigned idx) const {
    return getInst<Instruction>().getOperand(idx);
  }

  size_t getInstNumOps () const { return NumOps; }

  HWValDep *getValDep(unsigned idx) {
    assert(idx < NumOps && "index Out of range!");
    return cast<HWValDep>(getDep(idx));
  }

  HWAtom *getOperand(unsigned idx) {
    assert(idx < NumOps && "index Out of range!");
    //assert(&(getDep(idx)->getSrc()->getValue()) == getInst<Instruction>().getOperand(idx)
    //  && "HWPostBind operands broken!");
    return getDep(idx)->getDagSrc();
  }

  // Help the scheduler to identify difference operation class
  virtual enum HWResource::ResTypes getResClass() const {
    return HWResource::Trivial;
  }

  // Return the opcode of the instruction.
  unsigned getOpcode() const {
    return cast<Instruction>(getValue()).getOpcode();
  }

  
  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const HWAOpInst *A) { return true; }
  static inline bool classof(const HWAtom *A) {
    return A->getHWAtomType() == atomPreBind ||
      A->getHWAtomType() == atomPostBind;
  }
};

class HWAPostBind : public HWAOpInst {
public:
  template <class It>
  explicit HWAPostBind(const FoldingSetNodeIDRef ID, Instruction &Inst,
    unsigned latency, It depbegin, It depend, size_t OpNum,
      enum HWResource::ResTypes OpClass)
    : HWAOpInst(ID, atomPostBind, Inst, latency, 
    depbegin, depend, OpNum, OpClass) {}
  
  enum HWResource::ResTypes getResClass() const {
    return (HWResource::ResTypes)SubClassData;
  }

  void print(raw_ostream &OS) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const HWAPostBind *A) { return true; }
  static inline bool classof(const HWAtom *A) {
    return A->getHWAtomType() == atomPostBind;
  }
};

class HWAPreBind : public HWAOpInst {
public:
  template <class It>
  explicit HWAPreBind(const FoldingSetNodeIDRef ID, Instruction &Inst,
    unsigned latency, It depbegin, It depend, unsigned OpNum,
    enum HWResource::ResTypes OpClass, unsigned Instance = 0)
    : HWAOpInst(ID, atomPreBind, Inst, latency, depbegin, depend, OpNum,
    HWResource::createResId(OpClass, Instance)) {}

  HWAPreBind(const FoldingSetNodeIDRef ID, HWAPostBind &PostBind,
    unsigned Instance);

  /// @name The using resource
  //{
  // Help the scheduler to identify difference resource unit.
  HWResource::ResIdType getResourceId() const {
    return SubClassData;
  }

  enum HWResource::ResTypes getResClass() const {
    return HWResource::extractResType(getResourceId());
  }
  unsigned getAllocatedInstance() const {
    return HWResource::extractInstanceId(getResourceId());
  }
  //}

  void print(raw_ostream &OS) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const HWAPreBind *A) { return true; }
  static inline bool classof(const HWAtom *A) {
    return A->getHWAtomType() == atomPreBind;
  }
};

// Execute state.
class FSMState {
  typedef std::vector<HWAtom*> HWAtomVecType;

  HWAVRoot &EntryRoot;
  HWAOpInst &ExitRoot;

public:
  explicit FSMState(HWAVRoot *entry, HWAOpInst *exit)
    : EntryRoot(*entry), ExitRoot(*exit) {}


  /// @name Roots
  //{
  HWAVRoot &getEntryRoot() const { return EntryRoot; }
  HWAOpInst &getExitRoot() const { return ExitRoot; }

  HWAVRoot &getEntryRoot() { return EntryRoot; }
  HWAOpInst &getExitRoot() { return ExitRoot; }
  //}

  // Return the corresponding basiclbocl of this Execute stage.
  BasicBlock *getBasicBlock() { return &EntryRoot.getBasicBlock(); }
  BasicBlock *getBasicBlock() const { return &EntryRoot.getBasicBlock(); }

  // Successor tree iterator, travel the tree from entry node.
  usetree_iterator usetree_begin() { return EntryRoot.begin(); }
  const_usetree_iterator usetree_begin() const {
    return ((const HWAVRoot&)EntryRoot).begin();
  }

  usetree_iterator usetree_end() { return EntryRoot.end(); }
  const_usetree_iterator usetree_end() const {
    return ((const HWAVRoot&)EntryRoot).end();
  }

  deptree_iterator deptree_begin() { return deptree_iterator::begin(&ExitRoot); }
  const_deptree_iterator deptree_begin() const {
    return const_deptree_iterator::begin(&ExitRoot);
  }

  deptree_iterator deptree_end() { return deptree_iterator::end(&ExitRoot); }
  const_deptree_iterator deptree_end()  const {
    return const_deptree_iterator::end(&ExitRoot);
  }

  void resetSchedule() {
    for (usetree_iterator I = usetree_begin(), E = usetree_end(); I != E; ++I)
      (*I)->resetSchedule();
  }

  typedef std::multimap<unsigned, HWAtom*> ScheduleMapType;

  void getScheduleMap(ScheduleMapType &Atoms) const;

  void print(raw_ostream &OS) const;
};

} // end namespace

#endif
