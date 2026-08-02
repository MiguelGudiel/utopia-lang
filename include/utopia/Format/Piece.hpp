#pragma once
#include "utopia/Format/CodeWriter.hpp"
#include "utopia/Format/FormatCore.hpp"
#include <functional>
#include <vector>

namespace utopia {

class Piece {
public:
  virtual ~Piece() = default;

  virtual std::vector<State> additionalStates() const { return {}; }
  virtual int stateCost(const State &state) const { return state.cost; }

  virtual bool isBlockLike() const { return false; }

  virtual void format(
      CodeWriter &writer, const State &state,
      const std::function<void(const Piece *, State)> &formatChild) const = 0;

  virtual void applyConstraints(
      const State &state,
      const std::function<void(const Piece *, State)> &constrain) const {}

  virtual void
  forEachChild(const std::function<void(const Piece *)> &callback) const {}
};

class TextPiece : public Piece {
public:
  explicit TextPiece(std::string text) : text(std::move(text)) {}

  void format(CodeWriter &writer, const State &state,
              const std::function<void(const Piece *, State)> &formatChild)
      const override {
    writer.write(text);
  }

private:
  std::string text;
};

class StringPiece : public Piece {
public:
  explicit StringPiece(std::string text) : text(std::move(text)) {}

  void format(CodeWriter &writer, const State &state,
              const std::function<void(const Piece *, State)> &formatChild)
      const override {
    writer.writeString(text);
  }

private:
  std::string text;
};

class BlankLinePiece : public Piece {
public:
  void
  format(CodeWriter &writer, const State &state,
         const std::function<void(const Piece *, State)> &) const override {
    writer.blankLine();
  }
};

class NewlinesPiece : public Piece {
public:
  explicit NewlinesPiece(int count) : count(count) {}

  void
  format(CodeWriter &writer, const State &state,
         const std::function<void(const Piece *, State)> &) const override {
    writer.exactNewlines(count);
  }

private:
  int count;
};

class ConcatPiece : public Piece {
public:
  explicit ConcatPiece(std::vector<Piece *> elements)
      : elements(std::move(elements)) {}

  bool isBlockLike() const override {
    return !elements.empty() && elements.back()->isBlockLike();
  }

  void format(CodeWriter &writer, const State &state,
              const std::function<void(const Piece *, State)> &formatChild)
      const override {
    for (const auto &e : elements)
      formatChild(e, state);
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    for (const auto &e : elements)
      callback(e);
  }

private:
  std::vector<Piece *> elements;
};

enum class ChainLinkKind {
  Property,
  UnsplittableCall,
  SplittableCall,
  BlockFormatCall
};

struct ChainLink {
  Piece *piece;
  ChainLinkKind kind;
  bool isProperty() const { return kind == ChainLinkKind::Property; }
};

class ChainPiece : public Piece {
public:
  ChainPiece(Piece *target, std::vector<ChainLink> links, bool hasBlockFormat,
             bool hasProperties)
      : target(target), links(std::move(links)), hasBlockFormat(hasBlockFormat),
        hasProperties(hasProperties) {}

  std::vector<State> additionalStates() const override {
    std::vector<State> st;
    if (hasBlockFormat)
      st.push_back(State::BlockFormatTrailingCall);
    if (hasProperties)
      st.push_back(State::SplitAfterProperties);
    st.push_back(State::SplitAll);
    return st;
  }

  int stateCost(const State &state) const override {
    if (state == State::BlockFormatTrailingCall)
      return 0;
    if (state == State::SplitAfterProperties)
      return Cost::Normal * 2;
    if (state == State::SplitAll)
      return Cost::Normal * 3;
    return state.cost;
  }

  void format(CodeWriter &writer, const State &state,
              const std::function<void(const Piece *, State)> &formatChild)
      const override {
    formatChild(target, State::Unsplit);
    for (const auto &link : links) {
      bool split = false;
      if (state == State::SplitAll)
        split = true;
      else if (state == State::SplitAfterProperties)
        split = !link.isProperty();
      else if (state == State::BlockFormatTrailingCall)
        split = false;

      if (split) {
        writer.pushIndent(Indent::Expression);
        writer.newline();
      }

      formatChild(link.piece,
                  state == State::SplitAll ? State::Split : State::Unsplit);

      if (split) {
        writer.popIndent();
      }
    }
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    callback(target);
    for (const auto &link : links)
      callback(link.piece);
  }

private:
  Piece *target;
  std::vector<ChainLink> links;
  bool hasBlockFormat;
  bool hasProperties;
};

class AssignPiece : public Piece {
public:
  AssignPiece(Piece *left, std::string op, Piece *right)
      : left(left), op(std::move(op)), right(right) {}

  bool isBlockLike() const override { return right->isBlockLike(); }

  std::vector<State> additionalStates() const override {
    return {State::Split};
  }

  int stateCost(const State &state) const override {
    if (state == State::Split) {
      return Cost::Assign;
    }
    return state.cost;
  }

  void format(CodeWriter &writer, const State &state,
              const std::function<void(const Piece *, State)> &formatChild)
      const override {
    formatChild(left, State::Unsplit);
    writer.space();
    writer.write(op);

    if (state == State::Split) {
      writer.pushIndent(Indent::Assignment);
      writer.newline();
      formatChild(right, State::Unsplit);
      writer.popIndent();
    } else {
      writer.space();
      formatChild(right, State::Unsplit);
    }
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    callback(left);
    callback(right);
  }

private:
  Piece *left;
  std::string op;
  Piece *right;
};

class SequencePiece : public Piece {
public:
  explicit SequencePiece(std::vector<Piece *> elements)
      : elements(std::move(elements)) {}

  void format(CodeWriter &writer, const State &state,
              const std::function<void(const Piece *, State)> &formatChild)
      const override {
    for (size_t i = 0; i < elements.size(); ++i) {
      formatChild(elements[i], State::Unsplit);
      if (i < elements.size() - 1) {
        writer.newline();
      }
    }
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    for (auto *e : elements) {
      callback(e);
    }
  }

private:
  std::vector<Piece *> elements;
};

class BlockPiece : public Piece {
public:
  explicit BlockPiece(std::vector<Piece *> statements)
      : statements(std::move(statements)) {}

  bool isBlockLike() const override { return true; }

  void format(CodeWriter &writer, const State &state,
              const std::function<void(const Piece *, State)> &formatChild)
      const override {
    writer.write("{");
    if (!statements.empty()) {
      writer.pushIndent(Indent::Block);
      writer.newline();
      for (size_t i = 0; i < statements.size(); ++i) {
        formatChild(statements[i], State::Unsplit);
        if (i < statements.size() - 1) {
          writer.newline();
        }
      }
      writer.popIndent();
      writer.newline();
    } else {
      writer.write("}");
      return;
    }
    writer.write("}");
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    for (auto *s : statements)
      callback(s);
  }

private:
  std::vector<Piece *> statements;
};

class ListPiece : public Piece {
public:
  ListPiece(Piece *openBracket, std::vector<Piece *> elements,
            Piece *closeBracket)
      : openBracket(openBracket), elements(std::move(elements)),
        closeBracket(closeBracket) {}

  bool isBlockLike() const override { return true; }

  std::vector<State> additionalStates() const override {
    return {State::Split};
  }

  void format(CodeWriter &writer, const State &state,
              const std::function<void(const Piece *, State)> &formatChild)
      const override {
    int startLine = writer.getCurrentLine();

    if (openBracket) {
      formatChild(openBracket, State::Unsplit);
    }

    if (elements.empty()) {
      if (closeBracket)
        formatChild(closeBracket, State::Unsplit);
      return;
    }

    if (state == State::Split) {
      writer.pushIndent(Indent::Block);
      writer.newline();
      for (size_t i = 0; i < elements.size(); ++i) {
        formatChild(elements[i], State::Unsplit);
        /* Enforce trailing commas for all elements to match Dart styling */
        writer.write(",");
        writer.newline();
      }
      writer.popIndent();
    } else {
      for (size_t i = 0; i < elements.size(); ++i) {
        formatChild(elements[i], State::Unsplit);
        if (i < elements.size() - 1) {
          writer.write(",");
          writer.space();
        }
      }
    }

    if (closeBracket) {
      formatChild(closeBracket, State::Unsplit);
    }

    int endLine = writer.getCurrentLine();

    if (state == State::Unsplit && endLine > startLine) {
      bool allowed = false;
      if (!elements.empty() && elements.back()->isBlockLike()) {
        allowed = true;
      }
      if (!allowed) {
        writer.markInvalid();
      }
    }
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    if (openBracket)
      callback(openBracket);
    for (auto *e : elements)
      callback(e);
    if (closeBracket)
      callback(closeBracket);
  }

private:
  Piece *openBracket;
  std::vector<Piece *> elements;
  Piece *closeBracket;
};

class CallPiece : public Piece {
public:
  CallPiece(Piece *target, Piece *argsList)
      : target(target), argsList(argsList) {}

  void format(CodeWriter &writer, const State &state,
              const std::function<void(const Piece *, State)> &formatChild)
      const override {
    formatChild(target, State::Unsplit);
    formatChild(argsList, state);
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    callback(target);
    callback(argsList);
  }

private:
  Piece *target;
  Piece *argsList;
};

class InfixPiece : public Piece {
public:
  InfixPiece(Piece *left, std::string op, Piece *right)
      : left(left), op(std::move(op)), right(right) {}

  std::vector<State> additionalStates() const override {
    return {State::Split};
  }

  void format(CodeWriter &writer, const State &state,
              const std::function<void(const Piece *, State)> &formatChild)
      const override {
    formatChild(left, State::Unsplit);
    writer.space();
    writer.write(op);

    if (state == State::Split) {
      writer.pushIndent(Indent::Infix);
      writer.newline();
      formatChild(right, State::Unsplit);
      writer.popIndent();
    } else {
      writer.space();
      formatChild(right, State::Unsplit);
    }
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    callback(left);
    callback(right);
  }

private:
  Piece *left;
  std::string op;
  Piece *right;
};

class ControlFlowPiece : public Piece {
public:
  ControlFlowPiece(std::string keyword, Piece *condition, Piece *body,
                   Piece *elseBody = nullptr)
      : keyword(std::move(keyword)), condition(condition), body(body),
        elseBody(elseBody) {}

  void format(CodeWriter &writer, const State &state,
              const std::function<void(const Piece *, State)> &formatChild)
      const override {
    writer.write(keyword);
    if (condition) {
      writer.space();
      writer.write("(");
      formatChild(condition, State::Unsplit);
      writer.write(")");
    }
    if (body) {
      writer.space();
      formatChild(body, State::Unsplit);
    }
    if (elseBody) {
      writer.space();
      writer.write("else");
      writer.space();
      formatChild(elseBody, State::Unsplit);
    }
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    if (condition)
      callback(condition);
    callback(body);
    if (elseBody)
      callback(elseBody);
  }

private:
  std::string keyword;
  Piece *condition;
  Piece *body;
  Piece *elseBody;
};

class CasePiece : public Piece {
public:
  CasePiece(Piece *label, std::vector<Piece *> statements)
      : label(label), statements(std::move(statements)) {}

  bool isBlockLike() const override { return true; }

  void format(CodeWriter &writer, const State &state,
              const std::function<void(const Piece *, State)> &formatChild)
      const override {
    formatChild(label, State::Unsplit);
    if (!statements.empty()) {
      /* Create an isolated indentation context strictly for case block
       * statements */
      writer.pushIndent(Indent::Block);
      writer.newline();
      for (size_t i = 0; i < statements.size(); ++i) {
        formatChild(statements[i], State::Unsplit);
        if (i < statements.size() - 1) {
          writer.newline();
        }
      }
      writer.popIndent();
    }
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    callback(label);
    for (auto *s : statements)
      callback(s);
  }

private:
  Piece *label;
  std::vector<Piece *> statements;
};

} // namespace utopia