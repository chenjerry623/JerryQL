#include "catalog.h"

#include "memory_store.h"
#include "sql_error.h"

namespace jerryql {

std::optional<size_t> Schema::indexOf(const std::string& name) const {
    for (size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].name == name) return i;
    }
    return std::nullopt;
}

int64_t Table::keyForNewRow(const Row& row) {
    if (schema.primaryKey) return row[*schema.primaryKey].asInt();
    return nextRowId++;
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

Table& Catalog::createTable(const std::string& name, Schema schema) {
    if (tables_.count(name)) throw SqlError("table " + name + " already exists");
    Table table;
    table.name = name;
    table.schema = std::move(schema);
    table.store = std::make_unique<MemoryStore>();
    return tables_.emplace(name, std::move(table)).first->second;
}

bool Catalog::dropTable(const std::string& name) {
    return tables_.erase(name) > 0;
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
