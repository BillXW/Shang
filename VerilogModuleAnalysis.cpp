//===--------------------VerilogModuleAnalysis.cpp------------------------===//
//
// Copyright: 2011 by SYSU EDA Group. all rights reserved.
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
//
//
//
//===----------------------------------------------------------------------===//
#include "vtm/VerilogModuleAnalysis.h"

using namespace llvm;

INITIALIZE_PASS(VerilogModuleAnalysis, "verilog-module-analysis",
                "verilog module analysis", false, true)

char VerilogModuleAnalysis::ID = 0;

VerilogModuleAnalysis::VerilogModuleAnalysis() : MachineFunctionPass(ID) {
    initializeVerilogModuleAnalysisPass(*PassRegistry::getPassRegistry());
}

bool VerilogModuleAnalysis::runOnMachineFunction(MachineFunction &MF){
  return false;
}

Pass *llvm::createVerilogModuleAnalysisPass() {
  return new VerilogModuleAnalysis();
}
