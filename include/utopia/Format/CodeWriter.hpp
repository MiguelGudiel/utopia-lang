#pragma once
#include "utopia/Format/FormatCore.hpp"
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace utopia {

class Piece;

/*
 * Persistent immutable linked list node representing a formatting state bound
 * to a specific Piece.
 */
struct BoundStateNode {
  const Piece *piece;
  State state;
  const BoundStateNode *parent;
};

class CodeWriter {
public:
  explicit CodeWriter(int pageWidth = 80, int baseIndent = 0,
                      bool measureOnly = false,
                      const BoundStateNode *boundStates = nullptr)
      : pageWidth(pageWidth), baseIndent(baseIndent), currentIndent(baseIndent),
        currentColumn(baseIndent), measureOnly(measureOnly),
        boundStates(boundStates) {}

  void pushPiece(const Piece *p) { activePieces.push_back(p); }
  void popPiece() { activePieces.pop_back(); }

  void space() {
    pendingWhitespace = std::max(pendingWhitespace, Whitespace::Space);
  }

  void newline() {
    pendingWhitespace = std::max(pendingWhitespace, Whitespace::Newline);
  }

  void blankLine() {
    pendingWhitespace = std::max(pendingWhitespace, Whitespace::BlankLine);
  }

  void splitIf(bool condition) {
    if (condition)
      newline();
    else
      space();
  }

  int getCurrentLine() const { return currentLine; }
  int getCurrentIndent() const { return currentIndent; }

  void markInvalid() { isValid = false; }

  void writeString(const std::string &text) { _writeInternal(text, true); }

  void write(const std::string &text) { _writeInternal(text, false); }

  void writeComment(const std::string &text) {
    if (text.empty())
      return;

    flushWhitespace();

    size_t start = 0;
    while (start < text.length()) {
      size_t nextNewline = text.find('\n', start);
      size_t len = (nextNewline != std::string::npos) ? (nextNewline - start)
                                                      : (text.length() - start);
      std::string chunk = text.substr(start, len);

      if (!measureOnly) {
        buffer += chunk;
      }

      currentColumn += chunk.length();

      for (const Piece *p : activePieces) {
        if (std::find(currentLinePieces.begin(), currentLinePieces.end(), p) ==
            currentLinePieces.end()) {
          currentLinePieces.push_back(p);
        }
      }
      forgiveOverflow = true;

      if (nextNewline != std::string::npos) {
        currentLinePieces.clear();
        currentColumn = 0;

        start = nextNewline + 1;

        /*
         * Determine if the next line is an independent comment joined by the
         * parser rather than an internal line of a block comment. Joined
         * comments start exactly with '//' or '/*' without leading spaces.
         */
        bool isJoinedComment = false;
        if (start + 1 < text.length() && text[start] == '/' &&
            (text[start + 1] == '/' || text[start + 1] == '*')) {
          isJoinedComment = true;
        }

        if (!measureOnly) {
          buffer += "\n";
          if (isJoinedComment) {
            buffer.append(currentIndent, ' ');
          }
        }

        if (isJoinedComment) {
          currentColumn += currentIndent;
        }

        pendingWhitespace = Whitespace::None;
        forgiveOverflow = false;
        currentLine++;
      } else {
        break;
      }
    }
  }

  /**
   * Directly injects pre-solved formatting text while properly adjusting
   * internal line and column tracking metadata without re-triggering formatting
   * logic.
   */
  void writePreformatted(const std::string &text) {
    if (text.empty())
      return;

    flushWhitespace();

    if (!measureOnly) {
      buffer += text;
    }

    size_t lastNewline = text.find_last_of('\n');
    if (lastNewline != std::string::npos) {
      int newlines = 0;
      for (char c : text) {
        if (c == '\n')
          newlines++;
      }
      currentLine += newlines;
      currentColumn = text.length() - lastNewline - 1;
    } else {
      currentColumn += text.length();
    }

    if (currentColumn > pageWidth && !forgiveOverflow && !hasOverflowed) {
      recordPotentialOverflow();
    }
  }

  void _writeInternal(const std::string &text, bool isStringLiteral) {
    if (text.empty())
      return;

    size_t start = 0;
    while (start < text.length()) {
      size_t nextNewline = text.find('\n', start);
      size_t len = (nextNewline != std::string::npos) ? (nextNewline - start)
                                                      : std::string::npos;
      std::string chunk = text.substr(start, len);

      if (!chunk.empty() || nextNewline == std::string::npos) {
        flushWhitespace();

        if (!measureOnly) {
          buffer += chunk;
        }

        for (const Piece *p : activePieces) {
          if (std::find(currentLinePieces.begin(), currentLinePieces.end(),
                        p) == currentLinePieces.end()) {
            currentLinePieces.push_back(p);
          }
        }
        currentColumn += chunk.length();
        if (isStringLiteral) {
          forgiveOverflow = true;
        }

        if (currentColumn > pageWidth && !forgiveOverflow && !hasOverflowed) {
          recordPotentialOverflow();
        }
      }

      if (nextNewline != std::string::npos) {
        if (currentColumn > pageWidth && !forgiveOverflow) {
          totalOverflow += (currentColumn - pageWidth);
          if (!hasOverflowed) {
            recordPotentialOverflow();
          }
        }
        currentLinePieces.clear();
        currentColumn = 0;
        if (!measureOnly) {
          buffer += "\n";
          buffer.append(currentIndent, ' ');
        }
        currentColumn += currentIndent;
        pendingWhitespace = Whitespace::None;
        forgiveOverflow = false;
        currentLine++;
        start = nextNewline + 1;
      } else {
        break;
      }
    }
  }

  void finish() {
    flushWhitespace();
    if (currentColumn > pageWidth && !forgiveOverflow) {
      totalOverflow += (currentColumn - pageWidth);
      if (!hasOverflowed) {
        recordPotentialOverflow();
      }
    }
  }

  void pushIndent(Indent indent) {
    indentStack.push_back(indent);
    recalculateIndent();
  }

  void popIndent() {
    if (!indentStack.empty()) {
      indentStack.pop_back();
      recalculateIndent();
    }
  }

  void exactNewlines(int count) {
    if (count > pendingNewlines) {
      pendingNewlines = count;
    }
  }

  int getOverflow() const {
    int overflow = totalOverflow;
    if (currentColumn > pageWidth && !forgiveOverflow) {
      overflow += (currentColumn - pageWidth);
    }
    return overflow;
  }

  std::vector<const Piece *> getFirstOverflowPieces() const {
    return firstOverflowPieces;
  }

  std::string getOutput() const { return buffer; }

  bool isValid = true;

private:
  int pageWidth;
  int baseIndent;
  int currentIndent;
  int currentColumn = 0;
  int totalOverflow = 0;
  int currentLine = 0;
  int pendingNewlines = 0;
  bool measureOnly;
  bool forgiveOverflow = false;

  Whitespace pendingWhitespace = Whitespace::None;
  std::string buffer;
  std::vector<Indent> indentStack;

  std::vector<const Piece *> activePieces;
  std::vector<const Piece *> currentLinePieces;
  std::vector<const Piece *> firstOverflowPieces;
  bool hasOverflowed = false;

  const BoundStateNode *boundStates;

  void recordPotentialOverflow();

  void flushWhitespace() {
    if (pendingWhitespace == Whitespace::None && pendingNewlines == 0)
      return;

    if (pendingWhitespace == Whitespace::Space && pendingNewlines == 0) {
      currentColumn += 1;
      if (!measureOnly)
        buffer += " ";
    } else {
      if (currentColumn > pageWidth && !forgiveOverflow) {
        totalOverflow += (currentColumn - pageWidth);
        if (!hasOverflowed) {
          recordPotentialOverflow();
        }
      }

      currentLinePieces.clear();
      currentColumn = 0;

      int defaultLines = (pendingWhitespace == Whitespace::BlankLine) ? 2 : 1;
      int lines = std::max(pendingNewlines, defaultLines);

      if (!measureOnly) {
        buffer += std::string(lines, '\n');
        currentLine += lines;
        buffer.append(currentIndent, ' ');
      } else {
        currentLine += lines;
      }
      currentColumn += currentIndent;
    }
    pendingWhitespace = Whitespace::None;
    pendingNewlines = 0;
    forgiveOverflow = false;
  }

  void recalculateIndent() {
    /* Restore to baseIndent context instead of absolute 0 */
    currentIndent = baseIndent;
    Indent lastSignificant = Indent::None;

    for (Indent ind : indentStack) {
      if (ind == Indent::Infix && lastSignificant == Indent::Assignment) {
        continue;
      }
      if (ind == Indent::ControlFlowClause &&
          (lastSignificant == Indent::Expression ||
           lastSignificant == Indent::Infix)) {
        continue;
      }

      currentIndent += static_cast<int>(ind);
      if (ind != Indent::None && ind != Indent::Grouping) {
        lastSignificant = ind;
      }
    }
  }
};

} // namespace utopia