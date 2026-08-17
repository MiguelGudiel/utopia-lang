#pragma once
#include <cstdint>

namespace utopia {

/* Costs for the heuristics used to determine which set of splits is most
 * desirable. Mirrors dart_style's Cost class. */
struct Cost {
  static constexpr int Arrow = 0;
  static constexpr int Normal = 1;
  static constexpr int Assign = 1;
  static constexpr int AssignBlock = 2;
  static constexpr int FirstBlockArgument = 2;
  static constexpr int PositionalArguments = 2;
  static constexpr int SingleElementList = 2;
  static constexpr int SplitBlocks = 2;
  static constexpr int ConstructorName = 4;
  static constexpr int Index = 4;
  static constexpr int TypeArgument = 4;
  static constexpr int ParameterType = 4;
};

/* A state that a piece can be in.
 *
 * Each state identifies one way that a piece can be split into multiple lines.
 * Each piece determines how its states are interpreted. */
struct State {
  uint8_t value;
  int cost;

  constexpr State(uint8_t value, int cost = 1) : value(value), cost(cost) {}

  bool operator==(const State &other) const {
    return value == other.value && cost == other.cost;
  }

  bool operator!=(const State &other) const { return !(*this == other); }

  bool operator<(const State &other) const { return value < other.value; }

  bool operator>(const State &other) const { return value > other.value; }

  /* The unsplit state. */
  static const State Unsplit;

  /* The maximally split state. The value is somewhat arbitrary; it just needs
   * to be larger than any other value used by any piece. */
  static const State Split;
};

inline const State State::Unsplit = State(0, 0);
inline const State State::Split = State(255, Cost::Normal);

/* The spatial "shape" of a formatted piece.
 *
 * Much of the formatting style is defined by placing constraints on whether a
 * newline inside a child forces the parent to enter certain states. */
struct Shape {
  enum Value : uint8_t { Inline, Block, Headline, Other };

  Value value;

  constexpr Shape(Value v) : value(v) {}

  bool operator==(Shape other) const { return value == other.value; }

  bool operator!=(Shape other) const { return value != other.value; }

  /* Determines the resulting shape of a parent when it has children with this
   * shape and [other] shape. */
  Shape merge(Shape other) const {
    if (value == Inline)
      return other;
    if (other.value == Inline)
      return *this;
    return Shape(Other);
  }
};

/* Determines how a newline inside a piece or a child piece affects the shape
 * of the current piece. */
enum class ShapeMode {
  /* The piece's shape is merged with the incoming shape. */
  Merge,

  /* A newline makes this piece block-shaped. */
  Block,

  /* We are in the first line of a potentially headline-shaped piece. */
  BeforeHeadline,

  /* We've already written the headline part of a piece so a newline after this
   * is fine and still leaves it headline shaped. */
  AfterHeadline,

  /* A newline makes this piece have Shape::Other. */
  Other,
};

/* The number of spaces for various kinds of indentation. Each indentation type
 * also carries a semantic reason *why* it writes that, which allows merging or
 * combining indentation in smarter ways in some contexts. */
enum class Indent {
  /* No indentation. */
  None = 0,

  /* The right-hand side of an `=`, `:`, or `=>`. */
  Assignment = 4,

  /* The contents of a block-like structure: block, collection literal,
   * argument list, etc. */
  Block = 2,

  /* A split cascade chain. */
  Cascade = 2,

  /* Indentation when splits occur inside for-in and if-case clause headers. */
  ControlFlowClause = 4,

  /* Any general sort of split expression. */
  Expression = 4,

  /* "Indentation" for parenthesized expressions and other contexts where we
   * want to prevent some inner expression's indentation from merging with the
   * surrounding one. */
  Grouping = 0,

  /* An infix operator expression: `+`, `*`, `is`, etc. */
  Infix = 4,

  /* Constructor initializer when the parameter list doesn't have optional or
   * named parameters. */
  Initializer = 2,

  /* Constructor initializer when the parameter list does have optional or
   * named parameters. */
  InitializerWithOptionalParameter = 3,
};

constexpr int indentSpaces(Indent indent) {
  return static_cast<int>(indent);
}

/* Different kinds of pending whitespace that have been requested.
 *
 * Note that the order of values in the enum is significant: later ones have
 * more whitespace than previous ones. */
struct Whitespace {
  enum Value : uint8_t {
    /* No pending whitespace. */
    None,

    /* A single space. */
    Space,

    /* A single newline. */
    Newline,

    /* Two newlines. */
    BlankLine,
  };

  Value value;

  constexpr Whitespace(Value v) : value(v) {}

  bool operator==(Whitespace other) const { return value == other.value; }

  bool operator!=(Whitespace other) const { return value != other.value; }

  bool operator>=(Whitespace other) const { return value >= other.value; }

  /* Combines two pending whitespaces and returns the result.
   *
   * When two whitespaces overlap, they aren't both written: we don't want two
   * spaces or a newline followed by a space. Instead, the two whitespaces are
   * collapsed such that the largest one wins. */
  Whitespace collapse(Whitespace other) const {
    return value >= other.value ? *this : other;
  }

  /* Whether this whitespace contains at least one newline. */
  bool hasNewline() const {
    return value == Newline || value == BlankLine;
  }
};

} // namespace utopia
