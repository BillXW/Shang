//===-- VFInfo.cpp - VTM Per-Function Information Class Implementation ----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the interfaces that VTM uses to lower LLVM code
// into a selection DAG.
//
//===----------------------------------------------------------------------===//
#include "vtm/VFInfo.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

unsigned VFInfo::getTotalSlotFor(const MachineBasicBlock* MBB) const {
  std::map<const MachineBasicBlock*, StateSlots>::const_iterator
    at = StateSlotMap.find(MBB);

  assert(at != StateSlotMap.end() && "State not found!");
  return at->second.totalSlot;
}

unsigned VFInfo::getStartSlotFor(const MachineBasicBlock* MBB) const {
  std::map<const MachineBasicBlock*, StateSlots>::const_iterator
    at = StateSlotMap.find(MBB);

  assert(at != StateSlotMap.end() && "State not found!");
  return at->second.startSlot;
}

unsigned VFInfo::getIISlotFor(const MachineBasicBlock* MBB) const {
  std::map<const MachineBasicBlock*, StateSlots>::const_iterator
    at = StateSlotMap.find(MBB);

  assert(at != StateSlotMap.end() && "State not found!");
  return at->second.IISlot;
}

void VFInfo::remeberTotalSlot(const MachineBasicBlock* MBB, unsigned startSlot,
                              unsigned totalSlot, unsigned IISlot) {
  StateSlots SS;
  SS.startSlot = startSlot;
  SS.totalSlot = totalSlot;
  SS.IISlot = IISlot;
  StateSlotMap.insert(std::make_pair(MBB, SS));
}

void VFInfo::rememberAllocatedFU(FuncUnitId Id, unsigned EmitSlot,
                                 unsigned FinshSlot) {
  // Sometimes there are several instructions allocated to the same instruction,
  // and it is ok to try to insert the same FUId more than once.
  AllocatedFUs[Id.getFUType()].insert(Id);
  for (unsigned i = EmitSlot; i < FinshSlot; ++i)
    remeberActiveSlot(Id, i);
}

void VFInfo::allocateBRam(uint16_t ID, unsigned NumElem,
                          unsigned ElemSizeInBytes) {
  bool Inserted;
  BRamMapTy::iterator at;

  tie(at, Inserted)
    = BRams.insert(std::make_pair(ID, BRamInfo(NumElem, ElemSizeInBytes)));

  assert(Inserted && "BRam already existed!");
}

// Out of line virtual function to provide home for the class.
void VFInfo::anchor() {}

unsigned VFInfo::getOverlaps(unsigned R, unsigned Overlaps[5]) const {
  //unsigned TrailingZeros = CountTrailingZeros_32(R);
  //unsigned Idx = 0;
  //for (unsigned i = TrailingZeros + 1; i < 4; ++i)
  //  Overlaps[Idx++] = R & (~0 << i);

  //// We have at most 8 bytes register.
  //unsigned NumSubRegs = std::min(1 << TrailingZeros, 8);
  //for (unsigned i = 0; i < NumSubRegs; ++i)
  //  Overlaps[Idx++] = R + i;

  Overlaps[0] = R;
  Overlaps[1] = R & (~0 << 3);

  return 2;
}
