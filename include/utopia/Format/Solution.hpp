#pragma once
#include "utopia/Format/CodeWriter.hpp"
#include "utopia/Format/Piece.hpp"
#include <algorithm>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace utopia {

class SolutionCache;

/* A single possible set of formatting choices.
 *
 * Each solution binds some number of Pieces in the piece tree to States. (Any
 * pieces whose states are not bound are treated as having a default unsplit
 * state.)
 *
 * Given that set of states, we can create a CodeWriter and give that to all of
 * the pieces in the tree so they can format themselves. That in turn yields a
 * total number of overflow characters, cost, and formatted output, which are
 * all stored here. */
class Solution {
public:
  /* The states that pieces have been bound to.
   *
   * This is a linked list of nodes, where each node adds a single piece
   * binding. This allows "copying" the state map in O(1) time by just sharing
   * the list tail. Lookups are O(N) where N is the number of bound pieces, but
   * N is typically very small. Nodes are allocated in the arena owned by the
   * [SolutionCache], which outlives every solution. */
  const BoundStateNode *pieceStates = nullptr;

  /* The amount of penalties applied based on the chosen line splits. */
  int cost = 0;

  /* The cost of this solution from branches of the piece tree that were
   * separately formatted and merged in using [mergeSubtree]. We track this
   * separately so that when expanding a solution, we don't double count the
   * cost of separately formatted branches. */
  int subtreeCost = 0;

  /* False if this Solution contains a newline where one is prohibited. */
  bool isValid = true;

  /* Whether the solution contains an invalid newline and the piece that
   * prohibits the newline is bound in this solution. When this is `true`, it
   * means this solution and every solution that could be derived from it is
   * invalid so the whole solution tree can be discarded. */
  bool isDeadEnd = false;

  /* The total number of characters that do not fit inside the page width. */
  int overflow = 0;

  /* The formatted code. */
  std::string code;

  /* The unsolved pieces in this solution that should be expanded next to
   * produce new more refined solutions. */
  std::vector<const Piece *> expandPieces;

  /* The set of states that pieces are allowed to be in without violating
   * constraints of already bound pieces. */
  std::unordered_map<const Piece *, std::vector<State>> allowedStates;

  Solution(std::deque<BoundStateNode> *arena, const Piece *root, int cost,
           const BoundStateNode *pieceStates, State *rootState)
      : arena(arena), root(root), cost(cost), pieceStates(pieceStates) {
    /* If we're formatting a subtree of a larger Piece tree that binds [root]
     * to [rootState], then bind it in this solution too. */
    if (rootState != nullptr)
      bind(root, *rootState);
  }

  /* The total cost of this solution, including separately formatted subtrees.
   */
  int totalCost() const { return cost + subtreeCost; }

  /* Attempt to eagerly bind [piece] to a state given that it must fit within
   * [pageWidth]. If it can, binds the piece to that state in this solution and
   * returns `true`. Otherwise returns `false`. */
  bool tryBindByPageWidth(const Piece *piece, int pageWidth) {
    if (auto stateOpt = piece->fixedStateForPageWidth(pageWidth)) {
      bind(piece, *stateOpt);
      return true;
    }

    return false;
  }

  /* The state that [piece] is pinned to or that this solution selects. If no
   * state has been selected, defaults to the first state. */
  State pieceState(const Piece *piece) const {
    const State *bound = pieceStateIfBound(piece);
    return bound ? *bound : State::Unsplit;
  }

  /* The state that [piece] is pinned to or that this solution selects, or
   * `nullptr` if it isn't bound. */
  const State *pieceStateIfBound(const Piece *piece) const {
    if (const State *pinned = piece->pinnedState())
      return pinned;

    for (const BoundStateNode *node = pieceStates; node != nullptr;
         node = node->parent) {
      if (node->piece == piece)
        return &node->state;
    }

    return nullptr;
  }

  /* Whether [piece] has been bound to a state in this set (or is pinned). */
  bool isBound(const Piece *piece) const {
    return pieceStateIfBound(piece) != nullptr;
  }

  /* Increases the total overflow for this solution by [overflow]. Only called
   * by CodeWriter. */
  void addOverflow(int overflow) { this->overflow += overflow; }

  /* Apply the overflow, cost, and bound states from [subtreeSolution] to this
   * solution. Called when a subtree of a Piece tree is solved separately and
   * the resulting solution is being merged with this one. */
  void mergeSubtree(const Solution &subtreeSolution) {
    overflow += subtreeSolution.overflow;
    subtreeCost += subtreeSolution.totalCost();
  }

  /* Mark this solution as having a newline where none is permitted. Only
   * called by CodeWriter. */
  void invalidate(const Piece *piece) {
    /* Don't invalidate if the piece is pinned and can't do anything. */
    if (piece->pinnedState() != nullptr)
      return;

    isValid = false;
  }

  /* Derives new potential solutions from this one by binding [expandPieces]
   * to all of their possible states.
   *
   * If there is no potential piece to expand, or all attempts to expand it
   * fail, returns an empty list. */
  std::vector<Solution> expand(SolutionCache &cache, int pageWidth,
                               int leadingIndent, int subsequentIndent);

  /* Run a CodeWriter on this solution to produce the final formatted output
   * and calculate the overflow and expand pieces. */
  void format(SolutionCache &cache, int pageWidth, int leadingIndent,
              int subsequentIndent);

  /* Returns the formatted code. */
  const std::string &getCode() const { return code; }

  /* Compares two solutions where a more desirable solution comes first.
   *
   * For performance, we want to stop checking solutions as soon as we find the
   * best one. Best means the fewest overflow characters and the lowest cost.
   * Even though overflow is "worse" than cost, we order in terms of cost
   * because a solution with overflow may lead to a low-cost solution without
   * overflow, so we want to explore in cost order. */
  bool isWorseThan(const Solution &other) const {
    if (totalCost() != other.totalCost())
      return totalCost() > other.totalCost();

    if (overflow != other.overflow)
      return overflow > other.overflow;

    /* If all else is equal, prefer lower states in earlier bound pieces. Our
     * linked list is in reverse order (newest first), so reverse it to get the
     * pieces in insertion order. */
    std::vector<const Piece *> pieces;
    for (const BoundStateNode *node = pieceStates; node != nullptr;
         node = node->parent) {
      pieces.push_back(node->piece);
    }
    std::reverse(pieces.begin(), pieces.end());

    for (const Piece *piece : pieces) {
      State thisState = pieceState(piece);
      State otherState = other.pieceState(piece);
      if (thisState != otherState)
        return thisState > otherState;
    }

    return false;
  }

private:
  /* The arena where bound state nodes are allocated. Owned by the
   * SolutionCache so that nodes survive for the entire format operation. */
  std::deque<BoundStateNode> *arena;

  /* The root piece of the tree this solution is formatting. */
  const Piece *root;

  /* Creates a new solution that inherits all of the bindings of this one,
   * used when expanding. */
  Solution(std::deque<BoundStateNode> *arena, const Piece *root, int cost,
           const BoundStateNode *pieceStates,
           std::unordered_map<const Piece *, std::vector<State>> allowedStates)
      : arena(arena), root(root), cost(cost), pieceStates(pieceStates),
        allowedStates(std::move(allowedStates)) {}

  /* Attempts to add a binding from [piece] to [state] to the solution, and
   * then adds any further bindings from constraints that [piece] applies to
   * its children, recursively.
   *
   * This may invalidate the solution if [piece] is already bound to a
   * different [state], or if any constrained pieces are bound to different
   * states. */
  void bind(const Piece *piece, State state) {
    /* If we've already failed from a previous violation, early out. */
    if (isDeadEnd)
      return;

    /* Apply the new binding if it doesn't conflict with an existing one. */
    const State *alreadyBound = pieceStateIfBound(piece);
    if (alreadyBound == nullptr) {
      /* Binding an unbound piece to a state. */
      cost += piece->stateCost(state);
      arena->push_back({piece, state, pieceStates});
      pieceStates = &arena->back();

      /* This piece may in turn place further constraints on others. */
      piece->applyConstraints(state, [this](const Piece *other, State cs) {
        bind(other, cs);
      });

      /* If this piece's state prevents some of its children from having
       * newlines, then further constrain those children. */
      if (!isDeadEnd) {
        piece->forEachChild([&](const Piece *child) {
          /* Stop as soon as we fail. */
          if (isDeadEnd)
            return;

          /* If the child can't have newlines in any shape, then constrain it.
           */
          uint8_t allowedShapes =
              piece->allowedChildShapes(state, child);
          if (allowedShapes == ShapeMask::OnlyInline) {
            constrainOffspring(child);
          }
        });
      }
    } else if (*alreadyBound != state) {
      /* Already bound to a different state, so there's a conflict. */
      isDeadEnd = true;
      isValid = false;
    }
    /* Otherwise, already bound to the same state, so nothing to do. */
  }

  /* For [piece] and its transitive offspring subtree, eliminate any state that
   * will always produce a newline since that state is not permitted because
   * the parent of [piece] doesn't allow [piece] to have any newlines. */
  void constrainOffspring(const Piece *piece) {
    for (const Piece *offspring : piece->statefulOffspring()) {
      if (isDeadEnd)
        break;

      if (const State *boundState = pieceStateIfBound(offspring)) {
        /* This offspring is already pinned or bound to a state. If that state
         * will emit newlines, then this solution is invalid. */
        if (offspring->containsNewline(*boundState)) {
          isDeadEnd = true;
          isValid = false;
        }
      } else if (allowedStates.find(offspring) == allowedStates.end()) {
        /* If we get here, the offspring isn't bound to a state and we haven't
         * already constrained it. Eliminate any of its states that will emit
         * newlines. */
        bool allowedUnsplit = !offspring->containsNewline(State::Unsplit);

        std::vector<State> states = offspring->additionalStates();
        std::vector<State> remainingStates;
        for (const State &state : states) {
          if (!offspring->containsNewline(state)) {
            remainingStates.push_back(state);
          }
        }

        if (!allowedUnsplit && remainingStates.empty()) {
          /* There is no state this child can take that won't emit newlines,
           * and it's not allowed to, so this solution is bad. */
          isDeadEnd = true;
          isValid = false;
        } else if (remainingStates.empty()) {
          /* The only valid state is unsplit so bind it to that. */
          bind(offspring, State::Unsplit);
        } else if (!allowedUnsplit && remainingStates.size() == 1) {
          /* There's only one valid state, so bind it to that. */
          bind(offspring, remainingStates.front());
        } else if (remainingStates.size() < states.size()) {
          /* There are some constrained states, so keep the remaining ones. */
          allowedStates[offspring] = remainingStates;
        }
      }
    }
  }
};

} // namespace utopia
