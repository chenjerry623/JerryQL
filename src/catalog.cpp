#include "catalog.h"

#include <limits>
#include <set>
#include <stdexcept>

#include "parser.h"
#include "sql_error.h"
#include "storage/btree_store.h"
#include "storage/index_key.h"
#include "storage/row_codec.h"

namespace jerryql {

std::optional<size_t> Schema::indexOf(const std::string& name) const {
    for (size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].name == name) return i;
    }
    return std::nullopt;
}

Schema schemaFromDefinition(const CreateTableStmt& stmt) {
    Schema schema;
    std::set<std::string> names;
    for (const ColumnDef& def : stmt.columns) {
        if (!names.insert(def.name).second) throw SqlError("duplicate column name: " + def.name);
        if (def.primaryKey) {
            if (schema.primaryKey) throw SqlError("a table can have only one PRIMARY KEY");
            if (def.type != Type::Int) throw SqlError("PRIMARY KEY column must be INT");
            schema.primaryKey = schema.columns.size();
        }
        schema.columns.push_back({def.name, def.type});
    }
    return schema;
}

int64_t Table::keyForNewRow(const Row& row) {
    if (schema.primaryKey) return row[*schema.primaryKey].asInt();
    return store->allocateRowId();
}

int64_t Table::keyForUpdatedRow(const Row& row, int64_t existingKey) const {
    return schema.primaryKey ? row[*schema.primaryKey].asInt() : existingKey;
}

void Table::insertRow(int64_t key, const Row& row) {
    store->insert(key, row);
    for (Index& index : indexes) index.tree->insert(indexEntryKey(row[index.column], key), "");
}

void Table::replaceRow(int64_t key, const Row& oldRow, const Row& newRow) {
    store->replace(key, newRow);
    for (Index& index : indexes) {
        if (oldRow[index.column] == newRow[index.column]) continue;
        index.tree->erase(indexEntryKey(oldRow[index.column], key));
        index.tree->insert(indexEntryKey(newRow[index.column], key), "");
    }
}

void Table::eraseRow(int64_t key, const Row& oldRow) {
    store->erase(key);
    for (Index& index : indexes) index.tree->erase(indexEntryKey(oldRow[index.column], key));
}

void Table::checkIndexable(const Row& row) const {
    for (const Index& index : indexes) {
        if (encodeValueKey(row[index.column]).size() + 8 > kMaxKeySize) {
            throw SqlError("value in " + name + "." + schema.columns[index.column].name +
                           " is too long for index " + index.name + " (index keys are limited to " +
                           std::to_string(kMaxKeySize) + " bytes)");
        }
    }
}

const Index* Table::indexOn(size_t column) const {
    for (const Index& index : indexes) {
        if (index.column == column) return &index;
    }
    return nullptr;
}

std::string Table::indexSql(const Index& index) const {
    return "CREATE INDEX " + index.name + " ON " + name + " (" + schema.columns[index.column].name + ")";
}

std::string Table::toCreateSql() const {
    std::string sql = "CREATE TABLE " + name + " (";
    for (size_t i = 0; i < schema.columns.size(); ++i) {
        if (i > 0) sql += ", ";
        sql += schema.columns[i].name + " " + typeName(schema.columns[i].type);
        if (schema.primaryKey == i) sql += " PRIMARY KEY";
    }
    return sql + ")";
}

Catalog::Catalog(Pager& pager)
    : pager_(pager), schemaTree_(pager, Pager::kSchemaRootPage) {
    if (pager.pageCount() < 2) {  // only the header page exists: a brand-new database
        if (BTree::create(pager) != Pager::kSchemaRootPage) {
            throw std::logic_error("schema tree must be page 1");
        }
    } else {
        loadTables();
    }
}

Table Catalog::makeTable(int64_t id, const std::string& name, Schema schema, PageId root) {
    Table table;
    table.id = id;
    table.root = root;
    table.name = name;
    table.schema = std::move(schema);
    table.store = std::make_unique<BTreeStore>(pager_, root);
    return table;
}

Index Catalog::makeIndex(int64_t id, const std::string& name, size_t column, PageId root) {
    Index index;
    index.id = id;
    index.name = name;
    index.column = column;
    index.root = root;
    index.tree = std::make_unique<BTree>(pager_, root);
    return index;
}

int64_t Catalog::nextSchemaId() {
    int64_t id = int64_t(schemaTree_.aux()) + 1;
    schemaTree_.setAux(uint64_t(id));
    return id;
}

void Catalog::writeRecord(int64_t id, const std::string& name, const std::string& sql, PageId root) {
    schemaTree_.insert(encodeIntKey(id),
                       encodeRow({Value::text(name), Value::text(sql), Value::integer(root)}));
}

// Records are (name, CREATE TABLE or CREATE INDEX sql, root page), keyed by
// id. An index always has a larger id than its table, so tables load first.
void Catalog::loadTables() {
    BTreeCursor cursor = schemaTree_.scan("", std::nullopt);
    std::string key, payload;
    while (cursor.next(key, payload)) {
        int64_t id = decodeIntKey(key);
        Row record = decodeRow(payload);
        const std::string& name = record.at(0).asText();
        Statement statement = parseOne(record.at(1).asText());
        PageId root = PageId(record.at(2).asInt());
        if (auto* create = std::get_if<CreateIndexStmt>(&statement)) {
            Table& table = getTable(create->table);
            table.indexes.push_back(makeIndex(id, name, *table.schema.indexOf(create->column), root));
            indexTables_[name] = create->table;
            continue;
        }
        Schema schema = schemaFromDefinition(std::get<CreateTableStmt>(statement));
        tables_.emplace(name, makeTable(id, name, std::move(schema), root));
    }
}

Table& Catalog::createTable(const std::string& name, Schema schema) {
    if (tables_.count(name)) throw SqlError("table " + name + " already exists");
    Table table = makeTable(0, name, std::move(schema), 0);
    std::string record = encodeRow({Value::text(name), Value::text(table.toCreateSql()),
                                    Value::integer(0)});
    if (record.size() + 16 > kMaxPayload) throw SqlError("table definition is too long");

    table.root = BTree::create(pager_);
    table.id = nextSchemaId();
    table.store = std::make_unique<BTreeStore>(pager_, table.root);
    writeRecord(table.id, name, table.toCreateSql(), table.root);
    return tables_.emplace(name, std::move(table)).first->second;
}

bool Catalog::dropTable(const std::string& name) {
    auto it = tables_.find(name);
    if (it == tables_.end()) return false;
    for (Index& index : it->second.indexes) {
        index.tree->destroy();
        schemaTree_.erase(encodeIntKey(index.id));
        indexTables_.erase(index.name);
    }
    BTree(pager_, it->second.root).destroy();
    schemaTree_.erase(encodeIntKey(it->second.id));
    tables_.erase(it);
    return true;
}

Index& Catalog::createIndex(const std::string& name, const std::string& tableName,
                            const std::string& columnName) {
    if (indexTables_.count(name)) throw SqlError("index " + name + " already exists");
    Table& table = getTable(tableName);
    std::optional<size_t> column = table.schema.indexOf(columnName);
    if (!column) throw SqlError("no such column: " + columnName);
    if (table.schema.primaryKey == column) {
        throw SqlError("column " + columnName + " is the primary key, which the table is already sorted by");
    }
    if (table.indexOn(*column)) throw SqlError("column " + columnName + " already has an index");

    // Check every value fits before writing anything.
    std::vector<std::pair<int64_t, Value>> entries;
    {
        auto cursor = table.store->scan(KeyRange{});
        int64_t key;
        Row row;
        while (cursor->next(key, row)) entries.emplace_back(key, row[*column]);
    }
    for (const auto& entry : entries) {
        if (encodeValueKey(entry.second).size() + 8 > kMaxKeySize) {
            throw SqlError("a value in " + tableName + "." + columnName + " is too long to index (index keys are limited to " +
                           std::to_string(kMaxKeySize) + " bytes)");
        }
    }

    Index index = makeIndex(nextSchemaId(), name, *column, BTree::create(pager_));
    for (const auto& [key, value] : entries) index.tree->insert(indexEntryKey(value, key), "");
    writeRecord(index.id, name, table.indexSql(index), index.root);
    indexTables_[name] = tableName;
    table.indexes.push_back(std::move(index));
    return table.indexes.back();
}

bool Catalog::dropIndex(const std::string& name) {
    auto found = indexTables_.find(name);
    if (found == indexTables_.end()) return false;
    Table& table = getTable(found->second);
    for (auto it = table.indexes.begin(); it != table.indexes.end(); ++it) {
        if (it->name != name) continue;
        it->tree->destroy();
        schemaTree_.erase(encodeIntKey(it->id));
        table.indexes.erase(it);
        break;
    }
    indexTables_.erase(found);
    return true;
}

Table& Catalog::getTable(const std::string& name) {
    auto it = tables_.find(name);
    if (it == tables_.end()) throw SqlError("no such table: " + name);
    return it->second;
}

std::vector<std::string> Catalog::tableNames() const {
    std::vector<std::string> names;
    for (const auto& entry : tables_) names.push_back(entry.first);
    return names;
}

}  // namespace jerryql
