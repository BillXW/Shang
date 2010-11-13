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

#include "VInstrInfo.h"
#include "VTM.h"
#include "VTMConfig.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/ErrorHandling.h"

#include "VGenInstrInfo.inc"

using namespace llvm;

VInstrInfo::VInstrInfo(const TargetData &TD, const TargetLowering &TLI)
  : TargetInstrInfoImpl(VTMInsts, array_lengthof(VTMInsts)), RI(*this, TD, TLI)
  {}

unsigned VTIDReader::getLatency(const VTMConfig &VTC) const {
  VFUs::FUTypes ResTy = getFUType();

  if (ResTy == VFUs::Trivial)
    return getTrivialLatency();

  return VTC.getFUDesc(ResTy)->getLatency();
}


unsigned VTIDReader::getPrebindFUId()  const {
  // Dirty Hack: Bind all memory access to channel 0 at this moment.
  if (Instr->getOpcode() == VTM::VOpMemAccess)
    return 0;

  return TrivialFUId;
}

bool llvm::VTIDReader::isFUBinded() const {
  return getPrebindFUId() != TrivialFUId;
}