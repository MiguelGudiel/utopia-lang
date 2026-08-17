#pragma once
#include "utopia/Format/CodeWriter.hpp"
#include "utopia/Format/FormatCore.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace utopia {

/* Bitmask helpers for the set of shapes a child may take. Mirrors dart_style's
 * `Set<Shape>`. */
namespace ShapeMask {
inline constexpr uint8_t Inline = 1 << 0;
inline constexpr uint8_t Block = 1 << 1;
inline constexpr uint8_t Headline = 1 << 2;
inline constexpr uint8_t Other = 1 << 3;
inline constexpr uint8_t All = Inline | Block | Headline | Other;
inline constexpr uint8_t OnlyInline = Inline;
inline constexpr uint8_t OnlyBlock = Block;
inline constexpr uint8_t InlineOrBlock = Inline | Block;

inline uint8_t anyIf(bool condition) {
  return condition ? All : OnlyInline;
}
} // namespace ShapeMask

/* Callback used by pieces to constrain the state of other pieces. */
using Constrain = std::function<void(const Piece *, State)>;

/* Base class for the formatter's internal representation used for line
 * splitting.
 *
 * We visit the source AST and convert it to a tree of Pieces. This tree
 * roughly follows the AST but includes comments and is optimized for
 * formatting and line splitting. The final output is then determined by
 * deciding which pieces split and how. */
class Piece {
protected:
  mutable int cachedTotalCharacters = -1;
  mutable bool cachedContainsHardNewline = false;

  void setMetadata(int chars, bool hn) const {
    cachedTotalCharacters = chars;
    cachedContainsHardNewline = hn;
  }

  /* The pinned state of this piece, if any. */
  mutable std::optional<State> pinnedState_;

public:
  virtual ~Piece() = default;

  void ensureMetadata() const {
    if (cachedTotalCharacters == -1) {
      computeMetadata();
    }
  }

  /* Whether this piece or any of its children contain an explicit mandatory
   * newline. Lazily computed and cached, so should only be accessed after all
   * of the piece's children are known. */
  bool containsHardNewline() const {
    ensureMetadata();
    return cachedContainsHardNewline;
  }

  /* The total number of characters of content in this piece and all of its
   * children. Lazily computed and cached. */
  int totalCharacters() const {
    ensureMetadata();
    return cachedTotalCharacters;
  }

  virtual void computeMetadata() const = 0;

  /* The ordered list of all possible ways this piece could split.
   *
   * Each piece determines what each State in the list represents, including
   * the automatically included State::Unsplit. The list returned by this
   * function should be sorted so that earlier states in the list compare less
   * than later states. */
  virtual std::vector<State> additionalStates() const { return {}; }

  /* If this piece has been pinned to a specific state, that state. */
  const State *pinnedState() const {
    return pinnedState_ ? &*pinnedState_ : nullptr;
  }

  /* Whether this piece contains a newline when this piece is in [state].
   *
   * This should only return `true` if the piece will *always* write at least
   * one newline -- either itself or one of its children -- when in this state.
   *
   * By default, we assume that any piece not in State::Unsplit or that has a
   * hard newline will contain a newline. */
  virtual bool containsNewline(State state) const {
    return state != State::Unsplit || containsHardNewline();
  }

  /* Given that this piece is in [state], use [writer] to produce its formatted
   * output. */
  virtual void format(CodeWriter &writer, State state) const = 0;

  /* Invokes [callback] on each piece contained in this piece. */
  virtual void forEachChild(const std::function<void(const Piece *)> &callback)
      const {}

  /* If the piece can determine that it will always end up in a certain state
   * given [pageWidth] and size metrics returned by calling
   * [containsHardNewline] and [totalCharacters] on its children, then returns
   * that State. This is purely an optimization: running the Solver without
   * ever calling this and pinning the resulting State should yield the same
   * formatting. */
  virtual std::optional<State> fixedStateForPageWidth(int pageWidth) const {
    return std::nullopt;
  }

  /* The cost that this piece should apply to the solution when in [state]. */
  virtual int stateCost(State state) const { return state.cost; }

  /* Apply any constraints that this piece places on other pieces when this
   * piece is bound to [state]. */
  virtual void applyConstraints(State state, const Constrain &constrain) const {
  }

  /* What shapes the [child] of this piece may take when this piece is in
   * [state]. */
  virtual uint8_t allowedChildShapes(State state, const Piece *child) const {
    return ShapeMask::All;
  }

  /* All of the transitive children of this piece (including the piece itself)
   * that have more than one state. Cached because it's faster than traversing
   * the child tree. */
  const std::vector<const Piece *> &statefulOffspring() const {
    if (!statefulOffspringComputed) {
      std::vector<const Piece *> result;
      std::function<void(const Piece *)> traverse = [&](const Piece *piece) {
        if (!piece->additionalStates().empty()) {
          result.push_back(piece);
        }
        piece->forEachChild(traverse);
      };
      traverse(this);
      cachedStatefulOffspring = std::move(result);
      statefulOffspringComputed = true;
    }
    return cachedStatefulOffspring;
  }

  /* Forces this piece to always use [state].
   *
   * If this piece's pinned state constrains any child pieces, pin those too,
   * recursively. */
  void pin(State state) const {
    if (pinnedState_)
      return;

    pinnedState_ = state;

    applyConstraints(state, [](const Piece *other, State constrainedState) {
      other->pin(constrainedState);
    });
  }

  /* Pin the piece to whatever state is needed to prevent it from splitting. */
  virtual void preventSplit() const { pin(State::Unsplit); }

  virtual const char *debugName() const { return "Piece"; }

private:
  mutable std::vector<const Piece *> cachedStatefulOffspring;
  mutable bool statefulOffspringComputed = false;
};

/* A simple atomic piece of code.
 *
 * This may represent a series of tokens where no split can occur between them.
 * It may also contain one or more comment lines (stored as separate lines so
 * that column calculation during line splitting calculates each line
 * separately). */
class TextPiece : public Piece {
public:
  explicit TextPiece(std::string text, bool soft = false) : soft(soft) {
    size_t start = 0;
    while (true) {
      size_t next = text.find('\n', start);
      if (next == std::string::npos) {
        lines.push_back(text.substr(start));
        break;
      }
      lines.push_back(text.substr(start, next - start));
      start = next + 1;
    }
    if (lines.empty()) {
      lines.push_back("");
    }
  }

  void computeMetadata() const override {
    int chars = 0;
    bool hn = lines.size() > 1;
    /* Since soft text might not force an overflowing piece to split, we don't
     * include it in the calculation to preemptively split pieces. */
    if (!soft) {
      for (const auto &line : lines) {
        chars += static_cast<int>(line.length());
      }
    }
    setMetadata(chars, hn);
  }

  void format(CodeWriter &writer, State state) const override {
    /* Allow a multiline string or comment to be treated like a headline when
     * the right-hand side of an assignment or named argument. */
    if (lines.size() > 1) {
      writer.setShapeMode(ShapeMode::BeforeHeadline);
    }

    writer.write(lines[0], soft);

    if (lines.size() > 1) {
      writer.setShapeMode(ShapeMode::AfterHeadline);
    }

    for (size_t i = 1; i < lines.size(); i++) {
      writer.newline(/*blank=*/false, /*flushLeft=*/true);
      writer.write(lines[i], soft);
    }
  }

  const char *debugName() const override { return "Text"; }

private:
  std::vector<std::string> lines;
  bool soft;
};

/* A piece for a source code comment. */
class CommentPiece : public Piece {
public:
  explicit CommentPiece(std::string text, Whitespace trailingWhitespace =
                                              Whitespace::None)
      : text(std::move(text)), trailingWhitespace(trailingWhitespace) {}

  void computeMetadata() const override {
    setMetadata(0,
                text.find('\n') != std::string::npos ||
                    trailingWhitespace.hasNewline());
  }

  void format(CodeWriter &writer, State state) const override {
    writer.writeComment(text);
    if (trailingWhitespace != Whitespace::None) {
      writer.whitespace(trailingWhitespace);
    }
  }

  const char *debugName() const override { return "Comment"; }

private:
  std::string text;
  Whitespace trailingWhitespace;
};

/* A piece that writes a single space. */
class SpacePiece : public Piece {
public:
  void computeMetadata() const override { setMetadata(1, false); }

  void format(CodeWriter &writer, State state) const override {
    writer.space();
  }
};

/* A piece that writes a single newline. */
class NewlinePiece : public Piece {
public:
  void computeMetadata() const override { setMetadata(0, true); }

  void format(CodeWriter &writer, State state) const override {
    writer.newline();
  }
};

/* A piece that writes a blank line. */
class BlankLinePiece : public Piece {
public:
  void computeMetadata() const override { setMetadata(0, true); }

  void format(CodeWriter &writer, State state) const override {
    writer.newline(/*blank=*/true);
  }
};

/* A piece for a series of pieces written with no whitespace between them. */
class ConcatPiece : public Piece {
public:
  explicit ConcatPiece(std::vector<const Piece *> elements)
      : elements(std::move(elements)) {}

  void computeMetadata() const override {
    int chars = 0;
    bool hn = false;
    for (const auto &e : elements) {
      chars += e->totalCharacters();
      hn = hn || e->containsHardNewline();
    }
    setMetadata(chars, hn);
  }

  void format(CodeWriter &writer, State state) const override {
    for (const auto &e : elements)
      writer.format(e);
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    for (const auto &e : elements)
      callback(e);
  }

  const char *debugName() const override { return "Concat"; }

private:
  std::vector<const Piece *> elements;
};

/* A piece for a nested expression that should prevent its inner shape and
 * indentation from propagating outwards. Used for parenthesized expressions
 * and control flow conditions. */
class GroupingPiece : public Piece {
public:
  explicit GroupingPiece(const Piece *content) : content(content) {}

  void computeMetadata() const override {
    setMetadata(content->totalCharacters(),
                content->containsHardNewline());
  }

  void format(CodeWriter &writer, State state) const override {
    writer.pushIndent(Indent::Grouping);
    writer.setShapeMode(ShapeMode::Other);
    writer.format(content);
    writer.popIndent();
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    callback(content);
  }

  const char *debugName() const override { return "Grouping"; }

private:
  const Piece *content;
};

/* What kind of "call" a dotted expression in a call chain is. */
enum class ChainCallType {
  /* A property access, like `.foo`. */
  Property,

  /* A method call with an empty argument list that can't split. */
  UnsplittableCall,

  /* A method call with a non-empty argument list that can split but not block
   * format. */
  SplittableCall,

  /* A method call with a non-empty argument list that can be block formatted.
   */
  BlockFormatCall,
};

/* A method or getter call in a call chain. */
struct ChainCall {
  const Piece *call;
  ChainCallType type;

  bool canSplit() const {
    return type == ChainCallType::SplittableCall ||
           type == ChainCallType::BlockFormatCall;
  }
};

/* A dotted series of property access or method calls, like:
 *
 *     target.getter.method().another.method();
 *
 * This piece handles splitting before the `.` and controlling which argument
 * lists in the method calls are allowed to contain newlines. */
class ChainPiece : public Piece {
public:
  /* Allow newlines in the last (or next-to-last) call but nowhere else. */
  static const State BlockFormatTrailingCall;

  /* Split the call chain at each method call, but leave the leading properties
   * on the same line as the target. */
  static const State SplitAfterProperties;

  ChainPiece(const Piece *target, std::vector<ChainCall> calls,
             bool cascade = false, int leadingProperties = 0,
             int blockCallIndex = -1, Indent indent = Indent::Expression,
             bool hasSingleElementTarget = false)
      : target(target), calls(std::move(calls)), cascade(cascade),
        leadingProperties(leadingProperties), blockCallIndex(blockCallIndex),
        indent(indent), hasSingleElementTarget(hasSingleElementTarget) {}

  void computeMetadata() const override {
    int chars = target->totalCharacters();
    bool hn = target->containsHardNewline();
    for (const auto &link : calls) {
      chars += link.call->totalCharacters();
      hn = hn || link.call->containsHardNewline();
    }
    setMetadata(chars, hn);
  }

  std::vector<State> additionalStates() const override {
    std::vector<State> st;
    if (blockCallIndex != -1)
      st.push_back(BlockFormatTrailingCall);
    if (leadingProperties > 0)
      st.push_back(SplitAfterProperties);
    st.push_back(State::Split);
    return st;
  }

  int stateCost(State state) const override {
    /* When the target is a single-element argument list or collection, try to
     * avoid splitting it. Prefers:
     *
     *     function(argument)
     *         .method();
     *
     * Over:
     *
     *     function(
     *       argument,
     *     ).method(); */
    if (hasSingleElementTarget &&
        (state == SplitAfterProperties || state == State::Split)) {
      return 0;
    }

    /* If the chain is only properties, try to keep them together. Prefers:
     *
     *     variable =
     *         target.property.another; */
    if (!cascade && leadingProperties == (int)calls.size() &&
        state == State::Split) {
      return 2;
    }

    return state.cost;
  }

  uint8_t allowedChildShapes(State state, const Piece *child) const override {
    if (child == target) {
      if (state == State::Unsplit || state == BlockFormatTrailingCall ||
          state == SplitAfterProperties) {
        /* If the chain itself isn't fully split, only allow block splitting in
         * the target. */
        return ShapeMask::InlineOrBlock;
      }
      return ShapeMask::All;
    }

    if (state == State::Unsplit) {
      return ShapeMask::OnlyInline;
    } else if (state == SplitAfterProperties) {
      /* Don't allow splitting inside the properties. */
      for (int i = 0; i < leadingProperties; i++) {
        if (calls[i].call == child)
          return ShapeMask::OnlyInline;
      }
      return ShapeMask::All;
    } else if (state == BlockFormatTrailingCall) {
      return ShapeMask::anyIf(calls[blockCallIndex].call == child);
    }

    return ShapeMask::All;
  }

  void format(CodeWriter &writer, State state) const override {
    if (state == State::Unsplit) {
      writer.format(target);

      for (const auto &call : calls) {
        writer.format(call.call);
      }
    } else if (state == SplitAfterProperties) {
      writer.pushIndent(indent);
      writer.setShapeMode(ShapeMode::BeforeHeadline);
      writer.format(target);

      for (int i = 0; i < leadingProperties; i++) {
        writer.format(calls[i].call);
      }

      writer.setShapeMode(ShapeMode::AfterHeadline);

      for (int i = leadingProperties; i < (int)calls.size(); i++) {
        writer.newline();

        /* Every non-property call except the last will be on its own line. */
        writer.format(calls[i].call, /*separate=*/i < (int)calls.size() - 1);
      }

      writer.popIndent();
    } else if (state == BlockFormatTrailingCall) {
      /* Don't treat a cascade as block-shaped in the surrounding context even
       * if it block splits. */
      if (cascade)
        writer.setShapeMode(ShapeMode::Other);

      writer.format(target);

      for (const auto &call : calls) {
        writer.format(call.call);
      }
    } else {
      writer.pushIndent(indent);
      writer.setShapeMode(ShapeMode::BeforeHeadline);
      writer.format(target);
      writer.setShapeMode(ShapeMode::AfterHeadline);

      for (int i = 0; i < (int)calls.size(); i++) {
        writer.newline();

        /* The chain is fully split so every call except for the last is on its
         * own line. */
        writer.format(calls[i].call, /*separate=*/i < (int)calls.size() - 1);
      }

      writer.popIndent();
    }
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    callback(target);
    for (const auto &call : calls)
      callback(call.call);
  }

  const char *debugName() const override { return "Chain"; }

private:
  const Piece *target;
  std::vector<ChainCall> calls;
  bool cascade;
  int leadingProperties;
  int blockCallIndex;
  Indent indent;
  bool hasSingleElementTarget;
};

inline const State ChainPiece::BlockFormatTrailingCall = State(1, 0);
inline const State ChainPiece::SplitAfterProperties = State(2, Cost::Normal);

/* A piece for an assignment-like construct:
 *
 * - Assignment (`=`, `+=`, etc.)
 * - Named arguments (`:`)
 * - Map entries (`:`)
 * - Expression function bodies (`=>`)
 *
 * Unlike other infix operators, these have some special formatting. */
class AssignPiece : public Piece {
public:
  /* Allow the right-hand side to block split. */
  static const State BlockOrHeadlineSplitRight;

  /* Force the left-hand side to block split and allow the right-hand side to
   * split. */
  static const State BlockSplitLeft;

  AssignPiece(const Piece *left, const Piece *right, bool avoidSplit = true)
      : left(left), right(right), avoidSplit(avoidSplit) {}

  void computeMetadata() const override {
    setMetadata(left->totalCharacters() + right->totalCharacters(),
                left->containsHardNewline() || right->containsHardNewline());
  }

  std::vector<State> additionalStates() const override {
    return {BlockOrHeadlineSplitRight, BlockSplitLeft, State::Split};
  }

  int stateCost(State state) const override {
    if (state == State::Split) {
      return avoidSplit ? 1 : 0;
    }
    return state.cost;
  }

  uint8_t allowedChildShapes(State state, const Piece *child) const override {
    if (state == State::Unsplit) {
      return ShapeMask::OnlyInline;
    } else if (state == BlockSplitLeft) {
      if (child == left)
        return ShapeMask::OnlyBlock;
      if (child == right)
        return ShapeMask::Inline | ShapeMask::Other;
      return ShapeMask::All;
    } else if (state == BlockOrHeadlineSplitRight) {
      if (child == right)
        return ShapeMask::Block | ShapeMask::Headline;
      return ShapeMask::All;
    }
    return ShapeMask::All;
  }

  void format(CodeWriter &writer, State state) const override {
    if (state == State::Split) {
      /* When splitting at the operator, indent the operands. */
      writer.pushIndent(Indent::Expression);

      /* Treat a split `=` as potentially headline-shaped if the LHS doesn't
       * split. */
      writer.setShapeMode(ShapeMode::BeforeHeadline);
      writer.format(left);
      writer.setShapeMode(ShapeMode::AfterHeadline);

      writer.newline();
      writer.popIndent();
      writer.pushIndent(Indent::Assignment);
      writer.format(right);
      writer.popIndent();
    } else {
      writer.format(left);
      writer.space();
      writer.format(right);
    }
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    callback(left);
    callback(right);
  }

  const char *debugName() const override { return "Assign"; }

private:
  const Piece *left;
  const Piece *right;
  bool avoidSplit;
};

inline const State AssignPiece::BlockOrHeadlineSplitRight = State(1, 0);
inline const State AssignPiece::BlockSplitLeft = State(2, Cost::Normal);

/* A piece for a series of binary expressions at the same precedence, like:
 *
 *     a + b + c
 *
 * Since we don't split on both sides of the operator, the operators are
 * embedded in the operand pieces. If the operator is a hanging one, it will be
 * in the preceding operand, so `1 + 2` becomes Infix(`1 +`, `2`). */
class InfixPiece : public Piece {
public:
  InfixPiece(std::vector<const Piece *> operands, Indent indent,
             bool conditional = false)
      : operands(std::move(operands)), indent(indent),
        isConditional(conditional) {}

  void computeMetadata() const override {
    int chars = 0;
    bool hn = false;
    for (const auto &op : operands) {
      chars += op->totalCharacters();
      hn = hn || op->containsHardNewline();
    }
    setMetadata(chars, hn);
  }

  std::vector<State> additionalStates() const override {
    return {State::Split};
  }

  uint8_t allowedChildShapes(State state, const Piece *child) const override {
    return ShapeMask::anyIf(state == State::Split);
  }

  std::optional<State> fixedStateForPageWidth(int pageWidth) const override {
    int totalLength = 0;

    for (const auto &operand : operands) {
      /* If any operand contains a newline, then we have to split. */
      if (operand->containsHardNewline())
        return State::Split;

      totalLength += operand->totalCharacters();
      if (totalLength > pageWidth)
        break;
    }

    /* If the total length doesn't fit in the page, then we have to split. */
    if (totalLength > pageWidth)
      return State::Split;

    return std::nullopt;
  }

  void format(CodeWriter &writer, State state) const override {
    writer.pushIndent(indent);

    /* If this is a conditional expression (or chain of them), then allow the
     * leading condition to be headline formatted in an assignment. */
    if (isConditional)
      writer.setShapeMode(ShapeMode::BeforeHeadline);
    writer.format(operands[0]);
    if (isConditional)
      writer.setShapeMode(ShapeMode::AfterHeadline);

    for (size_t i = 1; i < operands.size(); i++) {
      writer.splitIf(state == State::Split);

      /* If this is a branch of a conditional expression, then indent the
       * branch's contents past the `?` or `:`. */
      if (isConditional)
        writer.pushIndent(Indent::Block);

      /* We can format each operand separately if the operand is on its own
       * line. This happens when the operator is split and we aren't the first
       * or last operand. */
      writer.format(operands[i],
                    /*separate=*/state == State::Split &&
                        i < operands.size() - 1);

      if (isConditional)
        writer.popIndent();
    }

    writer.popIndent();
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    for (const auto &op : operands)
      callback(op);
  }

  const char *debugName() const override { return "Infix"; }

private:
  std::vector<const Piece *> operands;
  Indent indent;
  bool isConditional;
};

/* A single branch in a chain of if-elses or body of a for or while. */
struct ControlFlowSection {
  const Piece *header;
  const Piece *statement;

  /* Whether the [statement] piece is from a block. */
  bool isBlock;

  ControlFlowSection(const Piece *header, const Piece *statement, bool isBlock)
      : header(header), statement(statement), isBlock(isBlock) {}
};

/* A piece for an if, while, or for statement. */
class ControlFlowPiece : public Piece {
public:
  ControlFlowPiece() = default;

  void add(const Piece *header, const Piece *statement, bool isBlock) {
    sections.push_back(ControlFlowSection(header, statement, isBlock));
  }

  void computeMetadata() const override {
    int chars = 0;
    bool hn = false;
    for (const auto &section : sections) {
      chars += section.header->totalCharacters() +
               section.statement->totalCharacters();
      hn = hn || section.header->containsHardNewline() ||
           section.statement->containsHardNewline();
    }
    setMetadata(chars, hn);
  }

  std::vector<State> additionalStates() const override {
    return {State::Split};
  }

  uint8_t allowedChildShapes(State state, const Piece *child) const override {
    return ShapeMask::anyIf(state == State::Split);
  }

  bool containsNewline(State state) const override {
    if (state == State::Split) {
      for (const auto &section : sections) {
        if (!section.isBlock)
          return true;
      }
    }

    return Piece::containsNewline(state);
  }

  void format(CodeWriter &writer, State state) const override {
    for (size_t i = 0; i < sections.size(); i++) {
      const auto &section = sections[i];

      /* A split in the condition forces the branches to split. */
      writer.format(section.header);

      if (!section.isBlock) {
        writer.pushIndent(Indent::Block);
        writer.splitIf(state == State::Split);
      }

      writer.format(section.statement);

      /* Reset the indentation for the subsequent `else` or `} else` line. */
      if (!section.isBlock)
        writer.popIndent();

      if (i < sections.size() - 1) {
        writer.splitIf(state == State::Split && !section.isBlock);
      }
    }
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    for (const auto &section : sections) {
      callback(section.header);
      callback(section.statement);
    }
  }

  const char *debugName() const override { return "Ctrl"; }

private:
  std::vector<ControlFlowSection> sections;
};

/* Where commas should be added in a ListPiece. */
enum class Commas {
  /* Add a comma after every element, regardless of whether or not it is
   * split. */
  AlwaysTrailing,

  /* Add a comma after every element when the elements split, including the
   * last. When not split, omit the trailing comma. */
  Trailing,

  /* Add a comma after every element except for the last, regardless of whether
   * or not it is split. */
  NonTrailing,

  /* Don't add commas after any elements. */
  None,
};

/* The various ways a "list" can appear syntactically and be formatted. */
struct ListStyle {
  Commas commas = Commas::Trailing;
  int splitCost = Cost::Normal;
  bool spaceWhenUnsplit = false;
};

class ListElementPiece : public Piece {
public:
  static const State AppendComma;

  ListElementPiece(const Piece *content, std::vector<const Piece *> leadingComments = {})
      : leadingComments(std::move(leadingComments)), content(content) {}

  explicit ListElementPiece(const Piece *comment, bool isComment)
      : content(nullptr) {
    hangingComments.push_back(comment);
  }

  void computeMetadata() const override {
    int chars = 0;
    bool hn = false;
    for (const auto &c : leadingComments) {
      chars += c->totalCharacters();
      hn = hn || c->containsHardNewline();
    }
    if (content) {
      chars += content->totalCharacters();
      hn = hn || content->containsHardNewline();
    }
    for (const auto &c : hangingComments) {
      chars += c->totalCharacters();
      hn = hn || c->containsHardNewline();
    }
    setMetadata(chars, hn);
  }

  std::vector<State> additionalStates() const override {
    return {AppendComma};
  }

  bool isComment() const { return content == nullptr; }

  /* Whether newlines are allowed in this element when this list is unsplit. */
  bool allowNewlinesWhenUnsplit = false;

  /* Whether we should increase indentation when formatting this element when
   * the list isn't split. */
  bool indentWhenBlockFormatted = false;

  void addHangingComment(const Piece *comment, bool beforeDelimiter = false) {
    hangingComments.push_back(comment);
    if (beforeDelimiter)
      commentsBeforeDelimiter++;
  }

  void format(CodeWriter &writer, State state) const override {
    for (const auto &comment : leadingComments) {
      writer.format(comment);
      writer.space();
    }

    if (content) {
      writer.format(content);

      for (int i = 0; i < commentsBeforeDelimiter; i++) {
        writer.space();
        writer.format(hangingComments[i]);
      }

      if (state == AppendComma)
        writer.write(",", /*soft=*/true);

      if (!delimiter.empty()) {
        writer.space();
        writer.write(delimiter);
      }
    }

    for (size_t i = commentsBeforeDelimiter; i < hangingComments.size(); i++) {
      if (i > 0 || content != nullptr)
        writer.space();
      writer.format(hangingComments[i]);
    }
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    for (const auto &c : leadingComments)
      callback(c);
    if (content)
      callback(content);
    for (const auto &c : hangingComments)
      callback(c);
  }

  /* Don't pin the ListElementPiece: its state is only used to determine
   * whether or not to write a comma. */
  void preventSplit() const override {}

  const char *debugName() const override { return "ListElem"; }

  void setDelimiter(std::string delim) { delimiter = std::move(delim); }

  /* Whether a space should be written after this element (before the comma).
   * No space after the "[" or "{" delimiter in a parameter list. */
  bool getSpaceAfterElement() const { return delimiter.empty(); }

  const Piece *getContent() const { return content; }

private:
  std::vector<const Piece *> leadingComments;
  const Piece *content;
  std::vector<const Piece *> hangingComments;
  std::string delimiter;
  int commentsBeforeDelimiter = 0;
};

inline const State ListElementPiece::AppendComma = State(1, 0);

/* A piece for a non-empty splittable series of items.
 *
 * Items may optionally be delimited with brackets and may have commas added
 * after elements. Used for argument lists, collection literals, parameter
 * lists, etc. This class handles adding and removing the trailing comma
 * depending on whether the list is split or not. */
class ListPiece : public Piece {
public:
  ListPiece(const Piece *before, std::vector<const Piece *> elements,
            const Piece *after, ListStyle style, int lastNonCommentElement,
            bool blockShaped)
      : before(before), elements(std::move(elements)), after(after),
        style(style), lastNonCommentElement(lastNonCommentElement),
        isBlockShaped(blockShaped) {
    /* For most elements, we know whether or not it will have a comma based
     * only on the comma style and its position in the list, so pin those
     * here. */
    for (size_t i = 0; i < this->elements.size(); i++) {
      const auto *element = this->elements[i];

      switch (style.commas) {
      case Commas::AlwaysTrailing:
        /* Has a comma after every element. */
        element->pin(ListElementPiece::AppendComma);
        break;

      case Commas::Trailing:
        /* Always has a comma after every element except the last. The last
         * will be constrained to have one or not depending on whether the
         * list splits. See applyConstraints(). */
        if ((int)i < lastNonCommentElement) {
          element->pin(ListElementPiece::AppendComma);
        }
        break;

      case Commas::NonTrailing:
        /* Never a trailing comma after the last element. */
        element->pin((int)i < lastNonCommentElement
                         ? ListElementPiece::AppendComma
                         : State::Unsplit);
        break;

      case Commas::None:
        /* No comma after any element. */
        element->pin(State::Unsplit);
        break;
      }
    }
  }

  void computeMetadata() const override {
    int chars = 0;
    bool hn = false;
    if (before) {
      chars += before->totalCharacters();
      hn = hn || before->containsHardNewline();
    }
    for (size_t i = 0; i < elements.size(); i++) {
      chars += elements[i]->totalCharacters();
      hn = hn || elements[i]->containsHardNewline();
      if (i < elements.size() - 1) {
        chars += 2;
      }
    }
    if (after) {
      chars += after->totalCharacters();
      hn = hn || after->containsHardNewline();
    }
    setMetadata(chars, hn);
  }

  std::vector<State> additionalStates() const override {
    return {State::Split};
  }

  /* Whether any element in this list can be block formatted. */
  bool hasBlockElement() const {
    for (const auto *element : elements) {
      if (const auto *le = dynamic_cast<const ListElementPiece *>(element)) {
        if (le->allowNewlinesWhenUnsplit)
          return true;
      }
    }
    return false;
  }

  void applyConstraints(State state, const Constrain &constrain) const
      override {
    /* Give the last element a trailing comma only if the list is split. */
    if (style.commas == Commas::Trailing && lastNonCommentElement != -1) {
      constrain(elements[lastNonCommentElement],
                state == State::Split ? ListElementPiece::AppendComma
                                      : State::Unsplit);
    }
  }

  int stateCost(State state) const override {
    if (state == State::Split) {
      return style.splitCost;
    }
    return state.cost;
  }

  uint8_t allowedChildShapes(State state, const Piece *child) const override {
    if (state == State::Split)
      return ShapeMask::All;
    if (child == before)
      return ShapeMask::All;
    if (child == after)
      return ShapeMask::All;

    /* Only some elements (usually a single block element) allow newlines when
     * the list itself isn't split. */
    if (const auto *le = dynamic_cast<const ListElementPiece *>(child)) {
      return ShapeMask::anyIf(le->allowNewlinesWhenUnsplit);
    }
    return ShapeMask::OnlyInline;
  }

  std::optional<State> fixedStateForPageWidth(int pageWidth) const override {
    int surroundingLength = 0;
    if (before) {
      /* A newline in the opening bracket (like a line comment after the
       * bracket) forces the list to split. */
      if (before->containsHardNewline())
        return State::Split;
      surroundingLength += before->totalCharacters();
    }

    if (after) {
      surroundingLength += after->totalCharacters();
    }

    int currentLineLength = surroundingLength;
    bool first = true;
    for (const auto *element : elements) {
      const auto *le = dynamic_cast<const ListElementPiece *>(element);
      /* If the element can be block formatted, then it might contain a newline
       * that doesn't force the list to split. */
      if (le && le->allowNewlinesWhenUnsplit) {
        currentLineLength = surroundingLength;
        continue;
      }

      if (element->containsHardNewline())
        return State::Split;

      currentLineLength += element->totalCharacters();

      /* The comma and space between elements. */
      if (!first)
        currentLineLength += 2;
      first = false;

      if (currentLineLength > pageWidth)
        return State::Split;
    }

    return std::nullopt;
  }

  void format(CodeWriter &writer, State state) const override {
    /* Format the opening bracket, if there is one. */
    if (before) {
      writer.format(before);

      if (state != State::Unsplit)
        writer.pushIndent(Indent::Block);

      if (isBlockShaped)
        writer.setShapeMode(ShapeMode::Block);

      /* Whitespace after the opening bracket. */
      writer.splitIf(state == State::Split,
                     /*space=*/style.spaceWhenUnsplit && !elements.empty());
    }

    /* Format the elements. */
    for (size_t i = 0; i < elements.size(); i++) {
      const auto *element = elements[i];

      /* If this element allows newlines when the list isn't split, add
       * indentation if it requires it. */
      const auto *le = dynamic_cast<const ListElementPiece *>(element);
      if (state == State::Unsplit && le && le->indentWhenBlockFormatted) {
        writer.pushIndent(Indent::Expression);
      }

      /* We can format each list item separately if the item is on its own
       * line. This happens when the list is split and there is something
       * before and after the item, either brackets or other items. */
      bool separate = state == State::Split && (i > 0 || before != nullptr) &&
                      (i < elements.size() - 1 || after != nullptr);
      writer.format(element, separate);

      if (state == State::Unsplit && le && le->indentWhenBlockFormatted) {
        writer.popIndent();
      }

      /* Write a space or newline between elements. */
      if (i < elements.size() - 1) {
        const auto *listElem = dynamic_cast<const ListElementPiece *>(element);
        bool spaceAfter = listElem ? listElem->getSpaceAfterElement() : true;
        bool blank = std::find(blanksAfter.begin(), blanksAfter.end(),
                               element) != blanksAfter.end();
        writer.splitIf(state == State::Split, /*space=*/spaceAfter,
                       /*blank=*/blank);
      }
    }

    /* Format the closing bracket, if any. */
    if (after) {
      if (state == State::Split)
        writer.popIndent();

      /* Whitespace before the closing bracket. */
      writer.splitIf(state == State::Split,
                     /*space=*/style.spaceWhenUnsplit && !elements.empty());

      if (isBlockShaped)
        writer.setShapeMode(ShapeMode::Merge);

      writer.format(after);
    }
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    if (before)
      callback(before);

    for (const auto *element : elements) {
      callback(element);
    }

    if (after)
      callback(after);
  }

  const char *debugName() const override { return "List"; }

  /* Elements that should have a blank line preserved between them and the
   * next piece. */
  std::vector<const Piece *> blanksAfter;

private:
  const Piece *before;
  std::vector<const Piece *> elements;
  const Piece *after;
  ListStyle style;
  int lastNonCommentElement;
  bool isBlockShaped;
};

/* An element inside a SequencePiece.
 *
 * Tracks the underlying piece along with surrounding whitespace. */
class SequenceElementPiece : public Piece {
public:
  SequenceElementPiece(Indent indent, const Piece *piece)
      : indent(indent), piece(piece) {}

  void computeMetadata() const override {
    setMetadata(piece->totalCharacters(), piece->containsHardNewline());
  }

  void format(CodeWriter &writer, State state) const override {
    writer.format(piece);

    for (const auto *comment : hangingComments) {
      writer.space();
      writer.format(comment);
    }
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    callback(piece);
    for (const auto *comment : hangingComments)
      callback(comment);
  }

  const char *debugName() const override { return "SeqElem"; }

  /* The indentation on the line before this element, relative to the
   * surrounding piece. */
  Indent indent;

  /* The piece for the element. */
  const Piece *piece;

  /* The comments that should appear at the end of this element's line. */
  std::vector<const Piece *> hangingComments;

  /* Whether there should be a blank line after this element. */
  bool blankAfter = false;
};

/* A piece for a series of statements or members inside a block or declaration
 * body or at the top level of a program. */
class SequencePiece : public Piece {
public:
  explicit SequencePiece(std::vector<const Piece *> elements)
      : elements(std::move(elements)) {}

  void computeMetadata() const override {
    int chars = 0;
    bool hn = false;
    for (const auto &e : elements) {
      chars += e->totalCharacters();
      hn = hn || e->containsHardNewline();
    }
    /* If there are multiple elements, there are newlines between them. */
    if (elements.size() > 1) {
      hn = true;
    }
    setMetadata(chars, hn);
  }

  void format(CodeWriter &writer, State state) const override {
    writer.pushIndent(Indent::None);

    for (size_t i = 0; i < elements.size(); i++) {
      writer.format(elements[i], /*separate=*/true);

      if (i < elements.size() - 1) {
        writer.popIndent();
        writer.pushIndent(
            dynamic_cast<const SequenceElementPiece *>(elements[i + 1])
                ->indent);
        const auto *element =
            dynamic_cast<const SequenceElementPiece *>(elements[i]);
        writer.newline(/*blank=*/element->blankAfter);
      }
    }

    writer.popIndent();
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    for (const auto &e : elements)
      callback(e);
  }

  const char *debugName() const override { return "Seq"; }

private:
  std::vector<const Piece *> elements;
};

/* A piece for a non-empty brace-delimited series of statements or members
 * inside a block or declaration body. Unlike ListPiece, always splits between
 * the elements. */
class BlockPiece : public Piece {
public:
  BlockPiece(const Piece *leftBracket, const Piece *elements,
             const Piece *rightBracket)
      : leftBracket(leftBracket), elements(elements),
        rightBracket(rightBracket) {}

  void computeMetadata() const override {
    setMetadata(leftBracket->totalCharacters() + elements->totalCharacters() +
                    rightBracket->totalCharacters(),
                true);
  }

  void format(CodeWriter &writer, State state) const override {
    writer.format(leftBracket);
    writer.pushIndent(Indent::Block);
    writer.setShapeMode(ShapeMode::Block);
    writer.newline();
    writer.format(elements);
    writer.popIndent();
    writer.newline();
    writer.setShapeMode(ShapeMode::Merge);
    writer.format(rightBracket);
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    callback(leftBracket);
    callback(elements);
    callback(rightBracket);
  }

  /* A BlockPiece is never empty and always splits between the delimiters. */
  bool containsNewline(State state) const override { return true; }

  const char *debugName() const override { return "Block"; }

private:
  const Piece *leftBracket;
  const Piece *elements;
  const Piece *rightBracket;
};

} // namespace utopia
