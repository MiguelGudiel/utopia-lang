#pragma once
#include "utopia/AST/AST.hpp"
#include "utopia/Format/PieceFactory.hpp"
#include "utopia/Format/Solver.hpp"

namespace utopia {

class Formatter {
public:
  static std::string format(const ASTNode *astRoot, int pageWidth = 80) {
    PieceFactory factory(pageWidth);
    Piece *rootPiece = factory.dispatch(astRoot);

    if (!rootPiece) {
      return "";
    }

    Solver solver;
    Solution optimal = solver.solve(rootPiece, pageWidth, 0);

    /* Perform the actual string generation once the optimal bound states are
     * found */
    CodeWriter writer(pageWidth, 0, false);
    std::function<void(const Piece *, State)> formatTree =
        [&](const Piece *p, State inheritedState) {
          State s = inheritedState;
          for (const BoundStateNode *n = optimal.boundStates; n != nullptr;
               n = n->parent) {
            if (n->piece == p) {
              s = n->state;
              break;
            }
          }
          p->format(writer, s, formatTree);
        };

    formatTree(rootPiece, State::Unsplit);
    return writer.getOutput();
  }
};

} // namespace utopia