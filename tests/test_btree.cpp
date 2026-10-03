#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <random>

#include "check.h"
#include "database.h"
#include "memory_store.h"
#include "sql_error.h"
#include "storage/btree.h"
#include "storage/btree_store.h"
#include "storage/pager.h"

using namespace jerryql;

namespace {

unsigned testSeed(unsigned fallback) {
    const char* env = std::getenv("JERRYQL_SEED");
    return env ? unsigned(std::strtoul(env, nullptr, 10)) : fallback;
}

std::unique_ptr<Pager> memoryPager(size_t poolPages) {
    PagerOptions options;
    options.poolPages = poolPages;
    auto pager = std::make_unique<Pager>(std::make_unique<MemoryFile>(), std::make_unique<MemoryFile>(), options);
    if (BTree::create(*pager) != Pager::kSchemaRootPage) throw std::logic_error("setup");
    return pager;
}

// A unique path in the temp directory, removed when the guard goes away.
struct TempFile {
    std::string path;
    TempFile() {
        static int counter = 0;
        path = (std::filesystem::temp_directory_path() /
                ("jerryql_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".db"))
                   .string();
        std::remove(path.c_str());
    }
    ~TempFile() { std::remove(path.c_str()); }
};

std::string payloadFor(int64_t key, size_t length) {
    std::string payload(length, 'a' + char(((key % 26) + 26) % 26));
    if (!payload.empty()) payload[0] = char(length % 251);
    return payload;
}

std::string scanAll(BTree& tree, int64_t lo, int64_t hi) {
    std::string out;
    BTreeCursor cursor = tree.scan(lo, hi);
    int64_t key;
    std::string payload;
    while (cursor.next(key, payload)) out += std::to_string(key) + ":" + std::to_string(payload.size()) + ",";
    return out;
}

std::string scanAll(const std::map<int64_t, std::string>& oracle, int64_t lo, int64_t hi) {
    std::string out;
    if (lo > hi) return out;
    for (auto it = oracle.lower_bound(lo); it != oracle.end() && it->first <= hi; ++it) {
        out += std::to_string(it->first) + ":" + std::to_string(it->second.size()) + ",";
    }
    return out;
}

}  // namespace

// Random inserts, replaces, erases, lookups and range scans checked against
// std::map, with a 16-page buffer pool so pages are evicted constantly.
// Reproduce a failure with JERRYQL_SEED=<seed> ./jerryql_tests btreeRandomized
TEST(btreeRandomizedAgainstMap) {
    const unsigned seed = testSeed(42);
    std::cout << "btreeRandomizedAgainstMap seed=" << seed << "\n";
    std::mt19937 rng(seed);
    auto randomInt = [&](int64_t lo, int64_t hi) {
        return std::uniform_int_distribution<int64_t>(lo, hi)(rng);
    };

    auto pager = memoryPager(16);
    BTree tree(*pager, BTree::create(*pager));
    std::map<int64_t, std::string> oracle;

    for (int step = 0; step < 40000; ++step) {
        int64_t key = randomInt(-4000, 4000);
        // Mostly small payloads, sometimes the maximum, to exercise uneven splits.
        size_t length = randomInt(0, 9) == 0 ? kMaxPayload : size_t(randomInt(0, 120));
        int op = int(randomInt(0, 9));
        if (op <= 4) {
            bool inserted = tree.insert(key, payloadFor(key, length));
            CHECK_EQ(inserted, oracle.count(key) == 0);
            if (inserted) oracle[key] = payloadFor(key, length);
        } else if (op <= 6) {
            bool replaced = tree.replace(key, payloadFor(key + 1, length));
            CHECK_EQ(replaced, oracle.count(key) == 1);
            if (replaced) oracle[key] = payloadFor(key + 1, length);
        } else if (op == 7) {
            CHECK_EQ(tree.erase(key), oracle.erase(key) == 1);
        } else if (op == 8) {
            auto found = tree.find(key);
            auto expected = oracle.find(key);
            CHECK_EQ(found.has_value(), expected != oracle.end());
            if (found && expected != oracle.end()) CHECK(*found == expected->second);
        } else {
            int64_t lo = randomInt(-4100, 4100), hi = lo + randomInt(-10, 400);
            std::string actual = scanAll(tree, lo, hi), expected = scanAll(oracle, lo, hi);
            if (actual != expected) {
                CHECK_EQ(actual, expected);
                std::cerr << "  diverged at step " << step << " with seed " << seed << "\n";
                return;
            }
        }
        if (step % 2000 == 0) tree.check();
    }
    CHECK_EQ(tree.size(), uint64_t(oracle.size()));
    CHECK_EQ(scanAll(tree, INT64_MIN, INT64_MAX), scanAll(oracle, INT64_MIN, INT64_MAX));
    BTreeShape shape = tree.check();
    CHECK(shape.depth >= 2);
    CHECK(pager->pool().stats().writeBacks > 0);  // evictions really happened
}

TEST(btreeGrowsToThreeLevels) {
    auto pager = memoryPager(64);
    BTree tree(*pager, BTree::create(*pager));
    std::mt19937 rng(7);
    std::vector<int64_t> keys(150000);
    for (size_t i = 0; i < keys.size(); ++i) keys[i] = int64_t(i) * 3;
    std::shuffle(keys.begin(), keys.end(), rng);
    for (int64_t key : keys) CHECK(tree.insert(key, payloadFor(key, 16)));
    BTreeShape shape = tree.check();
    CHECK_EQ(shape.depth, 3);
    CHECK_EQ(tree.size(), uint64_t(150000));
    CHECK(tree.find(299997).has_value());
    CHECK(!tree.find(299998).has_value());
    CHECK_EQ(scanAll(tree, 30, 40), std::string("30:16,33:16,36:16,39:16,"));
}

// Appending ascending keys splits off only the new cell, so leaves stay full.
TEST(btreeSequentialInsertsFillLeaves) {
    auto pager = memoryPager(64);
    BTree sequential(*pager, BTree::create(*pager));
    BTree shuffled(*pager, BTree::create(*pager));
    std::vector<int64_t> keys(50000);
    for (size_t i = 0; i < keys.size(); ++i) keys[i] = int64_t(i);
    for (int64_t key : keys) sequential.insert(key, payloadFor(key, 20));
    std::shuffle(keys.begin(), keys.end(), std::mt19937(3));
    for (int64_t key : keys) shuffled.insert(key, payloadFor(key, 20));

    BTreeShape a = sequential.check(), b = shuffled.check();
    std::cout << "leaf fill: sequential " << a.leafFill << " (" << a.leafPages << " leaves), random "
              << b.leafFill << " (" << b.leafPages << " leaves)\n";
    CHECK(a.leafFill > 0.97);
    CHECK(b.leafFill > 0.55);
    CHECK(a.leafPages < b.leafPages);
}

TEST(btreeReplaceThatGrowsSplitsTheLeaf) {
    auto pager = memoryPager(16);
    BTree tree(*pager, BTree::create(*pager));
    for (int64_t key = 0; key < 30; ++key) tree.insert(key, payloadFor(key, 100));
    CHECK_EQ(tree.check().depth, 1);
    for (int64_t key = 0; key < 30; ++key) CHECK(tree.replace(key, payloadFor(key, kMaxPayload)));
    BTreeShape shape = tree.check();
    CHECK_EQ(shape.depth, 2);
    CHECK_EQ(tree.size(), uint64_t(30));
    CHECK_EQ(tree.find(17)->size(), kMaxPayload);
    CHECK(!tree.replace(99, "x"));
}

TEST(btreeEraseEverythingThenReuse) {
    auto pager = memoryPager(16);
    BTree tree(*pager, BTree::create(*pager));
    for (int64_t key = 0; key < 5000; ++key) tree.insert(key, payloadFor(key, 50));
    for (int64_t key = 0; key < 5000; key += 2) CHECK(tree.erase(key));
    CHECK_EQ(scanAll(tree, 0, 7), std::string("1:50,3:50,5:50,7:50,"));
    for (int64_t key = 1; key < 5000; key += 2) CHECK(tree.erase(key));
    CHECK_EQ(tree.size(), uint64_t(0));
    CHECK_EQ(scanAll(tree, INT64_MIN, INT64_MAX), std::string(""));
    tree.check();  // empty leaves are allowed; ordering and links still hold
    for (int64_t key = 0; key < 100; ++key) CHECK(tree.insert(key, "again"));
    CHECK_EQ(tree.size(), uint64_t(100));
    tree.check();
}

TEST(btreeEdgeKeysAndEmptyRanges) {
    auto pager = memoryPager(16);
    BTree tree(*pager, BTree::create(*pager));
    tree.insert(INT64_MIN, "min");
    tree.insert(INT64_MAX, "max");
    tree.insert(0, "zero");
    CHECK_EQ(scanAll(tree, INT64_MIN, INT64_MAX), std::string("-9223372036854775808:3,0:4,9223372036854775807:3,"));
    CHECK_EQ(scanAll(tree, 1, 0), std::string(""));
    CHECK_EQ(scanAll(tree, INT64_MAX, INT64_MAX), std::string("9223372036854775807:3,"));
    CHECK_THROWS(tree.insert(5, std::string(kMaxPayload + 1, 'x')), std::invalid_argument, "too large");
}

TEST(droppedTablePagesAreReused) {
    Database db;
    db.execute("CREATE TABLE big (id INT PRIMARY KEY, body TEXT)");
    for (int batch = 0; batch < 20; ++batch) {
        std::string sql = "INSERT INTO big VALUES ";
        for (int i = 0; i < 100; ++i) {
            int id = batch * 100 + i;
            sql += (i ? ", (" : "(") + std::to_string(id) + ", '" + std::string(200, 'x') + "')";
        }
        db.execute(sql);
    }
    uint32_t pagesBefore = db.pager().pageCount();
    CHECK(pagesBefore > 100);
    db.execute("DROP TABLE big");
    CHECK(db.pager().freePageCount() + 3 >= pagesBefore);
    db.execute("CREATE TABLE again (id INT PRIMARY KEY, body TEXT)");
    db.execute("INSERT INTO again VALUES (1, 'reuses a free page')");
    CHECK_EQ(db.pager().pageCount(), pagesBefore);  // the file didn't grow
}

TEST(databasePersistsAcrossReopen) {
    TempFile file;
    {
        Database db(file.path);
        db.execute("CREATE TABLE people (id INT PRIMARY KEY, name TEXT, age INT)");
        db.execute("CREATE TABLE log (msg TEXT)");
        for (int batch = 0; batch < 50; ++batch) {
            std::string sql = "INSERT INTO people VALUES ";
            for (int i = 0; i < 200; ++i) {
                int id = batch * 200 + i;
                sql += (i ? ", (" : "(") + std::to_string(id) + ", 'person" + std::to_string(id) +
                       "', " + std::to_string(id % 90) + ")";
            }
            db.execute(sql);
        }
        db.execute("INSERT INTO log VALUES ('first'), ('second')");
        db.execute("DELETE FROM people WHERE id >= 100 AND id < 200");
        db.execute("UPDATE people SET name = 'renamed' WHERE id = 5000");
    }
    {
        Database db(file.path, DatabaseOptions{16});  // tiny pool: reads come from the file
        CHECK_EQ(db.tableNames().size(), size_t(2));
        CHECK_EQ(db.table("people").store->size(), size_t(9900));
        QueryResult r = db.execute("SELECT name, age FROM people WHERE id = 5000");
        CHECK_EQ(r.rows.size(), size_t(1));
        CHECK_EQ(r.rows[0][0].asText(), std::string("renamed"));
        CHECK_EQ(db.execute("SELECT id FROM people WHERE id >= 99 AND id <= 200").rows.size(), size_t(2));
        static_cast<BTreeStore&>(*db.table("people").store).tree().check();
        // Row ids keep counting after a reopen instead of restarting at 1.
        db.execute("INSERT INTO log VALUES ('third')");
        QueryResult log = db.execute("SELECT msg FROM log");
        CHECK_EQ(log.rows.size(), size_t(3));
        CHECK_EQ(log.rows[2][0].asText(), std::string("third"));
        CHECK_EQ(db.execute("EXPLAIN SELECT * FROM people WHERE id = 7").rows.back()[0].asText(),
                 std::string("  -> PK LOOKUP people (id = 7)"));
    }
}

TEST(rejectsFilesThatAreNotDatabases) {
    TempFile file;
    {
        FILE* f = std::fopen(file.path.c_str(), "wb");
        std::fputs("definitely not a database", f);
        std::fclose(f);
    }
    CHECK_THROWS(Database db(file.path), std::runtime_error, "not a JerryQL database");
}

TEST(rowsLargerThanAPageCellAreRejected) {
    Database db;
    db.execute("CREATE TABLE t (id INT PRIMARY KEY, body TEXT)");
    std::string big(kMaxPayload, 'x');
    CHECK_THROWS(db.execute("INSERT INTO t VALUES (1, '" + big + "')"), SqlError, "row too large");
    db.execute("INSERT INTO t VALUES (1, 'small')");
    CHECK_THROWS(db.execute("UPDATE t SET body = '" + big + "'"), SqlError, "row too large");
    CHECK_EQ(db.execute("SELECT body FROM t").rows[0][0].asText(), std::string("small"));
}

TEST(bufferPoolEvictsLeastRecentlyUsed) {
    MemoryFile file;
    FilePageIO io(file);
    BufferPool pool(io, 8);
    for (PageId id = 0; id < 8; ++id) {
        PageRef page = pool.fetchNew(id);
        page.mutableData()[0] = char('a' + id);
    }
    { PageRef touch = pool.fetch(0); }  // page 0 becomes most recently used
    { PageRef page = pool.fetchNew(8); }  // evicts page 1, the least recently used
    BufferPoolStats before = pool.stats();
    { PageRef page = pool.fetch(0); CHECK_EQ(page.data()[0], 'a'); }
    CHECK_EQ(pool.stats().misses, before.misses);  // page 0 was still cached
    { PageRef page = pool.fetch(1); CHECK_EQ(page.data()[0], 'b'); }  // re-read from the file
    CHECK_EQ(pool.stats().misses, before.misses + 1);
}

TEST(bufferPoolRefusesWhenEveryPageIsPinned) {
    MemoryFile file;
    FilePageIO io(file);
    BufferPool pool(io, 8);
    std::vector<PageRef> pinned;
    for (PageId id = 0; id < 8; ++id) pinned.push_back(pool.fetch(id));
    CHECK_THROWS(pool.fetch(100), std::runtime_error, "every page is pinned");
}

// The same random workload on MemoryStore (std::map) and BTreeStore must
// produce identical rows, including row ids for tables without a key.
TEST(btreeStoreMatchesMemoryStore) {
    auto pager = memoryPager(16);
    BTreeStore btree(*pager, BTree::create(*pager));
    MemoryStore reference;
    std::mt19937 rng(testSeed(11));
    for (int step = 0; step < 5000; ++step) {
        int64_t key = std::uniform_int_distribution<int64_t>(0, 500)(rng);
        Row row = {Value::integer(key), Value::text(std::string(size_t(key % 40), 'r'))};
        switch (rng() % 4) {
            case 0: CHECK_EQ(btree.insert(key, row), reference.insert(key, row)); break;
            case 1: CHECK_EQ(btree.replace(key, row), reference.replace(key, row)); break;
            case 2: CHECK_EQ(btree.erase(key), reference.erase(key)); break;
            default: CHECK_EQ(btree.allocateRowId(), reference.allocateRowId()); break;
        }
    }
    auto a = btree.scan(KeyRange{}), b = reference.scan(KeyRange{});
    int64_t ka, kb;
    Row ra, rb;
    size_t rows = 0;
    while (b->next(kb, rb)) {
        CHECK(a->next(ka, ra));
        CHECK_EQ(ka, kb);
        CHECK(ra == rb);
        ++rows;
    }
    CHECK(!a->next(ka, ra));
    CHECK_EQ(btree.size(), rows);
}
