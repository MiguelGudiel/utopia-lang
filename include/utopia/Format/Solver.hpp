#pragma once
#include "utopia/Format/Piece.hpp"
#include <queue>
#include <unordered_map>

namespace utopia {

struct Solution {
  std::unordered_map<const Piece *, State> boundStates;
  int cost = 0;
  int overflow = 0;
  bool isValid = true;

  bool operator>(const Solution &other) const {
    if (cost != other.cost)
      return cost > other.cost;
    return overflow > other.overflow;
  }
};

class Solver {
public:
  static constexpr int MaxAttempts = 10000;

  Solution solve(const Piece *root, int pageWidth = 80) {
    std::priority_queue<Solution, std::vector<Solution>, std::greater<Solution>>
        queue;

    Solution initial;
    std::vector<const Piece *> initialOverflow =
        evaluateSolution(initial, root, pageWidth);
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

      std::vector<const Piece *> overflowPieces =
          evaluateSolution(current, root, pageWidth);
      std::vector<const Piece *> unbound;

      for (const Piece *p : overflowPieces) {
        if (current.boundStates.find(p) == current.boundStates.end() &&
            !p->additionalStates().empty()) {
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
            nextOpt.boundStates[earlier] = State::Unsplit;
          }

          nextOpt.boundStates[p] = s;

          p->applyConstraints(s, [&](const Piece *child, State childState) {
            nextOpt.boundStates[child] = childState;
          });

          evaluateSolution(nextOpt, root, pageWidth);
          queue.push(nextOpt);
        }
      }
      attempts++;
    }

    return best;
  }

private:
  std::vector<const Piece *> evaluateSolution(Solution &sol, const Piece *root,
                                              int pageWidth) {
    CodeWriter writer(pageWidth, 0, true);
    sol.cost = 0;

    std::function<void(const Piece *, State)> formatTree =
        [&](const Piece *p, State inheritedState) {
          writer.pushPiece(p);
          State s = inheritedState;
          if (sol.boundStates.find(p) != sol.boundStates.end()) {
            s = sol.boundStates[p];
          }
          sol.cost += p->stateCost(s);
          p->format(writer, s, formatTree);
          writer.popPiece();
        };

    formatTree(root, State::Unsplit);
    writer.finish();

    sol.overflow = writer.getOverflow();
    sol.isValid = writer.isValid;
    return writer.getFirstOverflowPieces();
  }
};

} // namespace utopia