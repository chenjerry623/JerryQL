#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace jerryql {

enum class TokenKind { Identifier, Integer, String, Symbol, Error, End };

struct Token {
    TokenKind kind;
    std::string text;  // identifier/symbol as written, digits, unescaped string body,
                       // or the message for an Error token
    size_t offset;     // byte offset of the token in the source
};

// Splits SQL text into tokens. Keywords come out as identifiers; the parser
// decides which identifiers are keywords. Always ends with an End token.
// Throws SqlError on an unterminated string or an unexpected character.
std::vector<Token> tokenize(const std::string& sql);

// Like tokenize(), but reports bad input as Error tokens instead of throwing,
// so the parser can fail just the statement that contains them.
std::vector<Token> tokenizeLenient(const std::string& sql);

}  // namespace jerryql
