#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ast.h"
#include "storage/btree.h"
#include "storage/index_key.h"
#include "storage/pager.h"
#include "table_store.h"
#include "value.h"

namespace jerryql {

struct Column {
    std::string name;
    Type type;
    std::string qualifier;  // table name or alias, in a query's scope; empty in the catalog
};

struct Schema {
    std::vector<Column> columns;
    std::optional<size_t> primaryKey;  // index into columns; must be an INT column

    std::optional<size_t> indexOf(const std::string& name) const;
    // Resolves "qualifier.name" or a bare "name" in a query. Throws SqlError
    // if a bare name matches columns of more than one table.
    std::optional<size_t> resolve(const std::string& qualifier, const std::string& name) const;
};

// A table's schema with every column qualified by `qualifier` (alias or name).
Schema scopedSchema(const Schema& schema, const std::string& qualifier);

// Validates a CREATE TABLE definition and builds its schema. Throws SqlError.
Schema schemaFromDefinition(const CreateTableStmt& stmt);

// A secondary index: a B+tree of (column value, primary key) entries.
struct Index {
    int64_t id = 0;   // key of this index's record in the schema tree
    std::string name;
    size_t column = 0;
    PageId root = 0;
    std::unique_ptr<BTree> tree;
};

struct Table {
    int64_t id = 0;   // key of this table's record in the schema tree
    PageId root = 0;  // root page of the table's B+tree
    std::string name;
    Schema schema;
    std::unique_ptr<TableStore> store;
    std::vector<Index> indexes;

    // Row writes that keep every index in step with the table.
    void insertRow(int64_t key, const Row& row);
    void replaceRow(int64_t key, const Row& oldRow, const Row& newRow);
    void eraseRow(int64_t key, const Row& oldRow);
    // Throws SqlError if a value is too long to be an index key. Called
    // while validating, before anything is written.
    void checkIndexable(const Row& row) const;
    const Index* indexOn(size_t column) const;
    std::string indexSql(const Index& index) const;

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
    bool dropTable(const std::string& name);                     // and its indexes
    // Validates every existing row first, then builds the index from them.
    Index& createIndex(const std::string& name, const std::string& table, const std::string& column);
    bool dropIndex(const std::string& name);
    Table& getTable(const std::string& name);                    // throws if missing
    std::vector<std::string> tableNames() const;                 // sorted

private:
    void loadTables();
    Table makeTable(int64_t id, const std::string& name, Schema schema, PageId root);
    Index makeIndex(int64_t id, const std::string& name, size_t column, PageId root);
    int64_t nextSchemaId();
    void writeRecord(int64_t id, const std::string& name, const std::string& sql, PageId root);

    Pager& pager_;
    BTree schemaTree_;
    std::map<std::string, Table> tables_;
    std::map<std::string, std::string> indexTables_;  // index name -> table name
};

}  // namespace jerryql
