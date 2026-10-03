#include <limits>

#include "check.h"
#include "database.h"
#include "parser.h"
#include "planner.h"

using namespace jerryql;

namespace {

constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
constexpr int64_t kMax = std::numeric_limits<int64_t>::max();

Schema peopleSchema() {
    Schema schema;
    schema.columns = {{"id", Type::Int, ""}, {"age", Type::Int, ""}, {"name", Type::Text, ""}};
    schema.primaryKey = 0;
    return schema;
}

KeyRange rangeFor(const std::string& where) {
    Statement statement = parseOne("SELECT * FROM people WHERE " + where);
    return primaryKeyRange(std::get<SelectStmt>(statement).where.get(), peopleSchema());
}

bool rangeIs(const KeyRange& range, int64_t lo, int64_t hi) {
    return !range.empty && range.lo == lo && range.hi == hi;
}

std::string topPlanLine(Database& db, const std::string& select) {
    QueryResult result = db.execute("EXPLAIN " + select);
    return result.rows.back()[0].asText();  // the scan is the deepest operator
}

}  // namespace

TEST(keyRangeFromEquality) {
    CHECK(rangeIs(rangeFor("id = 7"), 7, 7));
    CHECK(rangeIs(rangeFor("7 = id"), 7, 7));
}

TEST(keyRangeFromBounds) {
    CHECK(rangeIs(rangeFor("id > 10"), 11, kMax));
    CHECK(rangeIs(rangeFor("id >= 10"), 10, kMax));
    CHECK(rangeIs(rangeFor("id < 10"), kMin, 9));
    CHECK(rangeIs(rangeFor("10 < id"), 11, kMax));
    CHECK(rangeIs(rangeFor("id >= 10 AND id < 20 AND name = 'x'"), 10, 19));
}

TEST(keyRangeDetectsContradictions) {
    CHECK(rangeFor("id > 5 AND id < 3").empty);
    CHECK(rangeFor("id = 1 AND id = 2").empty);
    CHECK(rangeFor("id > 9223372036854775807").empty);
    CHECK(rangeFor("id < -9223372036854775808").empty);
}

TEST(keyRangeIgnoresWhatItCannotUse) {
    CHECK(rangeFor("id = 1 OR id = 2").isAll());
    CHECK(rangeFor("age = 30").isAll());
    CHECK(rangeFor("id <> 5").isAll());
    CHECK(rangeFor("id + 0 = 5").isAll());
    CHECK(rangeFor("NOT id = 5").isAll());
    CHECK(rangeFor("id = 'five'").isAll());
}

TEST(explainShowsChosenAccessPath) {
    Database db;
    db.execute("CREATE TABLE people (id INT PRIMARY KEY, age INT, name TEXT)");
    db.execute("CREATE TABLE notes (body TEXT)");
    CHECK_EQ(topPlanLine(db, "SELECT * FROM people WHERE id = 3"),
             std::string("  -> PK LOOKUP people (id = 3)"));
    CHECK_EQ(topPlanLine(db, "SELECT * FROM people WHERE id >= 10 AND id < 20"),
             std::string("  -> PK RANGE SCAN people (10 <= id <= 19)"));
    CHECK_EQ(topPlanLine(db, "SELECT * FROM people WHERE id > 10"),
             std::string("  -> PK RANGE SCAN people (11 <= id)"));
    CHECK_EQ(topPlanLine(db, "SELECT * FROM people WHERE age = 3"),
             std::string("  -> SEQ SCAN people"));
    CHECK_EQ(topPlanLine(db, "SELECT * FROM people WHERE id > 5 AND id < 2"),
             std::string("  -> EMPTY SCAN people (WHERE can never match)"));
    CHECK_EQ(topPlanLine(db, "SELECT * FROM notes"), std::string("SEQ SCAN notes"));
}

TEST(explainShowsOperatorOrder) {
    Database db;
    db.execute("CREATE TABLE people (id INT PRIMARY KEY, age INT, name TEXT)");
    QueryResult result =
        db.execute("EXPLAIN SELECT name FROM people WHERE age > 1 ORDER BY age DESC LIMIT 2");
    CHECK_EQ(result.rows.size(), size_t(5));
    CHECK_EQ(result.rows[0][0].asText(), std::string("PROJECT name"));
    CHECK_EQ(result.rows[1][0].asText(), std::string("  -> LIMIT 2"));
    CHECK_EQ(result.rows[2][0].asText(), std::string("    -> SORT BY age DESC"));
    CHECK_EQ(result.rows[3][0].asText(), std::string("      -> FILTER age > 1"));
    CHECK_EQ(result.rows[4][0].asText(), std::string("        -> SEQ SCAN people"));
}
