#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ast.h"
#include "storage/btree.h"
#include "storage/pager.h"
#include "table_store.h"
#include "value.h"

namespace jerryql {

struct Column {
    std::string name;
    Type type;
};

struct Schema {
    std::vector<Column> columns;
    std::optional<size_t> primaryKey;  // index into columns; must be an INT column

    std::optional<size_t> indexOf(const std::string& name) const;
};

// Validates a CREATE TABLE definition and builds its schema. Throws SqlError.
Schema schemaFromDefinition(const CreateTableStmt& stmt);

struct Table {
    int64_t id = 0;   // key of this table's record in the schema tree
    PageId root = 0;  // root page of the table's B+tree
    std::string name;
    Schema schema;
    std::unique_ptr<TableStore> store;

    // The storage key for a new row: its primary key value, or a fresh row id.
    int64_t keyForNewRow(const Row& row);
    // The storage key a row would have; for tables without a primary key the
    // key never changes, so the existing key is returned.
    int64_t keyForUpdatedRow(const Row& row, int64_t existingKey) const;
    std::string toCreateSql() const;
};

// The set of tables, persisted in the schema tree (root page 1). Each record
// is keyed by table id and holds (name, CREATE TABLE sql, root page), like
// SQLite's sqlite_master. Opening a database re-parses the stored SQL.
class Catalog {
public:
    explicit Catalog(Pager& pager);

    Table& createTable(const std::string& name, Schema schema);  // throws if it exists
    bool dropTable(const std::string& name);
    Table& getTable(const std::string& name);                    // throws if missing
    std::vector<std::string> tableNames() const;                 // sorted

private:
    void loadTables();
    Table makeTable(int64_t id, const std::string& name, Schema schema, PageId root);

    Pager& pager_;
    BTree schemaTree_;
    std::map<std::string, Table> tables_;
};

}  // namespace jerryql
