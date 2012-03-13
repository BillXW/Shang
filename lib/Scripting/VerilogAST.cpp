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

#include "vtm/VerilogAST.h"
#include "vtm/MicroState.h"
#include "vtm/Utilities.h"

#include "llvm/Constants.h"
#include "llvm/GlobalVariable.h"
#include "llvm/DerivedTypes.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "verilog-ast"
#include "llvm/Support/Debug.h"

#include <sstream>

using namespace llvm;

static cl::opt<bool>
EnableBBProfile("vtm-enable-bb-profile",
                cl::desc("Generate counters to profile the design"),
                cl::init(false));

//===----------------------------------------------------------------------===//
// Value and type printing

std::string llvm::verilogBitRange(unsigned UB, unsigned LB, bool printOneBit) {
  std::stringstream bw;
  assert(UB && UB >= LB && "Bad bit range!");
  --UB;
  if (UB != LB)
    bw << "[" << UB << ":" << LB << "]";
  else if(printOneBit)
    bw << "[" << LB << "]";

  return bw.str();
}

std::string llvm::verilogConstToStr(Constant *CPV) {
  if (ConstantInt* CI=dyn_cast<ConstantInt>(CPV)) {
    bool isMinValue=CI->isMinValue(true);
    uint64_t v = isMinValue ? CI->getZExtValue() :
                             (uint64_t)CI->getSExtValue();
    return verilogConstToStr(v,CI->getBitWidth(),isMinValue);
  } else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(CPV)) {
    switch (CE->getOpcode()) {
    case Instruction::PtrToInt:
    case Instruction::IntToPtr:
    case Instruction::BitCast:
      return verilogConstToStr(CE->getOperand(0));
    }

  }

  return "??Constant??";
}

std::string llvm::verilogConstToStr(uint64_t value, unsigned bitwidth,
                                    bool isMinValue) {
  std::stringstream pc;
  pc <<bitwidth<< '\'';
  if (bitwidth == 1) pc << 'b';
  else               pc << "h";
  // Mask the value that small than 4 bit to prevent printing something
  // like 1'hf out.
  if (bitwidth < 4)
    value &= (1 << bitwidth) - 1;

  if(isMinValue) {
    pc<<std::hex<<value;
    return pc.str();
  }

  std::stringstream ss;
  ss<<std::hex<<value;
  unsigned int uselength = (bitwidth/4) + (((bitwidth&0x3) == 0) ? 0 : 1);
  std::string sout = ss.str();
  if(uselength < sout.length())
    sout=sout.substr(sout.length()-uselength,uselength);
  pc<<sout;

  return pc.str();
}

//----------------------------------------------------------------------------//
// Helper function for Verilog RTL printing.

static raw_ostream &printAssign(raw_ostream &OS, const VASTWire *W) {
  OS << "assign " << W->getName() << verilogBitRange(W->getBitWidth(), 0, false)
     << " = ";
  return OS;
}

void VASTNode::dump() const { print(dbgs()); }

//----------------------------------------------------------------------------//
// Classes in Verilog AST.
VASTUse::VASTUse(VASTValue *v)
  : User(0, USE_Value), UB(v->getBitWidth()), LB(0) {
  Data.V = v;
}

void VASTUse::removeFromList() {
  if (VASTValue *List = getOrNull())
    List->removeUseFromList(this);
}

void VASTUse::setUser(VASTValue *User) {
  assert(!ilist_traits<VASTUse>::inAnyList(this)
         && "Not unlink from old list!");
  if (VASTValue *List = getOrNull()) {
    assert(List != User && "Unexpected cycle!");
    this->User.setPointer(User);
    List->addUseToList(this);
  }
}

VASTUse VASTUse::getBitSlice(unsigned ub, unsigned lb) const {
  // Rebase ub and lb.
  ub += LB;
  lb += LB;

  assert(ub > lb && ub <= UB && lb >= LB && "Bad bitslice range!");
  // Bit slice is contained.
  if (ub == UB && lb == LB) return *this;

  switch (getUseKind()) {
  case USE_Value: return VASTUse(Data.V, ub, lb);
  case USE_Immediate: return VASTUse(getBitSlice64(Data.ImmVal, ub, lb), ub-lb);
  case USE_Symbol: break;
  default: llvm_unreachable("Get bitslice on bad use kind!");
  }

  return VASTUse();
}

bool VASTUse::operator==(const VASTValue *RHS) const {
  return getUseKind() == USE_Value && LB == 0
    && Data.V == RHS && UB == RHS->getBitWidth();
}

void VASTUse::print(raw_ostream &OS) const {
  OS << '(';
  // Print the bit range if the value is have multiple bits.
  switch (getUseKind()) {
  case USE_Value:
    Data.V->printAsOperand(OS, UB, LB);
    break;
  case USE_Symbol:
    OS << Data.SymbolName;
    if (UB) OS << verilogBitRange(UB, LB, false);
    break;
  case USE_Immediate:
    OS << verilogConstToStr(Data.ImmVal, UB, false);
    // No need to print bit range for immediate.
    break;
  default: llvm_unreachable("Broken VASTUse type!");
  }

  OS << ')';
}

void VASTUse::PinUser() const {
  if (VASTSignal *S = dyn_cast_or_null<VASTSignal>(getOrNull()))
    S->Pin();
}

VASTUse::iterator VASTUse::dp_src_begin() {
  if (getUseKind() != USE_Value)
    return reinterpret_cast<VASTUse::iterator>(0);

  if (VASTWire *W = dyn_cast<VASTWire>(get()))
    return W->op_begin();

  return reinterpret_cast<VASTUse::iterator>(0);
}

VASTUse::iterator VASTUse::dp_src_end() {
  if (getUseKind() != USE_Value)  return reinterpret_cast<VASTUse::iterator>(0);

  if (VASTWire *W = dyn_cast<VASTWire>(get()))
    return W->op_end();

  return reinterpret_cast<VASTUse::iterator>(0);
}

VASTSlot::VASTSlot(unsigned slotNum, unsigned parentIdx, VASTModule *VM)
  :VASTNode(vastSlot, slotNum), StartSlot(slotNum), EndSlot(slotNum), II(~0),
   ParentIdx(parentIdx) {

  // Create the relative signals.
  std::string SlotName = "Slot" + utostr_32(slotNum);
  SlotReg = VM->addRegister(SlotName + "r", 1, slotNum == 0,
                        VASTModule::DirectClkEnAttr.c_str());

  VASTWire *Ready = VM->addWire(SlotName + "Ready", 1,
                                VASTModule::DirectClkEnAttr.c_str());
  SlotReady = Ready;
  // Someone is using the signal.
  SlotReady.setUser(0);

  VASTWire *Active = VM->addWire(SlotName + "Active", 1,
                                 VASTModule::DirectClkEnAttr.c_str());
  SlotActive = Active;
  // Someone is using the signal.
  SlotActive.setUser(0);

  VM->buildExpr(VASTWire::dpAnd, SlotReg, Ready, 1, Active);
  // We need alias slot to build the ready signal, keep it as unknown now.

  assert(slotNum >= parentIdx && "Slotnum earlier than parent start slot!");
}

void VASTSlot::addNextSlot(VASTSlot *NextSlot, VASTUse Cnd) {
  (NextSlot->PredSlots).push_back(this);
  bool Inserted = NextSlots.insert(std::make_pair(NextSlot, Cnd)).second;
  assert(Inserted && "NextSlot already existed!");
  (void) Inserted;
}

void VASTSlot::addEnable(VASTValue *V, VASTUse Cnd) {
  bool Inserted = Enables.insert(std::make_pair(V, Cnd)).second;
  assert(Inserted && "NextSlot already existed!");
  (void) Inserted;
}

void VASTSlot::addReady(VASTValue *V, VASTUse Cnd) {
  bool Inserted = Readys.insert(std::make_pair(V, Cnd)).second;
  assert(Inserted && "NextSlot already existed!");
  (void) Inserted;
}

void VASTSlot::addDisable(VASTValue *V, VASTUse Cnd) {
  bool Inserted = Disables.insert(std::make_pair(V, Cnd)).second;
  assert(Inserted && "NextSlot already existed!");
  (void) Inserted;
}

VASTUse VASTSlot::buildFUReadyExpr(VASTModule &VM) {
  SmallVector<VASTUse, 4> Ops;

  for (VASTSlot::const_fu_ctrl_it I = ready_begin(), E = ready_end();
        I != E; ++I)
    // Print the code for ready signal.
    // If the condition is true then the signal must be 1 to ready.
    Ops.push_back(VM.buildExpr(VASTWire::dpOr, I->first,
                               VM.buildNotExpr(I->second), 1));
  
  // No waiting signal means always ready.
  if (Ops.empty()) Ops.push_back(VASTUse(true, 1));
  
  return VM.buildExpr(VASTWire::dpAnd, Ops, 1);
}

void VASTSlot::buildReadyLogic(VASTModule &Mod) {
  SmallVector<VASTUse, 4> Ops;
  // FU ready for current slot.
  Ops.push_back(buildFUReadyExpr(Mod));

  if (StartSlot != EndSlot) {
    for (unsigned slot = StartSlot; slot < EndSlot; slot += II) {
      if (slot == getSlotNum()) continue;

      VASTSlot *AliasSlot = Mod.getSlot(slot);

      if (!AliasSlot->readyEmpty()) {
        // FU ready for alias slot, when alias slot register is 1, its waiting
        // signal must be 1.
        Ops.push_back(Mod.buildExpr(VASTWire::dpOr,
                                  Mod.buildNotExpr(AliasSlot->getRegister()),
                                  AliasSlot->buildFUReadyExpr(Mod), 1));
      }
    }
  }

  // All signals should be 1.
  Mod.buildExpr(VASTWire::dpAnd, Ops, 1, cast<VASTWire>(getReady()));
}

bool VASTSlot::hasNextSlot(VASTSlot *NextSlot) const {
  if (NextSlots.empty()) return NextSlot->getSlotNum() == getSlotNum() + 1;

  return NextSlots.count(NextSlot);
}

void VASTSlot::buildCtrlLogic(VASTModule &Mod) {
  vlang_raw_ostream &CtrlS = Mod.getControlBlockBuffer();
  // TODO: Build the AST for these logic.
  CtrlS.if_begin(getName());
  bool ReadyPresented = !readyEmpty();

  // DirtyHack: Remember the enabled signals in alias slots, the signal may be
  // assigned at a alias slot.
  std::set<const VASTValue*> AliasEnables;
  // A slot may be enable by its alias slot if II of a pipelined loop is 1.
  VASTUse PredAliasSlots;

  if (StartSlot != EndSlot) {
    CtrlS << "// Alias slots: ";

    for (unsigned slot = StartSlot; slot < EndSlot; slot += II) {
      CtrlS << slot << ", ";
      if (slot == getSlotNum()) continue;

      const VASTSlot *AliasSlot = Mod.getSlot(slot);
      if (AliasSlot->hasNextSlot(this)) {
        assert(PredAliasSlots.isInvalid()
               && "More than one PredAliasSlots found!");
        PredAliasSlots = AliasSlot->getActive();
      }

      for (VASTSlot::const_fu_ctrl_it I = AliasSlot->enable_begin(),
           E = AliasSlot->enable_end(); I != E; ++I) {
        bool inserted = AliasEnables.insert(I->first).second;
        assert(inserted && "The same signal is enabled twice!");
        (void) inserted;
      }

      ReadyPresented  |= !AliasSlot->readyEmpty();
    }

    CtrlS << '\n';
  } // SS flushes automatically here.

  DEBUG(
  if (getSlotNum() != 0)
    CtrlS << "$display(\"" << getName() << " in " << Mod.getName()
          << " ready at %d\", $time());\n";
  );

  bool hasSelfLoop = false;
  SmallVector<VASTUse, 2> EmptySlotEnCnd;

  if (hasExplicitNextSlots()) {
    CtrlS << "// Enable the successor slots.\n";
    for (VASTSlot::const_succ_cnd_iterator I = succ_cnd_begin(),E = succ_cnd_end();
         I != E; ++I) {
      hasSelfLoop |= I->first->getSlotNum() == getSlotNum();
      VASTRegister *NextSlotReg = I->first->getRegister();
      Mod.addAssignment(NextSlotReg, I->second, this, EmptySlotEnCnd);
    }
  } else {
    // Enable the default successor slots.
    VASTSlot *NextSlot = Mod.getSlot(getSlotNum() + 1);
    VASTRegister *NextSlotReg = NextSlot->getRegister();
    Mod.addAssignment(NextSlotReg, VASTUse(true, 1), this, EmptySlotEnCnd);
    // And connect the fall through edge now.
    addNextSlot(NextSlot);
  }

  assert(!(hasSelfLoop && !PredAliasSlots.isInvalid())
         && "Unexpected have self loop and pred alias slot at the same time.");
  // Do not assign a value to the current slot enable twice.
  if (!hasSelfLoop) {
    // Only disable the current slot if there is no alias slot enable current
    // slot.
    if (!PredAliasSlots.isInvalid())
      EmptySlotEnCnd.push_back(Mod.buildNotExpr(PredAliasSlots));

    // Disable the current slot.
    Mod.addAssignment(getRegister(), VASTUse((int64_t)0, 1), this,
                      EmptySlotEnCnd);
  }

  if (!ReadyPresented) {
    //DEBUG(
    if (getSlotNum() != 0) {
      CtrlS << "if (start) begin $display(\"" << getName() << " in "
            << Mod.getName()
            << " bad start %b\\n\", start);  $finish(); end\n";

      CtrlS << "if (Slot0r) begin $display(\"" << getName() << " in "
            << Mod.getName()
            << " bad Slot0 %b\\n\", Slot0r);  $finish(); end\n";
    }

    CtrlS << "if (mem0en_r) begin $display(\"" << getName() << " in "
          << Mod.getName()
          << " bad mem0en_r %b\\n\", mem0en_r);  $finish(); end\n";
    //);
  }

  std::string SlotReady = std::string(getName()) + "Ready";
  CtrlS << "// Enable the active FUs.\n";
  for (VASTSlot::const_fu_ctrl_it I = enable_begin(), E = enable_end();
       I != E; ++I) {
    // No need to wait for the slot ready.
    // We may try to enable and disable the same port at the same slot.
    EmptySlotEnCnd.clear();
    EmptySlotEnCnd.push_back(getRegister());
    Mod.addAssignment(cast<VASTRegister>(I->first),
                      Mod.buildExpr(VASTWire::dpAnd, getReady(), I->second, 1),
                      this, EmptySlotEnCnd, false);
  }

  SmallVector<VASTUse, 4> DisableAndCnds;
  if (!disableEmpty()) {
    CtrlS << "// Disable the resources when the condition is true.\n";
    for (VASTSlot::const_fu_ctrl_it I = disable_begin(), E = disable_end();
         I != E; ++I) {
      // Look at the current enable set and alias enables set;
      // The port assigned at the current slot, and it will be disabled if
      // The slot is not ready or the enable condition is false. And it is
      // ok that the port is enabled.
      if (isEnabled(I->first)) continue;

      DisableAndCnds.push_back(getRegister());
      // If the port enabled in alias slots, disable it only if others slots is
      // not active.
      bool AliasEnabled = AliasEnables.count(I->first);
      if (AliasEnabled) {
        for (unsigned slot = StartSlot; slot < EndSlot; slot += II) {
          if (slot == getSlotNum()) continue;

          VASTSlot *ASlot = Mod.getSlot(slot);
          assert(!ASlot->isDiabled(I->first)
                 && "Same signal disabled in alias slot!");
          if (ASlot->isEnabled(I->first)) {
            DisableAndCnds.push_back(Mod.buildNotExpr(ASlot->getRegister()));
            continue;
          }
        }
      }

      DisableAndCnds.push_back(I->second);

      VASTRegister *En = cast<VASTRegister>(I->first);
      Mod.addAssignment(En, VASTUse((int64_t)0, 1), this, DisableAndCnds, false);
      DisableAndCnds.clear();
    }
  }
  CtrlS.exit_block("\n\n");
}

const char *VASTSlot::getName() const { return SlotReg->getName(); }

void VASTSlot::print(raw_ostream &OS) const {
  llvm_unreachable("VASTSlot::print should not be called!");
}

VASTRegister::VASTRegister(const char *Name, unsigned BitWidth,
                           unsigned initVal, const char *Attr)
  : VASTSignal(vastRegister, Name, BitWidth, Attr), InitVal(initVal) {}

void VASTRegister::addAssignment(VASTUse *Src, VASTWire *AssignCnd) {
  bool inserted = Assigns.insert(std::make_pair(AssignCnd, Src)).second;
  assert(inserted &&  "Assignment condition conflict detected!");
  Src->setUser(this);
  Slots.insert(AssignCnd->getSlot());
}

VASTUse VASTRegister::getConstantValue() const {
  std::set<VASTUse> Srcs;
  for (assign_itertor I = assign_begin(), E = assign_end(); I != E; ++I)
    Srcs.insert(*I->second);

  // Only 1 assignment?
  if (Srcs.size() != 1) return VASTUse();

  VASTUse C = *Srcs.begin();

  if (!C.isImm()) return VASTUse();

  // Some register assignment may have un-match bit-width
  return VASTUse(C.getImm(), getBitWidth());
}

void VASTRegister::printCondition(raw_ostream &OS, const VASTSlot *Slot,
                                  const AndCndVec &Cnds) {
  OS << '(';
  if (Slot) {
    VASTUse Active = Slot->getActive();
    Active.print(OS);
    Active.PinUser();
  } else      OS << "1'b1";

  typedef AndCndVec::const_iterator and_it;
  for (and_it CI = Cnds.begin(), CE = Cnds.end(); CI != CE; ++CI) {
    OS << " & ";
    CI->print(OS);
    CI->PinUser();
  }

  OS << ')';
}

void VASTRegister::printReset(raw_ostream &OS) const {
  OS << getName()  << " <= "
     << verilogConstToStr(InitVal, getBitWidth(), false) << ";";
}

void VASTRegister::printAssignment(vlang_raw_ostream &OS) const {
  if (Assigns.empty()) return;

  std::string Pred;
  raw_string_ostream SS(Pred);
  bool UseSwitch = Assigns.size() > 1;

  typedef std::vector<VASTWire*> OrVec;
  typedef std::map<VASTUse, OrVec> CSEMapTy;
  CSEMapTy SrcCSEMap;
  for (assign_itertor I = assign_begin(), E = assign_end(); I != E; ++I)
    SrcCSEMap[*I->second].push_back(I->first);

  OS << "\n// Assignment of " << getName() << '\n';
  if (UseSwitch) {
    OS << VASTModule::ParallelCaseAttr << ' ';
    OS.switch_begin("1'b1");
  }

  typedef CSEMapTy::iterator it;
  for (it I = SrcCSEMap.begin(), E = SrcCSEMap.end(); I != E; ++I) {
    SS << '(';

    OrVec &Ors = I->second;
    for (OrVec::iterator OI = Ors.begin(), OE = Ors.end(); OI != OE; ++OI) {
      (*OI)->printAsOperand(SS);
      SS << '|';
    }

    SS << "1'b0)";
    SS.flush();
    // Print the assignment under the condition.
    if (UseSwitch) OS.match_case(Pred);
    else OS.if_begin(Pred);
    printAsOperand(OS);
    OS << " <= ";
    I->first.print(OS);
    OS << ";\n";
    OS.exit_block();

    Pred.clear();
  }

  if (UseSwitch) OS.switch_end();
}

VASTWire::VASTWire(const char *Name, unsigned BitWidth, const char *Attr,
                   VASTUse *ops, uint8_t numOps, Opcode opc)
  : VASTSignal(vastWire, Name, BitWidth, Attr), Ops(ops), NumOps(numOps),
    Opc(opc) {
  Context.Latency = 0;
  assert((numOps || Ops == 0)&& "Unexpected empty operand list!");
  if (ops) buildUseList();
}

void VASTWire::setExpr(VASTUse *ops, uint8_t numOps, Opcode opc) {
  assert(((numOps) || (opc  == Dead && !isPinned()))
         && "Cannot set expression!");
  Ops = ops;
  NumOps = numOps;
  Opc = opc;

  buildUseList();
}

void VASTWire::buildUseList() {
  for (VASTUse *I = Ops, *E = Ops + NumOps; I != E; ++I)
    I->setUser(this);
}

std::string VASTModule::DirectClkEnAttr = "";
std::string VASTModule::ParallelCaseAttr = "";
std::string VASTModule::FullCaseAttr = "";

VASTModule::~VASTModule() {
  // Release all ports.
  Ports.clear();
  Wires.clear();
  Registers.clear();
  Slots.clear();
  Allocator.Reset();
  UseAllocator.DestroyAll();
  SymbolTable.clear();
  BBLatInfo.clear();

  delete &(DataPath.str());
  delete &(ControlBlock.str());
}

void VASTModule::printDatapath(raw_ostream &OS) const{
  for (WireVector::const_iterator I = Wires.begin(), E = Wires.end();
       I != E; ++I) {
    VASTWire *W = *I;
    if (W->isDead()) continue;

    (*I)->print(OS);
  }
}

void VASTModule::printRegisterAssign(vlang_raw_ostream &OS) const {
  for (RegisterVector::const_iterator I = Registers.begin(), E = Registers.end();
       I != E; ++I)
    (*I)->printAssignment(OS);
}

void VASTModule::buildSlotLogic(VASTModule::StartIdxMapTy &StartIdxMap) {
  for (SlotVecTy::const_iterator I = Slots.begin(), E = Slots.end();I != E;++I)
    if (VASTSlot *S = *I) {
      S->buildReadyLogic(*this);
      S->buildCtrlLogic(*this);

      // Create a profile counter for each BB.
      if (EnableBBProfile) writeProfileCounters(S, StartIdxMap);
    }
}

void VASTModule::printModuleDecl(raw_ostream &OS) const {
  OS << "module " << getName() << "(\n";
  Ports.front()->print(OS.indent(4));
  for (PortVector::const_iterator I = Ports.begin() + 1, E = Ports.end();
       I != E; ++I) {
    OS << ",\n";
    (*I)->print(OS.indent(4));
  }
  OS << ");\n";
}

void VASTModule::printSignalDecl(raw_ostream &OS) {
  for (WireVector::const_iterator I = Wires.begin(), E = Wires.end();
       I != E; ++I) {
    VASTWire *W = *I;
    if (W->isDead()) continue;

    W->printDecl(OS);
    OS << "\n";
  }

  for (RegisterVector::const_iterator I = Registers.begin(), E = Registers.end();
       I != E; ++I) {
    (*I)->printDecl(OS);
    OS << "\n";
  }
}

void VASTModule::printRegisterReset(raw_ostream &OS) {
  for (RegisterVector::const_iterator I = Registers.begin(), E = Registers.end();
       I != E; ++I) {
    (*I)->printReset(OS);
    OS << "\n";
  }
}

VASTUse VASTModule::buildNotExpr(VASTUse U) {
  return buildExpr(VASTWire::dpNot, U, U.getBitWidth());
}

VASTUse VASTModule::buildExpr(VASTWire::Opcode Opc, VASTUse Op,
                              unsigned BitWidth, VASTWire *DstWire) {
  switch (Opc) {
  case VASTWire::dpNot: {
     // Try to fold the not expression.
    if (VASTWire *E = dyn_cast_or_null<VASTWire>(Op.getOrNull()))
      if (E->getOpcode() == VASTWire::dpNot) {
        // We should also propagate the bit slice information.
        VASTUse U = E->getOperand(0).getBitSlice(Op.UB, Op.LB);
        return buildExpr(VASTWire::dpAssign, U, BitWidth, DstWire);
      }
    if (Op.isImm()) {
      unsigned Size = Op.getBitWidth();
      return buildExpr(VASTWire::dpAssign,
                       VASTUse(getBitSlice64(~Op.getImm(), Size), Size),
                       Size, DstWire);

    }
    break;
  }
  case VASTWire::dpAssign: {
    SmallVector<VASTWire*, 8> UpdatedUsers;

    if (!DstWire) return Op;

    // The wire can be ignored if it is replaced in all its users.
    if (replaceAndUpdateUseTree(DstWire, Op)) {
      if (!DstWire->isPinned())
        DstWire->setExpr(0, 0, VASTWire::Dead);
      else {
        // Build data-path logic for pinned signal.
        VASTUse Ops[] = { Op };
        createExpr(Opc, Ops, BitWidth, DstWire);
      }

      return Op;
    }

    break;
  }
  default: break;
  }

  VASTUse Ops[] = { Op };
  return createExpr(Opc, Ops, BitWidth, DstWire);
}

VASTUse VASTModule::buildLogicExpr(VASTWire::Opcode Opc, VASTUse LHS, VASTUse RHS,
                                  unsigned BitWidth, VASTWire *DstWire) {
  // Try to make RHS to be an constant.
  if (LHS.isImm()) std::swap(LHS, RHS);

  if (RHS.isImm()) {
    VASTUse ValForRHSIsZero, ValForRHSIsAllOnes;
    switch (Opc) {
    default: break;
    case VASTWire::dpAnd: {
      ValForRHSIsZero = RHS;
      ValForRHSIsAllOnes = LHS;
      break;
    }
    case VASTWire::dpOr: {
      ValForRHSIsZero = LHS;
      ValForRHSIsAllOnes = RHS;
      break;
    }
    }

    if (RHS.getImm() == 0)
      return buildExpr(VASTWire::dpAssign, ValForRHSIsZero, BitWidth, DstWire);

    if (getBitSlice64(RHS.getImm(), BitWidth) == getBitSlice64(-1, BitWidth))
      return buildExpr(VASTWire::dpAssign, ValForRHSIsAllOnes, BitWidth, DstWire);
  }


  VASTUse Ops[] = { LHS, RHS };
  return createExpr(Opc, Ops, BitWidth, DstWire);
}

VASTUse VASTModule::buildExpr(VASTWire::Opcode Opc, VASTUse LHS, VASTUse RHS,
                              unsigned BitWidth, VASTWire *DstWire) {
  switch (Opc) {
  default: break;
  case VASTWire::dpAnd: case VASTWire::dpOr: case VASTWire::dpXor:
    return buildLogicExpr(Opc, LHS, RHS, BitWidth, DstWire);
  }

  VASTUse Ops[] = { LHS, RHS };
  return createExpr(Opc, Ops, BitWidth, DstWire);
}

VASTUse VASTModule::buildExpr(VASTWire::Opcode Opc, VASTUse Op0, VASTUse Op1,
                              VASTUse Op2, unsigned BitWidth, VASTWire *DstWire) {
  VASTUse Ops[] = { Op0, Op1, Op2 };
  return buildExpr(Opc, Ops, BitWidth, DstWire);
}

VASTUse VASTModule::buildExpr(VASTWire::Opcode Opc, ArrayRef<VASTUse> Ops,
                              unsigned BitWidth, VASTWire *DstWire) {
  switch (Opc) {
  default: break;
  case VASTWire::dpNot: case VASTWire::dpAssign: {
    assert(Ops.size() == 1 && "Bad operand number!");
    return buildExpr(Opc, Ops[0], BitWidth, DstWire);
  }
  case VASTWire::dpAnd: case VASTWire::dpOr: {
    if (Ops.size() == 1)
      return buildExpr(VASTWire::dpAssign, Ops[0], BitWidth, DstWire);
    else if(Ops.size() == 2)
      return buildLogicExpr(Opc, Ops[0], Ops[1], BitWidth, DstWire);
    break;
  }
  }

  return createExpr(Opc, Ops, BitWidth, DstWire);
}

bool VASTModule::replaceAndUpdateUseTree(VASTValue *From, VASTUse To) {
  SmallVector<VASTWire*, 8> UpdatedUsers;
  // Update the use list of DstWire.
  bool AllReplaced = From->replaceAllUseWith(To, &UpdatedUsers);
  // Try to re-evaluate the users.
  while (!UpdatedUsers.empty()) {
    VASTWire *V = UpdatedUsers.back();
    UpdatedUsers.pop_back();

    buildExpr(V->getOpcode(), V->getOperands(), V->getBitWidth(), V);
  }

  return AllReplaced;
}

VASTWire *VASTModule::updateExpr(VASTWire *W, VASTWire::Opcode Opc,
                                 ArrayRef<VASTUse> Ops) {
  assert(W->num_operands() >= Ops.size()
         && "Unexpected number operands increased!");
  // Unlink all operand from its use list.
  W->dropOperandsFromUseList();

  std::uninitialized_copy(Ops.begin(), Ops.end(), W->Ops);
  W->setExpr(W->Ops, Ops.size(), Opc);

  return W;
}

VASTWire *VASTModule::createExpr(VASTWire::Opcode Opc, ArrayRef<VASTUse> Ops,
                                 unsigned BitWidth, VASTWire *DstWire) {
  assert(!Ops.empty() && "Unexpected empty expression");

  if (DstWire && DstWire->hasExpr()) {
    // We are re-evaluating an expression but nothing changed.
    if (DstWire->getOpcode() == Opc && DstWire->num_operands() == Ops.size())
      return DstWire;

    return updateExpr(DstWire, Opc, Ops);
  }

  VASTUse *OpArray = UseAllocator.Allocate(Ops.size());
  std::uninitialized_copy(Ops.begin(), Ops.end(), OpArray);
  if (DstWire) {
    DstWire->setExpr(OpArray, Ops.size(), Opc);
    return DstWire;
  }

  VASTWire *D = Allocator.Allocate<VASTWire>();
  return new (D) VASTWire(0, BitWidth, "", OpArray, Ops.size(), Opc);
}

VASTWire *VASTModule::buildAssignCnd(VASTSlot *Slot,
                                     SmallVectorImpl<VASTUse> &Cnds,
                                     bool AddSlotActive) {
  // We only assign the Src to Dst when the given slot is active.
  if (AddSlotActive) Cnds.push_back(Slot->getActive());
  VASTWire *AssignAtSlot = cast<VASTWire>(buildExpr(VASTWire::cpAssignAtSlot,
                                          buildExpr(VASTWire::dpAnd,Cnds,1), 1));
  AssignAtSlot->setSlot(Slot);
  // Recover the condition vector.
  if (AddSlotActive) Cnds.pop_back();

  return AssignAtSlot;
}

void VASTModule::addAssignment(VASTRegister *Dst, VASTUse Src, VASTSlot *Slot,
                               SmallVectorImpl<VASTUse> &Cnds,
                               bool AddSlotActive) {
  // No need to assign the invalid value.
  if (Src.isInvalid()) return;

  VASTUse *U = new (UseAllocator.Allocate()) VASTUse(Src);
  Dst->addAssignment(U, buildAssignCnd(Slot, Cnds, AddSlotActive));
}

bool VASTModule::eliminateConstRegisters() {
  bool Changed = false;
  typedef RegisterVector::iterator it;
  //for (it I = Registers.begin(), E = Registers.end(); I != E; ++I) {
    //VASTRegister *R = *I;

    // VASTUse Const = R->getConstantValue();

    //if (Const.isInvalid()) continue;

    // Replace the register by constant.
    // FIXME: Also check no one read the register before it is assign, otherwise
    // it is not safe to do the replacement.
    //if (replaceAndUpdateUseTree(R, Const))
    //  R->clearAssignments();

    // Changed = true;
  //}

  return Changed;
}

void VASTModule::print(raw_ostream &OS) const {
  // Print the verilog module?
}

VASTPort *VASTModule::addInputPort(const std::string &Name, unsigned BitWidth,
                                   PortTypes T /*= Others*/) {
  // DIRTYHACK: comment out the deceleration of the signal for ports.
  VASTWire *W = addWire(Name, BitWidth, "//");
  // Do not remove the wire for input port.
  W->setAsInput();

  VASTPort *Port = new (Allocator.Allocate<VASTPort>()) VASTPort(W, true);
  if (T < SpecialInPortEnd) {
    assert(Ports[T] == 0 && "Special port exist!");
    Ports[T] = Port;
    return Port;
  }

  // Return port is a output port.
  assert(T < RetPort && "Wrong port type!");
  if (T == ArgPort) {
    assert(NumArgPorts == Ports.size() - NumSpecialPort
      && "Unexpected port added before arg port!");
    ++NumArgPorts;
  }

  Ports.push_back(Port);
  return Port;
}

VASTPort *VASTModule::addOutputPort(const std::string &Name, unsigned BitWidth,
                                    PortTypes T /*= Others*/,
                                    bool isReg /*= true*/) {
  VASTSignal *V = 0;
  // DIRTYHACK: comment out the deceleration of the signal for ports.
  if (isReg) V = addRegister(Name, BitWidth, 0, "//");
  else       V = addWire(Name, BitWidth, "//");

  // Do not remove the signal for output port.
  V->Pin();

  VASTPort *Port = new (Allocator.Allocate<VASTPort>()) VASTPort(V, false);
  if (SpecialInPortEnd <= T && T < SpecialOutPortEnd) {
    assert(Ports[T] == 0 && "Special port exist!");
    Ports[T] = Port;
    return Port;
  }

  assert(T <= RetPort && "Wrong port type!");
  if (T == RetPort) {
    RetPortIdx = Ports.size();
    assert(RetPortIdx == NumArgPorts + NumSpecialPort
      && "Unexpected port added before return port!");
  }

  Ports.push_back(Port);
  return Port;
}

VASTUse VASTModule::indexVASTValue(unsigned RegNum, VASTUse V) {
  VASTUse *U = new (UseAllocator.Allocate()) VASTUse(V);
  bool inserted = RegsMap.insert(std::make_pair(RegNum, U)).second;
  assert(inserted && "RegNum already indexed some value!");

  // We want to replace the indexed value.
  U->setUser(0);

  return V;
}

VASTRegister *VASTModule::addRegister(const std::string &Name, unsigned BitWidth,
                                      unsigned InitVal, const char *Attr) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name);
  assert(Entry.second == 0 && "Symbol already exist!");
  VASTRegister *Reg = Allocator.Allocate<VASTRegister>();
  new (Reg) VASTRegister(Entry.getKeyData(), BitWidth, InitVal, Attr);
  Entry.second = Reg;
  Registers.push_back(Reg);

  return Reg;
}

VASTRegister *VASTModule::addRegister(unsigned RegNum, unsigned BitWidth,
                                      unsigned InitVal,
                                      const char *Attr) {
  std::string Name;

  if (TargetRegisterInfo::isVirtualRegister(RegNum))
    Name = "v" + utostr_32(TargetRegisterInfo::virtReg2Index(RegNum));
  else
    Name = "p" + utostr_32(RegNum);

  Name += "r";

  VASTRegister *R = addRegister(Name, BitWidth, 0, Attr);
  indexVASTValue(RegNum, R);
  return R;
}

VASTWire *VASTModule::addWire(const std::string &Name, unsigned BitWidth,
                              const char *Attr) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name);
  assert(Entry.second == 0 && "Symbol already exist!");
  VASTWire *Wire = Allocator.Allocate<VASTWire>();
  new (Wire) VASTWire(Entry.getKeyData(), BitWidth, Attr);
  Entry.second = Wire;
  Wires.push_back(Wire);

  return Wire;
}

VASTWire *VASTModule::addWire(unsigned WireNum, unsigned BitWidth,
                              const char *Attr) {
  std::string Name;

  assert(TargetRegisterInfo::isVirtualRegister(WireNum)
         && "Unexpected physics register as wire!");
  Name = "w" + utostr_32(TargetRegisterInfo::virtReg2Index(WireNum)) + "w";

  VASTWire *W = addWire(Name, BitWidth, Attr);
  indexVASTValue(WireNum, W);
  return W;
}

// Out of line virtual function to provide home for the class.
void VASTModule::anchor() {}

void VASTModule::writeProfileCounters(VASTSlot *S,
                                      VASTModule::StartIdxMapTy &StartIdxMap) {
  std::string BBCounter = "cnt" + utostr_32(S->getParentIdx());
  std::string FunctionCounter = "cnt" + getName();
  vlang_raw_ostream &CtrlS = getControlBlockBuffer();
  
  // Create the profile counter.
  // Write the counter for the function.
  if (S->getSlotNum() == 0) {
    addRegister(FunctionCounter, 64)->Pin();
    CtrlS.if_begin(getPortName(VASTModule::Finish));
    CtrlS << "$display(\"Module: " << getName();

    CtrlS << " total cycles" << "->%d\"," << FunctionCounter << ");\n";
    CtrlS.exit_block() << "\n";
  } else { // Dont count the ilde state at the moment.
    if (S->getParentIdx() == S->getSlotNum()) {
      unsigned ParentIdx = S->getParentIdx();
      addRegister(BBCounter, 64)->Pin();

      CtrlS.if_begin(getPortName(VASTModule::Finish));
      CtrlS << "$display(\"Module: " << getName();
      // Write the parent MBB name.
      if (ParentIdx) {
        const MachineBasicBlock *MBB = StartIdxMap.lookup(ParentIdx);
        CtrlS << " MBB#" << MBB->getNumber() << ": " << MBB->getName();
      }

      CtrlS << ' ' << "->%d\"," << BBCounter << ");\n";
      CtrlS.exit_block() << "\n";
    }

    // Increase the profile counter.
    if (S->isLeaderSlot()) {
      CtrlS.if_() << S->getRegister()->getName();
      for (unsigned i = S->alias_start(), e = S->alias_end(),
        k = S->alias_ii(); i < e; i += k) {
          CtrlS << '|' << getSlot(i)->getRegister()->getName();
      }

      CtrlS._then();
      CtrlS << BBCounter << " <= " << BBCounter << " +1;\n";
      CtrlS << FunctionCounter << " <= " << FunctionCounter << " +1;\n";
      CtrlS.exit_block() << "\n";
    }
  }
}


void VASTValue::print(raw_ostream &OS) const {
  assert(0 && "VASTValue::print should not be called!");
}

void VASTValue::printAsOperand(raw_ostream &OS, unsigned UB, unsigned LB) const{
  if (!getName()) {
    assert(UB == getBitWidth() && LB == 0
           && "Cannot print unamed object as operand!");
    print(OS);
    return;
  }

  OS << getName();
  if (UB) OS << verilogBitRange(UB, LB, getBitWidth() > 1);
}

bool VASTValue::replaceAllUseWith(VASTUse To,
                                  SmallVectorImpl<VASTWire*> *ReplacedUsers) {
  assert(To.getBitWidth() == getBitWidth() && "Bitwidth not match!");

  typedef VASTValue::use_iterator it;
  for (it I = use_begin(), E = use_end(); I != E; /*++I*/) {
    VASTUse *U = I.get();
    ++I;
    VASTUse NewU = To.getBitSlice(U->UB, U->LB);
    if (!NewU.isInvalid()) {
      VASTValue *User = U->getUser();
      // Unlink from old list.
      removeUseFromList(U);
      // Move to new list.
      U->set(NewU);
      U->setUser(User);

      if (!ReplacedUsers) continue;

      if (VASTWire *UserWire = dyn_cast_or_null<VASTWire>(User)) {
        assert(UserWire->hasExpr() && UserWire->num_operands()
               && "Use list broken!");
        ReplacedUsers->push_back(UserWire);
      }
    }
  }

  return use_empty();
}

void VASTSymbol::print(raw_ostream &OS) const {
  assert(0 && "VASTSymbol::print should not be called!");
}

void VASTPort::print(raw_ostream &OS) const {
  if (isInput())
    OS << "input ";
  else
    OS << "output ";

  if (isRegister())
    OS << "reg";
  else
    OS << "wire";

  if (getBitWidth() > 1) OS << "[" << (getBitWidth() - 1) << ":0]";

  OS << ' ' << getName();
}

void VASTPort::printExternalDriver(raw_ostream &OS, uint64_t InitVal) const {
  if (isInput())
    // We need a reg to drive input port.
    OS << "reg";
  else
    // We need a wire to accept the output value from dut.
    OS << "wire";

  if (getBitWidth() > 1)
    OS << "[" << (getBitWidth() - 1) << ":0]";

  OS << ' ' << getName();

  if (isInput())
    OS << " = " << verilogConstToStr(InitVal, getBitWidth(), false);

  OS << ';';
}

std::string VASTPort::getExternalDriverStr(unsigned InitVal) const {
  std::string ret;
  raw_string_ostream ss(ret);
  printExternalDriver(ss, InitVal);
  ss.flush();
  return ret;
}

// Out of line virtual function to provide home for the class.
void VASTPort::anchor() {}

void VASTSignal::printDecl(raw_ostream &OS) const {
  OS << AttrStr << ' ';
  if (isRegister())
    OS << "reg";
  else
    OS << "wire";

  if (getBitWidth() > 1)
    OS << "[" << (getBitWidth() - 1) << ":0]";

  OS << ' ' << getName();

  if (isRegister())
    OS << " = " << verilogConstToStr(0, getBitWidth(), false);

  OS << ";";
}

//----------------------------------------------------------------------------//
// Operand printing helper functions.
static void printSignedOperand(raw_ostream &OS, VASTUse U) {
  OS << "$signed(";
  U.print(OS);
  OS << ")";
}

static void printUnsignedOperand(raw_ostream &OS, VASTUse U) {
  U.print(OS);
}

template<typename PrintOperandFN>
static void printSimpleOp(raw_ostream &OS, ArrayRef<VASTUse> Ops,
                          const char *Opc, PrintOperandFN &FN) {
  unsigned NumOps = Ops.size();
  assert(NumOps && "Unexpected zero operand!");
  FN(OS, Ops[0]);

  for (unsigned i = 1; i < NumOps; ++i) {
    OS << Opc;
    FN(OS, Ops[i]);
  }
}

static void printSimpleUnsignedOp(raw_ostream &OS, ArrayRef<VASTUse> Ops,
                                  const char *Opc) {
  printSimpleOp(OS, Ops, Opc, printUnsignedOperand);
}

//static void printSimpleSignedOp(raw_ostream &OS, ArrayRef<VASTUse> Ops,
//                                  const char *Opc) {
//  printSimpleOp(OS, Ops, Opc, printSignedOperand);
//}

//----------------------------------------------------------------------------//
// Generic datapath printing helper function.
static void printUnaryOp(raw_ostream &OS, VASTUse U, const char *Opc) {
  OS << Opc;
  U.print(OS);
}

static void printSRAOp(raw_ostream &OS, ArrayRef<VASTUse> Ops) {
  printSignedOperand(OS, Ops[0]);
  OS << " >>> ";
  Ops[1].print(OS);
}

template<typename PrintOperandFN>
static void printCmpFU(raw_ostream &OS, ArrayRef<VASTUse> Ops,
                       PrintOperandFN &FN) {
  OS << "{ ((";
  // Port 4: gt.
  printSimpleOp<PrintOperandFN>(OS, Ops, " > ", FN);
  OS << ") ? 1'b1 : 1'b0), ((";
  // Port 3: gt.
  printSimpleOp<PrintOperandFN>(OS, Ops, " >= ", FN);
  OS << ") ? 1'b1 : 1'b0), ((";
  // Port 2: gt.
  printSimpleOp<PrintOperandFN>(OS, Ops, " == ", FN);
  OS << ") ? 1'b1 : 1'b0), ((";
  // Port 2: gt.
  printSimpleOp<PrintOperandFN>(OS, Ops, " != ", FN);
  OS << ") ? 1'b1 : 1'b0), 1'bx }";
}

static void printBitCat(raw_ostream &OS, ArrayRef<VASTUse> Ops) {
  OS << '{';
  printSimpleUnsignedOp(OS, Ops, " , ");
  OS << '}';
}

static void printBitRepeat(raw_ostream &OS, ArrayRef<VASTUse> Ops) {
  OS << '{';
  Ops[1].print(OS);
  OS << '{';
  Ops[0].print(OS);
  OS << "}}";
}

static void printCombMux(raw_ostream &OS, const VASTWire *W) {
  unsigned NumOperands = W->num_operands();
  assert((NumOperands & 0x1) == 0 && "Expect even operand number for CombMUX!");

  // Handle the trivial case trivially: Only 1 input.
  if (NumOperands == 2) {
    printAssign(OS, W);
    W->getOperand(1).print(OS);
    OS << ";\n";
    return;
  }

  // Create the temporary signal.
  OS << "// Combinational MUX\n"
     << "reg " << verilogBitRange(W->getBitWidth()) << ' ' << W->getName()
     << "_mux_wire;\n";

  // Assign the temporary signal to the wire.
  printAssign(OS, W) << W->getName() << "_mux_wire;\n";

  // Print the mux logic.
  OS << "always @(*)begin  // begin mux logic\n";
  OS.indent(2) << VASTModule::ParallelCaseAttr << " case (1'b1)\n";
  for (unsigned i = 0; i < NumOperands; i+=2) {
    OS.indent(4);
    W->getOperand(i).print(OS);
    OS << ": " << W->getName() << "_mux_wire = ";
    W->getOperand(i + 1).print(OS);
    OS << ";\n";
  }
  // Write the default condition, otherwise latch will be inferred.
  OS.indent(4) << "default: " << W->getName() << "_mux_wire = "
               << W->getBitWidth() << "'bx;\n";
  OS.indent(2) << "endcase\nend  // end mux logic\n";
}

void VASTWire::print(raw_ostream &OS) const {
  // Skip unknown or blackbox datapath, it should printed to the datapath
  //  buffer of the module.
  if (Opc == dpUnknown || Opc == InputPort || Opc == dpVarLatBB) return;

  // MUX need special printing method.
  if (Opc == dpMux) {
    printCombMux(OS, this);
    return;
  }
  
  printAssign(OS, this);
  printAsOperand(OS);
  OS << ";\n";
}

void VASTWire::printAsOperandInteral(raw_ostream &OS) const {
  OS << '(';
  typedef ArrayRef<VASTUse> UseArray;

  switch (Opc) {
  case dpNot: printUnaryOp(OS, getOperand(0), " ~ ");  break;

  case dpAnd: printSimpleUnsignedOp(OS, getOperands(), " & "); break;
  case dpOr:  printSimpleUnsignedOp(OS, getOperands(), " | "); break;
  case dpXor: printSimpleUnsignedOp(OS, getOperands(), " ^ "); break;

  case dpRAnd:  printUnaryOp(OS, getOperand(0), "&");  break;
  case dpROr:   printUnaryOp(OS, getOperand(0), "|");  break;
  case dpRXor:  printUnaryOp(OS, getOperand(0), "^");  break;

  case dpSCmp:  printCmpFU(OS, getOperands(), printSignedOperand); break;
  case dpUCmp:  printCmpFU(OS, getOperands(), printUnsignedOperand); break;

  case dpAdd: printSimpleUnsignedOp(OS, getOperands(), " + "); break;
  case dpMul: printSimpleUnsignedOp(OS, getOperands(), " * "); break;
  case dpShl: printSimpleUnsignedOp(OS, getOperands(), " << ");break;
  case dpSRL: printSimpleUnsignedOp(OS, getOperands(), " >> ");break;
  case dpSRA: printSRAOp(OS, getOperands());                   break;

  case cpAssignAtSlot:
  case dpAssign: getOperand(0).print(OS);     break;

  case dpBitCat:    printBitCat(OS, getOperands());    break;
  case dpBitRepeat: printBitRepeat(OS, getOperands()); break;

  default: llvm_unreachable("Unknown datapath opcode!"); break;
  }

  OS << ')';
}
