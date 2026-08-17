#pragma once
#include "utopia/AST/AST.hpp"
#include "utopia/Format/PieceFactory.hpp"
#include "utopia/Format/Solver.hpp"

namespace utopia {

class Formatter {
public:
  static std::string format(const ASTNode *astRoot, int pageWidth = 80) {
    PieceFactory factory(pageWidth);
    const Piece *rootPiece = factory.dispatch(astRoot);

    if (!rootPiece) {
      return "";
    }

    SolutionCache cache;
    Solver solver(cache, pageWidth, 0, 0);
    Solution optimal = solver.format(rootPiece);

    std::string output = optimal.code;

    /* Be a good citizen, end with a newline. */
    if (!output.empty() && output.back() != '\n') {
      output += '\n';
    }

    return output;
  }
};

} // namespace utopia
