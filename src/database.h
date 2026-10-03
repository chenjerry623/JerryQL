#pragma once

#include <string>
#include <vector>

#include "ast.h"
#include "catalog.h"

namespace jerryql {

struct QueryResult {
    bool hasRows = false;              // SELECT / EXPLAIN produce a table
    std::vector<std::string> columns;
    std::vector<Row> rows;
    std::string message;               // e.g. "INSERT 3" for statements without rows
};

class Database {
public:
    QueryResult execute(Statement& statement);
    QueryResult execute(const std::string& sql);  // exactly one statement

    std::vector<std::string> tableNames() const { return catalog_.tableNames(); }
    Table& table(const std::string& name) { return catalog_.getTable(name); }

private:
    QueryResult createTable(const CreateTableStmt& stmt);
    QueryResult dropTable(const DropTableStmt& stmt);
    QueryResult insert(InsertStmt& stmt);
    QueryResult select(SelectStmt& stmt);
    QueryResult update(UpdateStmt& stmt);
    QueryResult remove(DeleteStmt& stmt);

    Catalog catalog_;
};

}  // namespace jerryql
