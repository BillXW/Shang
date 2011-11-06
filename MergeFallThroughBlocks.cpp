//===- MergeFallThroughBlocks.cpp - Merge Fall Through Blocks ---*- C++ -*-===//
//
//                            The Verilog Backend
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements a pass that merge the fall through blocks into its
// predecessor blocks to increase instruction level parallelism.
//
//===----------------------------------------------------------------------===//

#include "vtm/Passes.h"
#include "vtm/VTM.h"
#include "vtm/VInstrInfo.h"
#include "vtm/MicroState.h"

#include "llvm/../../lib/CodeGen/BranchFolding.h"

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "vtm-merge-fallthroughs"
#include "llvm/Support/Debug.h"
#include <set>
#include <map>

using namespace llvm;

STATISTIC(NumFallThroughMerged,
          "VTM - Number of Fall Through Blocks Merged");

namespace {
struct MergeFallThroughBlocks : public MachineFunctionPass {
  static char ID;
  std::vector<unsigned> IncreasedLatencies;

  const TargetInstrInfo *TII;
  MachineRegisterInfo *MRI;
  MachineLoopInfo *LI;

  MergeFallThroughBlocks() : MachineFunctionPass(ID), TII(0), MRI(0) {}

  void getAnalysisUsage(AnalysisUsage &AU) const {
    MachineFunctionPass::getAnalysisUsage(AU);
    AU.addRequired<MachineLoopInfo>();
  }

  bool runOnMachineFunction(MachineFunction &MF);
  MachineBasicBlock *getMergeDst(MachineBasicBlock *Src,
                                 VInstrInfo::JT &SrcJT,
                                 VInstrInfo::JT &DstJT);

  bool mergeFallThroughBlock(MachineBasicBlock *MBB);

  void PredicateBlock(MachineOperand Cnd, MachineBasicBlock *BB);

};
}

char MergeFallThroughBlocks::ID = 0;

bool MergeFallThroughBlocks::runOnMachineFunction(MachineFunction &MF) {
  TII = MF.getTarget().getInstrInfo();
  MRI = &MF.getRegInfo();
  LI = &getAnalysis<MachineLoopInfo>();
  bool MakeChanged = false;
  bool BlockMerged = false;
  typedef MachineFunction::reverse_iterator rev_it;

  MF.RenumberBlocks();
  IncreasedLatencies.assign(MF.getNumBlockIDs(), 0);

  do {
    BlockMerged = false;
    for (rev_it I = MF.rbegin(), E = MF.rend(); I != E; ++I)
      MakeChanged |= BlockMerged |= mergeFallThroughBlock(&*I);
  } while (BlockMerged);

  // Tail merge tend to expose more if-conversion opportunities.
  BranchFolder BF(true);
  MakeChanged |= BF.OptimizeFunction(MF, TII, MF.getTarget().getRegisterInfo(),
                                     getAnalysisIfAvailable<MachineModuleInfo>());

  MF.RenumberBlocks();
  return MakeChanged;
}

MachineBasicBlock *MergeFallThroughBlocks::getMergeDst(MachineBasicBlock *SrcBB,
                                                       VInstrInfo::JT &SrcJT,
                                                       VInstrInfo::JT &DstJT) {
  // Only handle simple case at the moment
  if (SrcBB->pred_size() != 1) return 0;

  MachineBasicBlock *DstBB = *SrcBB->pred_begin();
  // Do not change the parent loop of MBB.
  if (LI->getLoopFor(SrcBB) != LI->getLoopFor(DstBB)) return 0;

  // Do not mess up with strange CFG.
  if (VInstrInfo::extractJumpTable(*DstBB, DstJT)) return 0;
  if (VInstrInfo::extractJumpTable(*SrcBB, SrcJT)) return 0;

  // Do not mess up with self loop.
  if (SrcJT.count(SrcBB)) return 0;

  // We need to predicate the block when merging it.
  for (MachineBasicBlock::iterator I = SrcBB->begin(), E = SrcBB->end();I != E;++I){
    MachineInstr *MI = I;
    if (!TII->isPredicable(MI))
      return 0;
  }

  return DstBB;
}

bool MergeFallThroughBlocks::mergeFallThroughBlock(MachineBasicBlock *FromBB) {
  VInstrInfo::JT FromJT, ToJT;
  MachineBasicBlock *ToBB = getMergeDst(FromBB, FromJT, ToJT);

  if (!ToBB) return false;

  unsigned IncreasedLatency = 0;
  CompLatency CL;
  unsigned OriginalLatency = CL.computeLatency(*ToBB);
  unsigned MergedLatency = CL.computeLatency(*FromBB);
  if (MergedLatency > OriginalLatency)
    IncreasedLatency = MergedLatency - OriginalLatency;
  double IncreaseRate = double(IncreasedLatency)/double(OriginalLatency);
  // Also take account of the latency increased by previous merge.
  IncreasedLatency += IncreasedLatencies[ToBB->getNumber()];

  if (IncreasedLatency > 4 || IncreaseRate > 0.1) return false;
  DEBUG(dbgs() << "Merging BB#" << FromBB->getNumber() << " To BB#"
         << ToBB->getNumber() << " IncreasedLatency " << IncreasedLatency
         << ' ' << int(IncreaseRate * 100.0) << "%\n");

  TII->RemoveBranch(*ToBB);
  TII->RemoveBranch(*FromBB);

  // Get the condition of jumping from ToBB to FromBB
  typedef VInstrInfo::JT::iterator jt_it;
  jt_it at = ToJT.find(FromBB);
  assert(at != ToJT.end() && "ToBB not branching to FromBB?");
  MachineOperand PredCnd = at->second;

  if (!VInstrInfo::isAlwaysTruePred(PredCnd))
    PredicateBlock(PredCnd, FromBB);

  // And merge the block into its predecessor.
  ToBB->splice(ToBB->end(), FromBB, FromBB->begin(), FromBB->end());

  SmallVector<MachineOperand, 1> PredVec(1, PredCnd);
  std::set<MachineBasicBlock*> NewSuccs;

  for (jt_it I = FromJT.begin(),E = FromJT.end(); I != E; ++I){
    MachineBasicBlock *Succ = I->first;
    // Merge the PHINodes.
    VInstrInfo::mergePHISrc(Succ, FromBB, ToBB, *MRI, PredVec);
    // We had assert FromJT not contains FromBB, so we do not need to worry
    // about adding FromBB to the successor of ToBB again.
    // Is it the successor of FromBB become the new successor of ToBB?
    if (!ToJT.count(Succ)) ToBB->addSuccessor(Succ);

    FromBB->removeSuccessor(Succ);

    // And predicate the jump table.
    I->second = VInstrInfo::MergePred(I->second, PredCnd, *ToBB, ToBB->end(),
                                      MRI, TII, VTM::VOpAnd);
  }

  // Do not jump to FromBB any more.
  ToBB->removeSuccessor(FromBB);
  ToJT.erase(FromBB);
  // We had assert FromJT not contains FromBB.
  // FromJT.erase(FromBB);

  // Build the new Jump table.
  for (jt_it I = FromJT.begin(), E = FromJT.end(); I != E; ++I) {
    MachineBasicBlock *Succ = I->first;
    jt_it at = ToJT.find(Succ);
    // If the entry already exist in target jump table, merge it with opcode OR.
    if (at != ToJT.end())
      at->second = VInstrInfo::MergePred(at->second, I->second,
                                         *ToBB, ToBB->end(), MRI, TII,
                                         VTM::VOpOr);
    else // Simply insert the entry.
      ToJT.insert(*I);
  }

  // Re-insert the jump table.
  VInstrInfo::insertJumpTable(*ToBB, ToJT, DebugLoc());
  ++NumFallThroughMerged;
  CL.reset();
  IncreasedLatency = CL.computeLatency(*ToBB) - OriginalLatency;

  DEBUG(dbgs() << "........BB#" << FromBB->getNumber()
         << " merged, IncreasedLatency " << IncreasedLatency
         << ' ' << int(double(IncreasedLatency)/double(OriginalLatency) * 100.0)
         << "%\n");
  // Accumulate the increased latency
  IncreasedLatencies[ToBB->getNumber()] += IncreasedLatency;
  return true;
}

void MergeFallThroughBlocks::PredicateBlock(MachineOperand Pred,
                                            MachineBasicBlock *MBB ){
  typedef std::map<unsigned, unsigned> PredMapTy;
  PredMapTy PredMap;
  SmallVector<MachineOperand, 1> PredVec(1, Pred);

  // Predicate the Block.
  for (MachineBasicBlock::iterator I = MBB->begin(), E = MBB->end();
       I != E; ++I) {
    if (I->isDebugValue())
      continue;

    if (VIDesc(*I).hasDatapath())
      continue;

    if (TII->isPredicated(I)) {
      ucOperand *MO = cast<ucOperand>(VInstrInfo::getPredOperand(I));
      unsigned k = MO->getReg() << 1 | (MO->isPredicateInverted() ? 1 :0 );
      unsigned &Reg = PredMap[k];
      if (!Reg)
        Reg = VInstrInfo::MergePred(*MO, Pred, *MBB, I, MRI,
                                    TII, VTM::VOpAnd).getReg();

      MO->ChangeToRegister(Reg, false);
      MO->setTargetFlags(1);
    } else if (I->getOpcode() <= TargetOpcode::COPY) {
      MachineInstr *PseudoInst = I;
      ++I; // Skip current instruction, we may change it.
      PseudoInst = VInstrInfo::PredicatePseudoInstruction(PseudoInst,
                                                          PredVec);
      if (!PseudoInst) {
#ifndef NDEBUG
        dbgs() << "Unable to predicate " << *I << "!\n";
#endif
        llvm_unreachable(0);
      }
      I = PseudoInst;
    } else if (!TII->PredicateInstruction(I, PredVec)) {
#ifndef NDEBUG
      dbgs() << "Unable to predicate " << *I << "!\n";
#endif
      llvm_unreachable(0);
    }
  }
}

Pass *llvm::createMergeFallThroughBlocksPass() {
  return new MergeFallThroughBlocks();
}
