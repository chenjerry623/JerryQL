#include <algorithm>
#include <cstdlib>
#include <random>

#include "check.h"
#include "database.h"
#include "sql_error.h"
#include "storage/btree.h"
#include "storage/index_key.h"

using namespace jerryql;

namespace {

unsigned seedFromEnv(unsigned fallback) {
    const char* env = std::getenv("JERRYQL_SEED");
    return env ? unsigned(std::strtoul(env, nullptr, 10)) : fallback;
}

std::string rowsOf(Database& db, const std::string& sql) {
    QueryResult result = db.execute(sql);
    std::string out;
    for (const Row& row : result.rows) {
        for (const Value& v : row) out += v.toSqlLiteral() + ",";
        out += ";";
    }
    return out;
}

std::string planOf(Database& db, const std::string& select) {
    return db.execute("EXPLAIN " + select).rows.back()[0].asText();
}

}  // namespace

// memcmp order of encodeValueKey must equal compareValues order, and no
// encoding may be a prefix of another (or composite keys would interleave).
TEST(indexKeyEncodingPreservesOrder) {
    std::vector<Value> ints = {Value::integer(INT64_MIN), Value::integer(-70000), Value::integer(-1),
                               Value::integer(0), Value::integer(1), Value::integer(255),
                               Value::integer(256), Value::integer(INT64_MAX)};
    std::vector<Value> texts;
    for (const char* t : {"", "a", "ab", "abc", "b", "ba", "z"}) texts.push_back(Value::text(t));
    texts.push_back(Value::text(std::string("ab\0", 3)));
    texts.push_back(Value::text(std::string("ab\0\0", 4)));
    texts.push_back(Value::text(std::string("ab\xff", 3)));
    texts.push_back(Value::text(std::string("\0", 1)));
    for (const std::vector<Value>* group : {&ints, &texts}) {
        for (const Value& a : *group) {
            for (const Value& b : *group) {
                std::string ka = encodeValueKey(a), kb = encodeValueKey(b);
                int expected = compareValues(a, b);
                int actual = compareKeys(ka, kb);
                CHECK_EQ(actual, expected);
                if (ka != kb) CHECK(ka.compare(0, kb.size(), kb) != 0 || ka.size() < kb.size());
            }
        }
    }
    // Composite keys keep value order regardless of the primary key.
    CHECK(compareKeys(indexEntryKey(Value::text("ab"), INT64_MAX),
                      indexEntryKey(Value::text(std::string("ab\0", 3)), INT64_MIN)) < 0);
    CHECK_EQ(primaryKeyOfEntry(indexEntryKey(Value::text("x"), -42)), int64_t(-42));
}

TEST(plannerChoosesIndexes) {
    Database db;
    db.execute("CREATE TABLE o (id INT PRIMARY KEY, city TEXT, amount INT, note TEXT)");
    db.execute("CREATE INDEX o_city ON o (city)");
    db.execute("CREATE INDEX o_amount ON o (amount)");
    CHECK_EQ(planOf(db, "SELECT * FROM o WHERE city = 'Lima'"),
             std::string("  -> INDEX SCAN o USING o_city (city = 'Lima')"));
    CHECK_EQ(planOf(db, "SELECT * FROM o WHERE amount >= 10 AND amount < 20"),
             std::string("  -> INDEX SCAN o USING o_amount (10 <= amount < 20)"));
    CHECK_EQ(planOf(db, "SELECT * FROM o WHERE 5 < amount"),
             std::string("  -> INDEX SCAN o USING o_amount (5 < amount)"));
    // An equality index beats a range index; the primary key beats both.
    CHECK_EQ(planOf(db, "SELECT * FROM o WHERE amount > 1 AND city = 'Lima'"),
             std::string("  -> INDEX SCAN o USING o_city (city = 'Lima')"));
    CHECK_EQ(planOf(db, "SELECT * FROM o WHERE id = 3 AND city = 'Lima'"),
             std::string("  -> PK LOOKUP o (id = 3)"));
    // No usable condition: no index.
    CHECK_EQ(planOf(db, "SELECT * FROM o WHERE note = 'x'"), std::string("  -> SEQ SCAN o"));
    CHECK_EQ(planOf(db, "SELECT * FROM o WHERE city = 'a' OR city = 'b'"), std::string("  -> SEQ SCAN o"));
    CHECK_EQ(planOf(db, "SELECT * FROM o WHERE amount = 'text'"), std::string("  -> SEQ SCAN o"));
    CHECK_EQ(planOf(db, "SELECT * FROM o WHERE amount > 5 AND amount < 5"),
             std::string("  -> EMPTY SCAN o (WHERE can never match)"));
}

TEST(indexErrorsAndLimits) {
    Database db;
    db.execute("CREATE TABLE t (id INT PRIMARY KEY, a INT, s TEXT)");
    db.execute("CREATE INDEX t_s ON t (s)");
    CHECK_THROWS(db.execute("CREATE INDEX t_s ON t (a)"), SqlError, "already exists");
    CHECK_THROWS(db.execute("CREATE INDEX t2 ON t (s)"), SqlError, "already has an index");
    CHECK_THROWS(db.execute("CREATE INDEX t3 ON t (id)"), SqlError, "is the primary key");
    CHECK_THROWS(db.execute("CREATE INDEX t4 ON missing (a)"), SqlError, "no such table");
    CHECK_THROWS(db.execute("CREATE INDEX t5 ON t (nope)"), SqlError, "no such column");
    CHECK_THROWS(db.execute("CREATE INDEX t6 ON t (a, s)"), SqlError, "indexes cover one column");
    CHECK_THROWS(db.execute("DROP INDEX nope"), SqlError, "no such index");
    std::string longText(300, 'x');
    CHECK_THROWS(db.execute("INSERT INTO t VALUES (1, 1, '" + longText + "')"), SqlError, "too long for index");
    db.execute("INSERT INTO t VALUES (1, 1, 'short')");
    CHECK_THROWS(db.execute("UPDATE t SET s = '" + longText + "'"), SqlError, "too long for index");
    db.execute("DROP INDEX t_s");
    db.execute("UPDATE t SET s = '" + longText + "'");  // no index, no limit
    CHECK_THROWS(db.execute("CREATE INDEX t_s ON t (s)"), SqlError, "too long to index");
    CHECK_EQ(db.checkIntegrity(), std::string(""));
}

TEST(rollbackUndoesIndexChanges) {
    Database db;
    db.execute("CREATE TABLE t (id INT PRIMARY KEY, a INT)");
    db.execute("INSERT INTO t VALUES (1, 10), (2, 20)");
    db.execute("BEGIN");
    db.execute("CREATE INDEX t_a ON t (a)");
    db.execute("INSERT INTO t VALUES (3, 30)");
    CHECK_EQ(rowsOf(db, "SELECT id FROM t WHERE a = 30"), std::string("3,;"));
    db.execute("ROLLBACK");
    CHECK_EQ(db.table("t").indexes.size(), size_t(0));
    db.execute("CREATE INDEX t_a ON t (a)");
    db.execute("BEGIN");
    db.execute("UPDATE t SET a = 99 WHERE id = 1");
    db.execute("DELETE FROM t WHERE id = 2");
    db.execute("ROLLBACK");
    CHECK_EQ(rowsOf(db, "SELECT id FROM t WHERE a = 10"), std::string("1,;"));
    CHECK_EQ(rowsOf(db, "SELECT id FROM t WHERE a = 99"), std::string(""));
    CHECK_EQ(db.checkIntegrity(), std::string(""));
}

// Two databases run the same random statements; only one has indexes. Every
// query must return the same rows, and the indexed one must stay consistent.
// Reproduce with JERRYQL_SEED=<seed> ./jerryql_tests indexDifferential
TEST(indexDifferentialAgainstNoIndex) {
    const unsigned seed = seedFromEnv(99);
    std::cout << "indexDifferentialAgainstNoIndex seed=" << seed << "\n";
    std::mt19937 rng(seed);
    auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    const char* const words[] = {"", "a", "ab", "abc", "b", "bb", "c", "Tokyo", "Lima", "z"};
    auto word = [&]() { return std::string("'") + words[pick(0, 9)] + "'"; };

    DatabaseOptions options;
    options.poolPages = 16;
    Database indexed(options), plain(options);
    for (Database* db : {&indexed, &plain}) {
        db->execute("CREATE TABLE t (id INT PRIMARY KEY, a INT, s TEXT, pad TEXT)");
    }
    indexed.execute("CREATE INDEX t_a ON t (a)");
    indexed.execute("CREATE INDEX t_s ON t (s)");
    const char* const ops[] = {"=", "<", "<=", ">", ">="};

    for (int step = 0; step < 4000; ++step) {
        std::string sql;
        int kind = pick(0, 9);
        int id = pick(0, 300);
        if (kind <= 3) {
            sql = "INSERT INTO t VALUES (" + std::to_string(id) + ", " + std::to_string(pick(-20, 20)) +
                  ", " + word() + ", '" + std::string(size_t(pick(0, 200)), 'p') + "')";
        } else if (kind == 4) {
            sql = "UPDATE t SET a = a + " + std::to_string(pick(-3, 3)) + " WHERE a " + ops[pick(0, 4)] + " " +
                  std::to_string(pick(-20, 20));
        } else if (kind == 5) {
            sql = "UPDATE t SET s = " + word() + ", id = id + 1000 WHERE id = " + std::to_string(id);
        } else if (kind == 6) {
            sql = "DELETE FROM t WHERE s " + std::string(ops[pick(0, 4)]) + " " + word();
            if (pick(0, 3) != 0) sql = "DELETE FROM t WHERE id = " + std::to_string(id);
        } else {
            std::string where = "a " + std::string(ops[pick(0, 4)]) + " " + std::to_string(pick(-20, 20));
            if (pick(0, 1)) where = "s " + std::string(ops[pick(0, 4)]) + " " + word();
            if (pick(0, 2) == 0) where += " AND a " + std::string(ops[pick(0, 4)]) + " " + std::to_string(pick(-20, 20));
            sql = "SELECT id, a, s FROM t WHERE " + where + " ORDER BY id";
        }
        std::string a, b;
        try { a = rowsOf(indexed, sql); } catch (const SqlError& e) { a = std::string("error: ") + e.what(); }
        try { b = rowsOf(plain, sql); } catch (const SqlError& e) { b = std::string("error: ") + e.what(); }
        if (a != b) {
            CHECK_EQ(a, b);
            std::cerr << "  diverged at step " << step << " (seed " << seed << "): " << sql << "\n";
            return;
        }
        if (step % 500 == 0) {
            std::string problem = indexed.checkIntegrity();
            if (!problem.empty()) {
                CHECK_EQ(problem, std::string(""));
                std::cerr << "  at step " << step << " (seed " << seed << ")\n";
                return;
            }
        }
    }
    CHECK_EQ(indexed.checkIntegrity(), std::string(""));
}
