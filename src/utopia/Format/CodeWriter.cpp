#include <utopia/Format/CodeWriter.hpp>
#include <utopia/Format/Piece.hpp>

namespace utopia {
void CodeWriter::recordPotentialOverflow() {
  bool canBeSplit = false;
  for (const Piece *p : currentLinePieces) {
    if (boundStates && boundStates->find(p) != boundStates->end()) {
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