#pragma once
#include <cstdint>

namespace utopia {

enum class Whitespace { None, Space, Newline, BlankLine };

enum class Indent {
  None = 0,
  Grouping = 0,
  Block = 2,
  Cascade = 2,
  Initializer = 2,
  InitializerWithOptionalParameter = 3,
  Assignment = 4,
  ControlFlowClause = 4,
  Expression = 4,
  Infix = 4
};

struct Cost {
  static constexpr int Normal = 1;
  static constexpr int Assign = 1;
  static constexpr int AssignBlock = 2;
  static constexpr int FirstBlockArgument = 2;
  static constexpr int PositionalArguments = 2;
  static constexpr int SingleElementList = 2;
  static constexpr int SplitBlocks = 2;
};

struct State {
  uint8_t value;
  int cost;

  bool operator==(const State &other) const {
    return value == other.value && cost == other.cost;
  }

  static const State Unsplit;
  static const State Split;

  static const State BlockFormatTrailingCall;
  static const State SplitAfterProperties;
  static const State SplitAll;
};

inline const State State::Unsplit = {0, 0};
inline const State State::Split = {255, Cost::Normal};
inline const State State::BlockFormatTrailingCall = {1, 0};
inline const State State::SplitAfterProperties = {2, Cost::Normal * 2};
inline const State State::SplitAll = {3, Cost::Normal * 3};

enum class Shape { Inline, Block, Headline, Other };

} // namespace utopia