#pragma once
#include "utopia/Format/FormatCore.hpp"
#include <string>
#include <vector>

namespace utopia {

class Piece;
class Solution;
class SolutionCache;

/* Persistent immutable linked list node representing a formatting state bound
 * to a specific Piece. Nodes live in an arena owned by the SolutionCache so
 * that all Solutions (and Solvers) for a format operation can share them. */
struct BoundStateNode {
  const Piece *piece;
  State state;
  const BoundStateNode *parent;
};

/* A level of indentation in the indentation stack. */
struct IndentLevel {
  /* The reason this indentation was added. */
  Indent type;

  /* The total number of spaces of indentation. */
  int spaces;

  IndentLevel(Indent type, int spaces) : type(type), spaces(spaces) {}
};

/* Information for each piece currently being formatted while CodeWriter
 * traverses the piece tree. */
struct FormatState {
  /* The piece being formatted. */
  const Piece *piece;

  /* The piece's shape. This changes based on the newlines the piece writes. */
  Shape shape = Shape::Inline;

  /* How a newline affects the shape of this piece. */
  ShapeMode mode = ShapeMode::Merge;

  FormatState(const Piece *piece) : piece(piece) {}
};

/* The interface used by Pieces to output formatted code.
 *
 * The back-end lowers the tree of pieces to the final formatted code by
 * allowing each piece to produce the output for the code it represents. */
class CodeWriter {
public:
  /* [leadingIndent] is the number of spaces of leading indentation at the
   * beginning of the first line and [subsequentIndent] is the indentation of
   * each line after that, independent of indentation created by pieces being
   * written. */
  CodeWriter(int pageWidth, int leadingIndent, int subsequentIndent,
             SolutionCache &cache, Solution &solution);

  /* Returns the final formatted code and the next pieces that can be expanded
   * from the solution this CodeWriter is writing, if any. */
  std::pair<std::string, std::vector<const Piece *>> finish();

  /* Appends [text] to the output.
   *
   * If [soft] is `true`, then [text] is considered to be "soft" code: string
   * literals, comments, etc. When an overflowing line of code ends in soft
   * characters, the overflow cost of all of those characters is collapsed to a
   * single point of penalty. */
  void write(const std::string &text, bool soft = false);

  /* Writes a comment. Internal newlines re-indent joined comment lines (`//`,
   * `/*` at the start of the line) but leave block comment internals flush
   * left. */
  void writeComment(const std::string &text);

  /* Directly injects pre-solved formatting text from a separately formatted
   * subtree. */
  void writePreformatted(const std::string &text);

  /* Increases the indentation by [indent] relative to the current amount of
   * indentation. */
  void pushIndent(Indent indent);

  /* Discards the indentation change from the last call to [pushIndent]. */
  void popIndent();

  /* Inserts a newline if [condition] is true.
   *
   * If [space] is `true` and [condition] is `false`, writes a space. If [blank]
   * is `true`, writes an extra newline to produce a blank line. */
  void splitIf(bool condition, bool space = true, bool blank = false);

  /* Writes a single space to the output. */
  void space() { whitespace(Whitespace::Space); }

  /* Inserts a line split in the output.
   *
   * If [blank] is `true`, writes an extra newline to produce a blank line. If
   * [flushLeft] is `true`, then the new line begins at column 1 and ignores any
   * surrounding indentation. Used for multi-line block comments and multi-line
   * strings. */
  void newline(bool blank = false, bool flushLeft = false) {
    whitespace(blank ? Whitespace::BlankLine : Whitespace::Newline,
               flushLeft);
  }

  /* Queues [whitespace] to be written to the output.
   *
   * If [flushLeft] is `true`, then the new line begins at column 1 and ignores
   * any surrounding indentation. */
  void whitespace(Whitespace whitespace, bool flushLeft = false);

  /* When a newline is written by the current piece or one of its children,
   * determines how that affects the current piece's shape. */
  void setShapeMode(ShapeMode mode);

  /* Format [piece] and insert the result into the code being written.
   *
   * If [separate] is `true`, then [piece] is formatted and solved using a
   * separate Solver and the result inserted into this CodeWriter's Solution.
   * It's only safe to pass [separate] when the piece's formatting depends only
   * on its starting indentation and state. */
  void format(const Piece *piece, bool separate = false);

  /* The number of spaces of leading indentation of the line currently being
   * written. */
  int getCurrentIndent() const { return indentStack.empty() ? 0 : indentStack.back().spaces; }

  /* The whitespace queued but not yet written (used by formatSeparate to
   * carry a subtree's trailing whitespace into the parent writer). */
  Whitespace pendingWhitespaceValue() const { return pendingWhitespace; }

private:
  int pageWidth;

  /* Previously cached formatted subtrees. */
  SolutionCache &cache;

  /* The solution this CodeWriter is generating code for. */
  Solution &solution;

  /* The code being written. */
  std::string code;

  /* What whitespace should be written before the next non-whitespace text. */
  Whitespace pendingWhitespace = Whitespace::None;

  /* The number of spaces of indentation that should begin the next line when
   * [pendingWhitespace] is a newline or blank line. */
  int pendingIndent = 0;

  /* The indentation to write before the next text, if a newline was just
   * flushed. -1 means none is pending. */
  int pendingIndentToWrite = -1;

  /* The number of characters in the line currently being written. */
  int column = 0;

  /* The number of characters at the end of the current line that are "soft". */
  int softCharacters = 0;

  /* The stack of indentation levels. */
  std::vector<IndentLevel> indentStack;

  /* The stack of information for each piece currently being formatted. */
  std::vector<FormatState> pieceFormats;

  /* Whether we have already found the first line whose pieces should be used
   * to expand further solutions. */
  bool foundExpandLine = false;

  /* The solvable pieces on the first overflowing or invalid line, if we've
   * found any. */
  std::vector<const Piece *> expandPieces;

  /* The stack of solvable pieces currently being formatted. */
  std::vector<const Piece *> currentUnsolvedPieces;

  /* The set of unsolved pieces that were being formatted when text was written
   * to the current line. */
  std::vector<const Piece *> currentLinePieces;

  /* Whether any text has been written yet (for the leading indent). */
  bool started = false;

  /* The number of spaces of leading indentation of the first line. */
  int leadingIndent;

  void flushWhitespace();
  void finishLine();

  /* Format [piece] using a separate Solver and merge the result into this
   * writer's solution. */
  void formatSeparate(const Piece *piece);

  /* Format [piece] writing directly into this CodeWriter. */
  void formatInline(const Piece *piece);

  /* Determine how a newline affects the current piece's shape. */
  void applyNewlineToShape(FormatState &state, Shape shape = Shape::Other);
};

} // namespace utopia
