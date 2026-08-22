#pragma once
#include "utopia/Format/Solution.hpp"
#include <deque>
#include <unordered_map>

namespace utopia {

class Solver;

/* Maintains a cache of Piece subtrees that have been previously solved.
 *
 * If a given Piece has newlines before and after it, then (in most cases,
 * assuming there are no other constraints) the way it is formatted only depends
 * on its leading indentation. In that case, the piece can be formatted with
 * a separate Solver and the result inserted into any Solution that has that
 * piece at that leading indentation.
 *
 * This cache stores those previously formatted subtree pieces so that
 * CodeWriter can reuse them across Solutions. The cache is shared across all
 * Solvers and Solutions for an entire format operation. */
class SolutionCache {
public:
  /* The key used to uniquely identify a previously formatted Piece.
   *
   * Each subtree solution depends only on the Piece and the amount of leading
   * indentation in the context where it appears (which may vary based on how
   * surrounding pieces end up splitting). */
  struct Key {
    const Piece *piece;
    int indent;
    int subsequentIndent;

    bool operator==(const Key &other) const {
      return piece == other.piece && indent == other.indent &&
             subsequentIndent == other.subsequentIndent;
    }
  };

  struct KeyHash {
    size_t operator()(const Key &key) const {
      size_t h1 = std::hash<const void *>()(key.piece);
      size_t h2 = std::hash<int>()(key.indent);
      size_t h3 = std::hash<int>()(key.subsequentIndent);
      return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
  };

  /* The arena where all bound state nodes are allocated. Shared by every
   * Solution so that nodes remain valid for the entire format operation. */
  std::deque<BoundStateNode> arena;

  /* Returns a previously cached solution for formatting [root] with leading
   * [indent] (and [subsequentIndent] for lines after the first) or produces a
   * new solution, caches it, and returns it.
   *
   * If [root] is already bound to a state in the surrounding piece tree's
   * Solution, then [stateIfBound] is that state. Otherwise, it is treated as
   * unbound and the cache will find a state for [root] as well as its
   * children. */
  const Solution &find(const Piece *root, const State *stateIfBound,
                       int pageWidth, int indent, int subsequentIndent);

private:
  std::unordered_map<Key, Solution, KeyHash> cache;
};

} // namespace utopia
