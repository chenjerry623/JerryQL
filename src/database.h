#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast.h"
#include "catalog.h"
#include "storage/pager.h"

namespace jerryql {

struct QueryResult {
    bool hasRows = false;              // SELECT / EXPLAIN produce a table
    std::vector<std::string> columns;
    std::vector<Row> rows;
    std::string message;               // e.g. "INSERT 3" for statements without rows
};

using DatabaseOptions = PagerOptions;

// A database: a pager (file, write-ahead log, buffer pool) and the catalog of
// tables stored in it.
//
// Transactions: outside BEGIN ... COMMIT every write statement commits on its
// own (autocommit). A commit is durable once it returns: its pages are in the
// fsynced write-ahead log. A statement that fails validation (bad type,
// duplicate key, ...) changes nothing and, inside a transaction, leaves the
// transaction open. Any other failure rolls the transaction back.
class Database {
public:
    // In-memory database (same code paths, backed by MemoryFiles).
    explicit Database(DatabaseOptions options = {});
    // Opens or creates a database file and its log, "<path>-wal". Opening
    // runs crash recovery.
    explicit Database(const std::string& path, DatabaseOptions options = {});
    // Uses the given files; for crash-simulation tests.
    Database(std::unique_ptr<File> dbFile, std::unique_ptr<File> walFile, DatabaseOptions options = {});
    Database(Database&&) = default;
    // Rolls back an open transaction, checkpoints, and closes.
    ~Database();

    QueryResult execute(Statement& statement);
    QueryResult execute(const std::string& sql);  // exactly one statement

    std::vector<std::string> tableNames() const { return catalog_->tableNames(); }
    Table& table(const std::string& name) { return catalog_->getTable(name); }
    Pager& pager() { return *pager_; }
    bool inTransaction() const { return inTransaction_; }

    // Checks every B+tree's structure and that each index holds exactly one
    // entry per row, carrying the row's current value. Returns "" if all is
    // consistent, otherwise a description of the first problem.
    std::string checkIntegrity();

private:
    QueryResult createTable(const CreateTableStmt& stmt);
    QueryResult dropTable(const DropTableStmt& stmt);
    QueryResult insert(InsertStmt& stmt);
    QueryResult select(SelectStmt& stmt);
    QueryResult update(UpdateStmt& stmt);
    QueryResult remove(DeleteStmt& stmt);
    QueryResult createIndex(const CreateIndexStmt& stmt);
    QueryResult dropIndex(const DropIndexStmt& stmt);
    QueryResult transaction(const TransactionStmt& stmt);
    QueryResult runWrite(Statement& statement);
    void rollbackAndReload();

    std::unique_ptr<Pager> pager_;
    std::unique_ptr<Catalog> catalog_;
    bool inTransaction_ = false;
};

}  // namespace jerryql
