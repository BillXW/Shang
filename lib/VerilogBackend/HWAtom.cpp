//===------------ HWAtom.cpp - Translate LLVM IR to HWAtom  -----*- C++ -*-===//
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
// This file implement the HWAtom class, which represent the basic atom
// operation in hardware.
//
//===----------------------------------------------------------------------===//

#include "vbe/HWAtom.h"

#include "llvm/Assembly/Writer.h"
#include "llvm/Support/Debug.h"
#include "llvm/Analysis/LoopInfo.h"

#include "vbe/ResourceConfig.h"

using namespace llvm;
using namespace esyn;


HWAtom::~HWAtom() {}

void HWAtom::dump() const {
  print(dbgs());
  dbgs() << '\n';
}

void HWAConst::print(raw_ostream &OS) const {
  OS << "Const (" << getValue() << ")";
}

void HWASigned::print(raw_ostream &OS) const {
  OS << "signed (";
  WriteAsOperand(OS, &getDep(0)->getValue(), false);
  OS << ")";
}


void HWARegister::print(raw_ostream &OS) const {
  OS << "Register: ";
  WriteAsOperand(OS, &Val, false);
  OS << " using ";
  WriteAsOperand(OS, &getDep(0)->getValue(), false);
}

void HWAStateEnd::print(raw_ostream &OS) const {
  OS << "State Transfer: " << Val;
}

void HWAState::getScheduleMap(ScheduleMapType &Atoms) const {
  for (HWAState::const_iterator I = begin(), E = end(); I != E; ++I) {
    HWAtom *A = *I;
    Atoms.insert(std::make_pair<unsigned, HWAtom*>(A->getSlot(), A));
  }
  // insert the end state
  HWAtom *End =const_cast<HWAStateEnd*>(getStateEnd());
  Atoms.insert(std::make_pair<unsigned, HWAtom*>(End->getSlot(), End));
}

void HWAState::print(raw_ostream &OS) const {
  OS << "State: ";
  WriteAsOperand(OS, &getValue(), false);
  OS << "\n";
  //for (HWAState::const_iterator I = begin(), E = end(); I != E; ++I) {
  //  (*I)->print(OS.indent(2));
  //  OS << "\n";
  //}
  unsigned oldSlot = 0;

  std::multimap<unsigned, HWAtom*> Atoms;
  getScheduleMap(Atoms);
  for (std::multimap<unsigned, HWAtom*>::iterator I = Atoms.begin(),
      E = Atoms.end(); I != E; ++I) {
    HWAtom *A = I->second;
    if (A->getSlot() != oldSlot) {
      oldSlot = A->getSlot();
      OS << "Cycle: " << oldSlot << "\n";
    }
    A->print(OS.indent(2));
    OS << " at "<< A->getSlot() << "\n";
  }

  const HWAStateEnd &StateEnd = *getStateEnd();
  StateEnd.print(OS.indent(2));
  OS << " at "<< StateEnd.getSlot() << "\n";
}

void HWAOpRes::print(raw_ostream &OS) const {
  WriteAsOperand(OS, &getValue(), false);
  OS << " Res: " << getUsedResource().getName()
    << " Instance: " << ResId << '\n';
}

void HWAOpInst::print(raw_ostream &OS) const {
  OS << "OpInst: ";
  WriteAsOperand(OS, &getValue(), false);
}

//===----------------------------------------------------------------------===//
void HWResTable::clear() {
  while (!ResSet.empty()) {
    ResourceSetType::iterator I = ResSet.begin();
    (*I)->clear();
    ResSet.erase(I);
  }
}

HWResTable::~HWResTable() {
  clear();
}

HWResource *HWResTable::initResource(std::string Name){
  HWResource *HR = RC.getResource(Name);
  if (HR != 0)
    ResSet.insert(HR);

  return HR;
}
