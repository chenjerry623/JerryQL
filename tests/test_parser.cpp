#include <limits>

#include "check.h"
#include "parser.h"
#include "sql_error.h"

using namespace jerryql;

namespace {

template <typename T>
T parseAs(const std::string& sql) {
    Statement statement = parseOne(sql);
    T* typed = std::get_if<T>(&statement);
    if (!typed) throw std::runtime_error("wrong statement type for: " + sql);
    return std::move(*typed);
}

std::string whereOf(const std::string& sql) {
    return exprToString(*parseAs<SelectStmt>(sql).where);
}

}  // namespace

TEST(parsesCreateTable) {
    auto stmt = parseAs<CreateTableStmt>("create table Users (ID int primary key, name TEXT)");
    CHECK_EQ(stmt.table, std::string("users"));
    CHECK_EQ(stmt.columns.size(), size_t(2));
    CHECK_EQ(stmt.columns[0].name, std::string("id"));
    CHECK(stmt.columns[0].primaryKey);
    CHECK(stmt.columns[1].type == Type::Text);
    CHECK(!stmt.columns[1].primaryKey);
}

TEST(parsesMultiRowInsertWithColumns) {
    auto stmt = parseAs<InsertStmt>("INSERT INTO t (b, a) VALUES (1, 'x'), (2, 'y')");
    CHECK_EQ(stmt.columns.size(), size_t(2));
    CHECK_EQ(stmt.rows.size(), size_t(2));
    CHECK_EQ(exprToString(*stmt.rows[1][1]), std::string("'y'"));
}

TEST(parsesFullSelect) {
    auto stmt = parseAs<SelectStmt>(
        "SELECT name, age * 2 AS double_age FROM people WHERE age > 30 "
        "ORDER BY age DESC, name LIMIT 5");
    CHECK_EQ(stmt.items.size(), size_t(2));
    CHECK_EQ(stmt.items[1].alias, std::string("double_age"));
    CHECK_EQ(stmt.orderBy.size(), size_t(2));
    CHECK(stmt.orderBy[0].descending);
    CHECK(!stmt.orderBy[1].descending);
    CHECK(stmt.limit.has_value() && *stmt.limit == 5);
    CHECK(!stmt.explain);
}

TEST(parsesExplain) {
    CHECK(parseAs<SelectStmt>("EXPLAIN SELECT * FROM t").explain);
}

TEST(respectsOperatorPrecedence) {
    CHECK_EQ(whereOf("SELECT * FROM t WHERE a = 1 OR b = 2 AND c = 3"),
             std::string("(a = 1) OR ((b = 2) AND (c = 3))"));
    CHECK_EQ(whereOf("SELECT * FROM t WHERE a + b * c > 4"),
             std::string("(a + (b * c)) > 4"));
    CHECK_EQ(whereOf("SELECT * FROM t WHERE NOT a = 1 AND b = 2"),
             std::string("(NOT (a = 1)) AND (b = 2)"));
    CHECK_EQ(whereOf("SELECT * FROM t WHERE (a = 1 OR b = 2) AND c = 3"),
             std::string("((a = 1) OR (b = 2)) AND (c = 3)"));
    CHECK_EQ(whereOf("SELECT * FROM t WHERE a - b - c = 0"),
             std::string("((a - b) - c) = 0"));
}

TEST(parsesIntegerEdgeCases) {
    auto stmt = parseAs<SelectStmt>("SELECT * FROM t WHERE a = -9223372036854775808");
    CHECK_EQ(stmt.where->right->value.asInt(), std::numeric_limits<int64_t>::min());
    CHECK_THROWS(parseOne("SELECT * FROM t WHERE a = 9223372036854775808"), SqlError, "out of range");
}

TEST(acceptsWhitespaceAroundOperators) {
    // The old engine silently ignored "WHERE id = 2" and deleted every row.
    auto stmt = parseAs<DeleteStmt>("DELETE FROM e WHERE id = 2");
    CHECK_EQ(exprToString(*stmt.where), std::string("id = 2"));
}

TEST(reportsSyntaxErrors) {
    CHECK_THROWS(parseOne("SELECT FROM t"), SqlError, "expected an expression");
    CHECK_THROWS(parseOne("SELECT * FROM"), SqlError, "expected table name but found end of input");
    CHECK_THROWS(parseOne("CREATE TABLE t (a FLOAT)"), SqlError, "column type");
    CHECK_THROWS(parseOne("SELECT * FROM select"), SqlError, "expected table name");
    CHECK_THROWS(parseOne("SELECT * FROM t WHERE a < b < c"), SqlError, "';' or end of statement");
    CHECK_THROWS(parseOne("SELECT * FROM t LIMIT -1"), SqlError, "non-negative integer");
    CHECK_THROWS(parseOne("SELECT * FROM t; SELECT * FROM t"), SqlError, "single statement");
    CHECK_THROWS(parseOne("FROB"), SqlError, "expected a statement");
}

TEST(parserRecoversAfterAnError) {
    std::string sql = "SELECT * FROM t; BROKEN STUFF; DROP TABLE t;";
    Parser parser(sql);
    int ok = 0, failed = 0;
    while (!parser.atEnd()) {
        try {
            parser.next();
            ++ok;
        } catch (const SqlError&) {
            ++failed;
            parser.skipToNextStatement();
        }
    }
    CHECK_EQ(ok, 2);
    CHECK_EQ(failed, 1);
}

TEST(recordsStatementText) {
    std::string sql = "  SELECT *  FROM t ;\nDROP TABLE t";
    Parser parser(sql);
    CHECK_EQ(parser.next().text, std::string("SELECT *  FROM t"));
    CHECK_EQ(parser.next().text, std::string("DROP TABLE t"));
    CHECK(parser.atEnd());
}
