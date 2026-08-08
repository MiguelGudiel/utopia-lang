#include <utopia/Format/CodeWriter.hpp>
#include <utopia/Format/Piece.hpp>

namespace utopia {
void CodeWriter::recordPotentialOverflow() {
  bool canBeSplit = false;
  for (const Piece *p : currentLinePieces) {
    bool found = false;
    for (const BoundStateNode *n = boundStates; n != nullptr; n = n->parent) {
      if (n->piece == p) {
        found = true;
        break;
      }
    }
    if (found) {
      continue;
    }
    if (!p->additionalStates().empty()) {
      canBeSplit = true;
      break;
    }
  }
  if (canBeSplit) {
    hasOverflowed = true;
    firstOverflowPieces = currentLinePieces;
  }
}
} // namespace utopia