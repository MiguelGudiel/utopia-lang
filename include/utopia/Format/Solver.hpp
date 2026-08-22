#pragma once
#include "utopia/Format/SolutionCache.hpp"
#include <queue>

namespace utopia {

/* Selects states for each piece in a tree of pieces to find the best set of
 * line splits that minimizes overflow characters and line splitting costs.
 *
 * This problem is combinatorial over the number of pieces and each of their
 * possible states, so it isn't feasible to brute force. There are a few
 * techniques used to avoid that:
 *
 * -   The initial state for each piece has no line splits or only mandatory
 *     ones. Thus, it tries solutions with a minimum number of line splits
 *     first.
 *
 * -   Solutions are explored in priority order, lowest cost first. The first
 *     solution with no overflow characters is the best one, so exploration
 *     stops there.
 *
 * -   When selecting states to expand, only pieces in the first line
 *     containing overflow characters or invalid newlines are considered.
 *
 * -   If a subtree Piece is sufficiently isolated from surrounding content
 *     (usually this means it is on its own line), the subtree is hoisted
 *     out, formatted with a separate Solver, and the result inserted into
 *     the Solution. The result is memoized and reused across different
 *     Solutions, so the Piece tree is divided and solved in parts while
 *     work is shared between solutions. */
class Solver {
public:
  /* The solver is capped at a fixed number of attempts so it cannot go
   * pathological on giant code; if the optimal solution is not found in
   * time, the best one so far is used. */
  static constexpr int MaxAttempts = 10000;

  Solver(SolutionCache &cache, int pageWidth, int leadingIndent = 0,
         int subsequentIndent = -1)
      : cache(cache), pageWidth(pageWidth), leadingIndent(leadingIndent),
        subsequentIndent(subsequentIndent >= 0 ? subsequentIndent
                                               : leadingIndent) {}

  /* Finds the best set of line splits for [root] piece and returns the
   * resulting formatted code.
   *
   * If [rootState] is given, then [root] is bound to that state. */
  Solution format(const Piece *root, const State *rootState = nullptr) {
    Solution solution(&cache.arena, root, 0, nullptr, const_cast<State *>(rootState));
    solution.format(cache, pageWidth, leadingIndent, subsequentIndent);

    queue.push(solution);

    /* The lowest cost solution found so far that does overflow. */
    Solution best = solution;

    int attempts = 0;

    while (!queue.empty() && attempts < MaxAttempts) {
      Solution current = std::move(queue.top());
      queue.pop();

      attempts++;

      if (current.isValid) {
        /* Since we process the solutions from lowest cost up, as soon as we
         * find a valid one that fits, it's the best. */
        if (current.overflow == 0) {
          best = std::move(current);
          break;
        }

        /* If not, keep track of the least-bad one we've found so far. */
        if (!best.isValid || current.overflow < best.overflow) {
          best = std::move(current);
        }
      }

      /* Otherwise, try to expand the solution to explore different splitting
       * options. */
      std::vector<Solution> expanded =
          current.expand(cache, pageWidth, leadingIndent, subsequentIndent);
      for (auto &e : expanded) {
        queue.push(std::move(e));
      }
    }

    /* If we didn't find a solution without overflow, pick the least bad one.
     */
    return best;
  }

private:
  struct SolutionCompare {
    bool operator()(const Solution &a, const Solution &b) const {
      return a.isWorseThan(b);
    }
  };

  SolutionCache &cache;
  int pageWidth;
  int leadingIndent;
  int subsequentIndent;

  std::priority_queue<Solution, std::vector<Solution>, SolutionCompare> queue;
};

} // namespace utopia
