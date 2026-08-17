#include "utopia/Format/Solution.hpp"
#include "utopia/Format/CodeWriter.hpp"
#include "utopia/Format/SolutionCache.hpp"
#include "utopia/Format/Solver.hpp"
#include <utility>

namespace utopia {

const Solution &SolutionCache::find(const Piece *root,
                                    const State *stateIfBound, int pageWidth,
                                    int indent, int subsequentIndent) {
  Key key{root, indent, subsequentIndent};

  auto it = cache.find(key);
  if (it != cache.end()) {
    return it->second;
  }

  Solver solver(*this, pageWidth, indent, subsequentIndent);
  Solution solution = solver.format(root, stateIfBound);

  auto [it2, inserted] = cache.emplace(key, std::move(solution));
  return it2->second;
}

void Solution::format(SolutionCache &cache, int pageWidth, int leadingIndent,
                      int subsequentIndent) {
  CodeWriter writer(pageWidth, leadingIndent, subsequentIndent, cache, *this);

  writer.format(root);

  auto [codeStr, pieces] = writer.finish();
  code = std::move(codeStr);
  expandPieces = std::move(pieces);
}

std::vector<Solution> Solution::expand(SolutionCache &cache, int pageWidth,
                                       int leadingIndent,
                                       int subsequentIndent) {
  /* If there is no piece that we can expand on this solution, it's a dead end
   * (or a winner). */
  if (expandPieces.empty())
    return {};

  std::vector<Solution> solutions;
  for (size_t i = 0; i < expandPieces.size(); i++) {
    /* For each non-default state that the expanding piece can be in, create a
     * new solution that inherits all of the bindings of this one, and binds
     * the expanding piece to that state (along with any further pieces
     * constrained by that one). */
    const Piece *expandPiece = expandPieces[i];

    auto allowedIt = allowedStates.find(expandPiece);
    const std::vector<State> &states =
        allowedIt != allowedStates.end() ? allowedIt->second
                                         : expandPiece->additionalStates();

    for (const State &state : states) {
      Solution expanded(arena, root, cost, pieceStates, allowedStates);

      /* Bind all preceding expand pieces to their unsplit state. Their other
       * states have already been expanded by earlier iterations of the outer
       * for loop. */
      bool valid = true;
      for (size_t j = 0; j < i; j++) {
        expanded.bind(expandPieces[j], State::Unsplit);
        if (expanded.isDeadEnd) {
          valid = false;
          break;
        }
      }

      /* Discard the solution if we hit a constraint violation. */
      if (!valid)
        continue;

      expanded.bind(expandPiece, state);

      /* Discard the solution if we hit a constraint violation. */
      if (!expanded.isDeadEnd) {
        expanded.format(cache, pageWidth, leadingIndent, subsequentIndent);

        /* We may not detect some newline violations until formatting. */
        if (!expanded.isDeadEnd) {
          solutions.push_back(std::move(expanded));
        }
      }
    }
  }

  return solutions;
}

} // namespace utopia
