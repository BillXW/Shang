//===---------- VInstrInfo.cpp - VTM Instruction Information -----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the VTM implementation of the TargetInstrInfo class.
//
//===----------------------------------------------------------------------===//

#include "VTargetMachine.h"

#include "vtm/VInstrInfo.h"
#include "vtm/VTM.h"

#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ErrorHandling.h"

#include "VGenInstrInfo.inc"

using namespace llvm;

VInstrInfo::VInstrInfo(const TargetData &TD, const TargetLowering &TLI)
  : TargetInstrInfoImpl(VTMInsts, array_lengthof(VTMInsts)), RI(*this, TD, TLI)
  {}

bool VInstrInfo::AnalyzeBranch(MachineBasicBlock &MBB, MachineBasicBlock *&TBB,
                               MachineBasicBlock *&FBB,
                               SmallVectorImpl<MachineOperand> &Cond,
                               bool AllowModify /* = false */) {
  // TODO: Write code for this function.
  return true;
}

FuncUnitId VInstr::getPrebindFUId()  const {
  // Dirty Hack: Bind all memory access to channel 0 at this moment.
  if (getTID().Opcode == VTM::VOpMemTrans)
    return FuncUnitId(VFUs::MemoryBus, 0);

  if (getTID().Opcode == VTM::VOpBRam) {
    unsigned Id = (*I.memoperands_begin())->getPointerInfo().getAddrSpace();
    return FuncUnitId(VFUs::BRam, Id);
  }

  return FuncUnitId();
}


BitWidthOperand::BitWidthOperand(const MachineInstr &MI)
  : Op(MI.getOperand(MI.getNumOperands() - 1).getImm()) {}

void BitWidthOperand::updateBitWidth(MachineInstr &MI) {
  MI.getOperand(MI.getNumOperands() - 1).setImm(Op);
}


// Out of line virtual function to provide home for the class.
void BitWidthOperand::anchor() {}

bool VInstr::mayLoad() const {
  switch (getTID().Opcode) {
  default: return false;
  // There is a "isLoad" flag in memory access operation.
  case VTM::VOpMemTrans: return !I.getOperand(3).getImm();
  }
}

bool VInstr::mayStore() const {
  switch (getTID().Opcode) {
  default: return false;
    // There is a "isLoad" flag in memory access operation.
  case VTM::VOpMemTrans: return I.getOperand(3).getImm();
  }
}

// Out of line virtual function to provide home for the class.
void VInstr::anchor() {}
