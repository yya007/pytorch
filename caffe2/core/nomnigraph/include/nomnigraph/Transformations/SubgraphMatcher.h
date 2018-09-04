#ifndef NOM_TRANFORMATIONS_SUBGRAPH_MATCHER_H
#define NOM_TRANFORMATIONS_SUBGRAPH_MATCHER_H

#include "nomnigraph/Graph/Graph.h"

#include <functional>
#include <sstream>
#include <vector>

namespace nom {

namespace matcher {

/**
 * MatchGraph is a graph of MatchNode.
 *
 * MatchNode needs a NodeMatchCriteria (a predicate for node matching) and
 * - A count, which means we may want to match this node multiple times from its
 * incoming edges. The count can be unlimited (think about it as a regex star).
 * - If nonTerminal flag is set, it means we will not consider outgoing edges
 * from the node when doing subgraph matching.
 */

template <typename NodeMatchCriteria>
class MatchNode {
 public:
  static const int kStarCount = -1;
  MatchNode(
      const NodeMatchCriteria& criteria,
      int count = 1,
      bool nonTerminal = false)
      : criteria_(criteria), count_(count), nonTerminal_(nonTerminal) {}

  NodeMatchCriteria getCriteria() const {
    return criteria_;
  }

  int getCount() const {
    return count_;
  }

  bool isNonTerminal() const {
    return nonTerminal_;
  }

 private:
  const NodeMatchCriteria criteria_;
  const int count_;
  const bool nonTerminal_;
};

template <typename NodeMatchCriteria>
using MatchGraph = Graph<MatchNode<NodeMatchCriteria>>;

template <typename NodeMatchCriteria>
using MatchNodeRef = typename MatchGraph<NodeMatchCriteria>::NodeRef;

template <typename NodeMatchCriteria>
MatchNodeRef<NodeMatchCriteria> tree(
    MatchGraph<NodeMatchCriteria>& graph,
    const NodeMatchCriteria& root,
    const std::vector<MatchNodeRef<NodeMatchCriteria>>& children,
    int count = 1,
    bool nonTerminal = false) {
  auto result =
      graph.createNode(MatchNode<NodeMatchCriteria>(root, count, nonTerminal));
  for (auto child : children) {
    graph.createEdge(result, child);
  }
  return result;
}

// TODO: Reuse convertToDotString once convertToDotString can work
// with subgraph.
template <typename NodeMatchCriteria>
std::string debugString(MatchNodeRef<NodeMatchCriteria> rootCriteriaRef) {
  std::ostringstream out;
  auto rootNode = rootCriteriaRef->data();
  out << "{rootCriteria = '" << rootNode.getCriteria() << "'";
  if (rootNode.getCount() != 1) {
    out << ", count = " << rootNode.getCount();
  }
  if (rootNode.isNonTerminal()) {
    out << ", nonTerminal = " << rootNode.isNonTerminal();
  }
  auto outEdges = rootCriteriaRef->getOutEdges();
  if (!outEdges.empty()) {
    out << ", childrenCriteria = [";
    for (auto& child : outEdges) {
      out << debugString<NodeMatchCriteria>(child->head()) << ", ";
    }
    out << "]";
  }
  out << "}";
  return out.str();
}

template <typename GraphType>
class SubtreeMatchResult {
 public:
  static SubtreeMatchResult<GraphType> notMatched(
      const std::string& debugMessage) {
    return SubtreeMatchResult<GraphType>(false, debugMessage);
  }

  static SubtreeMatchResult<GraphType> notMatched() {
    return SubtreeMatchResult<GraphType>(false, "Debug message is not enabled");
  }

  static SubtreeMatchResult<GraphType> matched() {
    return SubtreeMatchResult<GraphType>(true, "");
  }

  bool isMatch() const {
    return isMatch_;
  }

  std::string getDebugMessage() const {
    return debugMessage_;
  }

 private:
  SubtreeMatchResult(bool isMatch, const std::string& debugMessage)
      : isMatch_(isMatch), debugMessage_(debugMessage) {}

  const bool isMatch_;
  const std::string debugMessage_;
};

/*
 * Utilities for subgraph matching.
 */
template <
    typename GraphType,
    typename NodeMatchCriteria,
    typename NodeMatcherClass>
struct SubgraphMatcher {
  static bool isNodeMatch(
      typename GraphType::NodeRef node,
      const NodeMatchCriteria& criteria) {
    return NodeMatcherClass::isMatch(node, criteria);
  }

  // Check if there can be a sub-tree that matches the given criteria that
  // is rooted at the given rootNode.
  // The flag invertGraphTraversal specify if we should follow out edges or
  // in edges. The default is true which is useful for a functional
  // intepretation of a dataflow graph.
  static SubtreeMatchResult<GraphType> isSubtreeMatch(
      typename GraphType::NodeRef root,
      const MatchNodeRef<NodeMatchCriteria>& rootCriteriaRef,
      bool invertGraphTraversal = true,
      bool debug = false) {
    auto rootCriteriaNode = rootCriteriaRef->data();
    if (!isNodeMatch(root, rootCriteriaNode.getCriteria())) {
      if (debug) {
        std::ostringstream debugMessage;
        debugMessage << "Subtree root at " << root
                     << " does not match criteria "
                     << debugString<NodeMatchCriteria>(rootCriteriaRef);
        return SubtreeMatchResult<GraphType>::notMatched(debugMessage.str());
      } else {
        return SubtreeMatchResult<GraphType>::notMatched();
      }
    }
    if (rootCriteriaNode.isNonTerminal()) {
      // This is sufficient to be a match if this criteria specifies a non
      // terminal node.
      return SubtreeMatchResult<GraphType>::matched();
    }
    auto& edges =
        invertGraphTraversal ? root->getInEdges() : root->getOutEdges();

    int numEdges = edges.size();
    const auto outEdges = rootCriteriaRef->getOutEdges();
    int numChildrenCriteria = outEdges.size();

    // The current algorithm implies that the ordering of the children is
    // important. The children nodes will be matched with the children subtree
    // criteria in the given order.

    int currentEdgeIdx = 0;
    for (int criteriaIdx = 0; criteriaIdx < numChildrenCriteria;
         criteriaIdx++) {
      auto childrenCriteriaRef = outEdges[criteriaIdx]->head();

      int expectedCount = childrenCriteriaRef->data().getCount();
      bool isStarCount =
          expectedCount == MatchNode<NodeMatchCriteria>::kStarCount;

      int countMatch = 0;

      // Continue to match subsequent edges with the current children criteria.
      // Note that if the child criteria is a * pattern, this greedy algorithm
      // will attempt to find the longest possible sequence that matches the
      // children criteria.
      for (; currentEdgeIdx < numEdges &&
           (isStarCount || countMatch < expectedCount);
           currentEdgeIdx++) {
        auto edge = edges[currentEdgeIdx];
        auto child = invertGraphTraversal ? edge->tail() : edge->head();

        if (!isSubtreeMatch(child, childrenCriteriaRef, invertGraphTraversal)
                 .isMatch()) {
          if (!isStarCount) {
            // If the current criteria isn't a * pattern, this indicates a
            // failure.
            if (debug) {
              std::ostringstream debugMessage;
              debugMessage << "Child node at " << child
                           << " does not match child criteria "
                           << debugString<NodeMatchCriteria>(
                                  childrenCriteriaRef)
                           << ". We expected " << expectedCount
                           << " matches but only found " << countMatch << ".";
              return SubtreeMatchResult<GraphType>::notMatched(
                  debugMessage.str());
            } else {
              return SubtreeMatchResult<GraphType>::notMatched();
            }
          } else {
            // Otherwise, we should move on to the next children criteria.
            break;
          }
        }

        countMatch++;
      }

      if (countMatch < expectedCount) {
        // Fails because there are not enough matches as specified by the
        // criteria.
        if (debug) {
          std::ostringstream debugMessage;
          debugMessage << "Expected " << expectedCount
                       << " matches for child criteria "
                       << debugString<NodeMatchCriteria>(childrenCriteriaRef)
                       << " but only found " << countMatch;
          return SubtreeMatchResult<GraphType>::notMatched(debugMessage.str());
        } else {
          return SubtreeMatchResult<GraphType>::notMatched();
        }
      }
    }

    if (currentEdgeIdx < numEdges) {
      // Fails because there are unmatched edges.
      if (debug) {
        std::ostringstream debugMessage;
        debugMessage << "Unmatched children for subtree root at " << root
                     << ". There are " << numEdges
                     << " children, but only found " << currentEdgeIdx
                     << " matches for the children criteria.";
        return SubtreeMatchResult<GraphType>::notMatched(debugMessage.str());
      } else {
        return SubtreeMatchResult<GraphType>::notMatched();
      }
    }
    return SubtreeMatchResult<GraphType>::matched();
  }

  // Utility to transform a graph by looking for subtrees that match
  // a given pattern and then allow callers to mutate the graph based on
  // subtrees that are found.
  // The current implementation doesn't handle any graph transformation
  // itself. Callers should be responsible for all intended mutation, including
  // deleting nodes in the subtrees found by this algorithm.
  // Note: if the replaceFunction lambda returns false, the entire procedure
  // is aborted. This maybe useful in certain cases when we want to terminate
  // the subtree search early.
  // invertGraphTraversal flag: see documentation in isSubtreeMatch
  static void replaceSubtree(
      GraphType& graph,
      const MatchNodeRef<NodeMatchCriteria>& criteria,
      const std::function<
          bool(GraphType& g, typename GraphType::NodeRef subtreeRoot)>&
          replaceFunction,
      bool invertGraphTraversal = true) {
    for (auto nodeRef : graph.getMutableNodes()) {
      // Make sure the node is still in the graph.
      if (!graph.hasNode(nodeRef)) {
        continue;
      }
      if (isSubtreeMatch(nodeRef, criteria, invertGraphTraversal).isMatch()) {
        if (!replaceFunction(graph, nodeRef)) {
          // If replaceFunction returns false, it means that we should abort
          // the entire procedure.
          break;
        }
      }
    }
  }
};

} // namespace matcher

} // namespace nom

#endif // NOM_TRANFORMATIONS_SUBGRAPH_MATCHER_H
