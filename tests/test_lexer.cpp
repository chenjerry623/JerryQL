#include "check.h"
#include "lexer.h"
#include "sql_error.h"

using namespace jerryql;

namespace {

std::vector<std::string> texts(const std::string& sql) {
    std::vector<std::string> out;
    for (const Token& token : tokenize(sql)) out.push_back(token.text);
    return out;
}

}  // namespace

TEST(lexerSplitsSymbolsAndWords) {
    auto tokens = tokenize("SELECT a,b FROM t WHERE a<=10;");
    CHECK_EQ(tokens.size(), size_t(12));
    CHECK(tokens[0].kind == TokenKind::Identifier);
    CHECK(tokens[8].kind == TokenKind::Symbol);
    CHECK_EQ(tokens[8].text, std::string("<="));
    CHECK(tokens[9].kind == TokenKind::Integer);
    CHECK(tokens.back().kind == TokenKind::End);
}

TEST(lexerReadsTwoCharOperators) {
    auto t = texts("a<>b!=c>=d");
    CHECK_EQ(t[1], std::string("<>"));
    CHECK_EQ(t[3], std::string("!="));
    CHECK_EQ(t[5], std::string(">="));
}

TEST(lexerUnescapesStrings) {
    auto tokens = tokenize("'it''s here'");
    CHECK(tokens[0].kind == TokenKind::String);
    CHECK_EQ(tokens[0].text, std::string("it's here"));
}

TEST(lexerSkipsComments) {
    CHECK_EQ(texts("SELECT -- comment ; here\n 1").size(), size_t(3));
}

TEST(lexerRecordsOffsets) {
    auto tokens = tokenize("  SELECT x");
    CHECK_EQ(tokens[0].offset, size_t(2));
    CHECK_EQ(tokens[1].offset, size_t(9));
}

TEST(lexerRejectsBadInput) {
    CHECK_THROWS(tokenize("'open"), SqlError, "unterminated string");
    CHECK_THROWS(tokenize("a # b"), SqlError, "unexpected character '#'");
    CHECK_THROWS(tokenize("12abc"), SqlError, "invalid number");
}
