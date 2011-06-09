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

#include "llvm/Type.h"

#include "llvm/Target/Mangler.h"
#include "llvm/Target/TargetMachine.h"

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallString.h"

#include "llvm/ADT/StringExtras.h"

#include "llvm/Support/FormattedStream.h"

#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#define DEBUG_TYPE "vtm-rtl-codegen"
#include "llvm/Support/Debug.h"

using namespace llvm;
namespace {
class RTLCodegen : public MachineFunctionPass {
  vlang_raw_ostream Out;

  MachineFunction *MF;
  VFInfo *FInfo;
  MachineRegisterInfo *MRI;
  BitLevelInfo *BLI;
  VASTModule *VM;
  Mangler *Mang;

  unsigned TotalFSMStatesBit, CurFSMStateNum, SignedWireNum;

  // Mapping success fsm state to their predicate in current state.
  typedef std::map<MachineBasicBlock*, std::string> PredMapTy;

  // If the FSM ready to move to next state?
  std::string ReadyPred;
  void addReadyPred(std::string &Pred) {
    ReadyPred += " & (" + Pred + ")";
  }

  void emitFunctionSignature(int FNNum);
  void emitCommonPort(int FNNum);

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
      MuxWiresDeclS  << OutputName << "_mux_wire;\n";
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
        BusMux.MuxLogicS.match_case(PortReg);
      } else {
        BusMux.addOutput(PortName, BitWidth);
        BusMux.assignValueInCase(PortName, PortReg);
      }
    }

    std::string addSubModulePort(std::string &PortName, unsigned BitWidth,
                                 std::string &SubModuleName, bool isOut = true,
                                 bool isEn = false){
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

    void addSubModule(std::string &SubModuleName, raw_ostream &S) {
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

  void emitBasicBlock(MachineBasicBlock &MBB);

  void emitAllSignals();
  void emitWires(const TargetRegisterClass *RC);

  void clear();

  inline std::string getStateName(MachineBasicBlock *MBB) {
    SmallVector<char, 16> Name;
    // Use mangler to handle escaped characters.
    Mang->getNameWithPrefix(Name, MBB->getName().str() + "BB"
                            + itostr(MBB->getNumber()));
    return std::string(Name.data(), Name.size());
  }

  inline std::string getucStateEnableName(MachineBasicBlock *MBB) {
    std::string StateName = getStateName(MBB);

    StateName = "cur_" + StateName;

    return StateName + "_enable";
  }

  inline std::string getucStateEnable(ucState &State) {
    return getucStateEnable(State->getParent(), State.getSlot());
  }
  inline std::string getucStateEnable(MachineBasicBlock *MBB, unsigned Slot) {
    std::string StateName = getucStateEnableName(MBB);
    raw_string_ostream ss(StateName);
    // Ignore the laster slot, we do nothing at that slot.
    if (FInfo->getTotalSlotFor(MBB) > 1)
      ss << "[" << (Slot - FInfo->getStartSlotFor(MBB)) << "]";

    return ss.str();
  }

  void emitNextFSMState(raw_ostream &ss, MachineBasicBlock *SrcBB,
                        MachineBasicBlock *DstBB);
  void createucStateEnable(MachineBasicBlock *MBB);
  void emitNextMicroState(raw_ostream &ss, MachineBasicBlock *MBB,
                          const std::string &NewState);

  // Emit the operations in the first micro state in the FSM state when we are
  // jumping to it.
  void emitFirstCtrlState(MachineBasicBlock *SrcBB,
                          MachineBasicBlock *DstBB);

  void emitDatapath(ucState &State);

  void emitUnaryOp(ucOp &UnOp, const std::string &Operator);
  void emitBinaryOp(ucOp &BinOp, const std::string &Operator);

  // Emit a signal with "signed" modifier and return the name of signed signal.
  std::string emitSignedOperand(ucOperand &Op);

  void emitOpSRA(ucOp &OpSRA);

  void emitOpSel(ucOp &OpSel);

  void emitOpAdd(ucOp &OpAdd);
  void emitOpMult(ucOp &OpMult);

  void emitOpBitCat(ucOp &OpBitCat);
  void emitOpBitSlice(ucOp &OpBitSlice);
  void emitOpBitRepeat(ucOp &OpBitRepeat);

  void emitImplicitDef(ucOp &ImpDef);

  // Return true if the control operation contains a return operation.
  bool emitCtrlOp(ucState &State, PredMapTy &PredMap,
                  MachineBasicBlock *SrcBB = 0);

  void emitOpInternalCall(ucOp &OpInternalCall);
  void emitOpReadReturn(ucOp &OpReadSymbol);
  void emitOpRetVal(ucOp &OpRetVal);
  void emitOpRet(ucOp &OpRet);
  void emitOpCopy(ucOp &OpCopy);
  void emitOpMemTrans(ucOp &OpMemAccess);
  void emitOpBRam(ucOp &OpBRam);

  std::string getSubModulePortName(unsigned FNNum,
                                   const std::string PortName) const {
    const Function *F = FInfo->getCalleeFN(FNNum);
    return F->getNameStr() + "_" + PortName;
  }

  struct GetCalleeFNEnableNameFtor {
    RTLCodegen *CG;
    GetCalleeFNEnableNameFtor(RTLCodegen *cg) : CG(cg) {}
    std::string operator() (unsigned FNNum) const {
      return CG->getSubModulePortName(FNNum, "start");
    }
  };

  struct GetCalleeFNReadyNameFtor {
    RTLCodegen *CG;
    GetCalleeFNReadyNameFtor(RTLCodegen *cg) : CG(cg) {}
    std::string operator() (unsigned FNNum) const {
      return CG->getSubModulePortName(FNNum, "fin");
    }
  };

  // Return the FSM ready predicate.
  // The FSM only move to next micro-state if the predicate become true.
  void emitFUCtrlForState(vlang_raw_ostream &CtrlS, MachineBasicBlock *CurBB);

  template<enum VFUs::FUTypes T, typename GetReadyNameFunc>
  void emitWaitFUReadyForState(MachineBasicBlock *CurBB, unsigned Latency,
                               unsigned startSlot, unsigned endSlot,
                               GetReadyNameFunc GetReadyName);

  template<enum VFUs::FUTypes Ty, typename GetEnableNameFunc>
  void emitFUEnableForState(vlang_raw_ostream &CtrlS, MachineBasicBlock *CurBB,
                            unsigned startSlot, unsigned endSlot,
                            GetEnableNameFunc GetEnableName);

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
  void getAnalysisUsage(AnalysisUsage &AU) const;
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
INITIALIZE_PASS_DEPENDENCY(BitLevelInfo);
INITIALIZE_PASS_END(RTLCodegen, "vtm-rtl-info",
                    "Build RTL Verilog module for synthesised function.",
                    false, true)

RTLCodegen::RTLCodegen(raw_ostream &O) : MachineFunctionPass(ID), Out(O) {
  initializeRTLCodegenPass(*PassRegistry::getPassRegistry());
}

bool RTLCodegen::doInitialization(Module &M) {
  MachineModuleInfo *MMI = getAnalysisIfAvailable<MachineModuleInfo>();
  TargetData *TD = getAnalysisIfAvailable<TargetData>();

  assert(MMI && TD && "MachineModuleInfo and TargetData will always available"
                      " in a machine function pass!");
  Mang = new Mangler(MMI->getContext(), *TD);

  return false;
}

bool RTLCodegen::runOnMachineFunction(MachineFunction &F) {
  MF = &F;
  FInfo = MF->getInfo<VFInfo>();
  MRI = &MF->getRegInfo();
  BLI = &getAnalysis<BitLevelInfo>();

  DEBUG(
    Out << "`ifdef wtf_is_this\n" << "Function for RTL Codegen:\n";
    printVMF(Out, F);
    Out << "`endif\n";
  );

  SignedWireNum = 0;
  // Reset the current fsm state number.
  CurFSMStateNum = 0;
  // The FSM is always ready by default.
  ReadyPred = "1'b1";

  // FIXME: Demangle the c++ name.
  // Dirty Hack: Force the module have the name of the hw subsystem.
  VM = FInfo->getRtlMod();
  emitFunctionSignature(-1);

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

  // Datapath
  Out << "// Datapath\n";
  Out << VM->getDataPathStr();

  Out << "\n\n";
  Out << "// Always Block\n";
  Out.always_ff_begin();
  VM->printRegisterReset(Out);
  Out.else_begin().if_begin(ReadyPred, "// are all resources ready?\n");

  Out << "// FSM\n";
  Out.switch_begin("NextFSMState");
  Out << VM->getControlBlockStr();
  // Case default.
  Out << "default:  NextFSMState <= state_idle;\n";
  Out.switch_end();
  Out.else_begin("// else disable all resources\n");
  emitFUCtrlForState(Out, 0);
  Out.exit_block("// end control block\n");
  Out.always_ff_end();
  Out.module_end();
  Out.flush();

  return false;
}

void RTLCodegen::clear() {
  VM = 0;
}

void RTLCodegen::getAnalysisUsage(AnalysisUsage &AU) const {
  MachineFunctionPass::getAnalysisUsage(AU);
  AU.addRequired<BitLevelInfo>();
  AU.setPreservesAll();
}

void RTLCodegen::print(raw_ostream &O, const Module *M) const {

}

void RTLCodegen::emitFunctionSignature(int FNNum) {
  const TargetData *TD = MF->getTarget().getTargetData();
  raw_ostream &S = VM->getDataPathBuffer();

  const Function *F;
  if (FNNum != -1)
    F = FInfo->getCalleeFN(FNNum);
  else
    F = MF->getFunction();

  for (Function::const_arg_iterator I = F->arg_begin(), E = F->arg_end();
      I != E; ++I) {
    const Argument *Arg = I;
    std::string Name = Arg->getNameStr();
    unsigned BitWidth = TD->getTypeSizeInBits(Arg->getType());
    // Add port declaration.
    if (FNNum == -1)
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
    if (FNNum == -1)
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

  // Idle state
  verilogParam(VM->getStateDeclBuffer(), "state_idle", TotalFSMStatesBit, 0);
  CtrlS.match_case("state_idle");
  // Idle state is always ready.
  CtrlS.if_begin("start");
  // The module is busy now
  MachineBasicBlock *EntryBB =  GraphTraits<MachineFunction*>::getEntryNode(MF);
  emitNextFSMState(CtrlS, 0, EntryBB);
  //
  CtrlS.else_begin();
  CtrlS << "NextFSMState <= state_idle;\n";
  // End if-else
  CtrlS.exit_block();
  emitFUCtrlForState(CtrlS, 0);
  // End case.
  CtrlS.exit_block();
}

void RTLCodegen::emitBasicBlock(MachineBasicBlock &MBB) {
  std::string StateName = getStateName(&MBB);
  unsigned totalSlot = FInfo->getTotalSlotFor(&MBB);
  unsigned II = FInfo->getIIFor(&MBB);
  PredMapTy NextStatePred;

  vlang_raw_ostream &CtrlS = VM->getControlBlockBuffer();

  //unsigned StartSlot = State->getSlot(), EndSlot = State->getEndSlot();
  VM->getStateDeclBuffer() << "// State for " << StateName << '\n';
  verilogParam(VM->getStateDeclBuffer(), StateName, TotalFSMStatesBit,
               ++CurFSMStateNum);

  // State information.
  CtrlS << "// " << StateName << " Total Slot: " << totalSlot
                 << " II: " << II <<  '\n';
  // Mirco state enable.
  createucStateEnable(&MBB);

  // Case begin
  CtrlS.match_case(StateName);

  MachineBasicBlock::iterator I = ++MBB.getFirstNonPHI(),
                              E = MBB.getFirstTerminator();
  // FIXME: Refactor the loop.
  do {
    ucState CurDatapath = *I;
    // Emit the datepath of current state.
    emitDatapath(CurDatapath);

    // Emit next ucOp.
    ucState NextControl = *++I;
    CtrlS << "// Slot " << NextControl.getSlot() << '\n';
    emitCtrlOp(NextControl, NextStatePred);
  } while(++I != E);

  CtrlS << "// Next micro state.\n";
  PredMapTy::iterator at = NextStatePred.find(&MBB);
  if (at != NextStatePred.end())
    emitNextMicroState(CtrlS, &MBB, at->second);
  else
    emitNextMicroState(CtrlS, &MBB, "1'b0");

  emitFUCtrlForState(CtrlS, &MBB);

  // Case end
  CtrlS.exit_block();
}

static raw_ostream &ConnectPort(raw_ostream &OS, const std::string &PortName,
                                bool LastPort = false,
                                const std::string &SignalPrefix = "") {
  OS << "." << PortName << '(' << SignalPrefix << PortName << ')';
  if (!LastPort) OS << ",\n\t";
  return OS;
}

void RTLCodegen::emitCommonPort(int FNNum) {
  if (FNNum == -1) { // If F is current function.
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

  VFUMemBus *Bus = getFUDesc<VFUMemBus>();

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

  for (id_iterator I = FInfo->id_begin(VFUs::CalleeFN),
       E = FInfo->id_end(VFUs::CalleeFN); I != E; ++I) {
    FuncUnitId ID = *I;
    const Function *Callee = FInfo->getCalleeFN(ID.getFUNum());
    S << getSynSetting(Callee->getName())->getModName() << ' '
      << getSubModulePortName(ID.getFUNum(), "_inst")
      << "(\n\t";
    MBBuilder.addSubModule(getSubModulePortName(ID.getFUNum(), "_inst"), S);
    emitFunctionSignature(ID.getFUNum());
    S << ");\n";
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
       E = FInfo->phyreg_end(8); I < E; ++I)
    VM->addRegister("reg" + utostr(*I), 64);

  emitWires(VTM::WireRegisterClass);
  // FIXME: There are function units.
  emitWires(VTM::RADDRegisterClass);
  emitWires(VTM::RMULRegisterClass);
  emitWires(VTM::RSHTRegisterClass);
}

void RTLCodegen::emitWires(const TargetRegisterClass *RC) {
  // And Emit the wires defined in this module.
  const std::vector<unsigned>& Wires = MRI->getRegClassVirtRegs(RC);

  for (std::vector<unsigned>::const_iterator I = Wires.begin(), E = Wires.end();
    I != E; ++I) {
      unsigned WireNum = *I;
      const ucOperand *Op = cast<ucOperand>(MRI->getRegUseDefListHead(WireNum));
      // assert(Op && "Wire define not found!");
      if (!Op) continue;

      WireNum = TargetRegisterInfo::virtReg2Index(WireNum);
      VM->addWire("wire" + utostr(WireNum), Op->getBitWidth(), RC->getName());
  }
}


RTLCodegen::~RTLCodegen() {}

//===----------------------------------------------------------------------===//
void RTLCodegen::createucStateEnable(MachineBasicBlock *MBB)  {
  std::string StateName = getStateName(MBB);
  // We do not need the last state.
  unsigned totalSlot = FInfo->getTotalSlotFor(MBB);

  // current state
  VM->addRegister("cur_" + StateName + "_enable", totalSlot);
}

void RTLCodegen::emitNextFSMState(raw_ostream &ss, MachineBasicBlock *SrcBB,
                                  MachineBasicBlock *DstBB) {
  // Emit the first micro state of the target state.
  emitFirstCtrlState(SrcBB, DstBB);

  // Only jump to other state if target MBB is not current MBB.
  ss << "NextFSMState <= " << getStateName(DstBB) << ";\n";
  emitNextMicroState(VM->getControlBlockBuffer(), DstBB, "1'b1");
}

void RTLCodegen::emitNextMicroState(raw_ostream &ss, MachineBasicBlock *MBB,
                                   const std::string &NewState) {
  // We do not need the last state.
  unsigned totalSlot = FInfo->getTotalSlotFor(MBB);
  std::string StateName = getucStateEnableName(MBB);
  ss << StateName << " <= ";

  if (totalSlot > 1)
    ss << "{ " << StateName << verilogBitRange(totalSlot - 1) << ", ";

  ss << NewState;

  if (totalSlot > 1)
    ss << " }";

  ss << ";\n";
}

template<enum VFUs::FUTypes Ty, typename GetEnableNameFunc>
void RTLCodegen::emitFUEnableForState(vlang_raw_ostream &CtrlS,
                                      MachineBasicBlock *CurBB,
                                      unsigned startSlot, unsigned endSlot,
                                      GetEnableNameFunc GetEnableName) {
  // Emit function unit control.
  for (VFInfo::const_id_iterator I = FInfo->id_begin(Ty), E = FInfo->id_end(Ty);
       I != E; ++I) {
    FuncUnitId Id = *I;
    CtrlS << "// " << Id << " control for next micro state.\n";
    CtrlS << GetEnableName(Id.getFUNum()) << " <= 1'b0";
    // Resource control operation when in the current state.
    for (unsigned i = startSlot + 1, e = endSlot; i < e; ++i) {
      if (FInfo->isFUActiveAt(Id, i))
        CtrlS << " | " << getucStateEnable(CurBB, i - 1);
    }

    CtrlS << ";\n";
  }
}

template<enum VFUs::FUTypes T, typename GetReadyNameFunc>
void RTLCodegen::emitWaitFUReadyForState(MachineBasicBlock *CurBB,
                                         unsigned Latency,
                                         unsigned startSlot, unsigned endSlot,
                                         GetReadyNameFunc GetReadyName) {
  for (VFInfo::const_id_iterator I = FInfo->id_begin(T), E = FInfo->id_end(T);
       I != E; ++I) {
     FuncUnitId Id = *I;
    // Build the ready predicate for waiting membus ready.
    // We expect all operation will finish before the FSM jump to another state,
    // so we do not need to worry about if we need to wait the memory operation
    // issued from the previous state.
    for (unsigned i = startSlot + Latency, e = endSlot + 1; i != e; ++i)
      if (FInfo->isFUActiveAt(Id, i - Latency)) {
        std::string Pred = "~" +getucStateEnable(CurBB, i - 1)
          + "|" + GetReadyName(Id.getFUNum());
        addReadyPred(Pred);
      }
  }
}

static std::string GetMemBusEnableName(unsigned FUNum) {
  return VFUMemBus::getEnableName(FUNum) + "_r";
}

void RTLCodegen::emitFUCtrlForState(vlang_raw_ostream &CtrlS,
                                    MachineBasicBlock *CurBB) {
  unsigned startSlot = 0, endSlot = 0;
  // Get the slot information for no-idle state.
  if (CurBB) {
    startSlot = FInfo->getStartSlotFor(CurBB);
    endSlot = FInfo->getEndSlotFor(CurBB);
  }

  emitFUEnableForState<VFUs::MemoryBus>(CtrlS, CurBB, startSlot, endSlot,
                                        GetMemBusEnableName);
  emitFUEnableForState<VFUs::BRam>(CtrlS, CurBB, startSlot, endSlot,
                                   VFUBRam::getEnableName);
  emitFUEnableForState<VFUs::CalleeFN>(CtrlS, CurBB, startSlot, endSlot,
                                       GetCalleeFNEnableNameFtor(this));

  unsigned MemBusLatency = getFUDesc<VFUMemBus>()->getLatency();
  emitWaitFUReadyForState<VFUs::MemoryBus>(CurBB, MemBusLatency,
                                           startSlot, endSlot,
                                           VFUMemBus::getReadyName);

  // DirtyHack: Set the callee function latency to 1.
  emitWaitFUReadyForState<VFUs::CalleeFN>(CurBB, 1, startSlot, endSlot,
                                          GetCalleeFNReadyNameFtor(this));

  // Control the finish port
  CtrlS << "// Finish port control\n";
  CtrlS << "fin <= 1'b0";
  if (FInfo->isFUActiveAt(VFUs::FSMFinish, endSlot))
    CtrlS << " | " << getucStateEnable(CurBB, endSlot - 1);

  CtrlS << ";\n";
}

bool RTLCodegen::emitCtrlOp(ucState &State, PredMapTy &PredMap,
                            MachineBasicBlock *SrcBB) {
  assert(State->getOpcode() == VTM::Control && "Bad ucState!");
  bool IsRet = false;
  MachineBasicBlock *CurBB = State->getParent();
  unsigned startSlot = FInfo->getStartSlotFor(CurBB);

  vlang_raw_ostream &CtrlS = VM->getControlBlockBuffer();

  for (ucState::iterator I = State.begin(), E = State.end(); I != E; ++I) {
    ucOp Op = *I;
    unsigned Slot = Op->getPredSlot();
    assert(Slot != startSlot && "Unexpected first slot!");
    // Emit the control operation at the rising edge of the clock.
    std::string SlotPred = "(";
    raw_string_ostream SlotPredSS(SlotPred);

    SlotPredSS << getucStateEnable(CurBB, Slot - 1) << ')';

    // Emit the predicate operand.
    SlotPredSS << " & ";
    if (Op.getPredicate().getReg()) {
      SlotPredSS << '(';
      ucOperand &PredOp = Op.getPredicate();
      if (PredOp.isPredicateInverted()) SlotPredSS << '~';
      PredOp.print(SlotPredSS, 1, 0, true);
      SlotPredSS << ')';
    } else
      SlotPredSS << "1'b1";

    SlotPredSS.flush();

    // Special case for state transferring operation.
    if (VInstrInfo::isBrCndLike(Op->getOpcode())) {
      MachineBasicBlock *TargetBB = Op.getOperand(0).getMBB();

      // Emit control operation for next state.
      CtrlS.if_begin(SlotPred);
      if (TargetBB == CurBB) { // Self loop detected.
        CtrlS << "// Loop back to entry.\n";
        emitFirstCtrlState(CurBB, TargetBB);
      } else // Transfer to other state.
        emitNextFSMState(CtrlS, CurBB, TargetBB);

      CtrlS.exit_block();
      PredMap.insert(std::make_pair(TargetBB, SlotPred));
      continue;
    }

    CtrlS.if_begin(SlotPred);

    // Emit the operations.
    switch (Op->getOpcode()) {
    case VTM::VOpInternalCall:  emitOpInternalCall(Op);       break;
    case VTM::VOpReadReturn:    emitOpReadReturn(Op);         break;
    case VTM::VOpRetVal:        emitOpRetVal(Op);             break;
    case VTM::VOpRet:           emitOpRet(Op); IsRet = true;  break;
    case VTM::VOpMemTrans:      emitOpMemTrans(Op);           break;
    case VTM::VOpBRam:          emitOpBRam(Op);               break;
    case VTM::IMPLICIT_DEF:     emitImplicitDef(Op);          break;
    case VTM::VOpMove_ra:
    case VTM::VOpMove_ri:
    case VTM::VOpMove_rm:
    case VTM::VOpMove_rp:
    case VTM::VOpMove_rs:
    case VTM::VOpMove_rw:
    case VTM::COPY:             emitOpCopy(Op);               break;
    default:  assert(0 && "Unexpected opcode!");              break;
    }

    CtrlS.exit_block();
  }
  return IsRet;
}

void RTLCodegen::emitFirstCtrlState(MachineBasicBlock *SrcBB,
                                    MachineBasicBlock *DstBB) {
  // TODO: Emit PHINodes if necessary.
  ucState FirstState = *DstBB->getFirstNonPHI();
  assert(FInfo->getStartSlotFor(DstBB) == FirstState.getSlot()
         && "Broken Slot!");

  unsigned StateSlot = FirstState.getSlot();
  for (ucState::iterator I = FirstState.begin(), E = FirstState.end();
       I != E; ++I) {
    ucOp Op = *I;
    assert(Op->getOpcode() == VTM::IMPLICIT_DEF && "Unexpected operation!");
  }
}

void RTLCodegen::emitImplicitDef(ucOp &ImpDef) {
  raw_ostream &OS = VM->getDataPathBuffer();
  OS << "// IMPLICIT_DEF ";
  ImpDef.getOperand(0).print(OS);
  OS << "\n";
}

void RTLCodegen::emitOpCopy(ucOp &OpCopy) {
  raw_ostream &OS = VM->getControlBlockBuffer();
  OpCopy.getOperand(0).print(OS);
  OS << " <= ";
  OpCopy.getOperand(1).print(OS);
  OS << ";\n";
}

void RTLCodegen::emitOpReadReturn(ucOp &OpReadSymbol) {
  // Assign input port to some register.
  raw_ostream &OS = VM->getControlBlockBuffer();
  OpReadSymbol.getOperand(0).print(OS);
  OS << " <= "
     << getSubModulePortName(OpReadSymbol.getOperand(2).getImm(),
                             OpReadSymbol.getOperand(1).getSymbolName());
  OS << ";\n";
}

void RTLCodegen::emitOpInternalCall(ucOp &OpInternalCall) {
  // Assign input port to some register.
  raw_ostream &OS = VM->getControlBlockBuffer();
  unsigned FNNum = OpInternalCall.getOperand(1).getImm();
  const Function *FN = FInfo->getCalleeFN(FNNum);
  StringRef CalleeName = FN->getName();
  OS << "// Calling function: " << CalleeName << ";\n";
  Function::const_arg_iterator ArgIt = FN->arg_begin();
  for (unsigned i = 0, e = FN->arg_size(); i != e; ++i) {
    OS << getSubModulePortName(FNNum, ArgIt->getName()) << " <= ";
    OpInternalCall.getOperand(2 + i).print(OS);
    OS << ";\n";
    ++ArgIt;
  }
}

void RTLCodegen::emitOpRet(ucOp &OpArg) {
  raw_ostream &OS = VM->getControlBlockBuffer();
  OS << "NextFSMState <= state_idle;\n";
}

void RTLCodegen::emitOpRetVal(ucOp &OpRetVal) {
  raw_ostream &OS = VM->getControlBlockBuffer();
  unsigned retChannel = OpRetVal.getOperand(1).getImm();
  assert(retChannel == 0 && "Only support Channel 0!");
  OS << "return_value <= ";
  OpRetVal.getOperand(0).print(OS);
  OS << ";\n";
}

void RTLCodegen::emitOpMemTrans(ucOp &OpMemAccess) {
  unsigned FUNum = OpMemAccess->getFUId().getFUNum();

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
}

void RTLCodegen::emitOpBRam(ucOp &OpBRam) {
  unsigned FUNum = OpBRam->getFUId().getFUNum();

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
  VM->getDataPathBuffer() << "// Issue datapath for "
    "operations at slot " << State.getSlot() << '\n';

  for (ucState::iterator I = State.begin(), E = State.end(); I != E; ++I) {
    ucOp Op = *I;
    switch (Op->getOpcode()) {
    case VTM::VOpBitSlice:  emitOpBitSlice(Op);     break;
    case VTM::VOpBitCat:    emitOpBitCat(Op);       break;
    case VTM::VOpBitRepeat: emitOpBitRepeat(Op);    break;

    case VTM::VOpAdd:       emitOpAdd(Op);          break;
    case VTM::VOpMult:      emitOpMult(Op);         break;

    case VTM::VOpXor:       emitBinaryOp(Op, "^");  break;
    case VTM::VOpAnd:       emitBinaryOp(Op, "&");  break;
    case VTM::VOpOr:        emitBinaryOp(Op, "|");  break;
    case VTM::VOpSel:       emitOpSel(Op);         break;

    case VTM::VOpSHL:       emitBinaryOp(Op, "<<"); break;
    case VTM::VOpSRL:       emitBinaryOp(Op, ">>"); break;
    case VTM::VOpSRA:       emitOpSRA(Op);break;

    case VTM::VOpNot:       emitUnaryOp(Op, "~");   break;

    case VTM::VOpROr:       emitUnaryOp(Op, "|");   break;
    case VTM::VOpRAnd:      emitUnaryOp(Op, "&");   break;
    case VTM::VOpRXor:      emitUnaryOp(Op, "^");   break;

    default:  assert(0 && "Unexpected opcode!");    break;
    }
  }
}

void RTLCodegen::emitUnaryOp(ucOp &UnaOp, const std::string &Operator) {
  raw_ostream &OS = VM->getDataPathBuffer();
  OS << "assign ";
  UnaOp.getOperand(0).print(OS);
  OS << " = " << Operator << ' ';
  UnaOp.getOperand(1).print(OS);
  OS << ";\n";
}

void RTLCodegen::emitBinaryOp(ucOp &BinOp, const std::string &Operator) {
  raw_ostream &OS = VM->getDataPathBuffer();
  OS << "assign ";
  BinOp.getOperand(0).print(OS);
  OS << " = ";
  BinOp.getOperand(1).print(OS);
  OS << ' ' << Operator << ' ';
  BinOp.getOperand(2).print(OS);
  OS << ";\n";
}

std::string RTLCodegen::emitSignedOperand(ucOperand &Op) {
  unsigned BitWidth = cast<ucOperand>(Op).getBitWidth();

  raw_ostream &OS = VM->getDataPathBuffer();
  std::string WireName = "SignedWire" + utostr(SignedWireNum);
  OS << "wire signed" << verilogBitRange(BitWidth) << ' ' << WireName << " = ";
  Op.print(OS);
  OS << ";\n";

  ++SignedWireNum;
  return WireName;
}

void RTLCodegen::emitOpSRA(ucOp &OpSRA) {
  std::string Op0 = emitSignedOperand(OpSRA.getOperand(1));

  raw_ostream &OS = VM->getDataPathBuffer();
  OS << "assign ";
  OpSRA.getOperand(0).print(OS);
  OS << " = " << Op0 << " >>> ";
  OpSRA.getOperand(2).print(OS);
  OS << ";\n";
}

void RTLCodegen::emitOpSel(ucOp &OpSel) {
  raw_ostream &OS = VM->getDataPathBuffer();
  OS << "assign ";
  OpSel.getOperand(0).print(OS);
  OS << " = ";
  if (OpSel.getOperand(1).isPredicateInverted())
    OS << "~";
  OpSel.getOperand(1).print(OS, 1, 0, true);
  OS << " ? ";
  OpSel.getOperand(2).print(OS);
  OS << " : ";
  OpSel.getOperand(3).print(OS);
  OS << ";\n";

}

void RTLCodegen::emitOpAdd(ucOp &OpAdd) {
  raw_ostream &OS = VM->getDataPathBuffer();

  OS << "assign {";
  // Carry out.
  OpAdd.getOperand(1).print(OS);
  OS << ", ";
  // Sum.
  OpAdd.getOperand(0).print(OS);
  OS << "} = ";
  // Operands.
  OpAdd.getOperand(2).print(OS);
  OS << " + ";
  OpAdd.getOperand(3).print(OS);
  OS << " + ";
  // Carry in.
  OpAdd.getOperand(4).print(OS);
  OS << ";\n";
}

void RTLCodegen::emitOpMult(ucOp &OpMult) {
  raw_ostream &OS = VM->getDataPathBuffer();

  OS << "assign ";
  OpMult.getOperand(0).print(OS);
  OS << " = ";

  OpMult.getOperand(1).print(OS);
  OS << " * ";
  OpMult.getOperand(2).print(OS);
  OS << ";\n";
}

void RTLCodegen::emitOpBitSlice(ucOp &OpBitSlice) {
  raw_ostream &OS = VM->getDataPathBuffer();
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
  raw_ostream &OS = VM->getDataPathBuffer();
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
  raw_ostream &OS = VM->getDataPathBuffer();
  OS << "assign ";
  OpBitRepeat.getOperand(0).print(OS);
  OS << " = {";

  unsigned Times = OpBitRepeat.getOperand(2).getImm();
  OS << Times << '{';
  OpBitRepeat.getOperand(1).print(OS);
  OS << "}};\n";
}
