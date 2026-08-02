#pragma once
#include "utopia/AST/AST.hpp"
#include "utopia/Format/PieceFactory.hpp"
#include "utopia/Format/Solver.hpp"

namespace utopia {

class Formatter {
public:
  static std::string format(const ASTNode *astRoot, int pageWidth = 80) {
    PieceFactory factory;
    Piece *rootPiece = factory.dispatch(astRoot);

    if (!rootPiece) {
      return "";
    }

    Solver solver;
    Solution optimal = solver.solve(rootPiece, pageWidth);

    /* Perform the actual string generation once the optimal bound states are
     * found */
    CodeWriter writer(pageWidth, 0, false);
    std::function<void(const Piece *, State)> formatTree =
        [&](const Piece *p, State inheritedState) {
          State s = inheritedState;
          if (optimal.boundStates.find(p) != optimal.boundStates.end()) {
            s = optimal.boundStates[p];
          }
          p->format(writer, s, formatTree);
        };

    formatTree(rootPiece, State::Unsplit);
    return writer.getOutput();
  }
};

} // namespace utopia