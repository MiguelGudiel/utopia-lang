#pragma once
#include "utopia/Format/Piece.hpp"
#include <optional>
#include <queue>

namespace utopia {

struct Solution {
  const BoundStateNode *boundStates = nullptr;
  int cost = 0;
  int overflow = 0;
  bool isValid = true;
  std::vector<const Piece *> overflowPieces;

  bool operator>(const Solution &other) const {
    if (cost != other.cost)
      return cost > other.cost;
    return overflow > other.overflow;
  }
};

class Solver {
public:
  static constexpr int MaxAttempts = 10000;

  Solution solve(const Piece *root, int pageWidth = 80, int baseIndent = 0) {
    nodeArena.clear();

    const BoundStateNode *pinnedStates = nullptr;

    auto pinPieces = [&](auto &self, const Piece *p) -> void {
      if (auto stateOpt = p->fixedState(pageWidth)) {
        nodeArena.push_back({p, *stateOpt, pinnedStates});
        pinnedStates = &nodeArena.back();
      }
      p->forEachChild([&](const Piece *child) { self(self, child); });
    };

    pinPieces(pinPieces, root);

    std::priority_queue<Solution, std::vector<Solution>, std::greater<Solution>>
        queue;

    Solution initial;
    initial.boundStates = pinnedStates;
    evaluateSolution(initial, root, pageWidth, baseIndent);
    queue.push(initial);

    Solution best = initial;
    int attempts = 0;

    while (!queue.empty() && attempts < MaxAttempts) {
      Solution current = queue.top();
      queue.pop();

      if (current.isValid && current.overflow == 0) {
        return current;
      }

      if (current.isValid && current.overflow < best.overflow) {
        best = current;
      } else if (current.isValid && current.overflow == best.overflow &&
                 current.cost < best.cost) {
        best = current;
      }

      std::vector<const Piece *> unbound;

      for (const Piece *p : current.overflowPieces) {
        bool found = false;
        for (const BoundStateNode *n = current.boundStates; n != nullptr;
             n = n->parent) {
          if (n->piece == p) {
            found = true;
            break;
          }
        }

        if (!found && !p->additionalStates().empty()) {
          unbound.push_back(p);
        }
      }

      for (const Piece *p : unbound) {
        std::vector<State> statesToExplore = p->additionalStates();

        for (const State &s : statesToExplore) {
          Solution nextOpt = current;

          for (const Piece *earlier : unbound) {
            if (earlier == p)
              break;
            nodeArena.push_back({earlier, State::Unsplit, nextOpt.boundStates});
            nextOpt.boundStates = &nodeArena.back();
          }

          nodeArena.push_back({p, s, nextOpt.boundStates});
          nextOpt.boundStates = &nodeArena.back();

          p->applyConstraints(s, [&](const Piece *child, State childState) {
            nodeArena.push_back({child, childState, nextOpt.boundStates});
            nextOpt.boundStates = &nodeArena.back();
          });

          evaluateSolution(nextOpt, root, pageWidth, baseIndent);
          queue.push(nextOpt);
        }
      }
      attempts++;
    }

    return best;
  }

private:
  /* Linear allocator mimicking a memory arena. References to elements inside
   * std::deque are guaranteed not to be invalidated upon push_back. */
  std::deque<BoundStateNode> nodeArena;

  void evaluateSolution(Solution &sol, const Piece *root, int pageWidth,
                        int baseIndent) {
    CodeWriter writer(pageWidth, baseIndent, true, sol.boundStates);
    sol.cost = 0;

    std::function<void(const Piece *, State)> formatTree =
        [&](const Piece *p, State inheritedState) {
          writer.pushPiece(p);
          State s = inheritedState;

          for (const BoundStateNode *n = sol.boundStates; n != nullptr;
               n = n->parent) {
            if (n->piece == p) {
              s = n->state;
              break;
            }
          }

          sol.cost += p->stateCost(s);
          p->format(writer, s, formatTree);
          writer.popPiece();
        };

    formatTree(root, State::Unsplit);
    writer.finish();

    sol.overflow = writer.getOverflow();
    sol.isValid = writer.isValid;
    sol.overflowPieces = writer.getFirstOverflowPieces();
  }
};

} // namespace utopia