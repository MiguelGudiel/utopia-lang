#include <utopia/Format/CodeWriter.hpp>
#include <utopia/Format/Piece.hpp>
#include <utopia/Format/Solution.hpp>
#include <utopia/Format/SolutionCache.hpp>
#include <algorithm>

namespace utopia {

CodeWriter::CodeWriter(int pageWidth, int leadingIndent, int subsequentIndent,
                       SolutionCache &cache, Solution &solution)
    : pageWidth(pageWidth), cache(cache), solution(solution),
      leadingIndent(leadingIndent) {
  indentStack.push_back(IndentLevel(Indent::None, leadingIndent));

  /* Track the leading indent before the first line. */
  pendingIndent = leadingIndent;
  column = pendingIndent;

  /* If there is additional indentation on subsequent lines, then push that
   * onto the stack. When the first newline is written, pendingIndent will pick
   * this up and use it for subsequent lines. */
  if (subsequentIndent > leadingIndent) {
    indentStack.push_back(IndentLevel(Indent::None, subsequentIndent));
  }
}

std::pair<std::string, std::vector<const Piece *>> CodeWriter::finish() {
  finishLine();

  return {std::move(code), std::move(expandPieces)};
}

void CodeWriter::write(const std::string &text, bool soft) {
  if (text.empty())
    return;

  flushWhitespace();

  /* Write the leading indentation for the very first line of this writer's
   * output, if any. If a newline was just flushed, its indentation takes
   * precedence. */
  if (!started) {
    started = true;
    if (pendingIndentToWrite < 0 && leadingIndent > 0) {
      code.append(leadingIndent, ' ');
    }
  }

  if (pendingIndentToWrite >= 0) {
    code.append(pendingIndentToWrite, ' ');
    pendingIndentToWrite = -1;
  }

  code += text;
  column += static_cast<int>(text.length());

  if (soft) {
    softCharacters += static_cast<int>(text.length());
  } else {
    /* We only count trailing soft characters at the end of the line, so
     * writing any non-soft code resets the count. */
    softCharacters = 0;
  }

  /* If we haven't found an overflowing line yet, then this line might be one
   * so keep track of the unsolved pieces we've encountered on it. */
  if (!foundExpandLine) {
    for (const Piece *p : currentUnsolvedPieces) {
      if (std::find(currentLinePieces.begin(), currentLinePieces.end(), p) ==
          currentLinePieces.end()) {
        currentLinePieces.push_back(p);
      }
    }
  }
}

void CodeWriter::writeComment(const std::string &text) {
  if (text.empty())
    return;

  size_t start = 0;
  while (start < text.length()) {
    size_t nextNewline = text.find('\n', start);
    std::string chunk = (nextNewline != std::string::npos)
                            ? text.substr(start, nextNewline - start)
                            : text.substr(start);

    write(chunk, /*soft=*/true);

    if (nextNewline == std::string::npos)
      break;

    /* Determine if the next line is an independent comment joined by the
     * parser rather than an internal line of a block comment. Joined comments
     * start exactly with '//' or '/*' without leading spaces. */
    bool isJoinedComment = false;
    if (nextNewline + 1 < text.length() && text[nextNewline + 1] == '/' &&
        (text[nextNewline + 2] == '/' || text[nextNewline + 2] == '*')) {
      isJoinedComment = true;
    }

    newline(/*blank=*/false, /*flushLeft=*/!isJoinedComment);
    start = nextNewline + 1;
  }
}

void CodeWriter::writePreformatted(const std::string &text) {
  if (text.empty())
    return;

  flushWhitespace();

  if (pendingIndentToWrite >= 0) {
    /* The preformatted text carries its own leading indentation. */
    pendingIndentToWrite = -1;
  }

  if (!started) {
    started = true;
  }

  code += text;

  size_t lastNewline = text.find_last_of('\n');
  if (lastNewline != std::string::npos) {
    int newlines = 0;
    for (char c : text) {
      if (c == '\n')
        newlines++;
    }
    column = static_cast<int>(text.length() - lastNewline - 1);
    softCharacters = 0;
  } else {
    column += static_cast<int>(text.length());
  }
}

void CodeWriter::pushIndent(Indent indent) {
  const auto &parent = indentStack.back();

  /* Combine the new indentation with the surrounding one. */
  int offset = indentSpaces(indent);

  /* On the right-hand side of `=`, `:`, or `=>`, don't indent subsequent
   * infix operands so that they all align. */
  if (parent.type == Indent::Assignment && indent == Indent::Infix) {
    offset = 0;
  }

  /* We have already indented the control flow header, so collapse the
   * duplicate indentation. */
  if (parent.type == Indent::ControlFlowClause &&
      (indent == Indent::Expression || indent == Indent::Infix)) {
    offset = 0;
  }

  indentStack.push_back(IndentLevel(indent, parent.spaces + offset));
}

void CodeWriter::popIndent() { indentStack.pop_back(); }

void CodeWriter::splitIf(bool condition, bool space, bool blank) {
  if (condition) {
    newline(blank);
  } else if (space) {
    this->space();
  }
}

void CodeWriter::whitespace(Whitespace whitespace, bool flushLeft) {
  if (whitespace == Whitespace::Newline || whitespace == Whitespace::BlankLine) {
    applyNewlineToShape(pieceFormats.back());
    pendingIndent = flushLeft ? 0 : indentStack.back().spaces;
  }

  pendingWhitespace = pendingWhitespace.collapse(whitespace);
}

void CodeWriter::setShapeMode(ShapeMode mode) {
  pieceFormats.back().mode = mode;
}

void CodeWriter::format(const Piece *piece, bool separate) {
  if (separate) {
    formatSeparate(piece);
  } else {
    formatInline(piece);
  }
}

void CodeWriter::formatSeparate(const Piece *piece) {
  const Solution &subtreeSolution = cache.find(
      piece, solution.pieceStateIfBound(piece), pageWidth, pendingIndent,
      indentStack.back().spaces);

  pendingIndent = 0;
  flushWhitespace();

  solution.mergeSubtree(subtreeSolution);
  writePreformatted(subtreeSolution.getCode());
}

void CodeWriter::formatInline(const Piece *piece) {
  bool isUnsolved =
      !solution.isBound(piece) && !piece->additionalStates().empty();

  /* See if we can immediately bind it based on the page width and the piece's
   * contents. If so, do that now. Doing it here lets us take leading
   * indentation into account which may vary based on the surrounding pieces
   * when we get here. */
  if (isUnsolved) {
    isUnsolved = !solution.tryBindByPageWidth(piece,
                                              pageWidth - leadingIndent);
  }

  if (isUnsolved)
    currentUnsolvedPieces.push_back(piece);

  /* Begin a new formatting context for this child. */
  pieceFormats.push_back(FormatState(piece));

  /* Format the child piece. */
  piece->format(*this, solution.pieceState(piece));

  FormatState child = pieceFormats.back();
  pieceFormats.pop_back();

  /* Restore the surrounding piece's context. */
  if (isUnsolved)
    currentUnsolvedPieces.pop_back();

  /* Now that we know the child's shape, see if the parent permits it. */
  if (!pieceFormats.empty()) {
    FormatState &parent = pieceFormats.back();
    uint8_t allowedShapes = parent.piece->allowedChildShapes(
        solution.pieceState(parent.piece), child.piece);

    if (!(allowedShapes & (1 << (int)child.shape.value))) {
      solution.invalidate(parent.piece);
    }

    /* If the child had newlines, propagate that to the parent's shape. */
    if (child.shape != Shape::Inline) {
      applyNewlineToShape(parent, child.shape);
    }
  }
}

void CodeWriter::flushWhitespace() {
  switch (pendingWhitespace.value) {
  case Whitespace::None:
    break; /* Nothing to do. */

  case Whitespace::Newline:
  case Whitespace::BlankLine:
    finishLine();
    column = pendingIndent;
    softCharacters = 0;
    pendingIndentToWrite = pendingIndent;
    code += (pendingWhitespace == Whitespace::BlankLine) ? "\n\n" : "\n";
    break;

  case Whitespace::Space:
    code += " ";
    column++;
    softCharacters++;
    break;
  }

  pendingWhitespace = Whitespace::None;
}

void CodeWriter::finishLine() {
  /* If the completed line is too long, track the overflow. */
  if (column > pageWidth) {
    int overflow = column - pageWidth;

    /* If soft overflow is enabled, then collapse any trailing soft characters
     * to a single point of overflow. */
    if (softCharacters > 0) {
      if (softCharacters >= overflow) {
        /* All of the overflowing characters are soft. */
        overflow = 1;
      } else {
        /* The overflow contains both hard and soft overflow. Count each
         * character of hard overflow and collapse the soft overflow. */
        overflow = overflow - softCharacters + 1;
      }
    }

    solution.addOverflow(overflow);
  }

  /* If we found a problematic line, and there are pieces on the line that we
   * can try to split, then remember them so that the solution will expand
   * them next. */
  if (foundExpandLine)
    return;

  if (!currentLinePieces.empty() &&
      (column > pageWidth || !solution.isValid)) {
    expandPieces.insert(expandPieces.end(), currentLinePieces.begin(),
                        currentLinePieces.end());
    foundExpandLine = true;
  } else {
    /* This line was OK, so we don't need to expand the pieces on it. */
    currentLinePieces.clear();
  }
}

void CodeWriter::applyNewlineToShape(FormatState &state, Shape shape) {
  switch (state.mode) {
  case ShapeMode::Merge:
    state.shape = state.shape.merge(shape);
    break;
  case ShapeMode::Block:
    state.shape = Shape::Block;
    break;
  case ShapeMode::BeforeHeadline:
    state.shape = Shape::Other;
    break;
  case ShapeMode::AfterHeadline:
    /* If there were no newlines inside the headline, now that there is one,
     * we have a headline shape. */
    if (state.shape == Shape::Inline) {
      state.shape = Shape::Headline;
    }
    break;
  case ShapeMode::Other:
    state.shape = Shape::Other;
    break;
  }
}

} // namespace utopia
