#include "parser.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>

#include "sql_error.h"

namespace jerryql {

namespace {

std::string toUpper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isReservedWord(const std::string& word) {
    static const std::set<std::string> reserved = {
        "AND",    "AS",      "ASC",   "BEGIN",   "BY",     "COMMIT", "CREATE", "DELETE", "DESC",  "DROP",
        "EXPLAIN", "FROM",   "GROUP",  "HAVING", "INDEX",   "INSERT", "INT",    "INTEGER", "INTO",  "KEY",   "LIMIT",
        "NOT",    "OFFSET",  "ON",     "OR",      "ORDER", "PRIMARY", "ROLLBACK", "SELECT", "SET",    "TABLE", "TEXT",
        "TRANSACTION",
        "UPDATE", "VALUES",  "WHERE"};
    return reserved.count(toUpper(word)) > 0;
}

std::string describeToken(const Token& token) {
    switch (token.kind) {
        case TokenKind::End: return "end of input";
        case TokenKind::String: return "'" + token.text + "'";
        default: return "'" + token.text + "'";
    }
}

// Converts a run of digits to int64, allowing -9223372036854775808 when negated.
int64_t parseIntegerLiteral(const Token& token, bool negative) {
    const uint64_t limit = negative ? uint64_t(std::numeric_limits<int64_t>::max()) + 1
                                    : uint64_t(std::numeric_limits<int64_t>::max());
    uint64_t magnitude = 0;
    for (char c : token.text) {
        uint64_t digit = uint64_t(c - '0');
        if (magnitude > (limit - digit) / 10) {
            throw SqlError("integer literal out of range: " + std::string(negative ? "-" : "") +
                           token.text);
        }
        magnitude = magnitude * 10 + digit;
    }
    if (!negative) return int64_t(magnitude);
    if (magnitude == uint64_t(std::numeric_limits<int64_t>::max()) + 1) {
        return std::numeric_limits<int64_t>::min();
    }
    return -int64_t(magnitude);
}

}  // namespace

Parser::Parser(const std::string& sql) : sql_(sql), tokens_(tokenizeLenient(sql)) {}

// ---------- Token helpers ----------

const Token& Parser::peek(size_t ahead) const {
    return tokens_[std::min(pos_ + ahead, tokens_.size() - 1)];
}

const Token& Parser::advance() {
    const Token& token = peek();
    if (token.kind != TokenKind::End) ++pos_;
    return token;
}

bool Parser::isKeyword(const char* keyword, size_t ahead) const {
    const Token& token = peek(ahead);
    return token.kind == TokenKind::Identifier && toUpper(token.text) == keyword;
}

bool Parser::acceptKeyword(const char* keyword) {
    if (!isKeyword(keyword)) return false;
    advance();
    return true;
}

void Parser::expectKeyword(const char* keyword) {
    if (!acceptKeyword(keyword)) fail(keyword);
}

bool Parser::isSymbol(const char* symbol) const {
    return peek().kind == TokenKind::Symbol && peek().text == symbol;
}

bool Parser::acceptSymbol(const char* symbol) {
    if (!isSymbol(symbol)) return false;
    advance();
    return true;
}

void Parser::expectSymbol(const char* symbol) {
    if (!acceptSymbol(symbol)) fail(std::string("'") + symbol + "'");
}

// Table and column names are case-insensitive, so they're stored lowercased.
std::string Parser::expectName(const char* what) {
    const Token& token = peek();
    if (token.kind != TokenKind::Identifier || isReservedWord(token.text)) fail(what);
    return toLower(advance().text);
}

int64_t Parser::expectInteger(const char* what) {
    if (peek().kind != TokenKind::Integer) fail(what);
    return parseIntegerLiteral(advance(), false);
}

// No parse rule accepts an Error token, so every lexer error surfaces here.
void Parser::fail(const std::string& expected) const {
    const Token& token = peek();
    if (token.kind == TokenKind::Error) throw SqlError(token.text);
    throw SqlError("syntax error at position " + std::to_string(token.offset) + ": expected " +
                   expected + " but found " + describeToken(token));
}

// ---------- Statements ----------

bool Parser::atEnd() {
    while (acceptSymbol(";")) {
    }
    return peek().kind == TokenKind::End;
}

ParsedStatement Parser::next() {
    atEnd();  // skip empty statements
    statementStart_ = peek().offset;
    Statement statement = parseStatement();
    if (!isSymbol(";") && peek().kind != TokenKind::End) fail("';' or end of statement");
    std::string text = sourceUpTo(peek().offset);
    acceptSymbol(";");
    return {std::move(statement), text};
}

std::string Parser::skipToNextStatement() {
    while (peek().kind != TokenKind::End && !isSymbol(";")) advance();
    std::string text = sourceUpTo(peek().offset);
    acceptSymbol(";");
    return text;
}

std::string Parser::sourceUpTo(size_t end) const {
    std::string text = sql_.substr(statementStart_, end - statementStart_);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    return text;
}

Statement Parser::parseStatement() {
    if (isKeyword("CREATE") && isKeyword("INDEX", 1)) return parseCreateIndex();
    if (isKeyword("CREATE")) return parseCreateTable();
    if (isKeyword("DROP") && isKeyword("INDEX", 1)) return parseDropIndex();
    if (isKeyword("DROP")) return parseDropTable();
    if (isKeyword("INSERT")) return parseInsert();
    if (isKeyword("SELECT") || isKeyword("EXPLAIN")) return parseSelect();
    if (isKeyword("UPDATE")) return parseUpdate();
    if (isKeyword("DELETE")) return parseDelete();
    if (isKeyword("BEGIN") || isKeyword("COMMIT") || isKeyword("ROLLBACK")) return parseTransaction();
    fail("a statement (CREATE, DROP, INSERT, SELECT, EXPLAIN, UPDATE, DELETE, BEGIN, COMMIT, ROLLBACK)");
}

TransactionStmt Parser::parseTransaction() {
    TransactionStmt stmt;
    if (acceptKeyword("BEGIN")) {
        stmt.action = TransactionAction::Begin;
    } else if (acceptKeyword("COMMIT")) {
        stmt.action = TransactionAction::Commit;
    } else {
        expectKeyword("ROLLBACK");
        stmt.action = TransactionAction::Rollback;
    }
    acceptKeyword("TRANSACTION");
    return stmt;
}

CreateTableStmt Parser::parseCreateTable() {
    expectKeyword("CREATE");
    expectKeyword("TABLE");
    CreateTableStmt stmt;
    stmt.table = expectName("table name");
    expectSymbol("(");
    do {
        stmt.columns.push_back(parseColumnDef());
    } while (acceptSymbol(","));
    expectSymbol(")");
    return stmt;
}

ColumnDef Parser::parseColumnDef() {
    ColumnDef def;
    def.name = expectName("column name");
    if (acceptKeyword("INT") || acceptKeyword("INTEGER")) {
        def.type = Type::Int;
    } else if (acceptKeyword("TEXT")) {
        def.type = Type::Text;
    } else {
        fail("column type (INT or TEXT)");
    }
    if (acceptKeyword("PRIMARY")) {
        expectKeyword("KEY");
        def.primaryKey = true;
    }
    return def;
}

CreateIndexStmt Parser::parseCreateIndex() {
    expectKeyword("CREATE");
    expectKeyword("INDEX");
    CreateIndexStmt stmt;
    stmt.index = expectName("index name");
    expectKeyword("ON");
    stmt.table = expectName("table name");
    expectSymbol("(");
    stmt.column = expectName("column name");
    if (isSymbol(",")) fail("')' (indexes cover one column)");
    expectSymbol(")");
    return stmt;
}

DropIndexStmt Parser::parseDropIndex() {
    expectKeyword("DROP");
    expectKeyword("INDEX");
    return DropIndexStmt{expectName("index name")};
}

DropTableStmt Parser::parseDropTable() {
    expectKeyword("DROP");
    expectKeyword("TABLE");
    return DropTableStmt{expectName("table name")};
}

InsertStmt Parser::parseInsert() {
    expectKeyword("INSERT");
    expectKeyword("INTO");
    InsertStmt stmt;
    stmt.table = expectName("table name");
    if (acceptSymbol("(")) {
        do {
            stmt.columns.push_back(expectName("column name"));
        } while (acceptSymbol(","));
        expectSymbol(")");
    }
    expectKeyword("VALUES");
    do {
        stmt.rows.push_back(parseValuesTuple());
    } while (acceptSymbol(","));
    return stmt;
}

std::vector<ExprPtr> Parser::parseValuesTuple() {
    std::vector<ExprPtr> values;
    expectSymbol("(");
    do {
        values.push_back(parseExpr());
    } while (acceptSymbol(","));
    expectSymbol(")");
    return values;
}

SelectStmt Parser::parseSelect() {
    SelectStmt stmt;
    stmt.explain = acceptKeyword("EXPLAIN");
    expectKeyword("SELECT");
    if (!acceptSymbol("*")) {
        do {
            stmt.items.push_back(parseSelectItem());
        } while (acceptSymbol(","));
    }
    expectKeyword("FROM");
    stmt.table = expectName("table name");
    if (acceptKeyword("WHERE")) stmt.where = parseExpr();
    if (acceptKeyword("GROUP")) {
        expectKeyword("BY");
        do {
            stmt.groupBy.push_back(parseExpr());
        } while (acceptSymbol(","));
    }
    if (acceptKeyword("HAVING")) stmt.having = parseExpr();
    if (acceptKeyword("ORDER")) {
        expectKeyword("BY");
        do {
            OrderItem item;
            item.expr = parseExpr();
            if (acceptKeyword("DESC")) {
                item.descending = true;
            } else {
                acceptKeyword("ASC");
            }
            stmt.orderBy.push_back(std::move(item));
        } while (acceptSymbol(","));
    }
    if (acceptKeyword("LIMIT")) {
        stmt.limit = expectInteger("a non-negative integer after LIMIT");
        if (acceptKeyword("OFFSET")) stmt.offset = expectInteger("a non-negative integer after OFFSET");
    }
    return stmt;
}

SelectItem Parser::parseSelectItem() {
    SelectItem item;
    item.expr = parseExpr();
    if (acceptKeyword("AS")) item.alias = expectName("alias");
    return item;
}

UpdateStmt Parser::parseUpdate() {
    expectKeyword("UPDATE");
    UpdateStmt stmt;
    stmt.table = expectName("table name");
    expectKeyword("SET");
    do {
        std::string column = expectName("column name");
        expectSymbol("=");
        stmt.assignments.emplace_back(column, parseExpr());
    } while (acceptSymbol(","));
    if (acceptKeyword("WHERE")) stmt.where = parseExpr();
    return stmt;
}

DeleteStmt Parser::parseDelete() {
    expectKeyword("DELETE");
    expectKeyword("FROM");
    DeleteStmt stmt;
    stmt.table = expectName("table name");
    if (acceptKeyword("WHERE")) stmt.where = parseExpr();
    return stmt;
}

// ---------- Expressions ----------

ExprPtr Parser::parseExpr() {
    return parseOr();
}

ExprPtr Parser::parseOr() {
    ExprPtr left = parseAnd();
    while (acceptKeyword("OR")) left = makeBinary(BinaryOp::Or, std::move(left), parseAnd());
    return left;
}

ExprPtr Parser::parseAnd() {
    ExprPtr left = parseNot();
    while (acceptKeyword("AND")) left = makeBinary(BinaryOp::And, std::move(left), parseNot());
    return left;
}

ExprPtr Parser::parseNot() {
    if (acceptKeyword("NOT")) return makeUnary(UnaryOp::Not, parseNot());
    return parseComparison();
}

// Comparisons don't chain: "a < b < c" is a syntax error, as in standard SQL.
ExprPtr Parser::parseComparison() {
    ExprPtr left = parseAdditive();
    static const std::pair<const char*, BinaryOp> ops[] = {
        {"=", BinaryOp::Eq}, {"<>", BinaryOp::Ne}, {"!=", BinaryOp::Ne}, {"<", BinaryOp::Lt},
        {"<=", BinaryOp::Le}, {">", BinaryOp::Gt}, {">=", BinaryOp::Ge}};
    for (const auto& [symbol, op] : ops) {
        if (acceptSymbol(symbol)) return makeBinary(op, std::move(left), parseAdditive());
    }
    return left;
}

ExprPtr Parser::parseAdditive() {
    ExprPtr left = parseMultiplicative();
    while (true) {
        if (acceptSymbol("+")) {
            left = makeBinary(BinaryOp::Add, std::move(left), parseMultiplicative());
        } else if (acceptSymbol("-")) {
            left = makeBinary(BinaryOp::Subtract, std::move(left), parseMultiplicative());
        } else {
            return left;
        }
    }
}

ExprPtr Parser::parseMultiplicative() {
    ExprPtr left = parseUnary();
    while (true) {
        if (acceptSymbol("*")) {
            left = makeBinary(BinaryOp::Multiply, std::move(left), parseUnary());
        } else if (acceptSymbol("/")) {
            left = makeBinary(BinaryOp::Divide, std::move(left), parseUnary());
        } else {
            return left;
        }
    }
}

ExprPtr Parser::parseUnary() {
    if (acceptSymbol("-")) {
        // Fold "-<digits>" into one literal so INT64_MIN can be written.
        if (peek().kind == TokenKind::Integer) {
            return makeLiteral(Value::integer(parseIntegerLiteral(advance(), true)));
        }
        return makeUnary(UnaryOp::Negate, parseUnary());
    }
    acceptSymbol("+");
    return parsePrimary();
}

ExprPtr Parser::parsePrimary() {
    const Token& token = peek();
    if (token.kind == TokenKind::Integer) {
        return makeLiteral(Value::integer(parseIntegerLiteral(advance(), false)));
    }
    if (token.kind == TokenKind::String) return makeLiteral(Value::text(advance().text));
    if (acceptSymbol("(")) {
        ExprPtr inner = parseExpr();
        expectSymbol(")");
        return inner;
    }
    if (token.kind == TokenKind::Identifier && !isReservedWord(token.text)) {
        if (peek(1).kind == TokenKind::Symbol && peek(1).text == "(") return parseAggregateCall();
        return makeColumn(toLower(advance().text));
    }
    fail("an expression");
}

// COUNT(*), COUNT(expr), SUM(expr), MIN(expr), MAX(expr), AVG(expr).
ExprPtr Parser::parseAggregateCall() {
    static const std::pair<const char*, AggregateFunc> functions[] = {
        {"COUNT", AggregateFunc::Count}, {"SUM", AggregateFunc::Sum}, {"MIN", AggregateFunc::Min},
        {"MAX", AggregateFunc::Max},     {"AVG", AggregateFunc::Avg}};
    std::string name = toUpper(peek().text);
    for (const auto& [functionName, func] : functions) {
        if (name != functionName) continue;
        advance();
        expectSymbol("(");
        ExprPtr argument;
        if (func == AggregateFunc::Count && acceptSymbol("*")) {
            // COUNT(*): no argument
        } else {
            argument = parseExpr();
        }
        expectSymbol(")");
        return makeAggregate(func, std::move(argument));
    }
    fail("COUNT, SUM, MIN, MAX or AVG (no other functions are supported)");
}

Statement parseOne(const std::string& sql) {
    Parser parser(sql);
    if (parser.atEnd()) throw SqlError("empty statement");
    Statement statement = parser.next().statement;
    if (!parser.atEnd()) throw SqlError("expected a single statement");
    return statement;
}

}  // namespace jerryql
