#pragma once
#include "utopia/Format/CodeWriter.hpp"
#include "utopia/Format/FormatCore.hpp"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace utopia {

class Piece {
protected:
  mutable int cachedTotalCharacters = -1;
  mutable bool cachedContainsHardNewline = false;

  void setMetadata(int chars, bool hn) const {
    cachedTotalCharacters = chars;
    cachedContainsHardNewline = hn;
  }

public:
  virtual ~Piece() = default;

  void ensureMetadata() const {
    if (cachedTotalCharacters == -1) {
      computeMetadata();
    }
  }

  int totalCharacters() const {
    ensureMetadata();
    return cachedTotalCharacters;
  }

  bool containsHardNewline() const {
    ensureMetadata();
    return cachedContainsHardNewline;
  }

  virtual void computeMetadata() const = 0;

  virtual std::optional<State> fixedState(int pageWidth) const {
    return std::nullopt;
  }

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

  void computeMetadata() const override { setMetadata(text.length(), false); }

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

  void computeMetadata() const override {
    setMetadata(text.length(), text.find('\n') != std::string::npos);
  }

  void format(CodeWriter &writer, const State &state,
              const std::function<void(const Piece *, State)> &formatChild)
      const override {
    writer.writeString(text);
  }

private:
  std::string text;
};

class CommentPiece : public Piece {
public:
  explicit CommentPiece(std::string text) : text(std::move(text)) {}

  void computeMetadata() const override {
    setMetadata(text.length(), text.find('\n') != std::string::npos);
  }

  void format(CodeWriter &writer, const State &state,
              const std::function<void(const Piece *, State)> &formatChild)
      const override {
    writer.writeComment(text);
  }

private:
  std::string text;
};

class BlankLinePiece : public Piece {
public:
  void computeMetadata() const override { setMetadata(0, true); }

  void
  format(CodeWriter &writer, const State &state,
         const std::function<void(const Piece *, State)> &) const override {
    writer.blankLine();
  }
};

class NewlinesPiece : public Piece {
public:
  explicit NewlinesPiece(int count) : count(count) {}

  void computeMetadata() const override { setMetadata(0, true); }

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

  void computeMetadata() const override {
    int chars = 0;
    bool hn = false;
    for (const auto &e : elements) {
      chars += e->totalCharacters();
      hn = hn || e->containsHardNewline();
    }
    setMetadata(chars, hn);
  }

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

  void computeMetadata() const override {
    int chars = target->totalCharacters();
    bool hn = target->containsHardNewline();
    for (const auto &link : links) {
      chars += link.piece->totalCharacters();
      hn = hn || link.piece->containsHardNewline();
    }
    setMetadata(chars, hn);
  }

  bool isBlockLike() const override {
    if (!links.empty()) {
      return links.back().piece->isBlockLike();
    }
    return target->isBlockLike();
  }

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

  void computeMetadata() const override {
    int chars = left->totalCharacters() + 1 + op.length() + 1 +
                right->totalCharacters();
    bool hn = left->containsHardNewline() || right->containsHardNewline();
    setMetadata(chars, hn);
  }

  bool isBlockLike() const override { return right->isBlockLike(); }

  std::optional<State> fixedState(int pageWidth) const override {
    if (isBlockLike()) {
      return std::nullopt;
    }
    if (containsHardNewline() || totalCharacters() > pageWidth) {
      return State::Split;
    }
    return std::nullopt;
  }

  std::vector<State> additionalStates() const override {
    return {State::Split};
  }

  int stateCost(const State &state) const override {
    if (state == State::Split) {
      return isBlockLike() ? Cost::AssignBlock : Cost::Assign;
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

  void computeMetadata() const override {
    int chars = 0;
    bool hn = false;
    for (const auto &e : elements) {
      chars += e->totalCharacters();
      hn = hn || e->containsHardNewline();
    }
    if (elements.size() > 1) {
      hn = true;
    }
    setMetadata(chars, hn);
  }

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
  explicit BlockPiece(std::vector<Piece *> statements, bool hasBraces = true)
      : statements(std::move(statements)), hasBraces(hasBraces) {}

  void computeMetadata() const override {
    int chars = hasBraces ? 2 : 0;
    bool hn = false;
    for (const auto &s : statements) {
      chars += s->totalCharacters();
      hn = hn || s->containsHardNewline();
    }
    if (!statements.empty()) {
      hn = true;
    }
    setMetadata(chars, hn);
  }

  bool isBlockLike() const override { return hasBraces; }

  void format(CodeWriter &writer, const State &state,
              const std::function<void(const Piece *, State)> &formatChild)
      const override {
    if (hasBraces) {
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
    } else {
      for (size_t i = 0; i < statements.size(); ++i) {
        formatChild(statements[i], State::Unsplit);
        if (i < statements.size() - 1) {
          writer.newline();
        }
      }
    }
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    for (auto *s : statements)
      callback(s);
  }

private:
  std::vector<Piece *> statements;
  bool hasBraces;
};

class ListPiece : public Piece {
public:
  ListPiece(Piece *openBracket, std::vector<Piece *> elements,
            Piece *closeBracket, bool forceSplit = false)
      : openBracket(openBracket), elements(std::move(elements)),
        closeBracket(closeBracket), forceSplit(forceSplit) {}

  void computeMetadata() const override {
    int chars = 0;
    bool hn = forceSplit;
    if (openBracket) {
      chars += openBracket->totalCharacters();
      hn = hn || openBracket->containsHardNewline();
    }
    for (size_t i = 0; i < elements.size(); ++i) {
      chars += elements[i]->totalCharacters();
      hn = hn || elements[i]->containsHardNewline();
      if (i < elements.size() - 1) {
        chars += 2;
      }
    }
    if (closeBracket) {
      chars += closeBracket->totalCharacters();
      hn = hn || closeBracket->containsHardNewline();
    }
    setMetadata(chars, hn);
  }

  std::optional<State> fixedState(int pageWidth) const override {
    if (elements.empty()) {
      if (forceSplit)
        return State::Split;
      return State::Unsplit;
    }
    if (forceSplit || containsHardNewline() || totalCharacters() > pageWidth) {
      return State::Split;
    }
    return std::nullopt;
  }

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
  bool forceSplit;
};

class CallPiece : public Piece {
public:
  CallPiece(Piece *target, Piece *argsList)
      : target(target), argsList(argsList) {}

  void computeMetadata() const override {
    int chars = target->totalCharacters() + argsList->totalCharacters();
    bool hn = target->containsHardNewline() || argsList->containsHardNewline();
    setMetadata(chars, hn);
  }

  bool isBlockLike() const override { return argsList->isBlockLike(); }

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

  void computeMetadata() const override {
    int chars = left->totalCharacters() + 1 + op.length() + 1 +
                right->totalCharacters();
    bool hn = left->containsHardNewline() || right->containsHardNewline();
    setMetadata(chars, hn);
  }

  bool isBlockLike() const override { return right->isBlockLike(); }

  std::optional<State> fixedState(int pageWidth) const override {
    if (isBlockLike()) {
      return std::nullopt;
    }
    if (containsHardNewline() || totalCharacters() > pageWidth) {
      return State::Split;
    }
    return std::nullopt;
  }

  std::vector<State> additionalStates() const override {
    return {State::Split};
  }

  int stateCost(const State &state) const override {
    if (state == State::Split) {
      return isBlockLike() ? Cost::AssignBlock : Cost::Normal;
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
                   Piece *elseBody = nullptr, bool bodyOnNewLine = false,
                   bool elseBodyOnNewLine = false)
      : keyword(std::move(keyword)), condition(condition), body(body),
        elseBody(elseBody), bodyOnNewLine(bodyOnNewLine),
        elseBodyOnNewLine(elseBodyOnNewLine) {}

  void computeMetadata() const override {
    int chars = keyword.length();
    bool hn = false;
    if (condition) {
      chars += 2 + condition->totalCharacters();
      hn = hn || condition->containsHardNewline();
    }
    if (body) {
      chars += 1 + body->totalCharacters();
      hn = hn || body->containsHardNewline();
      if (bodyOnNewLine)
        hn = true;
    }
    if (elseBody) {
      chars += 5 + elseBody->totalCharacters();
      hn = hn || elseBody->containsHardNewline();
      if (bodyOnNewLine || elseBodyOnNewLine)
        hn = true;
    }
    setMetadata(chars, hn);
  }

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
      if (bodyOnNewLine) {
        writer.pushIndent(Indent::Block);
        writer.newline();
        formatChild(body, State::Unsplit);
        writer.popIndent();
      } else {
        writer.space();
        formatChild(body, State::Unsplit);
      }
    }
    if (elseBody) {
      if (bodyOnNewLine) {
        writer.newline();
      } else {
        writer.space();
      }
      writer.write("else");

      if (elseBodyOnNewLine) {
        writer.pushIndent(Indent::Block);
        writer.newline();
        formatChild(elseBody, State::Unsplit);
        writer.popIndent();
      } else {
        writer.space();
        formatChild(elseBody, State::Unsplit);
      }
    }
  }

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    if (condition)
      callback(condition);
    if (body)
      callback(body);
    if (elseBody)
      callback(elseBody);
  }

private:
  std::string keyword;
  Piece *condition;
  Piece *body;
  Piece *elseBody;
  bool bodyOnNewLine;
  bool elseBodyOnNewLine;
};

class CasePiece : public Piece {
public:
  CasePiece(Piece *label, std::vector<Piece *> statements)
      : label(label), statements(std::move(statements)) {}

  void computeMetadata() const override {
    int chars = label->totalCharacters();
    bool hn = label->containsHardNewline();
    for (const auto &s : statements) {
      chars += s->totalCharacters();
      hn = hn || s->containsHardNewline();
    }
    if (!statements.empty())
      hn = true;
    setMetadata(chars, hn);
  }

  bool isBlockLike() const override { return true; }

  void format(CodeWriter &writer, const State &state,
              const std::function<void(const Piece *, State)> &formatChild)
      const override {
    formatChild(label, State::Unsplit);
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

/**
 * Encapsulates a subtree as an atomic chunk that formats itself using an
 * independent Solver. Hides child complexity from the outer Solver bounds to
 * prevent combinatorial state explosion.
 */
class IndependentPiece : public Piece {
public:
  Piece *child;
  int pageWidth;
  mutable std::unordered_map<int, std::string> cache;

  IndependentPiece(Piece *child, int pageWidth)
      : child(child), pageWidth(pageWidth) {}

  void computeMetadata() const override {
    child->ensureMetadata();
    setMetadata(child->totalCharacters(), child->containsHardNewline());
  }

  bool isBlockLike() const override { return child->isBlockLike(); }

  void format(CodeWriter &writer, const State &state,
              const std::function<void(const Piece *, State)> &formatChild)
      const override;

  void forEachChild(
      const std::function<void(const Piece *)> &callback) const override {
    /* Intentionally left blank to hide children from outer Solver permutations
     */
  }
};

} // namespace utopia