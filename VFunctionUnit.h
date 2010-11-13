//===------- VFunctionUnit.h - VTM Function Unit Information ----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains define the function unit class in Verilog target machine.
//
//===----------------------------------------------------------------------===//

#ifndef VTM_FUNCTION_UNIT_H
#define VTM_FUNCTION_UNIT_H

#include "llvm/Support/raw_ostream.h"

namespace llvm {

namespace VFUs {
  enum FUTypes {
    Trivial = 0,
    MemoryBus = 1,
    SHL = 2,
    ASR = 3,
    LSR = 4,
    AddSub = 5,
    Mult = 6,

    FirstFUType = Trivial,
    LastFUType = Mult,
    NumFUs = LastFUType - FirstFUType + 1
  };
}

/// @brief The description of Verilog target machine function units.
class VFUDesc {
  // The HWResource baseclass this node corresponds to
  unsigned ResourceType;
  // How many cycles to finish?
  const unsigned Latency;
  // Start interval
  const unsigned StartInt;
  // How many resources available?
  const unsigned TotalRes;

  //// Use a map mapping instance to count?
  //typedef std::vector<unsigned> UsingCountVec;
  //UsingCountVec UsingCount;

  VFUDesc(const VFUDesc &);            // DO NOT IMPLEMENT
  void operator=(const VFUDesc &);  // DO NOT IMPLEMENT
protected:
  VFUDesc(VFUs::FUTypes type, unsigned latency, unsigned startInt,
          unsigned totalRes)
    : ResourceType(type), Latency(latency), StartInt(startInt),
    TotalRes(totalRes) {}
public:
  unsigned getType() const { return ResourceType; }
  
  unsigned getLatency() const { return Latency; }
  unsigned getTotalRes() const { return TotalRes; }
  unsigned getStartInt() const { return StartInt; }

  virtual void print(raw_ostream &OS) const;
}; 

class VFUMemBus : public VFUDesc {
  unsigned AddrWidth;
  unsigned DataWidth;

  VFUMemBus(unsigned latency, unsigned startInt, unsigned totalRes,
    unsigned addrWidth, unsigned dataWidth)
    : VFUDesc(VFUs::MemoryBus, latency, startInt, totalRes),
    AddrWidth(addrWidth), DataWidth(dataWidth) {}

  friend class VTMConfig;
public:
  unsigned getAddrWidth() const { return AddrWidth; }
  unsigned getDataWidth() const { return DataWidth; }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VFUMemBus *A) { return true; }
  static inline bool classof(const VFUDesc *A) {
    return A->getType() == VFUs::MemoryBus;
  }

  static std::string getTypeName() { return "MemoryBus"; }
  static unsigned getType() { return VFUs::MemoryBus; }
};

class VFUBinOpResType : public VFUDesc {
  unsigned MaxBitWidth;

protected:
  VFUBinOpResType(VFUs::FUTypes T, unsigned latency, unsigned startInt,
    unsigned totalRes, unsigned maxBitWidth)
    : VFUDesc(T, latency, startInt, totalRes), MaxBitWidth(maxBitWidth) {}

public:
  unsigned getMaxBitWidth() const { return MaxBitWidth; }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VFUBinOpResType *A) { return true; }
  static inline bool classof(const VFUDesc *A) {
    return A->getType() == VFUs::AddSub
      || A->getType() == VFUs::SHL
      || A->getType() == VFUs::ASR
      || A->getType() == VFUs::LSR
      || A->getType() == VFUs::Mult;
  }
};

#define BINOPRESTYPECLASS(Name) \
class VFU##Name : public VFUBinOpResType { \
  unsigned MaxBitWidth; \
  VFU##Name(unsigned latency, unsigned startInt, unsigned totalRes, \
  unsigned maxBitWidth) \
  : VFUBinOpResType(VFUs::##Name, latency, startInt, totalRes, \
  maxBitWidth) \
{} \
  friend class VTMConfig; \
public: \
  static inline bool classof(const VFU##Name *A) { return true; } \
  static inline bool classof(const VFUDesc *A) { \
  return A->getType() == VFUs::##Name; \
} \
  static std::string getTypeName() { return #Name; } \
  static unsigned getType() { return VFUs::##Name; } \
}

BINOPRESTYPECLASS(Mult);
BINOPRESTYPECLASS(AddSub);
BINOPRESTYPECLASS(SHL);
BINOPRESTYPECLASS(ASR);
BINOPRESTYPECLASS(LSR);
}

#endif
