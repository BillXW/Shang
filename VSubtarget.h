//===-------- VSubtarget.h - Define Subtarget for the VTM --------*- C++ -*-====//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file declares the VTM specific subclass of TargetSubtarget.
//
//===----------------------------------------------------------------------===//

#ifndef VSUBTARGET_H
#define VSUBTARGET_H

#include "llvm/Target/TargetSubtargetInfo.h"

#include <string>
#include <set>
#include <map>

namespace llvm {
class Module;

class VSubtarget : public TargetSubtargetInfo {
  /// Selected instruction itineraries (one entry per itinerary class.)
  InstrItineraryData InstrItins;
  // Just some dummy subtarget features.
  bool vtmattr;

public:
  VSubtarget(const std::string &TT, const std::string &FS);
  std::string ParseSubtargetFeatures(const std::string &FS,
                                     const std::string &CPU);

  /// getInstrItins - Return the instruction itineraies based on subtarget
  /// selection.
  const InstrItineraryData &getInstrItineraryData() const { return InstrItins; }
};

} // end namespace llvm

#endif
