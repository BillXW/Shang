//RegDependencyAnalysis.cpp-- Analyse the dependency between registers- C++ -=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass collect the slots information of register and map them into a map
// vector. then it will analyse dependency between registers.
//
//
//===----------------------------------------------------------------------===//

#include "vtm/VerilogAST.h"
#include "vtm/Passes.h"
#include "vtm/VFInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/SetOperations.h"
#define DEBUG_TYPE "vtm-reg-dependency"
#include "llvm/Support/Debug.h"
#include "llvm/Support/GraphWriter.h"

#include <map>

using namespace llvm;

namespace llvm {
  class ValueAtSlot;
  template<> struct FoldingSetTrait<ValueAtSlot>;

  class ValueAtSlot : public FoldingSetNode {
    friend struct FoldingSetTrait<ValueAtSlot>;

    VASTValue *V;
    VASTSlot *Slot;

    /// FastID - A reference to an Interned FoldingSetNodeID for this node.
    /// The ScalarEvolution's BumpPtrAllocator holds the data.
    FoldingSetNodeIDRef FastID;

    // Vector for the dependent ValueAtSlots which is a Predecessor VAS.
    typedef SmallVector<ValueAtSlot*, 4> VASVecTy;
    VASVecTy PredVAS;

    // Vector for the successor ValueAtSlot.
    VASVecTy SuccVAS;

  public:
    explicit ValueAtSlot(const FoldingSetNodeIDRef ID, VASTValue *v,
                         VASTSlot *slot) : FastID(ID), V(v), Slot(slot) {}

    void addPredValueAtSlot(ValueAtSlot *VAS){ PredVAS.push_back(VAS); }

    void addSuccValueAtSlot(ValueAtSlot *VAS) { SuccVAS.push_back(VAS); }

    VASTValue *getValue() const { return V; }

    VASTSlot *getSlot() const { return Slot; }


    typedef VASVecTy::iterator DVASIt;
    DVASIt pred_vas_begin() { return PredVAS.begin(); }
    DVASIt pred_vas_end() { return PredVAS.end(); }

    DVASIt succ_vas_begin() { return SuccVAS.begin(); }
    DVASIt succ_vas_end() { return SuccVAS.end(); }

    bool operator==(ValueAtSlot &RHS) const {
      if ((V == RHS.getValue()) && (Slot == RHS.getSlot())) return true;
      return false;
    }

  };

  template<> struct GraphTraits<ValueAtSlot*> {
    typedef ValueAtSlot NodeType;
    typedef NodeType::DVASIt ChildIteratorType;
    static NodeType *getEntryNode(NodeType* N) { return N; }
    static inline ChildIteratorType child_begin(NodeType *N) {
      return N->succ_vas_begin();
    }
    static inline ChildIteratorType child_end(NodeType *N) {
      return N->succ_vas_end();
    }
  };

  // Specialize FoldingSetTrait for ValueAtSlot to avoid needing to compute
  // temporary FoldingSetNodeID values.
  template<>
    struct FoldingSetTrait<ValueAtSlot> : DefaultFoldingSetTrait<ValueAtSlot> {
    static void Profile(const ValueAtSlot &X, FoldingSetNodeID& ID) {
      ID = X.FastID;
    }
    static bool Equals(const ValueAtSlot &X, const FoldingSetNodeID &ID,
      FoldingSetNodeID &TempID) {
        return ID == X.FastID;
    }
    static unsigned ComputeHash(const ValueAtSlot &X,
                                FoldingSetNodeID &TempID) {
      return X.FastID.ComputeHash();
    }
  };

  class SlotInfo {
  public:
    // Define the VAS set for the reaching definition dense map.
    typedef std::set<ValueAtSlot*> VASSetTy;
    typedef VASSetTy::iterator vasset_it;
  private:
    VASTSlot *S;

    // Define Set for the reaching definition.
    VASSetTy SlotGen;
    VASSetTy SlotKill;
    VASSetTy SlotIn;
    VASSetTy SlotOut;
  public:
    SlotInfo(VASTSlot *s) : S(s) {}
    // get the iterator of the defining map of reaching definition.
    vasset_it getVASGenBegin() const { return SlotGen.begin(); }
    vasset_it getVASGenEnd() const { return SlotGen.end(); }
    vasset_it getVASKillBegin() const { return SlotKill.begin(); }
    vasset_it getVASKillEnd() const { return SlotKill.end(); }
    vasset_it getVASInBegin() const { return SlotIn.begin(); }
    vasset_it getVASInEnd() const { return SlotIn.end(); }
    vasset_it getVASOutBegin() const { return SlotOut.begin(); }
    vasset_it getVASOutEnd() const { return SlotOut.end(); }

    VASSetTy getGenVASSet() const{ return SlotGen; }
    VASSetTy getKillVASSet() const { return SlotKill; }
    VASSetTy getInVASSet() const { return SlotIn; }
    VASSetTy getOutVASSet() const { return SlotOut; }
  };
}

namespace llvm{
class RegDependencyAnalysis : public MachineFunctionPass {
public:
  // define VASVec for the ValueAtSlot.
  typedef SmallVector<ValueAtSlot*, 4> VASVec;
  typedef VASVec::iterator vasvec_it;

  // Define small vector for the slots.
  typedef SmallVector<VASTSlot*, 4> SlotVecTy;
  typedef SlotVecTy::iterator slot_vec_it;

  typedef std::set<SlotInfo*> SlotInfoTy;

private:
  typedef SlotInfo::VASSetTy VASSet;
  SlotInfoTy SlotInfos;

  // This vector is for the ValueAtSlot.
  VASVec AllVASs;

  SlotVecTy SlotVec;

  MachineFunction *MF;

  typedef FoldingSet<ValueAtSlot>::iterator fs_vas_it;
  FoldingSet<ValueAtSlot> UniqueVASs;
  BumpPtrAllocator VASAllocator;

  // define VAS assign iterator.
  typedef VASTRegister::assign_itertor assign_it;

  // define a function pointer which is use to add dependent VAS.
  typedef void(RegDependencyAnalysis::* addDependentVASFuncTy)(ValueAtSlot*,
                                                               VASTRegister*);

public:
  static char ID;

  // All nodes (except exit node) are successors of the entry node.
  vasvec_it vas_begin() { return AllVASs.begin(); }
  vasvec_it vas_end() { return AllVASs.end(); }

  slot_vec_it slot_begin() { return SlotVec.begin(); }
  slot_vec_it slot_end() { return SlotVec.end(); }

  SlotInfoTy *getOrCreateSlotInfo(VASTSlot *S);

  ValueAtSlot *getOrCreateVAS(VASTValue *V, VASTSlot *S);

  // Traverse every register to define the ValueAtSlots.
  void defineVAS(VASTModule *VM);

  // Add dependent ValueAtSlot.
  void addDependentVAS(ValueAtSlot *VAS, VASTRegister *DefReg);

  // Traverse the dependent VASTUse to get the registers.
  void TraverseDependentRegister(VASTUse *DefUse, ValueAtSlot *VAS);

  // Traverse the use tree to get the registers.
  void DepthFirstTraverseUseTree(addDependentVASFuncTy F, VASTUse DefUse,
                                 ValueAtSlot *VAS);

  // Using the reaching definition algorithm to sort out the ultimate
  // relationship of registers.
  // Dirty hack: maybe there are two same statements is a slot, and we can use
  // bit vector to implement the algorithm similar to the compiler principle.
  void ComputeReachingDefinition();

  // collect the Generated and Killed statements of the slot.
  void ComputeGenAndKill();

  bool DetectChange(VASSet SlotOut, VASSet CurOut);

  void viewGraph();

  void releaseMemory() {
    UniqueVASs.clear();
    VASAllocator.Reset();
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    MachineFunctionPass::getAnalysisUsage(AU);
   // AU.addRequired<FindShortestPath>();
   // AU.addPreserved<FindShortestPath>();
  }

  bool runOnMachineFunction(MachineFunction &MF);

  RegDependencyAnalysis() : MachineFunctionPass(ID) {
    initializeRegDependencyAnalysisPass(*PassRegistry::getPassRegistry());
  }
};

template <> struct GraphTraits<RegDependencyAnalysis*>
: public GraphTraits<VASTSlot*> {

  typedef RegDependencyAnalysis::slot_vec_it nodes_iterator;
  static nodes_iterator nodes_begin(RegDependencyAnalysis *G) {
    return G->slot_begin();
  }
  static nodes_iterator nodes_end(RegDependencyAnalysis *G) {
    return G->slot_end();
  }
};

template<>
struct DOTGraphTraits<RegDependencyAnalysis*> : public DefaultDOTGraphTraits{
  typedef VASTSlot NodeTy;
  typedef RegDependencyAnalysis GraphTy;

  DOTGraphTraits(bool isSimple=false) : DefaultDOTGraphTraits(isSimple) {}

  static std::string getEdgeSourceLabel(const NodeTy *Node, NodeTy::succ_slot_it I){
    std::string Str;
    raw_string_ostream ss(Str);
    ss << Node->getName();
    return ss.str();
  }

  std::string getNodeLabel(const NodeTy *Node, const GraphTy *Graph) {
    std::string Str;
    raw_string_ostream ss(Str);
    /*for (RegDependencyAnalysis::vasset_it I = Graph->vas_gen_begin(Node),
         E = Graph->vas_gen_end(Node); I != E; ++I) {
      ValueAtSlot *VAS = *I;
        ss<<"    Gen: "<< VAS->getValue()->getName() << "   "
          << VAS->getSlot()->getName() << "\n";
    }
    ss << "\n\n";*/
    for (SlotInfo::vasset_it I = Graph->getVASKillBegin(Node),
         E = Graph->getVASKillEnd(Node); I != E; ++I) {
      ValueAtSlot *VAS = *I;
      ss<<"    kill: "<< VAS->getValue()->getName() << "   "
        << VAS->getSlot()->getName() << "\n";
    }
    ss << "\n\n";
    /*for (RegDependencyAnalysis::vasset_it I = Graph->vas_in_begin(Node),
      E = Graph->vas_in_end(Node); I != E; ++I) {
        ValueAtSlot *VAS = *I;
        ss<<"    In: "<< VAS->getValue()->getName() << "   "
          << VAS->getSlot()->getName() << "\n";
    }
    ss << "\n\n";*/
    for (SlotInfo::vasset_it I = Graph->getVASOutBegin(Node),
         E = Graph->getVASOutEnd(Node); I != E; ++I) {
      ValueAtSlot *VAS = *I;
      ss<<"    Out: "<< VAS->getValue()->getName() << "   "
        << VAS->getSlot()->getName() << "\n";
    }
    return ss.str();
  }

  static std::string getNodeAttributes(const NodeTy *Node,
    const GraphTy *Graph) {
      return "shape=Mrecord";
  }
};

void RegDependencyAnalysis::viewGraph() {
  ViewGraph(this, "CompatibilityGraph" + utostr_32(ID));
}

}

bool RegDependencyAnalysis::runOnMachineFunction(MachineFunction &F) {
  MF = &F;
  VASTModule *VM = MF->getInfo<VFInfo>()->getRtlMod();

  for (VASTModule::slot_iterator I = VM->slot_begin(), E = VM->slot_end();
    I != E; ++I) {
      VASTSlot *S = *I;
      // If the VASTslot is void, abandon it.
      if (!S) continue;

      SlotVec.push_back(S);
  }

  // Define the VAS.
  defineVAS(VM);

  ComputeReachingDefinition();

  viewGraph();

  return false;
}

ValueAtSlot *RegDependencyAnalysis::getOrCreateVAS(VASTValue *V,
                                                   VASTSlot *S){
  FoldingSetNodeID ID;
  ID.AddPointer(V);
  ID.AddPointer(S);
  void *IP = 0;
  if (ValueAtSlot *VAS = UniqueVASs.FindNodeOrInsertPos(ID, IP)) {
    //assert(cast<ValueAtSlot>(VAS)->getValue() == V &&
           //"Stale ValueAtSlot in uniquing map!");
    return VAS;
  }

  ValueAtSlot *VAS =
    new (VASAllocator)ValueAtSlot(ID.Intern(VASAllocator), V, S);
  UniqueVASs.InsertNode(VAS, IP);
  AllVASs.push_back(VAS);

  return VAS;
}

SlotInfo *RegDependencyAnalysis::getOrCreateSlotInfo(VASTSlot *S) {
  SlotInfo SI(S);
  SlotInfo *SIPointer = &SI;
  if (SlotInfos.insert(SIPointer).second) return SIPointer;

  SlotInfos.insert(SIPointer).second;
  return *SlotInfos.find(SI);
}

void RegDependencyAnalysis::addDependentVAS(ValueAtSlot *VAS,
                                            VASTRegister *DefReg) {
  for (assign_it I = DefReg->assign_begin(), E = DefReg->assign_end();
    I != E; ++I){
      VASTSlot *DefS = I->first->getSlot();
      ValueAtSlot *PredVAS = getOrCreateVAS(DefReg, DefS);
      VAS->addPredValueAtSlot(PredVAS);

      // Add the VAS to the successor VAS vector.
      PredVAS->addSuccValueAtSlot(VAS);
  }
}

void RegDependencyAnalysis::defineVAS(VASTModule *VM) {
  for (VASTModule::reg_iterator I = VM->reg_begin(), E = VM->reg_end(); I != E;
       ++I){
    VASTRegister *UseReg = *I;

    typedef VASTRegister::assign_itertor assign_it;
    for (assign_it I = UseReg->assign_begin(), E = UseReg->assign_end();
         I != E; ++I) {
      VASTSlot *S = I->first->getSlot();
      // Create the origin VAS.
      ValueAtSlot *VAS = getOrCreateVAS(UseReg,S);
      VASTUse *DefUse = I->second;
      // Traverse the dependent VAS.
      TraverseDependentRegister(DefUse, VAS);
    }
  }
}

void RegDependencyAnalysis::TraverseDependentRegister(VASTUse *DefUse,
                                                      ValueAtSlot *VAS){

  VASTValue *DefValue = DefUse->getOrNull();

  // If Define Value is immediate or symbol, skip it.
  if (!DefValue) return;

  // If the define Value is register, add the dependent VAS to the
  // dependentVAS.
  if (VASTRegister *DefReg = dyn_cast<VASTRegister>(DefValue)){
    addDependentVAS(VAS, DefReg);
    return;
  }

  // If the define Value is wire, traverse the use tree to get the
  // ultimate registers.
  DepthFirstTraverseUseTree(&RegDependencyAnalysis::addDependentVAS, *DefUse,
                            VAS);
}

void RegDependencyAnalysis::DepthFirstTraverseUseTree(addDependentVASFuncTy F,
                                                      VASTUse DefUse,
                                                      ValueAtSlot *VAS) {
  typedef VASTUse::iterator ChildIt;
  // Use seperate node and iterator stack, so we can get the path vector.
  typedef SmallVector<VASTUse, 16> NodeStackTy;
  typedef SmallVector<ChildIt, 16> ItStackTy;
  NodeStackTy NodeWorkStack;
  ItStackTy ItWorkStack;
  // Remember what we had visited.
  std::set<VASTUse> VisitedUses;

  // Put the current node into the node stack, so it will appears in the path.
  NodeWorkStack.push_back(VAS->getValue());

  // Put the root.
  NodeWorkStack.push_back(DefUse);
  ItWorkStack.push_back(DefUse.dp_src_begin());

  while (!ItWorkStack.empty()) {
    VASTUse Node = NodeWorkStack.back();

    ChildIt It = ItWorkStack.back();

    // Do we reach the leaf?
    if (Node.is_dp_leaf()) {
      if (VASTValue *V = Node.getOrNull()) {
        DEBUG(dbgs() << "Datapath:\t";
        for (NodeStackTy::iterator I = NodeWorkStack.begin(),
          E = NodeWorkStack.end(); I != E; ++I) {
            dbgs() << ", ";
            I->print(dbgs());
        });

        if (VASTRegister *R = dyn_cast<VASTRegister>(V)) {
          // Add dependent VAS. Use the function pointer to get the desired
          // function.
          (this->*F)(VAS, R);
        }

        DEBUG(dbgs() << '\n');
      }

      NodeWorkStack.pop_back();
      ItWorkStack.pop_back();
      continue;
    }

    // All sources of this node is visited.
    if (It == Node.dp_src_end()) {
      NodeWorkStack.pop_back();
      ItWorkStack.pop_back();
      continue;
    }

    // Depth first traverse the child of current node.
    VASTUse ChildNode = *It;
    ++ItWorkStack.back();

    // Had we visited this node? If the Use slots are same, the same subtree
    // will lead to a same slack, and we do not need to compute the slack agian.
    if (!VisitedUses.insert(ChildNode).second) continue;

    // If ChildNode is not visit, go on visit it and its childrens.
    NodeWorkStack.push_back(ChildNode);
    ItWorkStack.push_back(ChildNode.dp_src_begin());
  }

  assert(NodeWorkStack.back().get() == VAS->getValue() && "Node stack broken!");
}

bool RegDependencyAnalysis::DetectChange(VASSet SlotOut, VASSet CurOut) {
  // Compare the SlotOutMap and OldSlotOutMap, findout whether there are
  // changes in the SlotOut.
  for (SlotInfo::vasset_it I = CurOut.begin(), E = CurOut.end(); I != E;
       ++I) {
    ValueAtSlot *NewVAS = *I;
    if (!SlotOut.count(NewVAS)) {
      return true;
    }
  }

  for (SlotInfo::vasset_it I = SlotOut.begin(), E = SlotOut.end(); I != E;
       ++I) {
    ValueAtSlot *NewVAS = *I;
    if (!CurOut.count(NewVAS)) {
      return true;
    }
  }

  return false;
}

void RegDependencyAnalysis::ComputeReachingDefinition() {
  ComputeGenAndKill();

  bool Change;

  do {
    Change = false;

    for (slot_vec_it I = SlotVec.begin(), E = SlotVec.end(); I != E; ++I) {
      VASTSlot *S =*I;

      SlotInfo SI(S);

      // If the VASTslot is void, abandon it.
      if (!S) continue;
      VASSet SlotOut = SI.getOutVASSet();
      VASSet &CurIn = SI.getInVASSet(), &CurKill = SI.getKillVASSet(),
              &CurOut = SI.getOutVASSet(), &CurGen = SI.getGenVASSet();

      // Compute the SlotInMap.
      for (VASTSlot::pred_it I = S->pred_begin(), E = S->pred_end(); I != E;
           ++I) {
        VASTSlot *PS = *I;
        SlotInfo
        CurIn.insert(SlotOutMap[PS].begin(), SlotOutMap[PS].end());
      }

      // Compute the SlotInMap subtract the SlotKillMap.
      set_subtract(CurIn, CurKill);

      CurOut.clear();

      // Compute the SlotOutMap. insert the VAS from the SlotGenMap.
      CurOut.insert(CurGen.begin(), CurGen.end());
      // Compute the SlotOutMap. Insert the VAS from the SlotInMap.
      CurOut.insert(CurIn.begin(), CurIn.end());

      Change = DetectChange(SlotOut, CurOut);
    }
  } while (Change);
}

void RegDependencyAnalysis::ComputeGenAndKill(){
  // Collect the generated statements to the SlotGenMap, and collect the killed
  // statements to the SlotKillMap.
  for (slot_vec_it I = SlotVec.begin(), E = SlotVec.end(); I != E; ++I) {
    VASTSlot *S = *I;

    // If the VASTslot is void, abandon it.
    if (!S) continue;
    VASSetTy &CurGen = SlotGenMap[S], &CurKill = SlotKillMap[S];


    DEBUG(dbgs()<<"origin slot: "<< S->getName()<< "\n";);

    // Collect the generated statements to the SlotGenMap.
    for (vasvec_it I = AllVASs.begin(), E = AllVASs.end(); I != E; ++I) {
      ValueAtSlot *GenVAS = *I;

      // If the VAS have the same slot with S, then its a Generated VAS to this
      // slot.
      if (GenVAS->getSlot() == S) CurGen.insert(GenVAS);
    }

    // Collect the killed statements to the SlotGenMap.
    for (SlotInfo::vasset_it I = CurGen.begin(), E = CurGen.end(); I != E;
         ++I) {
      ValueAtSlot *GenVAS = *I;

      for (vasvec_it VI = AllVASs.begin(), VE = AllVASs.end(); VI != VE;
           ++VI) {
        ValueAtSlot *KillVAS = *VI;

        // abandon the same VAS.
        if (GenVAS == KillVAS) continue;

        // If the KillVAS have the same value with the GenVAS, then it's killed.
        if (GenVAS->getValue() == KillVAS->getValue())
          CurKill.insert(KillVAS);
      }
    }

    DEBUG(
      for (SlotInfo::vasset_it I = CurGen.begin(), E = CurGen.end(); I != E;
           ++I) {
        ValueAtSlot *VAS = *I;
        if (S->getSlotNum() == 2) {
          dbgs()<<"    Gen: "<< VAS->getValue()->getName() << "   "
                << VAS->getSlot()->getName() << "\n";
        }
      }
      for (SlotInfo::vasset_it I = CurKill.begin(), E = CurKill.end(); I != E;
           ++I) {
        ValueAtSlot *VAS = *I;
        if (S->getSlotNum() == 2) {
          dbgs()<<"    Kill: "<< VAS->getValue()->getName() << "   "
                << VAS->getSlot()->getName()  << "\n";
        }
      }
    );
  }
}

char RegDependencyAnalysis::ID = 0;
INITIALIZE_PASS_BEGIN(RegDependencyAnalysis, "RegDependencyAnalysis",
                      "RegDependencyAnalysiss", false, false)
INITIALIZE_PASS_END(RegDependencyAnalysis, "RegDependencyAnalysis",
                    "RegDependencyAnalysis", false, false)

Pass *llvm::createRegDependencyAnalysisPass() {
  return new RegDependencyAnalysis();
}
