#pragma once

#include <string>
#include <vector>

#include "ast.h"
#include "lexer.h"

namespace jerryql {

struct ParsedStatement {
    Statement statement{DropTableStmt{}};
    std::string text;  // source text of the statement, without the trailing ';'
};

// Recursive-descent parser for JerryQL's SQL subset. Parses one statement at a
// time so a syntax error in one statement doesn't stop the rest of a script:
//
//   Parser parser(sql);
//   while (!parser.atEnd()) {
//       try { auto stmt = parser.next(); ... }
//       catch (const SqlError&) { parser.skipToNextStatement(); }
//   }
class Parser {
public:
    explicit Parser(const std::string& sql);

    bool atEnd();
    ParsedStatement next();
    // Skips past the next ';' and returns the failed statement's text.
    std::string skipToNextStatement();

private:
    const std::string& sql_;
    std::vector<Token> tokens_;
    size_t pos_ = 0;
    size_t statementStart_ = 0;  // offset where the current statement began

    std::string sourceUpTo(size_t end) const;

    // Token helpers
    const Token& peek(size_t ahead = 0) const;
    const Token& advance();
    bool isKeyword(const char* keyword, size_t ahead = 0) const;
    bool acceptKeyword(const char* keyword);
    void expectKeyword(const char* keyword);
    bool isSymbol(const char* symbol) const;
    bool acceptSymbol(const char* symbol);
    void expectSymbol(const char* symbol);
    std::string expectName(const char* what);
    int64_t expectInteger(const char* what);
    [[noreturn]] void fail(const std::string& expected) const;

    // Statements
    Statement parseStatement();
    CreateTableStmt parseCreateTable();
    ColumnDef parseColumnDef();
    DropTableStmt parseDropTable();
    InsertStmt parseInsert();
    std::vector<ExprPtr> parseValuesTuple();
    SelectStmt parseSelect();
    SelectItem parseSelectItem();
    UpdateStmt parseUpdate();
    DeleteStmt parseDelete();
    TransactionStmt parseTransaction();

    // Expressions, lowest to highest precedence
    ExprPtr parseExpr();
    ExprPtr parseOr();
    ExprPtr parseAnd();
    ExprPtr parseNot();
    ExprPtr parseComparison();
    ExprPtr parseAdditive();
    ExprPtr parseMultiplicative();
    ExprPtr parseUnary();
    ExprPtr parsePrimary();
};

// Parses exactly one statement (a trailing ';' is allowed). Throws SqlError.
Statement parseOne(const std::string& sql);

}  // namespace jerryql
