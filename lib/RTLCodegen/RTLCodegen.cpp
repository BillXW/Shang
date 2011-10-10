//===- Writer.cpp - VTM machine instructions to RTL verilog  ----*- C++ -*-===//
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
// This file implement the RTLCodegen pass, which write VTM machine instructions
// in form of RTL verilog code.
//
//===----------------------------------------------------------------------===//

#include "vtm/Passes.h"
#include "vtm/VerilogAST.h"
#include "vtm/MicroState.h"
#include "vtm/VFInfo.h"
#include "vtm/LangSteam.h"
#include "vtm/BitLevelInfo.h"
#include "vtm/VRegisterInfo.h"
#include "vtm/VInstrInfo.h"
#include "vtm/Utilities.h"

#include "llvm/Constants.h"
#include "llvm/GlobalVariable.h"
#include "llvm/Type.h"
#include "llvm/Module.h"
#include "llvm/Target/Mangler.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#define DEBUG_TYPE "vtm-rtl-codegen"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(TotalRegisterBits,
          "Number of total register bits in synthesised modules");
namespace {
class RTLCodegen : public MachineFunctionPass {
  vlang_raw_ostream Out;

  const Module *M;
  MachineFunction *MF;
  TargetData *TD;
  VFInfo *FInfo;
  MachineRegisterInfo *MRI;
  VASTModule *VM;
  Mangler *Mang;

  unsigned TotalFSMStatesBit, CurFSMStateNum, SignedWireNum;

  // Mapping success fsm state to their predicate in current state.
  typedef std::map<MachineBasicBlock*, std::string> PredMapTy;

  void emitFunctionSignature(const Function *F);
  void emitCommonPort(unsigned FNNum);

  struct MuxBuilder {
    std::string MuxLogic;
    vlang_raw_ostream MuxLogicS;
    std::string MuxWiresDecl;
    vlang_raw_ostream MuxWiresDeclS;

    MuxBuilder()
      : MuxLogic("//Mux\n"),
        MuxLogicS(*new raw_string_ostream(MuxLogic), true, 2),
        MuxWiresDecl("// Mux\n"),
        MuxWiresDeclS(*new raw_string_ostream(MuxWiresDecl), true, 2) {
      MuxLogicS << "always @(*)";
      MuxLogicS.enter_block(" // begin mux logic\n");
      MuxLogicS.switch_begin("1'b1");
    }

    void addOutput(const std::string &OutputName, unsigned Bitwidth) {
      MuxWiresDeclS << "reg ";
      if (Bitwidth > 1) MuxWiresDeclS << verilogBitRange(Bitwidth, 0, false);
      MuxWiresDeclS  << OutputName << "_mux_wire = "
                     << verilogConstToStr(0, Bitwidth, false)<< ";\n";
      MuxWiresDeclS << "assign " << OutputName << " = " << OutputName
                    << "_mux_wire;\n";
    }

    void assignValueInCase(const std::string &Dst, const std::string &Src) {
      MuxLogicS << Dst << "_mux_wire = " << Src << ";\n";
    }

    void writeToStream(raw_ostream &S) {
      MuxWiresDeclS.flush();
      S << MuxWiresDecl << "\n";

      MuxLogicS.exit_block();
      MuxLogicS.switch_end();
      MuxLogicS.exit_block(" // end mux logic\n\n\n\n");
      MuxLogicS.flush();
      S << MuxLogic;
    }
  };

  struct MemBusBuilder {
    VASTModule *VM;
    VFUMemBus *Bus;
    unsigned BusNum;
    std::string EnableLogic;
    vlang_raw_ostream EnableLogicS;
    MuxBuilder BusMux;

    void createOutputPort(const std::string &PortName, unsigned BitWidth,
                          bool isEn = false) {
      // We need to create multiplexer to allow current module and its submodules
      // share the bus.
      std::string PortReg = PortName + "_r";
      VM->addRegister(PortReg, BitWidth);
      VM->addOutputPort(PortName, BitWidth, VASTModule::Others, false);
      if (isEn) {
        EnableLogicS << "assign " << PortName << " = " << PortReg;
        // The top level module control the memory bus by default.
        BusMux.MuxLogicS.match_case("default");
      } else {
        BusMux.addOutput(PortName, BitWidth);
        BusMux.assignValueInCase(PortName, PortReg);
      }
    }

    std::string addSubModulePort(const std::string &PortName, unsigned BitWidth,
                                 const std::string &SubModuleName,
                                 bool isOut = true, bool isEn = false){
      std::string ConnectedWire = PortName;
      if (isOut) { // Create extra wire for bus mux.
        ConnectedWire = SubModuleName + "_" + ConnectedWire;
        VM->addWire(ConnectedWire, BitWidth);
        if (isEn) {
          EnableLogicS << " | " << ConnectedWire ;
          BusMux.MuxLogicS.exit_block();
          BusMux.MuxLogicS.match_case(ConnectedWire);
        } else
          BusMux.assignValueInCase(PortName, ConnectedWire);
      }

      return "." + PortName + "(" +  ConnectedWire + "),\n\t";
    }

    void addSubModule(const std::string &SubModuleName, raw_ostream &S) {
      S << addSubModulePort(VFUMemBus::getEnableName(BusNum), 1, SubModuleName,
                            true, true);
      S << addSubModulePort(VFUMemBus::getWriteEnableName(BusNum), 1,
                            SubModuleName);
      S << addSubModulePort(VFUMemBus::getAddrBusName(BusNum),
                            Bus->getAddrWidth(), SubModuleName);
      S << addSubModulePort(VFUMemBus::getInDataBusName(BusNum),
                            Bus->getDataWidth(), SubModuleName, false);
      S << addSubModulePort(VFUMemBus::getOutDataBusName(BusNum),
                            Bus->getDataWidth(), SubModuleName);
      S << addSubModulePort(VFUMemBus::getByteEnableName(BusNum),
                            Bus->getDataWidth()/8, SubModuleName);
      S << addSubModulePort(VFUMemBus::getReadyName(BusNum), 1, SubModuleName,
                            false);
    }

    MemBusBuilder(VASTModule *M, unsigned N)
      : VM(M), Bus(getFUDesc<VFUMemBus>()), BusNum(N),
      EnableLogic("  // Membus enables\n"),
      EnableLogicS(*new raw_string_ostream(EnableLogic), true, 2) {
      // Build the ports for current module.
      FuncUnitId ID(VFUs::MemoryBus, BusNum);
      // We need to create multiplexer to allow current module and its submodules
      // share the memory bus.

      VM->setFUPortBegin(ID);
      // Control ports.
      createOutputPort(VFUMemBus::getEnableName(BusNum), 1, true);
      createOutputPort(VFUMemBus::getWriteEnableName(BusNum), 1);

      // Address port.
      createOutputPort(VFUMemBus::getAddrBusName(BusNum), Bus->getAddrWidth());
      // Data ports.
      VM->addInputPort(VFUMemBus::getInDataBusName(BusNum), Bus->getDataWidth());
      createOutputPort(VFUMemBus::getOutDataBusName(BusNum), Bus->getDataWidth());
      // Byte enable.
      createOutputPort(VFUMemBus::getByteEnableName(BusNum), Bus->getDataWidth() / 8);
      // Bus ready.
      VM->addInputPort(VFUMemBus::getReadyName(BusNum), 1);
    }

    void writeToStream(raw_ostream &S) {
      EnableLogicS.flush();
      S << EnableLogic << ";\n";
      BusMux.writeToStream(S);
    }
  };

  /// emitAllocatedFUs - Set up a vector for allocated resources, and
  /// emit the ports and register, wire and datapath for them.
  void emitAllocatedFUs();

  void emitIdleState();
  // We need create the slots for the empty slot.
  void emitSlotsForEmptyState(unsigned Slot, unsigned EndSlot, unsigned II) {
    // Dirty Hack: When we need is the slot before current slot, we issue the
    // control operation of current slot there.
    --Slot;

    while (Slot < EndSlot) {
      (void) VM->getSlot(Slot);
      Slot += II;
    }
  }

  void emitBasicBlock(MachineBasicBlock &MBB);

  void emitAllSignals();
  void emitSignals(const TargetRegisterClass *RC,
                   const std::string &Prefix);

  void clear();

  inline std::string getucStateEnable(ucState &State) {
    return getucStateEnable(State.getSlot());
  }

  inline std::string getucStateEnable(unsigned Slot) {
    return "Slot" + utostr_32(Slot) + "Active";
  }

  // Emit the operations in the first micro state in the FSM state when we are
  // jumping to it.
  void emitFirstCtrlState(MachineBasicBlock *DstBB);

  void emitDatapath(ucState &State);

  void emitUnaryOp(ucOp &UnOp, const std::string &Operator);
  void emitBinaryOp(ucOp &BinOp, const std::string &Operator);

  void emitOpShift(ucOp &OpSHT, const std::string &Operator);

  void emitOpSel(ucOp &OpSel);

  void emitOpAdd(ucOp &OpAdd);
  void emitOpMult(ucOp &OpMult);

  void emitOpBitCat(ucOp &OpBitCat);
  void emitOpBitSlice(ucOp &OpBitSlice);
  void emitOpBitRepeat(ucOp &OpBitRepeat);

  void emitImplicitDef(ucOp &ImpDef);

  void emitCtrlOp(ucState &State, PredMapTy &PredMap);

  static void printPredicate(ucOperand &Pred, raw_ostream &SS) {
    if (Pred.getReg()) {
      SS << '(';
      if (Pred.isPredicateInverted()) SS << '~';
      Pred.print(SS, 1, 0, true);
      SS << ')';
    } else
      SS << "1'b1";
  }

  void emitOpInternalCall(ucOp &OpInternalCall, VASTSlot *CurSlot);
  void emitOpReadReturn(ucOp &OpReadSymbol, VASTSlot *CurSlot);
  void emitOpRetVal(ucOp &OpRetVal);
  void emitOpRet(ucOp &OpRet, VASTSlot *CurSlot);
  // Special ucop for connecting wires.
  void emitOpConnectWire(ucOp &Op);
  void emitOpCopy(ucOp &OpCopy);
  void emitOpReadFU(ucOp &OpRdFU, VASTSlot *CurSlot);
  void emitOpMemTrans(ucOp &OpMemAccess, VASTSlot *CurSlot);
  void emitOpBRam(ucOp &OpBRam);

  std::string getSubModulePortName(unsigned FNNum,
                                   const std::string PortName) const {
    return "SubMod" + utostr(FNNum) + "_" + PortName;
  }

public:
  /// @name FunctionPass interface
  //{
  static char ID;
  RTLCodegen(raw_ostream &O);
  RTLCodegen() : MachineFunctionPass(ID) {
    assert( 0 && "Cannot construct the class without the raw_stream!");
  }

  ~RTLCodegen();

  bool doInitialization(Module &M);

  bool doFinalization(Module &M) {
    delete Mang;
    return false;
  }

  bool runOnMachineFunction(MachineFunction &MF);

  void releaseMemory() { clear(); }
  virtual void print(raw_ostream &O, const Module *M) const;
  //}
};

}

//===----------------------------------------------------------------------===//
char RTLCodegen::ID = 0;

Pass *llvm::createRTLCodegenPass(raw_ostream &O) {
  return new RTLCodegen(O);
}

INITIALIZE_PASS_BEGIN(RTLCodegen, "vtm-rtl-info",
                      "Build RTL Verilog module for synthesised function.",
                      false, true)
INITIALIZE_PASS_END(RTLCodegen, "vtm-rtl-info",
                    "Build RTL Verilog module for synthesised function.",
                    false, true)

RTLCodegen::RTLCodegen(raw_ostream &O) : MachineFunctionPass(ID), Out(O) {
  initializeRTLCodegenPass(*PassRegistry::getPassRegistry());
}

bool RTLCodegen::doInitialization(Module &Mod) {
  MachineModuleInfo *MMI = getAnalysisIfAvailable<MachineModuleInfo>();
  TD = getAnalysisIfAvailable<TargetData>();

  assert(MMI && TD && "MachineModuleInfo and TargetData will always available"
                      " in a machine function pass!");
  Mang = new Mangler(MMI->getContext(), *TD);
  M = &Mod;

  // Define the blockram base address.
  Out << "`ifdef __VERILATOR_SIM\n";
  for(Module::global_iterator GI = Mod.global_begin(), E = Mod.global_end();
      GI != E; ++GI){
      Out << "import \"DPI-C\" function chandle vlt_"
          << VBEMangle(GI->getNameStr()) << "();\n";

      Out << "`define gv" << VBEMangle(GI->getNameStr())
          << " vlt_" << VBEMangle(GI->getNameStr()) << "()\n";
  }

  Out << "`endif\n\n\n";

  return false;
}

bool RTLCodegen::runOnMachineFunction(MachineFunction &F) {
  MF = &F;
  FInfo = MF->getInfo<VFInfo>();
  MRI = &MF->getRegInfo();

  DEBUG(
    Out << "`ifdef wtf_is_this\n" << "Function for RTL Codegen:\n";
    printVMF(Out, F);
    Out << "`endif\n";
  );

  SignedWireNum = 0;
  // Reset the current fsm state number.
  CurFSMStateNum = 0;

  // FIXME: Demangle the c++ name.
  // Dirty Hack: Force the module have the name of the hw subsystem.
  VM = FInfo->getRtlMod();
  emitFunctionSignature(F.getFunction());

  // Emit control register and idle state
  unsigned totalFSMStates = MF->size() + 1;
  TotalFSMStatesBit = Log2_32_Ceil(totalFSMStates);

  VM->addRegister("NextFSMState", TotalFSMStatesBit);

  emitIdleState();

  emitAllSignals();
  emitAllocatedFUs();

  for (MachineFunction::iterator I = MF->begin(), E = MF->end(); I != E; ++I) {
    MachineBasicBlock &BB = *I;
    emitBasicBlock(BB);
  }

  // Dirty Hack: Disable the "Warning-WIDTH" from verilator.
  // FIXME: We should only generate this when we are going to simulate the
  // module with verilator.
  Out << "/* verilator lint_off WIDTH */\n";

  // Write buffers to output
  VM->printModuleDecl(Out);
  Out.module_begin();
  Out << "\n\n";
  // States
  Out << "// States\n";
  Out << VM->getStateDeclStr();
  Out << "\n\n";
  // Reg and wire
  Out << "// Reg and wire decl\n";
  VM->printSignalDecl(Out);
  Out << "\n\n";
  // Slot active signals.
  VM->printSlotActives(Out);
  // Datapath
  Out << "// Datapath\n";
  Out << VM->getDataPathStr();
  VM->printDatapath(Out);

  Out << "\n\n";
  Out << "// Always Block\n";
  Out.always_ff_begin();

  VM->printRegisterReset(Out);
  Out.else_begin();
  //Out.switch_begin("NextFSMState");
  Out << VM->getControlBlockStr();

  VM->printSlotCtrls(Out);

  Out.always_ff_end();

  Out.module_end();
  Out.flush();

  return false;
}

void RTLCodegen::clear() {
  VM = 0;
}

void RTLCodegen::print(raw_ostream &O, const Module *M) const {

}

void RTLCodegen::emitFunctionSignature(const Function *F) {
  raw_ostream &S = VM->getDataPathBuffer();
  unsigned FNNum = FInfo->getCalleeFNNum(F->getName());
  for (Function::const_arg_iterator I = F->arg_begin(), E = F->arg_end();
      I != E; ++I) {
    const Argument *Arg = I;
    std::string Name = Arg->getNameStr();
    unsigned BitWidth = TD->getTypeSizeInBits(Arg->getType());
    // Add port declaration.
    if (FNNum == 0)
      VM->addInputPort(Name, BitWidth, VASTModule::ArgPort);
    else {
      std::string RegName = getSubModulePortName(FNNum, Name);
      VM->addRegister(RegName, BitWidth);
      S << "." << Name << '(' << RegName << "),\n\t";
    }
  }

  const Type *RetTy = F->getReturnType();
  if (!RetTy->isVoidTy()) {
    assert(RetTy->isIntegerTy() && "Only support return integer now!");
    unsigned BitWidth = TD->getTypeSizeInBits(RetTy);
    if (FNNum == 0)
      VM->addOutputPort("return_value", BitWidth, VASTModule::RetPort);
    else {
      std::string WireName = getSubModulePortName(FNNum, "return_value");
      VM->addWire(WireName, BitWidth);
      S << ".return_value(" << WireName << "),\n\t";
    }
  }

  emitCommonPort(FNNum);
}

void RTLCodegen::emitIdleState() {
  vlang_raw_ostream &CtrlS = VM->getControlBlockBuffer();

  CtrlS.if_begin(getucStateEnable(0), "// Idle state\n");
  //CtrlS.match_case("state_idle");
  // Idle state is always ready.
  CtrlS.if_begin("start");
  // The module is busy now
  MachineBasicBlock *EntryBB =  GraphTraits<MachineFunction*>::getEntryNode(MF);
  VASTSlot *IdleSlot = VM->getSlot(0);
  VASTValue &StartPort = VM->getPort(VASTModule::Start);
  IdleSlot->addNextSlot(FInfo->getStartSlotFor(EntryBB),
                        &StartPort);
  IdleSlot->addNextSlot(0, VASTCnd(&StartPort, true));

  // Always Disable the finish signal.
  IdleSlot->addDisable(&VM->getPort(VASTModule::Finish));
  emitFirstCtrlState(EntryBB);
  // End if-else
  CtrlS.exit_block();
  // End idle state.
  CtrlS.exit_block("// End idle state\n");
}

void RTLCodegen::emitBasicBlock(MachineBasicBlock &MBB) {
  unsigned totalSlot = FInfo->getTotalSlotFor(&MBB);
  unsigned II = FInfo->getIIFor(&MBB);
  unsigned EndSlot = FInfo->getEndSlotFor(&MBB);
  PredMapTy NextStatePred;

  vlang_raw_ostream &CtrlS = VM->getControlBlockBuffer();

  // State information.
  CtrlS << "// BB#" << MBB.getNumber() << " Total Slot: " << totalSlot
        << " II: " << II;
  if (II != totalSlot) CtrlS << " pipelined";
  CtrlS << '\n';

  MachineBasicBlock::iterator I = MBB.getFirstNonPHI(),
                              E = MBB.getFirstTerminator();
  //ucState FstCtrl(I);
  //if (FstCtrl.empty())
  //  emitSlotsForEmptyState(FstCtrl.getSlot(), EndSlot, II);

  // FIXME: Refactor the loop.
  while(++I != E) {
    ucState CurDatapath = *I;
    // Emit the datepath of current state.
    emitDatapath(CurDatapath);

    // Emit next ucOp.
    ucState NextControl = *++I;
    CtrlS << "// Slot " << NextControl.getSlot() << '\n';
    if (NextControl.empty())
      emitSlotsForEmptyState(NextControl.getSlot(), EndSlot, II);

    emitCtrlOp(NextControl, NextStatePred);
  };
}

void RTLCodegen::emitCommonPort(unsigned FNNum) {
  if (FNNum == 0) { // If F is current function.
    VM->addInputPort("clk", 1, VASTModule::Clk);
    VM->addInputPort("rstN", 1, VASTModule::RST);
    VM->addInputPort("start", 1, VASTModule::Start);
    VM->addOutputPort("fin", 1, VASTModule::Finish);
  } else { // It is a callee function, emit the signal for the sub module.
    std::string StartPortName = getSubModulePortName(FNNum, "start");
    std::string FinPortName = getSubModulePortName(FNNum, "fin");
    VM->addRegister(StartPortName, 1);
    VM->addWire(FinPortName, 1);
    // Connect to the ports
    raw_ostream &S = VM->getDataPathBuffer();
    S << ".clk(clk),\n\t.rstN(rstN),\n\t";
    S << ".start(" << StartPortName << "),\n\t";
    S << ".fin(" <<FinPortName << ")";
  }
}

void RTLCodegen::emitAllocatedFUs() {
  // Dirty Hack: only Memory bus supported at this moment.
  typedef VFInfo::const_id_iterator id_iterator;

  //for (id_iterator I = FInfo->id_begin(VFUs::MemoryBus),
  //     E = FInfo->id_end(VFUs::MemoryBus); I != E; ++I) {
  // FIXME: In fact, *I return the FUId instead of FUNum.
  // DIRTYHACK: Every module use memory bus 0, connect the bus.
  MemBusBuilder MBBuilder(VM, 0);

  //}

  raw_ostream &S = VM->getDataPathBuffer();
  VFUBRam *BlockRam = getFUDesc<VFUBRam>();

  for (id_iterator I = FInfo->id_begin(VFUs::BRam),
       E = FInfo->id_end(VFUs::BRam); I != E; ++I) {
    FuncUnitId ID = *I;
    const VFInfo::BRamInfo &Info = FInfo->getBRamInfo(ID.getFUNum());

    S << BlockRam->generateCode(VM->getPortName(VASTModule::Clk), ID.getFUNum(),
                                Info.ElemSizeInBytes * 8,
                                Log2_32_Ceil(Info.NumElem))
      << '\n';
  }

  typedef VFInfo::const_fn_iterator fn_iterator;
  for (fn_iterator I = FInfo->fn_begin(), E = FInfo->fn_end(); I != E; ++I) {
    if (const Function *Callee = M->getFunction(I->getKey())) {
      if (!Callee->isDeclaration()) {
        S << getSynSetting(Callee->getName())->getModName() << ' '
          << getSubModulePortName(I->second, "_inst")
          << "(\n\t";
        MBBuilder.addSubModule(getSubModulePortName(I->second, "_inst"), S);
        emitFunctionSignature(Callee);
        S << ");\n";
        continue;
      }
    }

    unsigned FNNum = I->second;
    std::string Ports[5] = {
      VM->getPortName(VASTModule::Clk),
      VM->getPortName(VASTModule::RST),
      getSubModulePortName(FNNum, "start"),
      getSubModulePortName(FNNum, "fin"),
      getSubModulePortName(FNNum, "return_value")
    };
    // Add the finsh signal to the signal list.
    VM->addRegister(Ports[2], 1);

    // Else ask the constraint about how to instantiates this submodule.
    S << "// External module: " << I->getKey() << '\n';
    S << VFUs::instantiatesModule(I->getKey(), FNNum, Ports);
  }

  // Write the memory bus mux.
  MBBuilder.writeToStream(S);
}

void RTLCodegen::emitAllSignals() {
  // We organize the registers in a module like 64 bits width ram, and we treat
  // the register number as the address of the register in the 'ram'. The
  // address of the register is always aligned with the register's size in byte,
  // that is, the register number of a 16 bits register will always divisible by
  // 2. So we can address the register as if it is in the ram. for example, if
  // we have a 32 bit register whose number is 20, that means it located at the
  // high part of the 2th 64 bits physics register.

  // Emit the register with max word length.
  for (VFInfo::phyreg_iterator I = FInfo->phyreg_begin(8),
       E = FInfo->phyreg_end(8); I < E; ++I) {
    VM->addRegister("phy_reg" + utostr(*I), 64);
    TotalRegisterBits += 64;
  }

  emitSignals(VTM::DRRegisterClass, "reg");
  emitSignals(VTM::PHIRRegisterClass, "reg");

  emitSignals(VTM::WireRegisterClass, "wire");
  // FIXME: There are function units.
  emitSignals(VTM::RADDRegisterClass, "wire");
  emitSignals(VTM::RMULRegisterClass, "wire");
  emitSignals(VTM::RSHTRegisterClass, "wire");
}

void RTLCodegen::emitSignals(const TargetRegisterClass *RC,
                             const std::string &Prefix) {
  // And Emit the wires defined in this module.
  const std::vector<unsigned>& Wires = MRI->getRegClassVirtRegs(RC);

  for (std::vector<unsigned>::const_iterator I = Wires.begin(), E = Wires.end();
       I != E; ++I) {
    unsigned SignalNum = *I;
    MachineRegisterInfo::def_iterator DI = MRI->def_begin(SignalNum);
    if (DI == MRI->def_end()) continue;

    const ucOperand &Op = cast<ucOperand>(DI.getOperand());
    unsigned Bitwidth = Op.getBitWidth();
    VASTValue *V = 0;
    unsigned RegNum = TargetRegisterInfo::virtReg2Index(SignalNum);

    if (Prefix[0] == 'w')
      V = VM->addWire(Prefix + utostr(RegNum), Bitwidth, RC->getName());
    else {
      V = VM->addRegister(Prefix + utostr(RegNum), Bitwidth, RC->getName());
      TotalRegisterBits += Bitwidth;
    }
    // Add the signal to register map.
    VM->addVASTValue(SignalNum, V);
  }
}


RTLCodegen::~RTLCodegen() {}

//===----------------------------------------------------------------------===//
void RTLCodegen::emitCtrlOp(ucState &State, PredMapTy &PredMap) {
  assert(State->getOpcode() == VTM::Control && "Bad ucState!");
  MachineBasicBlock *CurBB = State->getParent();
  unsigned startSlot = FInfo->getStartSlotFor(CurBB);
  unsigned IISlot = FInfo->getIISlotFor(CurBB);
  unsigned EndSlot = FInfo->getEndSlotFor(CurBB);
  unsigned II = IISlot - startSlot;
  // There maybe serveral slots in a single ucState in a pipelined loop.
  unsigned stateSlot = State.getSlot() - 1;
  bool MultiSlots = false;

  vlang_raw_ostream &CtrlS = VM->getControlBlockBuffer();

  for (ucState::iterator I = State.begin(), E = State.end(); I != E; ++I) {
    ucOp Op = *I;

    unsigned SlotNum = Op->getPredSlot();
    VASTSlot *CurSlot = VM->getSlot(SlotNum - 1);
    MultiSlots |= (SlotNum - 1) != stateSlot;

    assert(SlotNum != startSlot && "Unexpected first slot!");
    // Emit the control operation at the rising edge of the clock.
    std::string SlotPred = "(";
    raw_string_ostream SlotPredSS(SlotPred);

    SlotPredSS << getucStateEnable(SlotNum - 1) << ')';

    // Emit the predicate operand.
    SlotPredSS << " & ";
    printPredicate(Op.getPredicate(), SlotPredSS);

    // Special case for state transferring operation.
    if (VInstrInfo::isBrCndLike(Op->getOpcode())) {
      SlotPredSS << " & ";
      ucOperand &CndOp = Op.getOperand(0);
      printPredicate(CndOp, SlotPredSS);

      MachineBasicBlock *TargetBB = Op.getOperand(1).getMBB();
      unsigned TargetSlotNum = FInfo->getStartSlotFor(TargetBB);
      assert(Op.getPredicate().getReg() == 0 && "Cannot handle predicated BrCnd");
      VASTCnd Cnd = VASTCnd::Create(VM, CndOp);
      CurSlot->addNextSlot(TargetSlotNum, Cnd);

      // Emit control operation for next state.
      SlotPredSS.flush();
      CtrlS.if_begin(SlotPred);
      if (TargetBB == CurBB && IISlot < EndSlot)
        // The loop op of pipelined loop enable next slot explicitly.
        CurSlot->addNextSlot(CurSlot->getSlotNum() + 1);

      // Emit the first micro state of the target state.
      emitFirstCtrlState(TargetBB);

      CtrlS.exit_block();
      PredMap.insert(std::make_pair(TargetBB, SlotPred));
      continue;
    }

    // Loop back PHI node moving only active when current slot and the same
    // slot at previous (.i.e Slot - II) are both enable. Which means we are
    // looping back.
    if (Op->getOpcode() == VTM::VOpMvPhi) {
      MachineBasicBlock *TargetBB = Op.getOperand(2).getMBB();
      unsigned CndSlot = SlotNum - II;
      SlotPredSS << " & (";
      if (TargetBB == CurBB && CndSlot > startSlot)
        SlotPredSS << getucStateEnable(CndSlot - 1);
      else {
        assert(PredMap.count(TargetBB) && "Loop back predicate not found!");
        SlotPredSS << PredMap.find(TargetBB)->second;
      }

      SlotPredSS << ')';
    }

    SlotPredSS.flush();
    CtrlS.if_begin(SlotPred);

    // Emit the operations.
    switch (Op->getOpcode()) {
    case VTM::VOpInternalCall:  emitOpInternalCall(Op, CurSlot);break;
    case VTM::VOpRetVal:        emitOpRetVal(Op);             break;
    case VTM::VOpRet:           emitOpRet(Op, CurSlot);       break;
    case VTM::VOpMemTrans:      emitOpMemTrans(Op, CurSlot);  break;
    case VTM::VOpBRam:          emitOpBRam(Op);               break;
    case VTM::VOpAdd:           emitOpAdd(Op);                break;
    case VTM::VOpMult:          emitOpMult(Op);               break;
    case VTM::VOpSHL:           emitOpShift(Op, "<<");       break;
    case VTM::VOpSRL:           emitOpShift(Op, ">>");       break;
    case VTM::VOpSRA:           emitOpShift(Op, ">>>");       break;
    case VTM::VOpReadFU:        emitOpReadFU(Op, CurSlot);    break;
    case VTM::IMPLICIT_DEF:     emitImplicitDef(Op);          break;
    case VTM::VOpMove_ra:
    case VTM::VOpMove_ri:
    case VTM::VOpMove_rm:
    case VTM::VOpMove_rs:
    case VTM::VOpMove_rw:
    case VTM::VOpMove_rr:
    case VTM::VOpMvPhi:
    case VTM::COPY:             emitOpCopy(Op);               break;
    case VTM::VOpMove_ww:       emitOpConnectWire(Op);        break;
    case VTM::VOpSel:           emitOpSel(Op);                break;
    case VTM::VOpReadReturn:    emitOpReadReturn(Op, CurSlot);break;
    default:  assert(0 && "Unexpected opcode!");              break;
    }

    CtrlS.exit_block();
  }

  if (MultiSlots) {
    for (unsigned slot = stateSlot; slot < EndSlot; slot += II)
      VM->getSlot(slot)->setAliasSlots(stateSlot, EndSlot, II);
  }
}

void RTLCodegen::emitFirstCtrlState(MachineBasicBlock *DstBB) {
  // TODO: Emit PHINodes if necessary.
  ucState FirstState = *DstBB->getFirstNonPHI();
  assert(FInfo->getStartSlotFor(DstBB) == FirstState.getSlot()
         && "Broken Slot!");

  for (ucState::iterator I = FirstState.begin(), E = FirstState.end();
       I != E; ++I) {
    ucOp Op = *I;
    switch (Op->getOpcode()) {
    case VTM::VOpRetVal:        emitOpRetVal(Op);             break;
    case VTM::IMPLICIT_DEF:     emitImplicitDef(Op);          break;
    case VTM::VOpMove_ra:
    case VTM::VOpMove_ri:
    case VTM::VOpMove_rm:
    case VTM::VOpMove_rs:
    case VTM::VOpMove_rw:
    case VTM::VOpMove_rr:
    case VTM::VOpMvPhi:
    case VTM::COPY:             emitOpCopy(Op);               break;
    case VTM::VOpMove_ww:       emitOpConnectWire(Op);        break;
    case VTM::VOpSel:           emitOpSel(Op);                break;
    default:  assert(0 && "Unexpected opcode!");              break;
    }
  }
}

void RTLCodegen::emitOpAdd(ucOp &OpAdd) {
  raw_ostream &CtrlS = VM->getControlBlockBuffer();
  // Allocate the function unit register.
  // FIXME: Move these to emitAllocatedFUs
  ucOperand &Sum = OpAdd.getOperand(0);
  unsigned FUWidth = Sum.getBitWidth();
  unsigned AddNum = TargetRegisterInfo::virtReg2Index(Sum.getReg());
  std::string SumName = "addsub" + utostr_32(AddNum);
  std::string OpAName = SumName + "_a";
  std::string OpBName = SumName + "_b";
  std::string OpCName = SumName + "_c";
  VM->addRegister(OpAName, FUWidth);
  VM->addRegister(OpBName, FUWidth);
  VM->addRegister(OpCName, 1);
  // Assign the value to function unit.
  CtrlS << OpAName << " <= ";
  OpAdd.getOperand(2).print(CtrlS);
  CtrlS << ";\n";
  CtrlS << OpBName << " <= ";
  OpAdd.getOperand(3).print(CtrlS);
  CtrlS << ";\n";
  CtrlS << OpCName << " <= ";
  OpAdd.getOperand(4).print(CtrlS);
  CtrlS << ";\n";

  // Write the datapath for function unit.
  VASTDatapath *data =  VM->createDatapath();
  VASTDatapath::builder_stream &DPS = data->getCodeBuffer();
  // FIXME: Move these to emitAllocatedFUs
  DPS << "assign {";
  // Carry out.
  OpAdd.getOperand(1).print(DPS);
  DPS << ", ";
  // Sum.
  OpAdd.getOperand(0).print(DPS);
  DPS << "} = " << OpAName << " + " << OpBName << " + " << OpCName << ";\n";

}

void RTLCodegen::emitOpShift(ucOp &OpSHT, const std::string &Operator) {
  raw_ostream &CtrlS = VM->getControlBlockBuffer();
  // Allocate the function unit register.
  // FIXME: Move these to emitAllocatedFUs
  ucOperand &Result = OpSHT.getOperand(0);
  unsigned FUWidth = Result.getBitWidth();
  unsigned ShiftNum = TargetRegisterInfo::virtReg2Index(Result.getReg());
  std::string SumName = "shift" + utostr_32(ShiftNum);
  std::string OpAName = SumName + "_a";
  std::string OpBName = SumName + "_b";
  VM->addRegister(OpAName, FUWidth);
  VM->addRegister(OpBName, Log2_32_Ceil(FUWidth));
  // Assign the value to function unit.
  CtrlS << OpAName << " <= ";
  OpSHT.getOperand(1).print(CtrlS);
  CtrlS << ";\n";
  CtrlS << OpBName << " <= ";
  OpSHT.getOperand(2).print(CtrlS);
  CtrlS << ";\n";

  // Write the datapath for function unit.
  VASTDatapath *data0 = VM->createDatapath();
  VASTDatapath::builder_stream &DPS = data0->getCodeBuffer();
  // Create the signed operand.
  if (OpSHT->getOpcode() == VTM::VOpSRA) {
    std::string NewOpAName = OpAName + "_signed";
    DPS << "wire signed" << verilogBitRange(FUWidth) << ' '
        << NewOpAName << " = " << OpAName << ";\n";
    OpAName = NewOpAName;
  }

  VASTDatapath *data1 = VM->createDatapath();
  VASTDatapath::builder_stream &OS = data1->getCodeBuffer();
  OS << "assign ";
  OpSHT.getOperand(0).print(OS);
  OS << " = " << OpAName << Operator << OpBName << ";\n";
}

void RTLCodegen::emitOpMult(ucOp &OpMult) {
  raw_ostream &CtrlS = VM->getControlBlockBuffer();
  // Allocate the function unit register.
  // FIXME: Move these to emitAllocatedFUs
  ucOperand &Product = OpMult.getOperand(0);
  unsigned FUWidth = Product.getBitWidth();
  unsigned MultNum = TargetRegisterInfo::virtReg2Index(Product.getReg());
  std::string SumName = "mult" + utostr_32(MultNum);
  std::string OpAName = SumName + "_a";
  std::string OpBName = SumName + "_b";
  VM->addRegister(OpAName, FUWidth);
  VM->addRegister(OpBName, FUWidth);
  // Assign the value to function unit.
  CtrlS << OpAName << " <= ";
  OpMult.getOperand(1).print(CtrlS);
  CtrlS << ";\n";
  CtrlS << OpBName << " <= ";
  OpMult.getOperand(2).print(CtrlS);
  CtrlS << ";\n";
  // Write the datapath for function unit.
  VASTDatapath *data = VM->createDatapath();
  VASTDatapath::builder_stream &DPS = data->getCodeBuffer();
  // FIXME: Move these to emitAllocatedFUs
  DPS << "assign ";
  // Sum.
  OpMult.getOperand(0).print(DPS);
  DPS << " = " << OpAName << " * " << OpBName << ";\n";
}

void RTLCodegen::emitImplicitDef(ucOp &ImpDef) {
  VASTDatapath *data = VM->createDatapath();
  VASTDatapath::builder_stream &OS = data->getCodeBuffer();
  OS << "// IMPLICIT_DEF ";
  ImpDef.getOperand(0).print(OS);
  OS << "\n";
}

void RTLCodegen::emitOpSel(ucOp &OpSel) {
  raw_ostream &OS = VM->getControlBlockBuffer();
  OpSel.getOperand(0).print(OS);
  OS << " <= ";
  if (OpSel.getOperand(1).isPredicateInverted())
    OS << "~";
  OpSel.getOperand(1).print(OS, 1, 0, true);
  OS << " ? ";
  OpSel.getOperand(2).print(OS);
  OS << " : ";
  OpSel.getOperand(3).print(OS);
  OS << ";\n";

}

void RTLCodegen::emitOpCopy(ucOp &OpCopy) {
  raw_ostream &OS = VM->getControlBlockBuffer();
  OpCopy.getOperand(0).print(OS);
  OS << " <= ";
  OpCopy.getOperand(1).print(OS);
  OS << ";\n";
}

void RTLCodegen::emitOpReadFU(ucOp &OpRdFU, VASTSlot *CurSlot) {
  FuncUnitId Id = OpRdFU->getFUId();
  VASTValue *ReadyPort = 0;

  switch (Id.getFUType()) {
  case VFUs::MemoryBus:
    ReadyPort = VM->getVASTValue(VFUMemBus::getReadyName(Id.getFUNum()));
    break;
  default:
    break;
  }

  if (ReadyPort) {
    CurSlot->addReady(ReadyPort, VASTCnd::Create(VM, OpRdFU.getPredicate()));
  }
  // The dst operand of ReadFU change to immediate if it is dead.
  if (OpRdFU.getOperand(0).isReg())
    emitOpCopy(OpRdFU);
}

void RTLCodegen::emitOpConnectWire(ucOp &Op) {
  VM->getControlBlockBuffer() << "// Connect wire in datapath.\n";

  VASTDatapath *data = VM->createDatapath();
  VASTDatapath::builder_stream &OS = data->getCodeBuffer();
  OS << "assign ";
  Op.getOperand(0).print(OS);
  OS << " = ";
  Op.getOperand(1).print(OS);
  OS << ";\n";
}

void RTLCodegen::emitOpReadReturn(ucOp &OpReadSymbol, VASTSlot *CurSlot) {
  raw_ostream &OS = VM->getControlBlockBuffer();
  OpReadSymbol.getOperand(0).print(OS);
  unsigned FNNum = OpReadSymbol.getOperand(1).getTargetFlags();
  // The FNNum is encoded into the target flags field of the MachineOperand.
  OS << " <= "
    << getSubModulePortName(FNNum, OpReadSymbol.getOperand(1).getSymbolName());
  OS << ";\n";

  VASTCnd Pred = VASTCnd::Create(VM, OpReadSymbol.getPredicate());
  std::string FinPortName = getSubModulePortName(FNNum, "fin");
  VASTValue *FinSignal = VM->getVASTSymbol(FinPortName);
  CurSlot->addReady(FinSignal, Pred);
}

void RTLCodegen::emitOpInternalCall(ucOp &OpInternalCall, VASTSlot *CurSlot) {
  // Assign input port to some register.
  raw_ostream &OS = VM->getControlBlockBuffer();
  const char *CalleeName = OpInternalCall.getOperand(1).getSymbolName();
  // The FNNum is encoded into the target flags field of the MachineOperand.
  unsigned FNNum = OpInternalCall.getOperand(1).getTargetFlags();
  OS << "// Calling function: " << CalleeName << ";\n";

  VASTCnd Pred = VASTCnd::Create(VM, OpInternalCall.getPredicate());
  std::string StartPortName = getSubModulePortName(FNNum, "start");
  VASTValue *StartSignal = VM->getVASTSymbol(StartPortName);
  CurSlot->addEnable(StartSignal, Pred);
  VASTSlot *NextSlot = VM->getSlot(CurSlot->getSlotNum() + 1);
  NextSlot->addDisable(StartSignal, Pred);

  if (const Function *FN = M->getFunction(CalleeName)) {
    if (!FN->isDeclaration()) {
      Function::const_arg_iterator ArgIt = FN->arg_begin();
      for (unsigned i = 0, e = FN->arg_size(); i != e; ++i) {
        OS << getSubModulePortName(FNNum, ArgIt->getName()) << " <= ";
        OpInternalCall.getOperand(2 + i).print(OS);
        OS << ";\n";
        ++ArgIt;
      }
      return;
    } else {
      // Dirty Hack.
      // TODO: Extract these to some special instruction?
      OS << "$c(\"" << FN->getName() << "(\",";
      for (unsigned i = 2, e = OpInternalCall.getNumOperands(); i != e; ++i) {
        ucOperand &Op = OpInternalCall.getOperand(i);
        if (Op.isReg() && (Op.getReg() == 0 || Op.isImplicit())) continue;

        if (i != 2) OS << ",\",\", ";

        if (Op.isGlobal()) // It is the format string?
          if (const GlobalVariable *Str = cast<GlobalVariable>(Op.getGlobal())){
            if (Str->hasInitializer()) {
              const Constant *Initialer = Str->getInitializer();
              if (const ConstantArray *Fmt = dyn_cast<ConstantArray>(Initialer)){
                 if (Fmt->isCString()) {
                   std::string FmtStr;
                   raw_string_ostream SS(FmtStr);
                   SS << '"';
                   PrintEscapedString(Fmt->getAsString(), SS);
                   SS << '"';
                   SS.flush();
                   OS << '"';
                   PrintEscapedString(FmtStr, OS);
                   OS << '"';
                   continue;
                 }
              }
            }
          }

        Op.print(OS);
      }

      OS << ", \");\""; // Enclose the c function call.
      OS << ");\n";
      return;
    }
  }

  // Else ask the constraint about how to handle this call.
  SmallVector<std::string, 8> InPorts;
  std::string s;
  raw_string_ostream SS(s);
  for (unsigned i = 2, e = OpInternalCall.getNumOperands(); i != e; ++i) {
    OpInternalCall.getOperand(i).print(SS);
    SS.flush();
    InPorts.push_back(SS.str());
    s.clear();
  }

  std::string Name = CalleeName;
  OS << VFUs::startModule(Name, FInfo->getCalleeFNNum(CalleeName), InPorts);
}

void RTLCodegen::emitOpRet(ucOp &OpArg, VASTSlot *CurSlot) {
  // Go back to the idle slot.
  CurSlot->addNextSlot(0);
  CurSlot->addEnable(&VM->getPort(VASTModule::Finish));
}

void RTLCodegen::emitOpRetVal(ucOp &OpRetVal) {
  raw_ostream &OS = VM->getControlBlockBuffer();
  unsigned retChannel = OpRetVal.getOperand(1).getImm();
  assert(retChannel == 0 && "Only support Channel 0!");
  OS << "return_value <= ";
  OpRetVal.getOperand(0).print(OS);
  OS << ";\n";
  OS << "`ifdef __VERILATOR_SIM_DEBUG\n"
        "$display(\"Module " << MF->getFunction()->getName()
     << " return %x\\n\", ";
  OpRetVal.getOperand(0).print(OS);
  OS << ");\n"
        "`endif\n";
}

void RTLCodegen::emitOpMemTrans(ucOp &OpMemAccess, VASTSlot *CurSlot) {
  unsigned FUNum = OpMemAccess->getFUId().getFUNum();
  VASTDatapath *data = VM->createDatapath();
  VASTDatapath::builder_stream &DPS = data->getCodeBuffer();
  DPS << "assign ";
  OpMemAccess.getOperand(0).print(DPS);
  DPS << " = " << VFUMemBus::getInDataBusName(FUNum) << ";\n";

  // Emit the control logic.
  raw_ostream &OS = VM->getControlBlockBuffer();
  // Emit Address.
  OS << VFUMemBus::getAddrBusName(FUNum) << "_r <= ";
  OpMemAccess.getOperand(1).print(OS);
  OS << ";\n";
  // Assign store data.
  OS << VFUMemBus::getOutDataBusName(FUNum) << "_r <= ";
  OpMemAccess.getOperand(2).print(OS);
  OS << ";\n";
  // And write enable.
  OS << VFUMemBus::getWriteEnableName(FUNum) << "_r <= ";
  OpMemAccess.getOperand(3).print(OS);
  OS << ";\n";
  // The byte enable.
  OS << VFUMemBus::getByteEnableName(FUNum) << "_r <= ";
  OpMemAccess.getOperand(4).print(OS);
  OS << ";\n";

  // Remember we enabled the memory bus at this slot.
  std::string EnableName = VFUMemBus::getEnableName(FUNum) + "_r";
  VASTValue *MemEn = VM->getVASTValue(EnableName);
  VASTCnd Pred = VASTCnd::Create(VM, OpMemAccess.getPredicate());
  CurSlot->addEnable(MemEn, Pred);

  // Disable the memory at next slot.
  // TODO: Assert the control flow is linear.
  VASTSlot *NextSlot = VM->getSlot(CurSlot->getSlotNum() + 1);
  NextSlot->addDisable(MemEn, Pred);
}

void RTLCodegen::emitOpBRam(ucOp &OpBRam) {
  unsigned FUNum = OpBRam->getFUId().getFUNum();

  VASTDatapath *data = VM->createDatapath();
  VASTDatapath::builder_stream &DPS = data->getCodeBuffer();

  DPS << "assign ";
  OpBRam.getOperand(0).print(DPS);
  DPS << " = " << VFUBRam::getInDataBusName(FUNum) << ";\n";

  // Emit the control logic.
  raw_ostream &OS = VM->getControlBlockBuffer();
  // Emit Address.
  OS << VFUBRam::getAddrBusName(FUNum) << " <= (";
  OpBRam.getOperand(1).print(OS);
  unsigned SizeInBits
    = FInfo->getBRamInfo(OpBRam->getFUId().getFUNum()).ElemSizeInBytes;
  OS << " >> " << Log2_32_Ceil(SizeInBits) << ");\n";
  // Assign store data.
  OS << VFUBRam::getOutDataBusName(FUNum) << " <= ";
  OpBRam.getOperand(2).print(OS);
  OS << ";\n";
  // And write enable.
  OS << VFUBRam::getWriteEnableName(FUNum) << " <= ";
  OpBRam.getOperand(3).print(OS);
  OS << ";\n";
  // The byte enable.
  // OS << VFUMemBus::getByteEnableName(FUNum) << " <= ";
  // OpBRam.getOperand(4).print(OS);
  // OS << ";\n";
}

void RTLCodegen::emitDatapath(ucState &State) {
  assert(State->getOpcode() == VTM::Datapath && "Bad ucState!");
  VASTDatapath *data =  VM->createDatapath();
  data->getCodeBuffer() << "// Issue datapath for "
    "operations at slot " << State.getSlot() << '\n';

  for (ucState::iterator I = State.begin(), E = State.end(); I != E; ++I) {
    ucOp Op = *I;
    switch (Op->getOpcode()) {
    case VTM::VOpBitSlice:  emitOpBitSlice(Op);     break;
    case VTM::VOpBitCat:    emitOpBitCat(Op);       break;
    case VTM::VOpBitRepeat: emitOpBitRepeat(Op);    break;

    case VTM::VOpXor:       emitBinaryOp(Op, "^");  break;
    case VTM::VOpAnd:       emitBinaryOp(Op, "&");  break;
    case VTM::VOpOr:        emitBinaryOp(Op, "|");  break;

    case VTM::VOpNot:       emitUnaryOp(Op, "~");   break;

    case VTM::VOpROr:       emitUnaryOp(Op, "|");   break;
    case VTM::VOpRAnd:      emitUnaryOp(Op, "&");   break;
    case VTM::VOpRXor:      emitUnaryOp(Op, "^");   break;

    default:  assert(0 && "Unexpected opcode!");    break;
    }
  }
}

void RTLCodegen::emitUnaryOp(ucOp &UnaOp, const std::string &Operator) {
  VASTDatapath *data =  VM->createDatapath();
  VASTDatapath::builder_stream &OS = data->getCodeBuffer();
  OS << "assign ";
  UnaOp.getOperand(0).print(OS);
  OS << " = " << Operator << ' ';
  UnaOp.getOperand(1).print(OS);
  OS << ";\n";
}

void RTLCodegen::emitBinaryOp(ucOp &BinOp, const std::string &Operator) {
  VASTDatapath *data =  VM->createDatapath();
  VASTDatapath::builder_stream &OS = data->getCodeBuffer();
  OS << "assign ";
  BinOp.getOperand(0).print(OS);
  OS << " = ";
  BinOp.getOperand(1).print(OS);
  OS << ' ' << Operator << ' ';
  BinOp.getOperand(2).print(OS);
  OS << ";\n";
}

void RTLCodegen::emitOpBitSlice(ucOp &OpBitSlice) {
  VASTDatapath *data =  VM->createDatapath();
  VASTDatapath::builder_stream &OS = data->getCodeBuffer();
  // Get the range of the bit slice, Note that the
  // bit at upper bound is excluded in VOpBitSlice,
  // now we are going to get the included upper bound.
  unsigned UB = OpBitSlice.getOperand(2).getImm(),
           LB = OpBitSlice.getOperand(3).getImm();

  OS << "assign ";
  OpBitSlice.getOperand(0).print(OS);
  OS << " = ";
  OpBitSlice.getOperand(1).print(OS, UB, LB);
  OS << ";\n";
}

void RTLCodegen::emitOpBitCat(ucOp &OpBitCat) {
  VASTDatapath *data =  VM->createDatapath();
  VASTDatapath::builder_stream &OS = data->getCodeBuffer();
  OS << "assign ";
  OpBitCat.getOperand(0).print(OS);
  OS << " = {";
  // BitCat is a binary instruction now.
  OpBitCat.getOperand(1).print(OS);
  OS << ',';
  OpBitCat.getOperand(2).print(OS);
  OS << "};\n";
}

void RTLCodegen::emitOpBitRepeat(ucOp &OpBitRepeat) {
  VASTDatapath *data =  VM->createDatapath();
  VASTDatapath::builder_stream &OS = data->getCodeBuffer();
  OS << "assign ";
  OpBitRepeat.getOperand(0).print(OS);
  OS << " = {";

  unsigned Times = OpBitRepeat.getOperand(2).getImm();
  OS << Times << '{';
  OpBitRepeat.getOperand(1).print(OS);
  OS << "}};\n";
}
