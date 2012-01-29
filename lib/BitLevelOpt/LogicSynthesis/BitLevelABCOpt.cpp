//======- BitLevelABCOpt.cpp - Verilog target machine bit level ABC Opt-======//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement Verilog target machine-specific bit level optimization
// using ABC.
//
//===----------------------------------------------------------------------===//

#include "vtm/Passes.h"
#include "vtm/VFInfo.h"
#include "vtm/VRegisterInfo.h"
#include "vtm/VInstrInfo.h"
#include "vtm/MicroState.h"

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/Passes.h"
#define DEBUG_TYPE "vtm-logic-synthesis"
#include "llvm/Support/Debug.h"

// The header of ABC
#define ABC_DLL
//#include "base/main/mainInt.h"
#include "base/main/main.h"
#include "map/fpga/fpga.h"
extern "C" {
  extern Abc_Ntk_t *Abc_NtkFpga(Abc_Ntk_t *pNtk, float DelayTarget,
                                int fRecovery, int fSwitching, int fLatchPaths,
                                int fVerbose);
}

using namespace llvm;

namespace {
struct LogicNetwork {
  MachineBasicBlock *BB;
  MachineRegisterInfo &MRI;
  Abc_Ntk_t *Ntk;

  LogicNetwork(MachineBasicBlock *MBB)
    : BB(MBB), MRI(MBB->getParent()->getRegInfo())
  {
    Ntk = Abc_NtkAlloc(ABC_NTK_STRASH, ABC_FUNC_AIG, 1);
    Ntk->pName = Extra_UtilStrsav(BB->getName().str().c_str());
  }

  ~LogicNetwork() {
    Abc_NtkDelete(Ntk);
  }

  // Helper class
  struct NetworkObj {
    Abc_Obj_t *Obj;
    ucOperand MO;
    unsigned ExposedUses;

    // The object is exposed by default.
    NetworkObj(Abc_Obj_t *O, ucOperand &M, unsigned NumUses)
      : Obj(O), MO(M), ExposedUses(NumUses) {}
    NetworkObj()
      : Obj(0), MO(MachineOperand::CreateReg(0, false)), ExposedUses(0) {}

    unsigned decreaseUses() {
      if (ExposedUses) --ExposedUses;

      return ExposedUses;
    }
  };

  // Mapping register number to logic network port.
  typedef DenseMap<ucOperand, NetworkObj, ucOperandValueTrait> ObjMapTy;
  // Nodes.
  ObjMapTy Nodes;

  // The indices of the blackbox instructions.
  typedef DenseMap<MachineInstr*, unsigned> IdxMapTy;
  IdxMapTy IdxMap;

  // Map the Abc_Obj_t name to Instruction.
  typedef StringMap<MachineInstr*> InstrMapTy;
  InstrMapTy InstrMap;
  typedef MachineBasicBlock::iterator IPTy;

  typedef StringMap<ucOperand> MOMapTy;
  MOMapTy MOMap;

  MachineInstr *getDefMI(Abc_Obj_t *FI) const {
    return InstrMap.lookup(Abc_ObjName(Abc_ObjRegular(FI)));
  }

  bool isAfter(MachineInstr *LHS, MachineInstr *RHS) const {
    return IdxMap.lookup(LHS) > IdxMap.lookup(RHS);
  }

  //
  Abc_Obj_t *getObj(ucOperand &MO) {
    ObjMapTy::iterator inNodes = Nodes.find(MO);

    if (inNodes != Nodes.end()) {
      // Decrease the reference count.
      inNodes->second.decreaseUses();
      return inNodes->second.Obj;
    }

    return 0;
  }

  Abc_Obj_t *getOrCreateObj(ucOperand &MO) {
    Abc_Obj_t *Obj = getObj(MO);

    // Object not existed, create a PI for the MO now.
    if (Obj == 0) {
      Obj = Abc_NtkCreatePi(Ntk);
      std::string PIName = "i" + utostr_32(Abc_ObjId(Abc_ObjRegular(Obj)));
      Abc_ObjAssignName(Obj, const_cast<char*>(PIName.c_str()), 0);
      char *Name = Abc_ObjName(Abc_ObjRegular(Obj));

      // Remember the MI that define this MO so we can compute the insert
      // position.
      if (MO.isReg() && MO.getReg()) {
        MachineInstr *DefMI = MRI.getVRegDef(MO.getReg());
        assert(DefMI && "VReg not defined?");
        if (DefMI->getParent() == BB)
          InstrMap.GetOrCreateValue(Name, DefMI);
      }

      // Map the PI to MO.
      MOMap.GetOrCreateValue(Name, MO);

      // PIs are not exposed.
      Nodes.insert(std::make_pair(MO, NetworkObj(Obj, MO, 0)));
    }

    return Obj;
  }

  ucOperand getOperand(Abc_Obj_t *Obj, unsigned SizeInBits = 0) {
    // Else look it up in the FO map.
    ucOperand &MO =
      MOMap.GetOrCreateValue(Abc_ObjName(Obj),
                             ucOperand::CreateReg(0, SizeInBits)).second;

    // Allocate the register.
    if (MO.isReg() && MO.getReg() == 0)
      MO.ChangeToRegister(MRI.createVirtualRegister(VTM::DRRegisterClass),
                          false);

    return MO;
  }

  void cleanUp() {
    // Build the POs
    typedef ObjMapTy::iterator it;

    for (it I = Nodes.begin(), E = Nodes.end(); I != E; ++I) {
      NetworkObj &Node = I->second;

      // Only create PO for exposed node.
      if (!Node.ExposedUses) continue;

      Abc_Obj_t *PO = Abc_NtkCreatePo(Ntk);
      Abc_Obj_t *&Obj = Node.Obj;
      Abc_ObjAddFanin(PO, Obj);
      Obj = PO;

      std::string POName = "o" + utostr_32(Abc_ObjId(Abc_ObjRegular(Obj)));
      Abc_ObjAssignName(Obj, const_cast<char*>(POName.c_str()), 0);

      // Remember the MO.
      MOMap.GetOrCreateValue(Abc_ObjName(Abc_ObjRegular(Obj)), Node.MO);
    }

    // Clean up the aig.
    Abc_AigCleanup((Abc_Aig_t *)Ntk->pManFunc);

    // Create default names.
    //Abc_NtkAddDummyPiNames(Ntk);
    //Abc_NtkAddDummyPoNames(Ntk);
    // We do not have boxes.
    //Abc_NtkAddDummyBoxNames(Ntk);

    // Check the Aig
    assert(Abc_NtkCheck(Ntk) && "The AIG construction has failed!");
  }

  void performLUTMapping() {
    // Map the network to LUTs
    Ntk = Abc_NtkFpga(Ntk, 1, 0, 0, 0, 0);
    assert(Ntk && "Fail to perform LUT mapping!");

    // Translate the network to netlist.
    Ntk = Abc_NtkToNetlist(Ntk);
    assert(Ntk && "Network doese't exist!!!");
    assert(Abc_NtkHasBdd(Ntk) && "Expect Bdd after LUT mapping!");
    int res = Abc_NtkBddToSop(Ntk, 0);
    assert(res && "BddToSop fail!");
    (void) res;
  }

  // Function for logic network building.
  template <typename BuildFunc>
  void buildBinaryOpNode(MachineInstr *MI, BuildFunc F) {
    Abc_Obj_t *Op0 = getOrCreateObj(cast<ucOperand>(MI->getOperand(1))),
              *Op1 = getOrCreateObj(cast<ucOperand>(MI->getOperand(2)));
  
    // Create the internal node for this machine instruction.
    Abc_Obj_t *Res = F((Abc_Aig_t *)Ntk->pManFunc, Op0, Op1);
    ucOperand ResMO = cast<ucOperand>(MI->getOperand(0));
    // Create the define flag, because the virtual register define is ignore by
    // the DenseMap.
    ResMO.setIsDef(false);

    unsigned NumUse = std::distance(MRI.use_begin(ResMO.getReg()), MRI.use_end());
    // Remember the node, assumes it is a PO.
    Nodes.insert(std::make_pair(ResMO, NetworkObj(Res, ResMO, NumUse)));
  }

  void buildNotNode(MachineInstr *MI) {
    Abc_Obj_t *Op0 = getOrCreateObj(cast<ucOperand>(MI->getOperand(1)));
    // Create the internal node for this machine instruction.
    Abc_Obj_t *Res = Abc_ObjNot(Op0);
    ucOperand ResMO = cast<ucOperand>(MI->getOperand(0));
    // Create the define flag, because the virtual register define is ignore by
    // the DenseMap.
    ResMO.setIsDef(false);

    unsigned NumUse = std::distance(MRI.use_begin(ResMO.getReg()), MRI.use_end());
    // Remember the node, assumes it is a PO.
    Nodes.insert(std::make_pair(ResMO, NetworkObj(Res, ResMO, NumUse)));
  }

  bool addInstr(MachineInstr *MI);

  IPTy getInsertPos(Abc_Obj_t *Node, IPTy LastIP);
};

struct LogicSynthesis : public MachineFunctionPass {
  static char ID;

  VFInfo *VFI;

  LogicSynthesis() : MachineFunctionPass(ID), VFI(0) {
    Abc_Start();
    Fpga_SetSimpleLutLib(4);
  }

  ~LogicSynthesis() {
    Abc_Stop();
  }

  bool runOnMachineFunction(MachineFunction &MF);
  bool synthesisBasicBlock(MachineBasicBlock *BB);
};
}
//===----------------------------------------------------------------------===//
// Implementation of logic network class.
bool LogicNetwork::addInstr(MachineInstr *MI) {
  switch (MI->getOpcode()) {
  default: break;
  case VTM::VOpAnd:
    buildBinaryOpNode(MI, Abc_AigAnd);
    return true;
  case VTM::VOpOr:
    buildBinaryOpNode(MI, Abc_AigOr);
    return true;
  case VTM::VOpXor:
    buildBinaryOpNode(MI, Abc_AigXor);
    return true;
  case VTM::VOpNot:
    buildNotNode(MI);
    return true;
  }

  // Add the black box instruction to index map.
  IdxMap.insert(std::make_pair(MI, IdxMap.size()));
  return false;
}

LogicNetwork::IPTy LogicNetwork::getInsertPos(Abc_Obj_t *Node,
                                              LogicNetwork::IPTy LastIP) {
  // Check the fanins of Node, find a position that all the fanins are defined.
  Abc_Obj_t *FI;
  int i;
  Abc_ObjForEachFanin(Node, FI, i) {
    // Get the instruction that defines the PI.
    MachineInstr *DefMI = getDefMI(FI);

    // Ignore the FI that defined in other BB.
    if (!DefMI) continue;

    // Is the PI defined before LastIP?
    while (!isAfter(LastIP, DefMI))
      ++LastIP;
  }

  // TODO: Asser FO used after all FI.

  return LastIP;
}

//===----------------------------------------------------------------------===//
// Implement of the logic synthesis pass.
char LogicSynthesis::ID = 0;

Pass *llvm::createBitLevelABCOptPass() {
  return new LogicSynthesis();
}

bool LogicSynthesis::runOnMachineFunction(MachineFunction &MF) {
  bool Changed = false;
  VFI = MF.getInfo<VFInfo>();

  for (MachineFunction::iterator I = MF.begin(), E = MF.end(); I != E; ++I)
    Changed = synthesisBasicBlock(I);

  // Verify the function.
  MF.verify(this);

  return Changed;
}

bool LogicSynthesis::synthesisBasicBlock(MachineBasicBlock *BB) {
  bool Changed = false;
  LogicNetwork Ntk(BB);

  typedef MachineBasicBlock::iterator it;
  for (it I = BB->begin(), E = BB->end(); I != E; /*++I*/) {
    MachineInstr *MI = I++;

    // Try to add the instruction into the logic network.
    if (!Ntk.addInstr(MI)) continue;

    MI->eraseFromParent();
    Changed = true;
  }

  // Not change at all.
  if (!Changed) return false;

  // Clean up the network, prepare for logic optimization.
  Ntk.cleanUp();

  // Map the logic network to LUTs
  Ntk.performLUTMapping();

  // Build the BB from the logic netlist.
  MachineBasicBlock::iterator IP = BB->getFirstNonPHI();

  SmallVector<ucOperand, 2> Ops;
  Abc_Obj_t *Obj;
  int i;
  Abc_NtkForEachNode(Ntk.Ntk, Obj, i) {
    IP = Ntk.getInsertPos(Obj, IP);

    Abc_Obj_t *FO = Abc_ObjFanout0(Obj), *FI;
    dbgs() << Abc_ObjName(FO) << '\n';

    // Get all operands and compute the bit width of the result.
    Ops.clear();
    unsigned SizeInBits = 0;
    int j;
    Abc_ObjForEachFanin(Obj, FI, j) {
      dbgs() << Abc_ObjName(FI) << '\n';
      ucOperand MO = Ntk.getOperand(FI);
      assert((SizeInBits == 0 || SizeInBits == MO.getBitWidth())
             && "Operand SizeInBits not match!");
      Ops.push_back(MO);
      SizeInBits = MO.getBitWidth();
    }
    
    // Get the result.
    ucOperand DefMO = Ntk.getOperand(FO, SizeInBits);
    assert(DefMO.getBitWidth() == SizeInBits && "Result SizeInBits not match!");
    DefMO.setIsDef(true);

    char *data = (char*)Abc_ObjData(Obj);
    // The sum of product table.
    dbgs() << data << '\n';

    MachineInstrBuilder Builder =
      BuildMI(*BB, IP, DebugLoc(), VInstrInfo::getDesc(VTM::VOpLUT))
        .addOperand(DefMO)
        .addExternalSymbol(VFI->allocateSymbol(data), Abc_ObjFaninNum(Obj))
        .addOperand(ucOperand::CreatePredicate())
        .addOperand(ucOperand::CreateTrace(BB));

    for (unsigned k = 0, e = Ops.size(); k != e; ++k)
      Builder.addOperand(Ops[k]);
  }

  BB->dump();

  return true;
}
