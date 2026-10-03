#include <cstring>
#include <memory>

#include "check.h"
#include "database.h"
#include "sql_error.h"
#include "storage/btree_store.h"
#include "storage/wal.h"

using namespace jerryql;

namespace {

// A database over MemoryFiles we keep pointers to, so a test can copy the
// files' bytes at any moment and open the copy, as if the process died there.
struct CrashableDb {
    MemoryFile* dbFile = nullptr;
    MemoryFile* walFile = nullptr;
    std::unique_ptr<Database> db;

    explicit CrashableDb(DatabaseOptions options = {}) {
        auto dbOwned = std::make_unique<MemoryFile>();
        auto walOwned = std::make_unique<MemoryFile>();
        dbFile = dbOwned.get();
        walFile = walOwned.get();
        db = std::make_unique<Database>(std::move(dbOwned), std::move(walOwned), options);
    }

    // Opens a copy of the files as they are right now. The original keeps running.
    std::unique_ptr<Database> openSnapshot(DatabaseOptions options = {}) const {
        auto dbCopy = std::make_unique<MemoryFile>();
        auto walCopy = std::make_unique<MemoryFile>();
        dbCopy->bytes() = dbFile->bytes();
        walCopy->bytes() = walFile->bytes();
        return std::make_unique<Database>(std::move(dbCopy), std::move(walCopy), options);
    }
};

size_t rowCount(Database& db, const std::string& table) {
    return db.execute("SELECT * FROM " + table).rows.size();
}

void insertRows(Database& db, int first, int count, size_t textLength = 100) {
    std::string sql = "INSERT INTO t VALUES ";
    for (int i = 0; i < count; ++i) {
        sql += (i ? ", (" : "(") + std::to_string(first + i) + ", '" + std::string(textLength, 'v') + "')";
    }
    db.execute(sql);
}

void checkTree(Database& db, const std::string& table) {
    static_cast<BTreeStore&>(*db.table(table).store).tree().check();
}

}  // namespace

TEST(transactionCommitAndRollback) {
    Database db;
    db.execute("CREATE TABLE t (id INT PRIMARY KEY, v TEXT)");
    db.execute("BEGIN");
    insertRows(db, 0, 10);
    CHECK_EQ(rowCount(db, "t"), size_t(10));  // a transaction sees its own writes
    db.execute("ROLLBACK");
    CHECK_EQ(rowCount(db, "t"), size_t(0));

    db.execute("BEGIN TRANSACTION");
    insertRows(db, 0, 10);
    db.execute("COMMIT");
    CHECK_EQ(rowCount(db, "t"), size_t(10));
}

TEST(rollbackUndoesSchemaChanges) {
    Database db;
    db.execute("CREATE TABLE t (id INT PRIMARY KEY, v TEXT)");
    db.execute("BEGIN");
    db.execute("CREATE TABLE extra (x INT)");
    db.execute("DROP TABLE t");
    CHECK_EQ(db.tableNames().size(), size_t(1));
    db.execute("ROLLBACK");
    CHECK_EQ(db.tableNames().size(), size_t(1));
    CHECK_EQ(db.tableNames()[0], std::string("t"));
    insertRows(db, 0, 3);
    CHECK_EQ(rowCount(db, "t"), size_t(3));
}

TEST(transactionStatementErrors) {
    Database db;
    db.execute("CREATE TABLE t (id INT PRIMARY KEY, v TEXT)");
    CHECK_THROWS(db.execute("COMMIT"), SqlError, "no transaction is open");
    CHECK_THROWS(db.execute("ROLLBACK"), SqlError, "no transaction is open");
    db.execute("BEGIN");
    CHECK_THROWS(db.execute("BEGIN"), SqlError, "already open");
    insertRows(db, 0, 2);
    // A failed statement changes nothing and leaves the transaction open.
    CHECK_THROWS(insertRows(db, 1, 1), SqlError, "duplicate primary key");
    CHECK(db.inTransaction());
    insertRows(db, 5, 1);
    db.execute("COMMIT");
    CHECK_EQ(rowCount(db, "t"), size_t(3));
}

// With a 16-page pool, a 2,000-row transaction must evict dirty pages into
// the log as uncommitted frames; rollback has to forget all of them.
TEST(transactionLargerThanBufferPool) {
    DatabaseOptions options;
    options.poolPages = 16;
    Database db(options);
    db.execute("CREATE TABLE t (id INT PRIMARY KEY, v TEXT)");
    insertRows(db, 100000, 5);

    db.execute("BEGIN");
    for (int batch = 0; batch < 20; ++batch) insertRows(db, batch * 100, 100);
    CHECK_EQ(rowCount(db, "t"), size_t(2005));
    checkTree(db, "t");
    db.execute("ROLLBACK");
    CHECK_EQ(rowCount(db, "t"), size_t(5));
    checkTree(db, "t");

    db.execute("BEGIN");
    for (int batch = 0; batch < 20; ++batch) insertRows(db, batch * 100, 100);
    db.execute("COMMIT");
    CHECK_EQ(rowCount(db, "t"), size_t(2005));
    checkTree(db, "t");
}

// Copy the files mid-transaction (some uncommitted pages already evicted to
// the log) and open the copy: committed rows are there, uncommitted are not.
TEST(recoveryKeepsCommittedAndDropsUncommitted) {
    DatabaseOptions options;
    options.poolPages = 16;
    options.checkpointFrames = 1000000;  // keep everything in the log
    CrashableDb crashable(options);
    Database& db = *crashable.db;
    db.execute("CREATE TABLE t (id INT PRIMARY KEY, v TEXT)");
    for (int batch = 0; batch < 10; ++batch) insertRows(db, batch * 100, 100);
    db.execute("BEGIN");
    for (int batch = 10; batch < 30; ++batch) insertRows(db, batch * 100, 100);
    db.execute("DELETE FROM t WHERE id < 500");
    CHECK(crashable.walFile->size() > 0);

    std::unique_ptr<Database> recovered = crashable.openSnapshot(options);
    CHECK_EQ(rowCount(*recovered, "t"), size_t(1000));
    CHECK(recovered->pager().stats().recoveredFrames > 0);
    CHECK(recovered->pager().stats().discardedFrames > 0);
    checkTree(*recovered, "t");
}

// A crash in the middle of writing the last commit's frames: the torn frame
// fails its checksum and that whole transaction is ignored.
TEST(recoveryIgnoresATornFinalFrame) {
    DatabaseOptions options;
    options.checkpointFrames = 1000000;
    CrashableDb crashable(options);
    Database& db = *crashable.db;
    db.execute("CREATE TABLE t (id INT PRIMARY KEY, v TEXT)");
    insertRows(db, 0, 10);
    size_t sizeBefore = crashable.walFile->size();
    insertRows(db, 10, 10);
    // Tear the last frame (the commit frame): keep half of it.
    crashable.walFile->truncate(crashable.walFile->size() - Wal::kFrameSize / 2);
    CHECK(crashable.walFile->size() > sizeBefore);

    std::unique_ptr<Database> recovered = crashable.openSnapshot(options);
    CHECK_EQ(rowCount(*recovered, "t"), size_t(10));
}

TEST(recoveryIgnoresCorruptFrameAndEverythingAfter) {
    DatabaseOptions options;
    options.checkpointFrames = 1000000;
    CrashableDb crashable(options);
    Database& db = *crashable.db;
    db.execute("CREATE TABLE t (id INT PRIMARY KEY, v TEXT)");
    insertRows(db, 0, 10);
    size_t firstGood = crashable.walFile->size();
    insertRows(db, 10, 10);
    insertRows(db, 20, 10);
    // Flip a byte inside the page data of the first frame of the second insert.
    crashable.walFile->bytes()[firstGood + Wal::kFrameHeaderSize + 100] ^= 0x5a;

    std::unique_ptr<Database> recovered = crashable.openSnapshot(options);
    CHECK_EQ(rowCount(*recovered, "t"), size_t(10));
}

TEST(checkpointsKeepTheLogSmall) {
    DatabaseOptions options;
    options.checkpointFrames = 20;
    CrashableDb crashable(options);
    Database& db = *crashable.db;
    db.execute("CREATE TABLE t (id INT PRIMARY KEY, v TEXT)");
    for (int batch = 0; batch < 50; ++batch) insertRows(db, batch * 50, 50);
    CHECK(db.pager().stats().checkpoints > 3);
    CHECK(crashable.walFile->size() <= Wal::kHeaderSize + 40 * Wal::kFrameSize);

    std::unique_ptr<Database> recovered = crashable.openSnapshot(options);
    CHECK_EQ(rowCount(*recovered, "t"), size_t(2500));
    checkTree(*recovered, "t");
}

TEST(reopeningARecoveredDatabaseIsStable) {
    DatabaseOptions options;
    options.checkpointFrames = 1000000;
    CrashableDb crashable(options);
    crashable.db->execute("CREATE TABLE t (id INT PRIMARY KEY, v TEXT)");
    insertRows(*crashable.db, 0, 300);
    std::unique_ptr<Database> first = crashable.openSnapshot(options);
    insertRows(*first, 300, 10);
    CHECK_EQ(rowCount(*first, "t"), size_t(310));
}

TEST(walRecoveryStopsAtFirstBadFrame) {
    MemoryFile file;
    Wal wal(file);
    CHECK_EQ(wal.open().pages.size(), size_t(0));
    char page[kPageSize];
    std::memset(page, 'a', sizeof(page));
    wal.append(5, page, 0);
    wal.append(6, page, 10);  // commit
    wal.markCommitted();
    wal.append(7, page, 0);   // never committed

    MemoryFile copy;
    copy.bytes() = file.bytes();
    Wal reopened(copy);
    Wal::Recovery recovery = reopened.open();
    CHECK_EQ(recovery.pages.size(), size_t(2));
    CHECK_EQ(recovery.pageCount, uint32_t(10));
    CHECK_EQ(recovery.frames, uint64_t(2));
    CHECK_EQ(recovery.discardedFrames, uint64_t(1));
    CHECK(recovery.pages.count(7) == 0);
}

TEST(walResetInvalidatesOldFrames) {
    MemoryFile file;
    Wal wal(file);
    wal.open();
    char page[kPageSize] = {};
    wal.append(3, page, 4);
    wal.markCommitted();
    std::vector<char> withFrames = file.bytes();
    wal.reset();
    // Simulate the truncate being lost: the old frames are still in the file,
    // but under the old salt, so they must not be replayed.
    std::vector<char> header(file.bytes().begin(), file.bytes().begin() + Wal::kHeaderSize);
    std::copy(header.begin(), header.end(), withFrames.begin());
    MemoryFile copy;
    copy.bytes() = withFrames;
    Wal reopened(copy);
    CHECK_EQ(reopened.open().pages.size(), size_t(0));
}
