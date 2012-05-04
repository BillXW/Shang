//===---- CompGraph.h - Compatibility Graph ---------------------*- C++ -*-===//
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
// This file defines the CompGraphNode and CompGraph, which are used in the
// resource allocation and binding algorithm.
//
//===----------------------------------------------------------------------===//
#ifndef COMPATIBILITY_GRAPH_H
#define COMPATIBILITY_GRAPH_H

#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"

#include <vector>
#include <map>

namespace llvm {
class raw_ostream;

namespace CompGraphWeights {
  static const int HUGE_NEG_VAL = -1000000000;
  static const int TINY_VAL = 1;
}

template<class T>
class CompGraphNode {
  typedef CompGraphNode<T> Self;
  typedef CompGraphTraits<T> Traits;
  // The underlying data.
  T N;

  typedef SmallPtrSet<Self*, 8> NodeVecTy;
  // Predecessors and Successors.
  NodeVecTy Preds, Succs;

  typedef std::map<Self*, int> WeightVecTy;
  WeightVecTy SuccWeights;

public:
  explicit CompGraphNode(T Node = T()) : N(Node) {}

  bool isTrivial() const { return N == T(); }

  T get() const { return N; }
  T operator->() const { return N; }

  //void print(raw_ostream &OS) const;
  //void dump() const;

  //typedef NodeVecTy::iterator iterator;
  typedef typename NodeVecTy::const_iterator iterator;

  iterator succ_begin() const { return Succs.begin(); }
  iterator succ_end()   const { return Succs.end(); }
  unsigned num_succ()   const { return Succs.size(); }
  bool     succ_empty() const { return Succs.empty(); }

  iterator pred_begin() const { return Preds.begin(); }
  iterator pred_end()   const { return Preds.end(); }
  unsigned num_pred()   const { return Preds.size(); }
  bool     pred_empty() const { return Preds.empty(); }

  int getWeightTo(Self *To) const {
    return SuccWeights.find(To)->second;
  }

  // Unlink the Succ from current node.
  void unlinkSucc(CompGraphNode *Succ) {
    bool deleted = Succs.erase(Succ);
    assert(deleted && "Succ is not the successor of this!");
    SuccWeights.erase(Succ);

    // Current node is not the predecessor of succ node too.
    deleted = Succ->Preds.erase(this);
    assert(deleted && "this is not the predecessor of succ!");
    (void) deleted;
  }

  // Unlink the Pred from current node.
  void unlinkPred(CompGraphNode *Pred) {
    bool deleted = Preds.erase(Pred);
    assert(deleted && "Pred is not the predecessor of this!");

    // Current node is not the successor of pred node too.
    deleted = Pred->Succs.erase(this);
    assert(deleted && "this is not the successor of Pred!");
    (void) deleted;
  }

  void unlink() {
    while (!succ_empty())
      unlinkSucc(*succ_begin());

    while (!pred_empty())
      unlinkPred(*pred_begin());
  }

  template<class CompEdgeWeight>
  void updateEdgeWeight(CompEdgeWeight &C) {
    for (iterator I = succ_begin(), E = succ_end(); I != E; ++I) {
      Self *Succ = *I;
      // Not need to update the weight of the exit edge.
      if (Succ->get()) SuccWeights[Succ] = C(this->get(), Succ->get());
      // Make find longest path prefer to end with exit if possible.
      else             SuccWeights[Succ] = CompGraphWeights::TINY_VAL;
    }
  }

  // Make the edge with default weight, we will udate the weight later.
  static void MakeEdge(CompGraphNode *Src, CompGraphNode *Dst) {
    T SrcN = Src->get(), DstN = Dst->get();
    // Make sure source is earlier than destination.
    if (SrcN != T() && DstN != T() && Traits::isEarlier(DstN, SrcN))
      std::swap(Dst, Src);

    Src->Succs.insert(Dst);
    Src->SuccWeights.insert(std::make_pair(Dst, 0));
    Dst->Preds.insert(Src);
  }
};

template<class T> struct GraphTraits<CompGraphNode<T>*> {
  typedef CompGraphNode<T> NodeType;
  typedef typename NodeType::iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->succ_begin();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->succ_end();
  }
};

template<typename T, typename IDTy = unsigned>
class CompGraph {
public:
  typedef CompGraphNode<T> NodeTy;
private:
  typedef CompGraphTraits<T> Traits;
  typedef DenseMap<T, NodeTy*> NodeMapTy;
  // The dummy entry node of the graph.
  NodeTy Entry, Exit;
  // Nodes vector.
  NodeMapTy Nodes;

public:
  const IDTy ID;
  CompGraph(IDTy id = IDTy()) : ID(id) {}

  ~CompGraph() {
    DeleteContainerSeconds(Nodes);
  }

  typedef typename NodeTy::iterator iterator;

  // All nodes (except exit node) are successors of the entry node.
  iterator begin() { return Entry.succ_begin(); }
  iterator end()   { return Entry.succ_end(); }

  NodeTy *GetOrCreateNode(T N) {
    assert(N && "Unexpected null pointer pass to GetOrCreateNode!");
    NodeTy *&Node = Nodes[N];
    // Create the node if it not exists yet.
    if (Node == 0) {
      Node = new NodeTy(N);
      // And insert the node into the graph.
      for (iterator I = begin(), E = end(); I != E; ++I) {
        NodeTy *Other = *I;

        // Make edge between compatible nodes.
        if (Traits::compatible(Node->get(), Other->get()))
          NodeTy::MakeEdge(Node, Other);
      }

      // There will always edge from entry to a node and from node to exit.
      NodeTy::MakeEdge(&Entry, Node);
      NodeTy::MakeEdge(Node, &Exit);
    }

    return Node;
  }

  void deleteNode(NodeTy *N) {
    Nodes.erase(N->get());
    N->unlink();
    delete N;
  }

  // Return true if the longest path is not trivial (have more than 1 nodes).
  int findLongestPath(SmallVectorImpl<T> &Path, bool DelNodes = false) {
    std::map<NodeTy*, unsigned> LenMap;

    std::map<NodeTy*, NodeTy*> PathPred;
    std::map<NodeTy*, int> PathWeight;

    //for each vertex v in topOrder(G) do
    typedef typename NodeTy::iterator ChildIt;
    SmallVector<std::pair<NodeTy*, ChildIt>, 32> WorkStack;
    std::map<NodeTy*, unsigned> VisitCount;

    WorkStack.push_back(std::make_pair(&Entry, Entry.succ_begin()));
    PathWeight[&Entry] = 0;

    while (!WorkStack.empty()) {
      NodeTy *Node = WorkStack.back().first;
      ChildIt It = WorkStack.back().second;

      if (It == Node->succ_end())
        WorkStack.pop_back();
      else {
        //
        NodeTy *ChildNode = *It;
        ++WorkStack.back().second;
        unsigned VC = ++VisitCount[ChildNode];

        // for each edge (Node, ChildNode) in E(G) do
        int EdgeWeight = Node->getWeightTo(ChildNode);
        // Do not introduce zero weight edge to the longest path.
        if (EdgeWeight > 0) {
          int NewPathWeight = PathWeight[Node] + EdgeWeight;
          int &OldPathWeight = PathWeight[ChildNode];
          if (OldPathWeight < NewPathWeight) {
            // Update the weight
            OldPathWeight = NewPathWeight;
            // And the pred
            PathPred[ChildNode] = Node;
          }
        }

        // Only move forward when we visit the node from all its preds.
        if (VC == ChildNode->num_pred())
          WorkStack.push_back(std::make_pair(ChildNode, ChildNode->succ_begin()));
      }
    }

    unsigned FinalPathWeight = PathWeight[&Exit] - CompGraphWeights::TINY_VAL;
    if (FinalPathWeight > 0) {
      // Fill the result vector.
      for (NodeTy *I = PathPred[&Exit]; I && I != &Entry; I = PathPred[I]) {
        Path.push_back(I->get());
        if (DelNodes) deleteNode(I);
      }
    }

    return FinalPathWeight;
  }

  template<class CompEdgeWeight>
  void updateEdgeWeight(CompEdgeWeight &C) {
    for (iterator I = begin(), E = end(); I != E; ++I)
      (*I)->updateEdgeWeight(C);
  }

  void viewGraph();
};

template <typename T1, typename T2> struct GraphTraits<CompGraph<T1, T2>*>
  : public GraphTraits<CompGraphNode<T1>*> {
  
  typedef typename CompGraph<T1, T2>::iterator nodes_iterator;
  static nodes_iterator nodes_begin(CompGraph<T1, T2> *G) {
    return G->begin();
  }
  static nodes_iterator nodes_end(CompGraph<T1, T2> *G) {
    return G->end();
  }
};

}

#endif
