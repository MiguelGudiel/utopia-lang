#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

namespace utopia {

enum class TokenType : uint8_t {
  EOF_TOK,
  IDENTIFIER,
  NUMBER,
  CHAR_LITERAL,
  RUNE_LITERAL,
  STRING_LITERAL,
  TYPE_KW,
  CONST_KW,
  ANNOTATION_KW,
  STATIC_KW,
  REQUIRED_KW,
  PUBLIC_KW,  /* public */
  PRIVATE_KW, /* private */
  FOR_KW,
  WHILE_KW,
  IF_KW,
  ELSE_KW,
  SWITCH_KW,   /*switch*/
  CASE_KW,     /*case:*/
  DEFAULT_KW,  /*default:*/
  BREAK_KW,    /*break*/
  CONTINUE_KW, /*continue*/
  ELLIPSIS,
  AS,
  PLUS,
  MINUS,
  STAR,
  SLASH,
  PERCENT, // %
  AMPERSAND,
  PIPE,   // |
  CARET,  // ^
  LSHIFT, // <<
  RSHIFT, // >>
  LOGICAL_AND,
  LOGICAL_OR,
  EQ,
  NEQ,
  LT,
  GT,
  LE,
  GE,
  BANG,
  AT,
  STRUCT_KW,
  UNION_KW, /* union */
  CLASS_KW,
  THIS_KW,
  DOT,
  TILDE,
  ASSIGN,
  PLUS_EQ,      // +=
  MINUS_EQ,     // -=
  STAR_EQ,      // *=
  SLASH_EQ,     // /=
  PERCENT_EQ,   // %=
  AMPERSAND_EQ, // &=
  PIPE_EQ,      // |=
  CARET_EQ,     // ^=
  LSHIFT_EQ,    // <<=
  RSHIFT_EQ,    // >>=
  ARROW,
  COLON,
  QUESTION, /* ? */
  TRUE_KW,
  FALSE_KW,
  LPAREN,
  RPAREN,
  LBRACE,
  RBRACE,
  LBRACKET,   /* [ */
  RBRACKET,   /* ] */
  NEW_KW,     /* new */
  DELETE_KW,  /* delete */
  NULL_KW,    /* null */
  TYPEDEF_KW, /* typedef */
  ENUM_KW,    /* enum */
  FUNCTION_KW,
  OPERATOR_KW, /* operator */
  COMMA,
  SEMICOLON,
  RETURN,
  COMMENT,
  PLUS_PLUS,   /*++*/
  MINUS_MINUS, /*--*/
  UNKNOWN
};

/**
 * zero-copy slice of the input source buffer.
 */
struct Token {
  TokenType type;
  std::string_view value;
  int line;
  int column;
  std::vector<std::string_view> leadingComments;
  std::string_view trailingComment;
};

} // namespace utopia