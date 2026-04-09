#include "utopia/Formatter/Formatter.hpp"
#include <sstream>

namespace utopia {

Formatter::Formatter(const std::vector<Token> &tokens, int tabSize)
    : tokens(tokens), tabSize(tabSize) {}

void Formatter::appendIndent() {
  for (int i = 0; i < indentLevel * tabSize; ++i) {
    result += " ";
  }
}

/**
 * EVIL POINTER HEURISTICS
 * * Determines if a '*' is a pointer declaration or a dirty multiplication.
 * We look at the preceding token to decide if we are in a type context.
 */
bool Formatter::isPointerStar(size_t index) {
  if (index == 0)
    return false;
  TokenType prev = tokens[index - 1].type;

  // If preceded by a type or another pointer, it's definitely a pointer
  if (isTypeToken(prev) || prev == TokenType::STAR ||
      prev == TokenType::IDENTIFIER) {
    // This is a bit of a gamble with IDENTIFIER, but in Utopia/C++
    // a type name is an identifier.
    return true;
  }
  return false;
}

bool Formatter::isTypeToken(TokenType type) {
  switch (type) {
  case TokenType::KW_INT:
  case TokenType::KW_UINT:
  case TokenType::KW_FLOAT:
  case TokenType::KW_DOUBLE:
  case TokenType::KW_CHAR:
  case TokenType::KW_VOID:
  case TokenType::KW_BOOL:
    return true;
  default:
    return false;
  }
}

bool Formatter::isBinaryOp(TokenType type) {
  switch (type) {
  case TokenType::PLUS:
  case TokenType::MINUS:
  case TokenType::SLASH:
  case TokenType::PERCENT:
  case TokenType::EQ:
  case TokenType::NEQ:
  case TokenType::LT:
  case TokenType::GT:
  case TokenType::LTE:
  case TokenType::GTE:
  case TokenType::AND:
  case TokenType::OR:
  case TokenType::ASSIGN:
    return true;
  default:
    return false;
  }
}

std::string Formatter::format() {
  result.clear();
  indentLevel = 0;

  for (size_t i = 0; i < tokens.size(); ++i) {
    const Token &tok = tokens[i];

    // Handle leading documentation/comments
    if (!tok.leadingDoc.empty()) {
      if (!result.empty() && result.back() != '\n')
        result += "\n";
      std::stringstream ss(tok.leadingDoc);
      std::string line;
      while (std::getline(ss, line)) {
        appendIndent();
        result += "// " + line + "\n";
      }
    }

    if (tok.type == TokenType::EOF_TOK)
      break;

    // Indentation logic for start of lines
    if (result.empty() || result.back() == '\n') {
      if (tok.type != TokenType::RBRACE) {
        appendIndent();
      }
    }

    switch (tok.type) {
    case TokenType::LBRACE:
      if (!result.empty() && result.back() != ' ')
        result += " ";
      result += "{\n";
      indentLevel++;
      break;

    case TokenType::RBRACE:
      indentLevel--;
      if (result.back() != '\n')
        result += "\n";
      appendIndent();
      result += "}";
      if (i + 1 < tokens.size() && tokens[i + 1].type != TokenType::SEMICOLON) {
        result += "\n";
      }
      break;

    case TokenType::SEMICOLON:
      result += ";\n";
      break;

    case TokenType::STAR:
      if (isPointerStar(i)) {
        // C++ style: Type* name
        result += "*";
        if (i + 1 < tokens.size() && tokens[i + 1].type != TokenType::STAR &&
            tokens[i + 1].type != TokenType::SEMICOLON &&
            tokens[i + 1].type != TokenType::RPAREN) {
          result += " ";
        }
      } else {
        // Math style: a * b
        if (result.back() != ' ')
          result += " ";
        result += "* ";
      }
      break;

    case TokenType::KW_IF:
    case TokenType::KW_WHILE:
    case TokenType::KW_FOR:
    //case TokenType::KW_SWITCH:
    //  result += tok.value + " ";
    //  break;
    case TokenType::COMMA:
      result += ", ";
      break;
    //case TokenType::COLON:
    //  result += ": ";
    //  break;
    default:
      if (isBinaryOp(tok.type)) {
        if (result.back() != ' ')
          result += " ";
        result += tok.value + " ";
      } else if (tok.type == TokenType::IDENTIFIER || isTypeToken(tok.type)) {
        // Avoid sticking identifiers together
        if (!result.empty() && std::isalnum(result.back())) {
          result += " ";
        }
        result += tok.value;
      } else {
        result += tok.value;
      }
      break;
    }
  }

  return result;
}

} // namespace utopia