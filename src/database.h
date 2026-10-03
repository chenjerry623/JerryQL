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

struct DatabaseOptions {
    size_t bufferPoolPages = 1024;  // 4 MiB of 4 KiB pages
};

// A database: a pager (file + buffer pool) and the catalog of tables stored
// in it. Every write statement ends by flushing dirty pages and fsyncing.
// Without a write-ahead log a crash during that flush can still corrupt the
// file; crash safety is the next milestone.
class Database {
public:
    // In-memory database (same B+tree code, backed by a MemoryFile).
    explicit Database(DatabaseOptions options = {});
    // Opens or creates a database file.
    explicit Database(const std::string& path, DatabaseOptions options = {});
    Database(Database&&) = default;
    ~Database();

    QueryResult execute(Statement& statement);
    QueryResult execute(const std::string& sql);  // exactly one statement

    std::vector<std::string> tableNames() const { return catalog_.tableNames(); }
    Table& table(const std::string& name) { return catalog_.getTable(name); }
    Pager& pager() { return *pager_; }

private:
    QueryResult createTable(const CreateTableStmt& stmt);
    QueryResult dropTable(const DropTableStmt& stmt);
    QueryResult insert(InsertStmt& stmt);
    QueryResult select(SelectStmt& stmt);
    QueryResult update(UpdateStmt& stmt);
    QueryResult remove(DeleteStmt& stmt);

    std::unique_ptr<Pager> pager_;
    Catalog catalog_;
};

}  // namespace jerryql
