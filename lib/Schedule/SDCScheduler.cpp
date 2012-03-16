//===- SDCScheduler.cpp ------- SDCScheduler --------------------*- C++ -*-===//
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

#include "SchedulingBase.h"
#include "vtm/VInstrInfo.h"
#include "lp_solve/lp_lib.h"
#define DEBUG_TYPE "SDCdebug"
#include "llvm/Support/Debug.h"

using namespace llvm;

SDCScheduler::SDCScheduler(VSchedGraph &S)
  : SchedulingBase(S), NumVars(0), NumInst(0) {
}

void SDCScheduler::createStepVariables(lprec *lp) {
  unsigned Col =  1;
  typedef VSchedGraph::sched_iterator it;
  for (it I = State.sched_begin(),E = State.sched_end();I != E; ++I) {
    ++NumInst;
    const VSUnit* U = *I;
    // Set up the scheduling variables for VSUnits.
    SUIdx[U] = NumVars;
    for(unsigned i = 0, j = getMaxLatency(U); i <= j; i++){
      std::string SVStart = "sv" + utostr_32(U->getIdx()) + "start" + utostr_32(i);
      DEBUG(dbgs()<<"the col is"<<Col<<"the colName is"<<SVStart<<"\n");
      set_col_name(lp, Col, const_cast<char*>(SVStart.c_str()));
      set_int(lp,Col,TRUE);
      ++Col;
      ++NumVars;
    }
  }
}

void SDCScheduler::addStepConstraints(lprec *lp){
  int Col[2];
  /*const*/ REAL Val[2] = { -1.0, 1.0 };
  //Build the constraints for LP Variables as SVXStart1 - SVXstart0 = 1.
  for(SUIdxIt EI = SUIdx.begin(), EE = SUIdx.end(); EI != EE; ++EI){
    unsigned Idx = EI->second;
    const VSUnit* U = EI->first;
    unsigned MaxLatency = getMaxLatency(U);
    if(MaxLatency < 1) continue;
    for(unsigned i = 0, j = MaxLatency; i < j; ++i){
      Col[0] = 1 + Idx + i;
      Col[1] = 1 + Idx + i + 1;
      if(!add_constraintex(lp, 2, Val, Col, EQ, 1.0))
        report_fatal_error("SDCScheduler: Can NOT step Variable Constraints"
          " at VSUnit " + utostr_32(U->getIdx()) );        
    }
  }
}

void SDCScheduler::addDependencyConstraints(lprec *lp) {
  int Col[2];
  /*const*/ REAL Val[2] = { -1.0, 1.0 };
  for(VSchedGraph::sched_iterator I = State.sched_begin(), E = State.sched_end();
      I != E; ++I) {
    const VSUnit *U = *I;
    assert(U->isControl() && "Unexpected datapath in scheduler!");

    // Build the constraint for Dst_SU_startStep - Src_SU_endStep >= 0.
    for (VSUnit::const_use_iterator DI = U->use_begin(),
        DE = U->use_end(); DI != DE;++DI) {
      const VSUnit *depIn = *DI;
      const VDEdge *Edge = depIn->getEdgeFrom(U);
      unsigned Latency = Edge->getLatency();
      unsigned SrcEndIdx =  SUIdx[U] + Edge->getLatency();
      unsigned DstStartIdx = SUIdx[depIn];

      // Build the LP.
      Col[0] = 1 + SrcEndIdx;
      Col[1] = 1 + DstStartIdx;
      if(!add_constraintex(lp, 2, Val, Col, GE, 0.0))
        report_fatal_error("SDCScheduler: Can NOT step Dependency Constraints"
        " at VSUnit " + utostr_32(U->getIdx()) );   
    }
  }
}

void SDCScheduler::addResourceConstraints(lprec *lp) {
  //The table of the ALAP and the VSUnits.
  Step2SUMap IdenticalMap;
  BoundSUVec OrderIdentity;
  BoundSUVec FUASAPVec;
  Step2SUMap FUMap;
  //The table of the TimeFrame and the VSUnits.
  Step2SUMap OrderIdentityMap;
  typedef std::pair<const VSUnit*, const VSUnit*> CommonInputPair;
  CommonInputPair CommonPair;
  typedef std::vector<CommonInputPair> CommonInputVec;
  CommonInputVec CommonVec;

  //Get the VSUnits that need add into the resource constraints.
  for(VSchedGraph::sched_iterator I = State.sched_begin(), E = State.sched_end();
    I != E; ++I) {
    const VSUnit *SV = *I;
    if (SV->getFUType() > VFUs::LastCommonFUType
      || SV->getFUType() < VFUs::FirstNonTrivialFUType)
      continue;
      FUMap[SV->getFUType()].push_back(SV);
  }
  
  for( Step2SUMap::iterator FI = FUMap.begin(), FE = FUMap.end(); 
    FI != FE; FI++){
    bool MemType = false;
    unsigned FuType = FI->first;
    DEBUG(dbgs()<<"The FU Type is :"<<FuType<<"\n");

    if(FuType == VFUs::BRam || FuType == VFUs::MemoryBus || FuType == VFUs::CalleeFN)
      MemType = true;

    BoundSUVec Set = FI->second;
    if(Set.size()<=1) continue;
   
    //Map the VSUnits to their ALAPStep.
    unsigned FirstSlot = 10000;
    unsigned LastSlot = 0;
    BoundSUVec::iterator LastIt = Set.begin();
    for(BoundSUVec::iterator iB = Set.begin(),eB = Set.end(); iB != eB; iB++){
      const VSUnit *V = *iB;
      if(getASAPStep(V) < FirstSlot){
        FirstSlot = getASAPStep(V);
        FUASAPVec.clear();
        FUASAPVec.push_back(V);
      }
      else if(getASAPStep(V) == FirstSlot)
        FUASAPVec.push_back(V);

      if(getALAPStep(V) > LastSlot)
        LastSlot = getALAPStep(V);

      IdenticalMap[getALAPStep(V)].push_back(V);
    }

    //Set the slot of the LastBindFU to its ALAPSlot.
  /*  bool hasCriticalPath = false;
    for(BoundSUVec::iterator iT = FUASAPVec.begin(), eT =FUASAPVec.end(); iT != eT;
        ++iT){
      const VSUnit* FirstBindFU = *iT;
      if(getTimeFrame(FirstBindFU) == 1){
        hasCriticalPath = true;
        setConstantResourceConstraints(lp, FirstBindFU, FirstSlot);
      }
    }*/
    

    //Sort the IdenticalMap in descending order of the TimeFrame.
    //The TimeFrame means the freedom of the VSUnits in scheduling. 
    //SDCScheduler prefer to move the VSUnits that have the bigger TimeFrame 
    //to next slot.
    unsigned CriticalFUNum = 0;
    for(Step2SUMap::iterator IS = IdenticalMap.begin(), ES = IdenticalMap.end();
      IS != ES; IS++){
        BoundSUVec V = IS->second;
        unsigned idx = IS->first;

        if(V.size()<=1) continue;      
        for(BoundSUVec::iterator iU = V.begin(),eU = V.end(); iU != eU; iU++){
          const VSUnit* U = *iU;
          OrderIdentityMap [getTimeFrame(U)].push_back(U);
        }

        //-------------------------------------------//
        unsigned Num = OrderIdentityMap[1].size();
        if(Num > CriticalFUNum)
          CriticalFUNum = Num;
        //-------------------------------------------//
        while(!OrderIdentityMap.empty()){
          unsigned MinTimeFrame = 100;   
          for(Step2SUMap::iterator IU = OrderIdentityMap.begin(), 
            EU = OrderIdentityMap.end(); IU != EU; IU++){
            unsigned CurTF = IU->first;
            if(CurTF < MinTimeFrame)
              MinTimeFrame = CurTF;
          }
          Step2SUMap::iterator it = OrderIdentityMap.find(MinTimeFrame);
          BoundSUVec B = it->second;
          typedef BoundSUVec::iterator OrderIt;
          for(OrderIt i = B.begin(),e = B.end(); i != e; i++){
            const VSUnit *O = *i;
            OrderIdentity.push_back(O);
          }
          OrderIdentityMap.erase(it);
        }//end while
        IdenticalMap[idx] = OrderIdentity;
        OrderIdentity.clear();
    }

    //Compute the average number of the FU.
    double AverageRC = double(Set.size())/double((LastSlot - FirstSlot + 1));
    unsigned AverageNum = std::max<unsigned>(ceil(AverageRC), CriticalFUNum);
    DEBUG(dbgs()<<"the average resource is :"<<AverageNum<<"\n");

    if(MemType || AverageNum <= 1)
      addTopologyResourceConstraints(lp, IdenticalMap);

    // Order the IdenticalMap for MultiFU.
    else{
      unsigned PassFUNum = 0;
      for(Step2SUMap::iterator iS = IdenticalMap.begin(), eS = IdenticalMap.end();
        iS != eS; iS++){         
          unsigned MapIdx = iS->first;
          BoundSUVec BV = iS->second;
          if(BV.size() <= 1) continue;
          int ExtraFU = BV.size() - AverageNum;
          //Find the operations that have the same inputs.
          //for(BoundSUVec::iterator iU = BV.begin(),eU = BV.end(); iU != eU; iU++){
          //  const VSUnit* U = *iU;
          //  BoundSUVec::iterator i = iU + 1;
          //  for(BoundSUVec::iterator iV = i,eV = BV.end(); iV != eV; iV++){
          //    const VSUnit* V = *iV;
          //    if(hasCommonInput(U,V)){
          //      CommonPair = std::make_pair(U,V);
          //      BV.erase(iV);
          //      CommonVec.push_back(CommonPair);
          //      --ExtraFU;
          //    }// end if
          //  }//end for            
          //}//end for

          if(ExtraFU <= 0) {
            PassFUNum = 0;
            continue;
          }
          // Build the constraints to satisfy the limited resource number.
          BoundSUVec::iterator begin = BV.begin();
          const VSUnit* UBegin = *begin;
          unsigned FUCount = 0;
          for(int i = 0, j = ExtraFU; i < j; i++){
            BoundSUVec::reverse_iterator rit = BV.rbegin();
            const VSUnit* UEnd = *rit;
            CommonPair = std::make_pair(UBegin, UEnd);
            CommonVec.push_back(CommonPair);
            ++rit;
            ++FUCount;
          }
          PassFUNum = FUCount;

          // Add LP constraints.
          for(CommonInputVec::iterator iC = CommonVec.begin(), eC = CommonVec.end(); 
            iC != eC; iC++){
              int col[2];
              REAL val[2];
              CommonPair = *iC;
              const VSUnit *front = CommonPair.first;
              const VSUnit *back = CommonPair.second;
              unsigned FrontStartIdx = SUIdx[front];
              unsigned BackStartIdx = SUIdx[back];
              col[0] = 1 + BackStartIdx;
              val[0] = -1.0;
              col[1] = 1 + FrontStartIdx;
              val[1] = 1.0;
              if(!add_constraintex(lp, 2, val, col, GE, 1.0))
                report_fatal_error("SDCScheduler: Can NOT step Resource Constraints"
                " at VSUnit " + utostr_32(front->getIdx()) );  
          }
          CommonVec.clear();
      }// end for
      IdenticalMap.clear();
    }// end else

  }   
  FUMap.clear();
  return;
}

void SDCScheduler::addTopologyResourceConstraints(lprec *lp, Step2SUMap &Map){
   BoundSUVec OrderVec;
   //Sort the operations in descending order.   
    while(!Map.empty()){
      unsigned MaxIdx = 0;
      for(Step2SUMap::iterator iB = Map.begin(),eB = Map.end(); iB != eB; iB++){
          unsigned CurIdx = iB->first;
        if(CurIdx>MaxIdx)
          MaxIdx = CurIdx;
      }
      Step2SUMap::iterator it = Map.find(MaxIdx);
      BoundSUVec G = it->second;
      typedef BoundSUVec::iterator orderIt;
      for(orderIt i = G.begin(),e = G.end(); i != e; i++){
        const VSUnit *O = *i;
        OrderVec.push_back(O);
      }
      Map.erase(it);
    }
    Map.clear();

    //Build the constraints for Dst_SU_startStep - Src_SU_startStep >= Lantency
    //that means Dst_SU and Src_SU can not be scheduled in the same step.
    BoundSUVec::iterator i = OrderVec.begin();
    const VSUnit *front = *i;
    unsigned DstStartIdx = SUIdx[front];
    for (BoundSUVec::iterator OVB = ++i,OVE = OrderVec.end();
          OVB != OVE; OVB++){
      int col[2];
      REAL val[2];
      const VSUnit *back = *OVB;
      unsigned SrcStartIdx = SUIdx[back];
      col[0] = 1 + DstStartIdx;
      val[0] = 1.0;
      col[1] = 1 + SrcStartIdx;
      val[1] = -1.0;
      if(!add_constraintex(lp, 2, val, col, GE, int(front->getLatency())))
        report_fatal_error("SDCScheduler: Can NOT stepTopology Resource Constraints"
        " at VSUnit " + utostr_32(back->getIdx()) );  
      front = back;
      DstStartIdx = SrcStartIdx;
    }
    OrderVec.clear();
    return;
  }


void SDCScheduler::setConstantResourceConstraints(lprec *lp, const VSUnit* U, 
  unsigned Slot){
    unsigned Idx = SUIdx[U];
    int col[1];
    REAL val[1];
    col[0] = 1 + Idx;
    val[0] = 1.0;
    if(!add_constraintex(lp, 1, val, col, EQ, int(Slot - State.getStartSlot())))
      report_fatal_error("SDCScheduler: Can NOT step Constant Resource Constraints"
      " at VSUnit " + utostr_32(U->getIdx()) );
    return;
}

bool SDCScheduler::hasCommonInput(const VSUnit* Src, const VSUnit* Dst){
  std::set<const VSUnit*> DepSet;
  unsigned SrcCounter = 0;
  for(VSUnit::const_dep_iterator iS = Src->dep_begin(), eS = Src->dep_end();
      iS != eS; iS++){
    if(iS.getEdge()->getEdgeType() != VDEdge::edgeValDep) continue;
    const VSUnit* U = *iS;
    DepSet.insert(U);
    ++SrcCounter;
  }
  unsigned DstCounter = 0;
  for(VSUnit::const_dep_iterator iD = Dst->dep_begin(), eD = Dst->dep_end();
      iD != eD; iD++){
    if(iD.getEdge()->getEdgeType() != VDEdge::edgeValDep) continue;
    const VSUnit* U = *iD;
    DepSet.insert(U);
    ++DstCounter;
  }

  unsigned OriginSize = SrcCounter + DstCounter;
  return (DepSet.size() - OriginSize);
}

void SDCScheduler::buildASAPObject() {
  std::vector<int> Indices(NumInst);
  std::vector<REAL> Coefficients(NumInst);

  unsigned Col = 0;
  //Build the ASAP object function.
  typedef VSchedGraph::sched_iterator it;
  for(it I = State.sched_begin(),E = State.sched_end();I != E; ++I) {
      const VSUnit* U = *I;
    unsigned Idx = SUIdx[U];
    Indices[Col] = 1 + Idx;
    Coefficients[Col] = 1.0;
    ++Col;
  }

  set_obj_fnex(lp, Col, Coefficients.data(), Indices.data());
  set_minim(lp);
  DEBUG(write_lp(lp, "log.lp"));
}

void SDCScheduler::buildOptimizingSlackDistributionObject(){
  std::vector<int> Indices(NumInst);
  std::vector<REAL> Coefficients(NumInst);

  unsigned Col = 0;
  //Build the Optimizing Slack object function.
  typedef VSchedGraph::sched_iterator it;
  for(it I = State.sched_begin(),E = State.sched_end();I != E; ++I) {
    const VSUnit* U = *I;
    int Indeg = U->getNumDeps();
    int Outdeg = U->getNumUses();
    unsigned Idx = SUIdx[U];
    Indices[Col] = 1 + Idx;
    Coefficients[Col] = Outdeg - Indeg;
    ++Col;
  }

  set_obj_fnex(lp, Col, Coefficients.data(), Indices.data());
  set_maxim(lp);

  DEBUG(write_lp(lp, "log.lp"));

}

void SDCScheduler::buildSchedule(lprec *lp) {
  typedef VSchedGraph::sched_iterator it;
  for(it I = State.sched_begin(),E = State.sched_end();I != E; ++I) {
      VSUnit *U = *I;
    unsigned Offset = SUIdx[U];
    unsigned cur = State.getStartSlot();
    unsigned j = get_var_primalresult(lp, TotalRows + Offset + 1);
    DEBUG(dbgs() << "the row is:" << TotalRows + Offset + 1
                 <<"the result is:" << j << "\n");
    unsigned shedslot = j+State.getStartSlot();
    U->scheduledTo(j+State.getStartSlot());
  }
}

bool SDCScheduler::scheduleState() {
  buildFDepHD(true);
  //DEBUG(viewGraph());
  // Ensure there is no resource conflict in critical path.
  if (!scheduleCriticalPath(false))
    return false;

  if (allNodesSchedued()) return true;

  lp = make_lp(0, NumVars);

  set_add_rowmode(lp, TRUE);

  // Build the step variables.
  createStepVariables(lp);

  // Build the constraints.
  addStepConstraints(lp);
  addDependencyConstraints(lp);
  addResourceConstraints(lp);
  // Turn off the add rowmode and start to solve the model.
  set_add_rowmode(lp, FALSE);
  TotalRows = get_Nrows(lp);
  //buildAXAPObject();
  buildOptimizingSlackDistributionObject();
  int result = solve(lp);
  //viewGraph();
  switch (result) {
  case INFEASIBLE:
    delete_lp(lp);
    return false;
  case SUBOPTIMAL:
    DEBUG(dbgs() << "Note: suboptimal schedule found!\n");
  case OPTIMAL:
  case PRESOLVED:
    break;
  default:
    report_fatal_error(Twine("SDCScheduler Schedule fail: "));
  }
  // Schedule the state with the ILP result.
  buildSchedule(lp);
  delete_lp(lp);
  SUIdx.clear();
  return true;
}

