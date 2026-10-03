#include "lexer.h"

#include <cctype>

#include "sql_error.h"

namespace jerryql {

namespace {

bool isIdentStart(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalpha(u) || c == '_';
}

bool isIdentChar(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_';
}

bool isDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

bool isSpace(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

class Lexer {
public:
    explicit Lexer(const std::string& sql) : sql_(sql) {}

    std::vector<Token> run() {
        std::vector<Token> tokens;
        while (true) {
            skipWhitespaceAndComments();
            if (pos_ >= sql_.size()) break;
            try {
                tokens.push_back(nextToken());
            } catch (const SqlError& e) {
                tokens.push_back({TokenKind::Error, e.what(), errorStart_});
            }
        }
        tokens.push_back({TokenKind::End, "", sql_.size()});
        return tokens;
    }

private:
    const std::string& sql_;
    size_t pos_ = 0;
    size_t errorStart_ = 0;  // start of the token being read, for Error tokens

    char peekChar(size_t ahead = 0) const {
        return pos_ + ahead < sql_.size() ? sql_[pos_ + ahead] : '\0';
    }

    void skipWhitespaceAndComments() {
        while (pos_ < sql_.size()) {
            if (isSpace(sql_[pos_])) {
                ++pos_;
            } else if (peekChar() == '-' && peekChar(1) == '-') {
                while (pos_ < sql_.size() && sql_[pos_] != '\n') ++pos_;
            } else {
                break;
            }
        }
    }

    Token nextToken() {
        size_t start = pos_;
        errorStart_ = start;
        char c = sql_[pos_];
        if (isIdentStart(c)) {
            while (pos_ < sql_.size() && isIdentChar(sql_[pos_])) ++pos_;
            return {TokenKind::Identifier, sql_.substr(start, pos_ - start), start};
        }
        if (isDigit(c)) {
            while (pos_ < sql_.size() && isDigit(sql_[pos_])) ++pos_;
            if (pos_ < sql_.size() && isIdentStart(sql_[pos_])) {
                while (pos_ < sql_.size() && isIdentChar(sql_[pos_])) ++pos_;
                throw SqlError("invalid number at position " + std::to_string(start));
            }
            return {TokenKind::Integer, sql_.substr(start, pos_ - start), start};
        }
        if (c == '\'') return stringLiteral();
        return symbol();
    }

    // 'it''s' -> it's
    Token stringLiteral() {
        size_t start = pos_;
        ++pos_;  // opening quote
        std::string body;
        while (true) {
            if (pos_ >= sql_.size()) {
                throw SqlError("unterminated string starting at position " + std::to_string(start));
            }
            char c = sql_[pos_++];
            if (c != '\'') {
                body += c;
            } else if (peekChar() == '\'') {
                body += '\'';
                ++pos_;
            } else {
                break;
            }
        }
        return {TokenKind::String, body, start};
    }

    Token symbol() {
        size_t start = pos_;
        static const char* const twoChar[] = {"<=", ">=", "<>", "!="};
        for (const char* op : twoChar) {
            if (peekChar() == op[0] && peekChar(1) == op[1]) {
                pos_ += 2;
                return {TokenKind::Symbol, op, start};
            }
        }
        static const std::string oneChar = "(),;*+-/=<>";
        char c = sql_[pos_];
        if (oneChar.find(c) == std::string::npos) {
            ++pos_;
            throw SqlError(std::string("unexpected character '") + c + "' at position " +
                           std::to_string(start));
        }
        ++pos_;
        return {TokenKind::Symbol, std::string(1, c), start};
    }
};

}  // namespace

std::vector<Token> tokenizeLenient(const std::string& sql) {
    return Lexer(sql).run();
}

std::vector<Token> tokenize(const std::string& sql) {
    std::vector<Token> tokens = tokenizeLenient(sql);
    for (const Token& token : tokens) {
        if (token.kind == TokenKind::Error) throw SqlError(token.text);
    }
    return tokens;
}

}  // namespace jerryql
