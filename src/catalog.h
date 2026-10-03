#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

struct Table {
    std::string name;
    Schema schema;
    std::unique_ptr<TableStore> store;
    int64_t nextRowId = 1;  // hidden key for tables without a primary key

    // The storage key for a new row: its primary key value, or the next row id.
    int64_t keyForNewRow(const Row& row);
    // The storage key a row would have; for tables without a primary key the
    // key never changes, so the existing key is returned.
    int64_t keyForUpdatedRow(const Row& row, int64_t existingKey) const;
    std::string toCreateSql() const;
};

class Catalog {
public:
    Table& createTable(const std::string& name, Schema schema);  // throws if it exists
    bool dropTable(const std::string& name);
    Table& getTable(const std::string& name);                    // throws if missing
    std::vector<std::string> tableNames() const;                 // sorted

private:
    std::map<std::string, Table> tables_;
};

}  // namespace jerryql
