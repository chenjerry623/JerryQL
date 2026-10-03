#include <algorithm>
#include <cstdlib>
#include <map>
#include <random>
#include <tuple>

#include "check.h"
#include "database.h"
#include "sql_error.h"

using namespace jerryql;

namespace {

std::string rowsOf(Database& db, const std::string& sql) {
    QueryResult result = db.execute(sql);
    std::string out;
    for (const Row& row : result.rows) {
        for (const Value& v : row) out += v.toString() + ",";
        out += ";";
    }
    return out;
}

std::string planOf(Database& db, const std::string& select) {
    std::string out;
    for (const Row& row : db.execute("EXPLAIN " + select).rows) out += row[0].asText() + "\n";
    return out;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST(joinPlansPickTheRightAlgorithm) {
    Database db;
    db.execute("CREATE TABLE users (id INT PRIMARY KEY, name TEXT, city TEXT)");
    db.execute("CREATE TABLE orders (id INT PRIMARY KEY, user_id INT, amount INT)");
    std::string byPk = planOf(db, "SELECT * FROM orders o JOIN users u ON o.user_id = u.id");
    CHECK(contains(byPk, "INDEX NESTED LOOP JOIN u USING PRIMARY KEY (o.user_id = u.id)"));
    std::string hash = planOf(db, "SELECT * FROM users u JOIN orders o ON o.user_id = u.id");
    CHECK(contains(hash, "HASH JOIN o ON (o.user_id = u.id)"));
    db.execute("CREATE INDEX orders_user ON orders (user_id)");
    std::string byIndex = planOf(db, "SELECT * FROM users u JOIN orders o ON o.user_id = u.id");
    CHECK(contains(byIndex, "INDEX NESTED LOOP JOIN o USING orders_user (o.user_id = u.id)"));
    std::string nested = planOf(db, "SELECT * FROM users a JOIN users b ON a.id < b.id");
    CHECK(contains(nested, "NESTED LOOP JOIN b ON (a.id < b.id)"));
    // A single-table WHERE condition is pushed into that table's scan.
    std::string pushed = planOf(db, "SELECT * FROM users u JOIN orders o ON o.user_id = u.id WHERE u.id = 7");
    CHECK(contains(pushed, "PK LOOKUP users (id = 7)"));
}

TEST(joinErrors) {
    Database db;
    db.execute("CREATE TABLE a (id INT PRIMARY KEY, name TEXT)");
    db.execute("CREATE TABLE b (id INT PRIMARY KEY, name TEXT, a_id INT)");
    CHECK_THROWS(db.execute("SELECT name FROM a JOIN b ON b.a_id = a.id"), SqlError, "ambiguous");
    CHECK_THROWS(db.execute("SELECT * FROM a JOIN a ON a.id = a.id"), SqlError, "used twice");
    CHECK_THROWS(db.execute("SELECT * FROM a LEFT JOIN b ON b.a_id = a.id"), SqlError, "only inner joins");
    CHECK_THROWS(db.execute("SELECT * FROM a JOIN b ON c.id = a.id"), SqlError, "no such column: c.id");
    CHECK_THROWS(db.execute("SELECT * FROM a JOIN missing ON missing.id = a.id"), SqlError, "no such table");
    CHECK_THROWS(db.execute("SELECT * FROM a JOIN b ON COUNT(*) = 1"), SqlError, "only allowed in SELECT");
    // Qualified names work in single-table statements too.
    db.execute("INSERT INTO a VALUES (1, 'x')");
    CHECK_EQ(rowsOf(db, "SELECT a.name FROM a WHERE a.id = 1"), std::string("x,;"));
    CHECK_EQ(rowsOf(db, "SELECT t.name FROM a t WHERE t.id = 1"), std::string("x,;"));
    db.execute("UPDATE a SET name = 'y' WHERE a.id = 1");
    CHECK_EQ(rowsOf(db, "SELECT name FROM a"), std::string("y,;"));
}

// Random data and random join queries; expected results come from plain
// C++ loops over copies of the rows. Covers primary-key, index, hash and
// nested-loop joins, with an index on b.a_id created and dropped as it runs.
// Reproduce with JERRYQL_SEED=<seed> ./jerryql_tests joinRandomized
TEST(joinRandomizedAgainstLoops) {
    const char* env = std::getenv("JERRYQL_SEED");
    const unsigned seed = env ? unsigned(std::strtoul(env, nullptr, 10)) : 5u;
    std::cout << "joinRandomizedAgainstLoops seed=" << seed << "\n";
    std::mt19937 rng(seed);
    auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

    Database db;
    db.execute("CREATE TABLE a (id INT PRIMARY KEY, x INT, s TEXT)");
    db.execute("CREATE TABLE b (id INT PRIMARY KEY, a_id INT, y INT)");
    std::map<int64_t, std::pair<int64_t, std::string>> aRows;  // id -> (x, s)
    std::map<int64_t, std::pair<int64_t, int64_t>> bRows;      // id -> (a_id, y)
    const char* const words[] = {"p", "q", "r"};
    bool indexed = false;

    for (int step = 0; step < 600; ++step) {
        int kind = pick(0, 9);
        if (kind <= 2) {
            int64_t id = pick(0, 40), x = pick(0, 8);
            std::string s = words[pick(0, 2)];
            if (!aRows.count(id)) {
                db.execute("INSERT INTO a VALUES (" + std::to_string(id) + ", " + std::to_string(x) + ", '" + s + "')");
                aRows[id] = {x, s};
            }
        } else if (kind <= 5) {
            int64_t id = pick(0, 80), aId = pick(0, 45), y = pick(0, 8);
            if (!bRows.count(id)) {
                db.execute("INSERT INTO b VALUES (" + std::to_string(id) + ", " + std::to_string(aId) + ", " +
                           std::to_string(y) + ")");
                bRows[id] = {aId, y};
            }
        } else if (kind == 6) {
            int64_t id = pick(0, 80);
            db.execute("DELETE FROM b WHERE id = " + std::to_string(id));
            bRows.erase(id);
            if (pick(0, 3) == 0) {
                db.execute(indexed ? "DROP INDEX b_a" : "CREATE INDEX b_a ON b (a_id)");
                indexed = !indexed;
            }
        } else {
            int threshold = pick(0, 8);
            using Pair = std::tuple<int64_t, int64_t>;
            std::vector<Pair> expected;
            std::string sql;
            int query = pick(0, 3);
            for (const auto& [aid, av] : aRows) {
                for (const auto& [bid, bv] : bRows) {
                    bool match = false;
                    if (query == 0) match = bv.first == aid && av.first >= threshold;   // b.a_id = a.id
                    if (query == 1) match = bv.first == aid && bv.second < threshold;   // a by primary key
                    if (query == 2) match = bv.second == av.first && av.second == "p";  // hash join on values
                    if (query == 3) match = bv.second > av.first && aid < 10;           // nested loop
                    if (match) expected.emplace_back(aid, bid);
                }
            }
            std::sort(expected.begin(), expected.end());
            std::string t = std::to_string(threshold);
            if (query == 0) sql = "SELECT a.id, b.id FROM a JOIN b ON b.a_id = a.id WHERE a.x >= " + t;
            if (query == 1) sql = "SELECT a.id, b.id FROM b JOIN a ON a.id = b.a_id WHERE b.y < " + t;
            if (query == 2) sql = "SELECT a.id, b.id FROM a JOIN b ON b.y = a.x WHERE a.s = 'p'";
            if (query == 3) sql = "SELECT a.id, b.id FROM a JOIN b ON b.y > a.x WHERE a.id < 10";
            sql += " ORDER BY a.id, b.id";
            std::string want;
            for (const Pair& p : expected) want += std::to_string(std::get<0>(p)) + "," + std::to_string(std::get<1>(p)) + ",;";
            std::string got = rowsOf(db, sql);
            if (got != want) {
                CHECK_EQ(got, want);
                std::cerr << "  diverged at step " << step << " (seed " << seed << "): " << sql << "\n"
                          << planOf(db, sql);
                return;
            }
        }
    }
    CHECK_EQ(db.checkIntegrity(), std::string(""));
}

TEST(joinThreeTablesWithAggregates) {
    Database db;
    db.execute("CREATE TABLE users (id INT PRIMARY KEY, name TEXT, city TEXT)");
    db.execute("CREATE TABLE orders (id INT PRIMARY KEY, user_id INT, amount INT)");
    db.execute("CREATE TABLE cities (name TEXT, country TEXT)");
    db.execute("INSERT INTO users VALUES (1, 'Ada', 'London'), (2, 'Grace', 'NYC'), (3, 'Linus', 'Helsinki')");
    db.execute("INSERT INTO orders VALUES (10, 1, 250), (11, 1, 75), (12, 2, 300), (13, 3, 40), (14, 9, 5)");
    db.execute("INSERT INTO cities VALUES ('London', 'UK'), ('Helsinki', 'FI'), ('NYC', 'US')");
    CHECK_EQ(rowsOf(db,
                    "SELECT c.country, COUNT(*), SUM(o.amount) FROM orders o JOIN users u ON u.id = o.user_id "
                    "JOIN cities c ON c.name = u.city GROUP BY c.country ORDER BY c.country"),
             std::string("FI,1,40,;UK,2,325,;US,1,300,;"));
    CHECK_EQ(rowsOf(db, "SELECT COUNT(*) FROM orders o JOIN users u ON u.id = o.user_id"), std::string("4,;"));
}
