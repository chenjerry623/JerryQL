#include <cstdlib>
#include <map>
#include <random>
#include <sstream>

#include "check.h"
#include "database.h"
#include "shell.h"
#include "sql_error.h"

using namespace jerryql;

namespace {

Database peopleDb() {
    Database db;
    db.execute("CREATE TABLE people (id INT PRIMARY KEY, name TEXT, age INT)");
    db.execute(
        "INSERT INTO people VALUES (1, 'Ada', 36), (2, 'Brian', 29), (3, 'Cleo', 41), "
        "(4, 'Dan', 29)");
    return db;
}

// Results as "a,b;c,d" for compact comparisons.
std::string rowsOf(Database& db, const std::string& sql) {
    QueryResult result = db.execute(sql);
    std::string out;
    for (size_t r = 0; r < result.rows.size(); ++r) {
        if (r > 0) out += ";";
        for (size_t c = 0; c < result.rows[r].size(); ++c) {
            if (c > 0) out += ",";
            out += result.rows[r][c].toString();
        }
    }
    return out;
}

}  // namespace

TEST(selectsWithFilterOrderAndLimit) {
    Database db = peopleDb();
    CHECK_EQ(rowsOf(db, "SELECT name FROM people WHERE age = 29"), std::string("Brian;Dan"));
    CHECK_EQ(rowsOf(db, "SELECT name FROM people ORDER BY age DESC, name LIMIT 3"),
             std::string("Cleo;Ada;Brian"));
    CHECK_EQ(rowsOf(db, "SELECT name FROM people ORDER BY age, name DESC"),
             std::string("Dan;Brian;Ada;Cleo"));
    CHECK_EQ(rowsOf(db, "SELECT id FROM people WHERE id >= 2 AND id < 4"), std::string("2;3"));
    CHECK_EQ(rowsOf(db, "SELECT id FROM people WHERE NOT (age = 29 OR id = 1)"), std::string("3"));
    CHECK_EQ(rowsOf(db, "SELECT name FROM people WHERE name > 'B' AND name < 'D'"),
             std::string("Brian;Cleo"));
    CHECK_EQ(rowsOf(db, "SELECT id FROM people LIMIT 0"), std::string(""));
    CHECK_EQ(rowsOf(db, "SELECT id FROM people LIMIT 2 OFFSET 1"), std::string("2;3"));
    CHECK_EQ(rowsOf(db, "SELECT id FROM people LIMIT 5 OFFSET 3"), std::string("4"));
    CHECK_EQ(rowsOf(db, "SELECT id FROM people LIMIT 5 OFFSET 10"), std::string(""));
    CHECK_EQ(rowsOf(db, "SELECT name FROM people ORDER BY name DESC LIMIT 1 OFFSET 1"), std::string("Cleo"));
}

TEST(selectsExpressionsWithHeaders) {
    Database db = peopleDb();
    QueryResult result = db.execute("SELECT name, age + 1 AS next_age, id * 10 FROM people WHERE id = 1");
    CHECK_EQ(result.columns.size(), size_t(3));
    CHECK_EQ(result.columns[1], std::string("next_age"));
    CHECK_EQ(result.columns[2], std::string("id * 10"));
    CHECK_EQ(rowsOf(db, "SELECT name, age + 1 AS next_age, id * 10 FROM people WHERE id = 1"),
             std::string("Ada,37,10"));
    CHECK_EQ(rowsOf(db, "SELECT -age, 7 / 2, age > 30 FROM people WHERE id = 1"),
             std::string("-36,3,1"));
}

TEST(insertsWithColumnListInAnyOrder) {
    Database db = peopleDb();
    db.execute("INSERT INTO people (age, id, name) VALUES (50, 9, 'Zed')");
    CHECK_EQ(rowsOf(db, "SELECT id, name, age FROM people WHERE id = 9"), std::string("9,Zed,50"));
}

TEST(insertRejectsBadRowsAtomically) {
    Database db = peopleDb();
    CHECK_THROWS(db.execute("INSERT INTO people VALUES (5, 'Eve', 1), (1, 'Dup', 2)"), SqlError,
                 "duplicate primary key: people.id = 1");
    CHECK_THROWS(db.execute("INSERT INTO people VALUES (6, 'A', 1), (6, 'B', 2)"), SqlError,
                 "duplicate primary key");
    CHECK_THROWS(db.execute("INSERT INTO people VALUES (7, 'Eve', 'old')"), SqlError,
                 "column age expects INT but got TEXT 'old'");
    CHECK_THROWS(db.execute("INSERT INTO people VALUES (7, 'Eve')"), SqlError,
                 "expected 3 values but got 2");
    CHECK_THROWS(db.execute("INSERT INTO people (id, name) VALUES (7, 'Eve')"), SqlError,
                 "missing value for column age");
    CHECK_THROWS(db.execute("INSERT INTO people VALUES (id, 'x', 1)"), SqlError,
                 "column id cannot be used here");
    CHECK_EQ(rowsOf(db, "SELECT id FROM people"), std::string("1;2;3;4"));
}

TEST(updatesSeeOldValues) {
    Database db;
    db.execute("CREATE TABLE pair (id INT PRIMARY KEY, a INT, b INT)");
    db.execute("INSERT INTO pair VALUES (1, 10, 20)");
    CHECK_EQ(db.execute("UPDATE pair SET a = b, b = a").message, std::string("UPDATE 1"));
    CHECK_EQ(rowsOf(db, "SELECT a, b FROM pair"), std::string("20,10"));
}

TEST(updatesCanMovePrimaryKeys) {
    Database db = peopleDb();
    // Every key shifts up by one; collected before writing, so no row is visited twice.
    CHECK_EQ(db.execute("UPDATE people SET id = id + 1").message, std::string("UPDATE 4"));
    CHECK_EQ(rowsOf(db, "SELECT id, name FROM people"), std::string("2,Ada;3,Brian;4,Cleo;5,Dan"));
    CHECK_THROWS(db.execute("UPDATE people SET id = 5 WHERE id = 2"), SqlError,
                 "duplicate primary key: people.id = 5");
    CHECK_THROWS(db.execute("UPDATE people SET id = 1"), SqlError, "duplicate primary key");
    CHECK_EQ(rowsOf(db, "SELECT id FROM people"), std::string("2;3;4;5"));
}

TEST(deletesOnlyMatchingRows) {
    Database db = peopleDb();
    // Regression test: the old engine deleted every row for "WHERE id = 2".
    CHECK_EQ(db.execute("DELETE FROM people WHERE id = 2").message, std::string("DELETE 1"));
    CHECK_EQ(rowsOf(db, "SELECT id FROM people"), std::string("1;3;4"));
    CHECK_EQ(db.execute("DELETE FROM people WHERE age > 100").message, std::string("DELETE 0"));
    CHECK_EQ(db.execute("DELETE FROM people").message, std::string("DELETE 3"));
}

TEST(tablesWithoutPrimaryKeyKeepInsertOrder) {
    Database db;
    db.execute("CREATE TABLE log (msg TEXT, n INT)");
    db.execute("INSERT INTO log VALUES ('b', 1), ('a', 1), ('c', 2)");
    db.execute("UPDATE log SET n = 5 WHERE msg = 'a'");
    CHECK_EQ(rowsOf(db, "SELECT msg, n FROM log"), std::string("b,1;a,5;c,2"));
}

TEST(reportsRuntimeErrors) {
    Database db = peopleDb();
    CHECK_THROWS(db.execute("SELECT * FROM nope"), SqlError, "no such table: nope");
    CHECK_THROWS(db.execute("SELECT height FROM people"), SqlError, "no such column: height");
    CHECK_THROWS(db.execute("SELECT * FROM people WHERE name = 3"), SqlError,
                 "cannot compare TEXT with INT");
    CHECK_THROWS(db.execute("SELECT * FROM people WHERE name"), SqlError, "expected a condition");
    CHECK_THROWS(db.execute("SELECT age / 0 FROM people"), SqlError, "division by zero");
    CHECK_THROWS(db.execute("SELECT age * 9223372036854775807 FROM people"), SqlError,
                 "integer overflow");
    CHECK_THROWS(db.execute("SELECT name + 1 FROM people"), SqlError, "needs INT operands");
    CHECK_THROWS(db.execute("CREATE TABLE people (x INT)"), SqlError, "already exists");
    CHECK_THROWS(db.execute("CREATE TABLE t (a INT, a TEXT)"), SqlError, "duplicate column");
    CHECK_THROWS(db.execute("CREATE TABLE t (a TEXT PRIMARY KEY)"), SqlError, "must be INT");
    CHECK_THROWS(db.execute("CREATE TABLE t (a INT PRIMARY KEY, b INT PRIMARY KEY)"), SqlError,
                 "only one PRIMARY KEY");
    CHECK_THROWS(db.execute("DROP TABLE nope"), SqlError, "no such table");
    CHECK_THROWS(db.execute("UPDATE people SET age = 1, age = 2"), SqlError, "assigned twice");
}

TEST(shortCircuitSkipsRightSide) {
    Database db = peopleDb();
    // The right side would divide by zero if it were evaluated.
    CHECK_EQ(rowsOf(db, "SELECT id FROM people WHERE id > 0 OR age / 0 = 1"),
             std::string("1;2;3;4"));
    CHECK_EQ(rowsOf(db, "SELECT id FROM people WHERE id < 0 AND age / 0 = 1"), std::string(""));
    CHECK_THROWS(db.execute("SELECT id FROM people WHERE id = 1 OR age / 0 = 1"), SqlError,
                 "division by zero");
}

TEST(scriptRunnerContinuesAfterErrors) {
    Database db;
    std::ostringstream out;
    int failures = runScript(db,
                             "CREATE TABLE t (a INT); INSERT INTO t VALUES ('x'); "
                             "SELEC oops; INSERT INTO t VALUES (1); SELECT * FROM t;",
                             out);
    CHECK_EQ(failures, 2);
    CHECK_EQ(out.str(),
             std::string("CREATE TABLE\n"
                         "Error: column a expects INT but got TEXT 'x'\n"
                         "Error: syntax error at position 52: expected a statement (CREATE, DROP, "
                         "INSERT, SELECT, EXPLAIN, UPDATE, DELETE, BEGIN, COMMIT, ROLLBACK) but "
                         "found 'SELEC'\n"
                         "INSERT 1\n"
                         "+---+\n| a |\n+---+\n| 1 |\n+---+\n(1 row)\n"));
}

// Randomized differential test: apply random statements to the engine and to
// a std::map oracle, and compare range queries after every step.
// Reproduce a failure with JERRYQL_SEED=<seed> ./jerryql_tests randomized
TEST(randomizedAgainstMapOracle) {
    const char* seedEnv = std::getenv("JERRYQL_SEED");
    const unsigned seed = seedEnv ? unsigned(std::strtoul(seedEnv, nullptr, 10)) : 20261003u;
    std::cout << "randomizedAgainstMapOracle seed=" << seed << "\n";
    std::mt19937 rng(seed);
    auto randomInt = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

    Database db;
    db.execute("CREATE TABLE kv (k INT PRIMARY KEY, v INT)");
    std::map<int64_t, int64_t> oracle;
    const char* const ops[] = {"=", "<", "<=", ">", ">="};

    for (int step = 0; step < 3000; ++step) {
        int choice = randomInt(0, 9);
        int64_t a = randomInt(-60, 60);
        std::string op = ops[randomInt(0, 4)];
        auto matches = [&](int64_t k) {
            if (op == "=") return k == a;
            if (op == "<") return k < a;
            if (op == "<=") return k <= a;
            if (op == ">") return k > a;
            return k >= a;
        };

        if (choice <= 4) {  // insert
            int64_t v = randomInt(0, 1000);
            std::string sql = "INSERT INTO kv VALUES (" + std::to_string(a) + ", " + std::to_string(v) + ")";
            if (oracle.count(a)) {
                CHECK_THROWS(db.execute(sql), SqlError, "duplicate primary key");
            } else {
                db.execute(sql);
                oracle[a] = v;
            }
        } else if (choice == 5) {  // delete by key predicate
            db.execute("DELETE FROM kv WHERE k " + op + " " + std::to_string(a));
            for (auto it = oracle.begin(); it != oracle.end();) {
                it = matches(it->first) ? oracle.erase(it) : std::next(it);
            }
        } else if (choice == 6) {  // update values by key predicate
            db.execute("UPDATE kv SET v = v + 1 WHERE " + std::to_string(a) + " " + op + " k");
            for (auto& [k, v] : oracle) {
                // "a op k" is the mirrored predicate
                bool hit = (op == "=" && a == k) || (op == "<" && a < k) || (op == "<=" && a <= k) ||
                           (op == ">" && a > k) || (op == ">=" && a >= k);
                if (hit) ++v;
            }
        } else if (choice == 7) {  // move one key
            int64_t target = randomInt(-60, 60);
            std::string sql = "UPDATE kv SET k = " + std::to_string(target) + " WHERE k = " + std::to_string(a);
            bool moves = oracle.count(a) && target != a;
            if (moves && oracle.count(target)) {
                CHECK_THROWS(db.execute(sql), SqlError, "duplicate primary key");
            } else {
                db.execute(sql);
                if (moves) {
                    oracle[target] = oracle[a];
                    oracle.erase(a);
                }
            }
        } else {  // range query, compared below
        }

        int64_t lo = randomInt(-70, 70), hi = randomInt(-70, 70);
        std::string actual = rowsOf(db, "SELECT k, v FROM kv WHERE k >= " + std::to_string(lo) +
                                            " AND k <= " + std::to_string(hi));
        std::string expected;
        for (auto it = oracle.lower_bound(lo); it != oracle.end() && it->first <= hi; ++it) {
            if (!expected.empty()) expected += ";";
            expected += std::to_string(it->first) + "," + std::to_string(it->second);
        }
        if (actual != expected) {
            CHECK_EQ(actual, expected);
            std::cerr << "  diverged at step " << step << " with seed " << seed << "\n";
            return;
        }
    }
    CHECK_EQ(db.table("kv").store->size(), oracle.size());
}

TEST(aggregatesAndGroupBy) {
    Database db;
    db.execute("CREATE TABLE s (id INT PRIMARY KEY, city TEXT, amount INT)");
    db.execute("INSERT INTO s VALUES (1, 'b', 10), (2, 'a', 20), (3, 'b', 30), (4, 'c', 5)");
    CHECK_EQ(rowsOf(db, "SELECT COUNT(*), SUM(amount), MIN(amount), MAX(amount), AVG(amount) FROM s"),
             std::string("4,65,5,30,16"));
    CHECK_EQ(rowsOf(db, "SELECT city, COUNT(*), SUM(amount) FROM s GROUP BY city"),
             std::string("a,1,20;b,2,40;c,1,5"));
    CHECK_EQ(rowsOf(db, "SELECT city, SUM(amount) AS t FROM s GROUP BY city ORDER BY t DESC"),
             std::string("b,40;a,20;c,5"));
    CHECK_EQ(rowsOf(db, "SELECT city FROM s GROUP BY city HAVING COUNT(*) > 1"), std::string("b"));
    CHECK_EQ(rowsOf(db, "SELECT COUNT(*) FROM s WHERE amount > 100"), std::string("0"));
    CHECK_EQ(rowsOf(db, "SELECT SUM(amount) * 2 + COUNT(*) FROM s"), std::string("134"));
    CHECK_EQ(rowsOf(db, "SELECT city, COUNT(*) FROM s WHERE amount > 100 GROUP BY city"), std::string(""));
    CHECK_THROWS(db.execute("SELECT city, amount FROM s GROUP BY city"), SqlError, "must appear in GROUP BY");
    CHECK_THROWS(db.execute("SELECT * FROM s WHERE COUNT(*) > 1"), SqlError, "only allowed in SELECT");
    CHECK_THROWS(db.execute("SELECT MIN(amount) FROM s WHERE id > 9"), SqlError, "no NULL");
    CHECK_THROWS(db.execute("SELECT SUM(SUM(amount)) FROM s"), SqlError, "can't be nested");
    CHECK_THROWS(db.execute("SELECT SUM(amount * 9223372036854775807) FROM s"), SqlError, "overflow");
}

TEST(orderByAlias) {
    Database db;
    db.execute("CREATE TABLE s (id INT PRIMARY KEY, amount INT)");
    db.execute("INSERT INTO s VALUES (1, 30), (2, 10), (3, 20)");
    CHECK_EQ(rowsOf(db, "SELECT id, amount * 2 AS d FROM s ORDER BY d"), std::string("2,20;3,40;1,60"));
    // An alias wins over a column of the same name, as in SQLite and PostgreSQL.
    CHECK_EQ(rowsOf(db, "SELECT id AS amount FROM s ORDER BY amount DESC"), std::string("3;2;1"));
}
