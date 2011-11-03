//===----- VFunctionUnit.cpp - VTM Function Unit Information ----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains implementation of the function unit class in
// Verilog target machine.
//
//===----------------------------------------------------------------------===//

#include "vtm/FUInfo.h"
#include "vtm/SynSettings.h" // DiryHack: Also implement the SynSetting class.
#include "vtm/LuaScript.h"
#include "vtm/VRegisterInfo.h"
#include "vtm/VTM.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SourceMgr.h"
#define DEBUG_TYPE "vtm-fu-info"
#include "llvm/Support/Debug.h"

using namespace llvm;

//===----------------------------------------------------------------------===//
/// Hardware resource.
void VFUDesc::print(raw_ostream &OS) const {
  // OS << "Resource: " << Name << '\n';
  OS.indent(2) << "TotalNum: " << TotalRes << '\n';
  OS.indent(2) << "Latency: " << Latency << '\n';
  OS.indent(2) << "StartInterval: " << StartInt << '\n';
}

namespace llvm {
  namespace VFUs {
    const char *VFUNames[] = {
      "Trivial", "AddSub", "Shift", "Mult", "MemoryBus", "BRam",
      "CalleeFN", "FSMFinish"
    };

    const TargetRegisterClass *getRepRegisterClass(unsigned OpCode, unsigned i){
      switch (OpCode) {
      default:                  return VTM::WireRegisterClass;
      case VTM::VOpAdd:
        if (i == 0)             return VTM::RADDRegisterClass;
        else                    return VTM::RCARRegisterClass;
      case VTM::VOpSRA:         return VTM::RASRRegisterClass;
      case VTM::VOpSRL:         return VTM::RLSRRegisterClass;
      case VTM::VOpSHL:         return VTM::RSHLRegisterClass;
      case VTM::VOpMult:        return VTM::RMULRegisterClass;
      case VTM::VOpCmdSeq:
      case VTM::VOpMemTrans:    return VTM::RINFRegisterClass;
      case VTM::VOpInternalCall:return VTM::RCFNRegisterClass;
      case VTM::VOpBRam:        return VTM::RBRMRegisterClass;
      }

      return 0;
    }

    // Default area cost parameter.
    unsigned LUTCost = 64;
    unsigned RegCost = 64;
    unsigned MUXCost = 64;
    unsigned AddCost = 64;
    unsigned MulCost = 128;
    unsigned ShiftCost = 256;
    unsigned MuxSizeCost = 48;
  }
}

//===----------------------------------------------------------------------===//
// Helper functions for reading function unit table from script.
template<class PropType>
static PropType getProperty(luabind::object &FUTable,
                              const std::string &PropName) {
  //luabind::object FUTable =
  //  Script->getRawObject("FU" + std::string(FUType::getTypeName()));

  if (luabind::type(FUTable) != LUA_TTABLE) return PropType();

  boost::optional<PropType> Result =
    luabind::object_cast_nothrow<PropType>(FUTable[PropName]);

  if (!Result) return PropType();

  return Result.get();
}

VFUDesc::VFUDesc(VFUs::FUTypes type, luabind::object FUTable)
  : ResourceType(type),
    Latency(getProperty<unsigned>(FUTable, "Latency")),
    StartInt(getProperty<unsigned>(FUTable, "StartInterval")),
    TotalRes(getProperty<unsigned>(FUTable, "TotalNumber")),
    MaxBitWidth(getProperty<unsigned>(FUTable, "OperandWidth")),
    Cost(getProperty<unsigned>(FUTable, "Cost")) {}

VFUMemBus::VFUMemBus(luabind::object FUTable)
  : VFUDesc(VFUs::MemoryBus,
            getProperty<unsigned>(FUTable, "Latency"),
            getProperty<unsigned>(FUTable, "StartInterval"),
            getProperty<unsigned>(FUTable, "TotalNumber"),
            getProperty<unsigned>(FUTable, "DataWidth")),
    AddrWidth(getProperty<unsigned>(FUTable, "AddressWidth")) {}

VFUBRam::VFUBRam(luabind::object FUTable)
  : VFUDesc(VFUs::BRam,
            getProperty<unsigned>(FUTable, "Latency"),
            getProperty<unsigned>(FUTable, "StartInterval"),
            getProperty<unsigned>(FUTable, "TotalNumber"),
            getProperty<unsigned>(FUTable, "DataWidth")),
  Template(getProperty<std::string>(FUTable, "Template")),
  InitFileDir(getProperty<std::string>(FUTable, "InitFileDir")){}

// Dirty Hack: anchor from SynSettings.h
SynSettings::SynSettings(StringRef Name, SynSettings &From)
  : PipeAlg(From.PipeAlg), SchedAlg(From.SchedAlg),
  ModName(Name), HierPrefix(""), IsTopLevelModule(false) {}

SynSettings::SynSettings(luabind::object SettingTable)
  : PipeAlg(SynSettings::DontPipeline),
    SchedAlg(SynSettings::ILP), IsTopLevelModule(true) {
  if (luabind::type(SettingTable) != LUA_TTABLE)
    return;

  if (boost::optional<std::string> Result =
      luabind::object_cast_nothrow<std::string>(SettingTable["ModName"]))
    ModName = Result.get();

  if (boost::optional<std::string> Result =
      luabind::object_cast_nothrow<std::string>(SettingTable["HierPrefix"]))
    HierPrefix = Result.get();

  if (boost::optional<ScheduleAlgorithm> Result =
    luabind::object_cast_nothrow<ScheduleAlgorithm>(SettingTable["Scheduling"]))
    SchedAlg = Result.get();

  if (boost::optional<PipeLineAlgorithm> Result =
    luabind::object_cast_nothrow<PipeLineAlgorithm>(SettingTable["Pipeline"]))
    PipeAlg = Result.get();

  if (boost::optional<bool> Result =
    luabind::object_cast_nothrow<bool>(SettingTable["isTopMod"]))
    IsTopLevelModule = Result.get();
}

unsigned FuncUnitId::getTotalFUs() const {
  // If the function unit is binded, there is only one function unit with
  // the specific function unit id available.
  if (isBound()) return 1;

  // Else we can just choose a function unit from all available function units.
  return getFUDesc(getFUType())->getTotalRes();
}

void FuncUnitId::print(raw_ostream &OS) const {
  OS << VFUs::VFUNames[getFUType()];
  // Print the function unit id if necessary.
  if (isBound()) OS << " Bound to " << getFUNum();
}

void FuncUnitId::dump() const {
  print(dbgs());
}

std::string VFUBRam::generateCode(const std::string &Clk, unsigned Num, unsigned ID,
                                  unsigned DataWidth, unsigned AddrWidth) const {
  std::string Script;
  raw_string_ostream ScriptBuilder(Script);

  std::string ResultName = "bram" + utostr_32(Num) + "_"
                           + utostr_32(DataWidth) + "x" + utostr_32(AddrWidth)
                           + "_result";
  // FIXME: Use LUA api directly?
  // Call the preprocess function.
  ScriptBuilder <<
    /*"local " <<*/ ResultName << ", message = require \"luapp\" . preprocess {"
  // The inpute template.
                << "input=[=[" << Template <<"]=],"
  // And the look up.
                << "lookup={ "
                << "datawidth=" << DataWidth << ", addrwidth=" << AddrWidth
                << ", num=" << Num << ", ID="<< ID <<", clk='" << Clk
  // End the look up and the function call.
                << "'}}\n";
  DEBUG(ScriptBuilder << "print(" << ResultName << ")\n");
  DEBUG(ScriptBuilder << "print(message)\n");
  ScriptBuilder.flush();
  DEBUG(dbgs() << "Going to execute:\n" << Script);

  SMDiagnostic Err;
  if (!scriptEngin().runScriptStr(Script, Err))
    report_fatal_error("Block Ram code generation:" + Err.getMessage());

  return scriptEngin().getValueStr(ResultName);
}



void VFUBRam::printConstantArray(raw_ostream &Out, ConstantArray *CPA, 
  unsigned DataWidth, bool Static) {
    // As a special case, print the array as a string if it is an array of
    // ubytes or an array of sbytes with positive values.
    //
    const Type *ETy = CPA->getType()->getElementType();
    bool isString = (ETy == Type::getInt8Ty(CPA->getContext()) ||
      ETy == Type::getInt8Ty(CPA->getContext())); 

    // Make sure the last character is a null char, as automatically added by C
    /*if (isString && (CPA->getNumOperands() == 0 ||
      !cast<Constant>(*(CPA->op_end()-1))->isNullValue()))
      isString = false;*/
    }
    //   }
}

void VFUBRam::printConstant(raw_ostream &Out, Constant *CPV, 
                     unsigned DataWidth,bool Static) {
   /*if (ConstantInt *CI = dyn_cast<ConstantInt>(CPV)) {
      const Type* Ty = CI->getType();
      std::string tempstr;
      if (Ty == Type::getInt1Ty(CPV->getContext())) {
        tempstr = (CI->getZExtValue() ? '1' : '0');
      }
      else {
 /* if (ConstantInt *CI = dyn_cast<ConstantInt>(CPV)) {
    const Type* Ty = CI->getType();
    std::string tempstr;
    if (Ty == Type::getInt1Ty(CPV->getContext()))
      tempstr = (CI->getZExtValue() ? '1' : '0');
    else if (Ty == Type::getInt32Ty(CPV->getContext()))
      tempstr = utohexstr(CI->getZExtValue());
    else if (Ty->getPrimitiveSizeInBits() > 32)
      tempstr = utohexstr(CI->getZExtValue());
    else {
      if (CI->isMinValue(true))
        tempstr = utohexstr(CI->getZExtValue());
      else
        tempstr = utohexstr(CI->getSExtValue());
    }*/

    }

    switch (CPV->getType()->getTypeID()) {
    case Type::ArrayTyID:
      // Use C99 compound expression literal initializer syntax.
      if (!Static) {
        Out << "(";
        printType(Out, CPV->getType());
        Out << ")";
      }
      //Out << "{ "; // Arrays are wrapped in struct types.
      if (ConstantArray *CA = dyn_cast<ConstantArray>(CPV)) {
        printConstantArray(Out, CA, DataWidth, Static);
      } else {
        assert(isa<ConstantAggregateZero>(CPV) || isa<UndefValue>(CPV));
        const ArrayType *AT = cast<ArrayType>(CPV->getType());
        //Out << '{';
        if (AT->getNumElements()) {
          //Out << ' ';
          Constant *CZ = Constant::getNullValue(AT->getElementType());
          printConstant(Out, CZ, DataWidth, Static);
          for (unsigned i = 1, e = AT->getNumElements(); i != e; ++i) {
            //Out << ", ";
            Out << "\n";
            printConstant(Out, CZ, DataWidth, Static);
          }
        }
        //Out << " }";
      }
      // Out << " }"; // Arrays are wrapped in struct types.
      break;
    default:
#ifndef NDEBUG
      errs() << "Unknown constant type: " << *CPV << "\n";
#endif
      llvm_unreachable(0);
    }
}

void VFUBRam::printZeros(raw_ostream &Out, unsigned int NumElement, 
  unsigned int Bytes){
    std::string element = utostr_32(Bytes)+"'h";
    std::string S;
    for(unsigned int i = 0; i < NumElement; ++i) 
      S += element;
    Out << S;
}

void VFUBRam::generateInitFile(unsigned BramID, unsigned BramNum,
                                   unsigned DataWidth, const Value* Initializer, 
                                   unsigned NumElem) {
  std::string FileName = InitFileDir + "bram" + utostr_32(BramNum) + '_' 
    + utostr_32(BramID) + ".txt";
  raw_ostream& InitS = scriptEngin().getOutputFileStream(FileName);
  if (GlobalVariable *GV = const_cast<GlobalVariable*>
     (dyn_cast<GlobalVariable>(Initializer))) {
      Constant* CPV = GV->getInitializer();
      //There is initial value, print the constant array.
      printConstant(InitS, CPV, DataWidth, 1); 
  } else {
    //There is no initial value, print Zeros to the InitS.
    printZeros(InitS, NumElem, DataWidth/8); 
  }
}

std::string VFUs::instantiatesModule(const std::string &ModName, unsigned ModNum,
                                     ArrayRef<std::string> Ports) {
  std::string Script;
  raw_string_ostream ScriptBuilder(Script);

  luabind::object ModTemplate = scriptEngin().getModTemplate(ModName);
  std::string Template = getProperty<std::string>(ModTemplate, "InstTmplt");

  std::string ResultName = ModName + utostr_32(ModNum) + "_inst";
  // FIXME: Use LUA api directly?
  // Call the preprocess function.
  ScriptBuilder <<
    /*"local " <<*/ ResultName << ", message = require \"luapp\" . preprocess {"
  // The inpute template.
                << "input=[=[";
  if (Template.empty()) {
    ScriptBuilder << "// " << ModName << " not available!\n";
    errs() << "Instantiation template for external module :" << ModName
           << " not available!\n";
    // Dirty Hack: create the finish signal.
    return "parameter " + Ports[3] + "= 1'b1;\n";
  } else
    ScriptBuilder << Template;
  ScriptBuilder <<"]=],"
  // And the look up.
                   "lookup={ num=" << ModNum << ", clk='" << Ports[0]
                << "', rst = '" <<  Ports[1] << "', en = '" <<  Ports[2]
                << "', fin = '" <<  Ports[3];
  // The output ports.
  for (unsigned i = 4, e = Ports.size(); i < e; ++i)
    ScriptBuilder << "', out" << (i - 4) << " = '" <<  Ports[i];

  // End the look up and the function call.
  ScriptBuilder << "'}}\n";
  DEBUG(ScriptBuilder << "print(" << ResultName << ")\n");
  DEBUG(ScriptBuilder << "print(message)\n");
  ScriptBuilder.flush();
  DEBUG(dbgs() << "Going to execute:\n" << Script);

  SMDiagnostic Err;
  if (!scriptEngin().runScriptStr(Script, Err))
    report_fatal_error("External module instantiation:" + Err.getMessage());

  return scriptEngin().getValueStr(ResultName);
}

std::string VFUs::startModule(const std::string &ModName, unsigned ModNum,
                              ArrayRef<std::string> InPorts) {
  std::string Script;
  raw_string_ostream ScriptBuilder(Script);

  luabind::object ModTemplate = scriptEngin().getModTemplate(ModName);
  std::string Template = getProperty<std::string>(ModTemplate, "StartTmplt");

  std::string ResultName = ModName + utostr_32(ModNum) + "_start";
  // FIXME: Use LUA api directly?
  // Call the preprocess function.
  ScriptBuilder <<
    /*"local " <<*/ ResultName << ", message = require \"luapp\" . preprocess {"
  // The inpute template.
                   "input=[=[";
  if (Template.empty()) {
    ScriptBuilder << "// " << ModName << " not available!\n";
    errs() << "Start template for external Module :" << ModName
           << " not available!\n";
  } else
    ScriptBuilder << Template;
  ScriptBuilder << "]=],"
  // And the look up.
                   "lookup={ num=" << ModNum;
  // The input ports.
  for (unsigned i = 0, e = InPorts.size(); i < e; ++i)
    ScriptBuilder << ", in" << i << " = [=[" <<  InPorts[i] << "]=]";

  // End the look up and the function call.
  ScriptBuilder << "}}\n";
  DEBUG(ScriptBuilder << "print(" << ResultName << ")\n");
  DEBUG(ScriptBuilder << "print(message)\n");
  ScriptBuilder.flush();
  DEBUG(dbgs() << "Going to execute:\n" << Script);

  SMDiagnostic Err;
  if (!scriptEngin().runScriptStr(Script, Err))
    report_fatal_error("External module starting:" + Err.getMessage());

  return scriptEngin().getValueStr(ResultName);
}
