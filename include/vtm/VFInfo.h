//====------ VFunInfo.h - Verilog target machine function info --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file declares Verilog target machine-specific per-machine-function
// information.
//
//===----------------------------------------------------------------------===//

#ifndef VTM_FUNCTION_INFO_H
#define VTM_FUNCTION_INFO_H

#include "vtm/SynSettings.h"
#include "vtm/FUInfo.h"

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Support/StringPool.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/ADT/OwningPtr.h"

#include <set>
#include <map>

namespace llvm {
class MachineBasicBlock;
class MachineInstr;
class VASTModule;

class VFInfo : public MachineFunctionInfo {
  // Information about slots.
  struct StateSlots{
    unsigned startSlot : 32;
    unsigned totalSlot : 16;
    unsigned IISlot    : 16;
  };
  std::map<const MachineBasicBlock*, StateSlots> StateSlotMap;

  // Remember the scheduled slot of PHI nodes, it will lose after PHIElemination.
  typedef std::map<const MachineInstr*,
                   std::pair<int, const MachineBasicBlock*> >
          PhiSlotMapTy;
  PhiSlotMapTy PHISlots;

  // Allocated physics registers in a MachineFunction/RTL module.
  // TODO: we need to perform per-BasicBlock register allocation to reduce
  // the length of interconnection.
  unsigned TotalRegs;
  static const unsigned fistPhyReg = 8;

public:
  // The data structure to describe the block ram.
  struct BRamInfo {
    unsigned NumElem, ElemSizeInBytes;

    BRamInfo(unsigned numElem, unsigned elemSizeInBytes)
      : NumElem(numElem), ElemSizeInBytes(elemSizeInBytes) {}
  };

private:
  typedef std::map<uint16_t, BRamInfo> BRamMapTy;
  BRamMapTy BRams;


  // Mapping Function unit number to callee function name.
  typedef StringMap<unsigned> FNMapTy;
  typedef StringMapEntry<unsigned> FNEntryTy;
  FNMapTy UsedFNs;
  const SynSettings *Info;
  // Rtl module.
  VASTModule *Mod;
  // If bit width information annotated to the annotator?
  bool BitWidthAnnotated;
public:
  explicit VFInfo(MachineFunction &MF);
  ~VFInfo();

  bool isBitWidthAnnotated() const { return BitWidthAnnotated; }
  void removeBitWidthAnnotators() {
    assert(isBitWidthAnnotated() && "Annotators arealy removed!");
    BitWidthAnnotated = false;
  }

  const SynSettings &getInfo() const { return *Info; }

  void setTotalSlots(unsigned Slots);

  /// Verilog module for the machine function.
  VASTModule *getRtlMod() const;

  /// Slots information for machine basicblock.
  unsigned getStartSlotFor(const MachineBasicBlock* MBB) const;
  unsigned getTotalSlotFor(const MachineBasicBlock *MBB) const;
  unsigned getEndSlotFor(const MachineBasicBlock *MBB) const {
    return getStartSlotFor(MBB) + getTotalSlotFor(MBB);
  }
  unsigned getIISlotFor(const MachineBasicBlock* MBB) const;
  unsigned getIIFor(const MachineBasicBlock *MBB) const {
    return getIISlotFor(MBB) - getStartSlotFor(MBB);
  }

  void rememberTotalSlot(const MachineBasicBlock* MBB,
                        unsigned startSlot,
                        unsigned totalSlot,
                        unsigned IISlot);

  void rememberPHISlot(const MachineInstr *PN, unsigned Slot,
                       bool Pipe = false) {
    int S = Pipe ? - Slot : Slot;
    bool success =
      PHISlots.insert(std::make_pair(PN,
                                     std::make_pair(S, PN->getParent()))).second;
    assert(success && "Insert the same phinode twice?");
    (void) success;
  }


  typedef FNMapTy::const_iterator const_fn_iterator;
  const_fn_iterator fn_begin() const { return UsedFNs.begin(); }
  const_fn_iterator fn_end() const { return UsedFNs.end(); }

  unsigned getOrCreateCalleeFN(StringRef FNName) {
    FNMapTy::iterator at = UsedFNs.find(FNName);
    if (at != UsedFNs.end()) return at->second;

    unsigned CalleeFNNum = UsedFNs.size() + 1;
    FNEntryTy *FN = FNEntryTy::Create(FNName.begin(), FNName.end());
    FN->second = CalleeFNNum;
    UsedFNs.insert(FN);
    return CalleeFNNum;
  }

  unsigned getCalleeFNNum(StringRef FNName) const {
    return UsedFNs.lookup(FNName);
  }

  //const Function *getCalleeFN(unsigned FNNum) const {
  //  assert(FNNum < UsedFNs.size() && "Invalid FNNum!");
  //  return UsedFNs[FNNum];
  //}

  std::pair<int, const MachineBasicBlock*>
    lookupPHISlot(const MachineInstr *PN) const;

  //const char *allocateSymbol(const std::string &Str) {
  //  PooledStringPtr PSP = SymbolPool.intern(Str.c_str());
  //  Symbols.insert(PSP);
  //  return *PSP;
  //}

  // Block Ram management.
  void allocateBRam(uint16_t ID, unsigned NumElem, unsigned ElemSizeInBytes);

  const BRamInfo &getBRamInfo(uint16_t ID) const {
    BRamMapTy::const_iterator at = BRams.find(ID);
    assert(at != BRams.end() && "BRam not exists!");
    return at->second;
  }

  // Allocate a Physics register, its sizeInBytes can be 1/2/3/4
  unsigned allocatePhyReg(unsigned SizeInBytes) {
    unsigned ret = RoundUpToAlignment(TotalRegs, SizeInBytes);
    // The register should always align.
    TotalRegs = ret + SizeInBytes;
    return ret;
  }

  class phyreg_iterator : public std::iterator<std::forward_iterator_tag,
                                               unsigned> {
    unsigned i, sizeInBytes;
  public:
    phyreg_iterator(unsigned I, unsigned SizeInBytes)
      : i(I), sizeInBytes(SizeInBytes) {}

    inline bool operator==(const phyreg_iterator RHS) const {
      assert(sizeInBytes == RHS.sizeInBytes
             && "Can not compare phyreg_iterator with different sizeInBytes!");
      return i == RHS.i;
    }

    inline bool operator!=(const phyreg_iterator RHS) const {
      return !operator==(RHS);
    }

    inline bool operator<(const phyreg_iterator RHS) const {
      assert(sizeInBytes == RHS.sizeInBytes
        && "Can not compare phyreg_iterator with different sizeInBytes!");
      return i < RHS.i;
    }

    inline unsigned operator*() const { return i; }

    inline phyreg_iterator &operator++() {
      i += sizeInBytes;
      return *this;
    }

    inline phyreg_iterator operator++(int) {
      phyreg_iterator tmp = *this;
      ++*this;
      return tmp;
    }
  };

  phyreg_iterator phyreg_begin(unsigned sizeInByte) const {
    return phyreg_iterator(fistPhyReg,  sizeInByte);
  }

  phyreg_iterator phyreg_end(unsigned  sizeInByte) const {
    return phyreg_iterator(TotalRegs,  sizeInByte);
  }

  unsigned getOverlaps(unsigned R, unsigned Overlaps[5]) const;

  // Out of line virtual function to provide home for the class.
  virtual void anchor();
};

}

#endif
