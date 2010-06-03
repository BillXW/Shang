//===------------- VLang.h - Verilog HDL writing engine ---------*- C++ -*-===//
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
// This file implement the VLang class, with provide funtions to complete
// common Verilog HDL writing task.
//
//===----------------------------------------------------------------------===//

#include "VLang.h"
#include "llvm/DerivedTypes.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/ErrorHandling.h"

#include <sstream>

using namespace llvm;
using namespace xVerilog;

namespace {
  class VBEMCAsmInfo : public MCAsmInfo {
  public:
    VBEMCAsmInfo() {
      GlobalPrefix = "";
      PrivateGlobalPrefix = "";
    }
  };
}

//===----------------------------------------------------------------------===//
// Value and type printing
std::string VLang::VLangMangle(const std::string &S) {
  std::string Result;

  for (unsigned i = 0, e = S.size(); i != e; ++i)
  if (isalnum(S[i]) || S[i] == '_') {
  Result += S[i];
  } else {
  Result += '_';
  Result += 'A'+(S[i]&15);
  Result += 'A'+((S[i]>>4)&15);
  Result += '_';
  }
  return Result;
}
std::string VLang::printBitWitdh(const Type *Ty, int LowestBit,
                                           bool printOneBit) {
  std::stringstream bw;
  int BitWitdh = cast<IntegerType>(Ty)->getBitWidth();
  if (BitWitdh !=1) 
    bw << "[" << (BitWitdh - 1 + LowestBit) << ":" << LowestBit << "] ";
  else if(printOneBit)
    bw << "[" << LowestBit << "] ";
  bw << " ";
  return bw.str();
}

std::string VLang::GetValueName(const Value *Operand) {
  // Mangle globals with the standard mangler interface for LLC compatibility.
  if (const GlobalValue *GV = dyn_cast<GlobalValue>(Operand)) {
    SmallString<128> Str;
    Mang->getNameWithPrefix(Str, GV, false);
    return VLangMangle(Str.str().str());
  }

  std::string Name = Operand->getName();

  if (Name.empty()) { // Assign unique names to local temporaries.
    unsigned &No = AnonValueNumbers[Operand];
    if (No == 0)
      No = ++NextAnonValueNumber;
    Name = "tmp__" + utostr(No);
  }

  std::string VarName;
  VarName.reserve(Name.capacity());

  for (std::string::iterator I = Name.begin(), E = Name.end();
      I != E; ++I) {
    char ch = *I;

  if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
      (ch >= '0' && ch <= '9') || ch == '_')) {
    char buffer[5];
    sprintf(buffer, "_%x_", ch);
    VarName += buffer;
  } else
    VarName += ch;
  }

  return "llvm_vbe_" + VarName;
}

std::string VLang::printType(const Type *Ty,
                             bool isSigned /* = false */,
                             const std::string &VariableName /* =  */,
                             const std::string &SignalType /* =  */,
                             const std::string &Direction /* =  */,
                             bool IgnoreName /* = false */,
                             const AttrListPtr &PAL /* = AttrListPtr */) {
  std::stringstream ss;
  ss << Direction;
  if (Ty->isIntegerTy()) {
    ss << printSimpleType(Ty, isSigned, VariableName, SignalType);
    return ss.str();
  }
  
}

std::string VLang::printSimpleType(const Type *Ty, bool isSigned,
                                   const std::string &NameSoFar /* =  */,
                                   const std::string &SignalType /* = */){
  std::stringstream ss;
  //wire or reg?
	ss << SignalType;
	//signed?
	if(isSigned)
		ss << "signed ";

  switch (Ty->getTypeID()) {
  case Type::IntegerTyID:
    ss << printBitWitdh(Ty) << NameSoFar;
    return ss.str();
  default:
    llvm_unreachable("Unsupport type!");
  }
}
