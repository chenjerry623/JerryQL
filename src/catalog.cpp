#include "catalog.h"

#include <limits>
#include <set>
#include <stdexcept>

#include "parser.h"
#include "sql_error.h"
#include "storage/btree_store.h"
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

void Catalog::loadTables() {
    BTreeCursor cursor = schemaTree_.scan(std::numeric_limits<int64_t>::min(),
                                          std::numeric_limits<int64_t>::max());
    int64_t id;
    std::string payload;
    while (cursor.next(id, payload)) {
        Row record = decodeRow(payload);  // (name, sql, root page)
        const std::string& name = record.at(0).asText();
        Statement statement = parseOne(record.at(1).asText());
        Schema schema = schemaFromDefinition(std::get<CreateTableStmt>(statement));
        PageId root = PageId(record.at(2).asInt());
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
    table.id = int64_t(schemaTree_.aux()) + 1;
    schemaTree_.setAux(uint64_t(table.id));
    table.store = std::make_unique<BTreeStore>(pager_, table.root);
    schemaTree_.insert(table.id, encodeRow({Value::text(name), Value::text(table.toCreateSql()),
                                            Value::integer(table.root)}));
    return tables_.emplace(name, std::move(table)).first->second;
}

bool Catalog::dropTable(const std::string& name) {
    auto it = tables_.find(name);
    if (it == tables_.end()) return false;
    BTree(pager_, it->second.root).destroy();
    schemaTree_.erase(it->second.id);
    tables_.erase(it);
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
