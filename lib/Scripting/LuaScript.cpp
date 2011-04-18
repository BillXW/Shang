//===----- Scripting.cpp - Scripting engine for verilog backend --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// 
//
//===----------------------------------------------------------------------===//
#include "vtm/FUInfo.h"
#include "vtm/SystemInfo.h"
#include "vtm/LuaScript.h"

#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/ADT/STLExtras.h"

// Include the lua headers (the extern "C" is a requirement because we're
// using C++ and lua has been compiled as C code)
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

using namespace llvm;

LuaScript::LuaScript() : State(lua_open()) {}

LuaScript::~LuaScript() {
  DeleteContainerSeconds(Files);
  lua_close(State);
}

void LuaScript::keepAllFiles() {
  for (FileMapTy::iterator I = Files.begin(), E = Files.end(); I != E; ++I)
    I->second->keep();
}

bool LuaScript::runScript(const std::string &ScriptPath, SMDiagnostic &Err) {
  luabind::open(State);

  // Bind the C++ classes.
  luabind::module(State)[
    luabind::class_<FUInfo>("FUInfo")
      .def("setupMemBus", &FUInfo::setupMemBus)
      .def("setupAddSub", &FUInfo::setupBinOpRes<VFUs::AddSub>)
      .def("setupShift",  &FUInfo::setupBinOpRes<VFUs::Shift>)
      .def("setupMult",   &FUInfo::setupBinOpRes<VFUs::Mult>)
      .def("setupBRam",  &FUInfo::setupBinOpRes<VFUs::BRam>),

      luabind::class_<ConstraintsInfo>("ConstraintsInfo")
      .enum_("PipeLine")[
        luabind::value("IMS", ConstraintsInfo::IMS),
          luabind::value("FDMS", ConstraintsInfo::FDMS),
          luabind::value("DontPipeline", ConstraintsInfo::DontPipeline)
      ]
    .enum_("Schedule")[
      luabind::value("FDS", ConstraintsInfo::FDS),
        luabind::value("FDLS", ConstraintsInfo::FDLS),
        luabind::value("ILP", ConstraintsInfo::ILP)
    ]
    .def_readwrite("PipeLine", &ConstraintsInfo::PipeAlg)
      .def_readwrite("Schedule", &ConstraintsInfo::SchedAlg),

      luabind::class_<SystemInfo>("SystemInfo")
      .def("setHardware", &SystemInfo::setHardware)
      .def("getInfo", &SystemInfo::getInfo)
      .def_readwrite("HwModName", &SystemInfo::hwModName)
  ];

  // Bind the object.
  luabind::globals(State)["FUs"] = &FUI;
  luabind::globals(State)["System"] = &SystemI;

  // Run the script.
  if (luaL_dofile(State, ScriptPath.c_str())) {
    Err = SMDiagnostic(ScriptPath, lua_tostring(State, -1));
    return false;
  }

  return true;
}

raw_ostream &LuaScript::getOutputStream(const std::string &Name) {
  std::string Path = getValueStr(Name);

  // Try to return the existing file.
  FileMapTy::const_iterator at = Files.find(Path);

  if (at != Files.end()) return at->second->os();

  if (Path.empty()) return outs();

  std::string error;

  tool_output_file *NewFile = new tool_output_file(Path.c_str(), error);
  // TODO: Support binary file.
  Files.insert(std::make_pair(Path, NewFile));

  return NewFile->os();
}

std::string LuaScript::getTargetDataStr() const {
  std::string ret;
  raw_string_ostream s(ret);

  // FIXME: Set the correct endian.
  s << 'e';

  s << '-';

  // Setup the address width (pointer width).
  unsigned PtrSize = FUI.getFUDesc<VFUMemBus>()->getAddrWidth();
  s << "p:" << PtrSize << ':' << PtrSize << ':' << PtrSize << '-';

  // FIXME: Setup the correct integer layout.
  s << "i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-";
  s << "n8:16:32:64";

  s.flush();
  return s.str();
}

ManagedStatic<LuaScript> Script;

const FUInfo &llvm::vtmfus() {
  return Script->FUI;
}

const SystemInfo &llvm::sysinfo() {
  return Script->SystemI;
}

LuaScript &llvm::getScript() {
  return *Script;
}
