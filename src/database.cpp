#include "database.h"

#include <set>

#include "expr_eval.h"
#include "parser.h"
#include "planner.h"
#include "sql_error.h"
#include "storage/btree.h"
#include "storage/file.h"
#include "storage/row_codec.h"

namespace jerryql {

namespace {

QueryResult messageResult(std::string message) {
    QueryResult result;
    result.message = std::move(message);
    return result;
}

// B+tree cells hold at most kMaxPayload bytes; there are no overflow pages yet.
void checkRowSize(const Row& row) {
    size_t bytes = encodeRow(row).size();
    if (bytes > kMaxPayload) {
        throw SqlError("row too large: " + std::to_string(bytes) + " bytes (the limit is " +
                       std::to_string(kMaxPayload) + ")");
    }
}

void checkType(const Column& column, const Value& value) {
    if (value.type() != column.type) {
        throw SqlError("column " + column.name + " expects " + typeName(column.type) + " but got " +
                       typeName(value.type()) + " " + value.toSqlLiteral());
    }
}

std::string duplicateKeyError(const Table& table, int64_t key) {
    const std::string& pk = table.schema.columns[*table.schema.primaryKey].name;
    return "duplicate primary key: " + table.name + "." + pk + " = " + std::to_string(key);
}

// For each table column, the position of its value in an INSERT tuple.
std::vector<size_t> insertColumnOrder(const InsertStmt& stmt, const Schema& schema) {
    std::vector<size_t> order(schema.columns.size());
    if (stmt.columns.empty()) {
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        return order;
    }
    std::vector<bool> seen(schema.columns.size(), false);
    for (size_t pos = 0; pos < stmt.columns.size(); ++pos) {
        auto index = schema.indexOf(stmt.columns[pos]);
        if (!index) throw SqlError("no such column: " + stmt.columns[pos]);
        if (seen[*index]) throw SqlError("column " + stmt.columns[pos] + " listed twice");
        seen[*index] = true;
        order[*index] = pos;
    }
    for (size_t i = 0; i < seen.size(); ++i) {
        if (!seen[i]) {
            throw SqlError("missing value for column " + schema.columns[i].name +
                           " (JerryQL has no NULLs or defaults yet)");
        }
    }
    return order;
}

}  // namespace

Database::Database(DatabaseOptions options)
    : pager_(std::make_unique<Pager>(std::make_unique<MemoryFile>(), options.bufferPoolPages)),
      catalog_(*pager_) {}

Database::Database(const std::string& path, DatabaseOptions options)
    : pager_(std::make_unique<Pager>(std::make_unique<PosixFile>(path), options.bufferPoolPages)),
      catalog_(*pager_) {
    pager_->flush();  // a new file gets its header and schema tree right away
}

Database::~Database() {
    if (!pager_) return;  // moved from
    try {
        pager_->flush();
    } catch (const std::exception&) {
        // Destructors must not throw; every write statement already flushed.
    }
}

QueryResult Database::execute(const std::string& sql) {
    Statement statement = parseOne(sql);
    return execute(statement);
}

QueryResult Database::execute(Statement& statement) {
    if (auto* s = std::get_if<SelectStmt>(&statement)) return select(*s);
    QueryResult result;
    if (auto* s = std::get_if<CreateTableStmt>(&statement)) {
        result = createTable(*s);
    } else if (auto* s = std::get_if<DropTableStmt>(&statement)) {
        result = dropTable(*s);
    } else if (auto* s = std::get_if<InsertStmt>(&statement)) {
        result = insert(*s);
    } else if (auto* s = std::get_if<UpdateStmt>(&statement)) {
        result = update(*s);
    } else {
        result = remove(std::get<DeleteStmt>(statement));
    }
    pager_->flush();
    return result;
}

QueryResult Database::createTable(const CreateTableStmt& stmt) {
    catalog_.createTable(stmt.table, schemaFromDefinition(stmt));
    return messageResult("CREATE TABLE");
}

QueryResult Database::dropTable(const DropTableStmt& stmt) {
    if (!catalog_.dropTable(stmt.table)) throw SqlError("no such table: " + stmt.table);
    return messageResult("DROP TABLE");
}

// Validates every row (types, duplicate keys) before inserting any, so a
// failing multi-row INSERT leaves the table unchanged.
QueryResult Database::insert(InsertStmt& stmt) {
    Table& table = catalog_.getTable(stmt.table);
    const Schema& schema = table.schema;
    std::vector<size_t> order = insertColumnOrder(stmt, schema);
    const size_t expectedValues = stmt.columns.empty() ? schema.columns.size() : stmt.columns.size();

    std::vector<Row> rows;
    std::set<int64_t> batchKeys;
    for (const auto& tuple : stmt.rows) {
        if (tuple.size() != expectedValues) {
            throw SqlError("expected " + std::to_string(expectedValues) + " values but got " +
                           std::to_string(tuple.size()));
        }
        Row row(schema.columns.size());
        for (size_t col = 0; col < schema.columns.size(); ++col) {
            row[col] = evaluate(*tuple[order[col]], Row{});
            checkType(schema.columns[col], row[col]);
        }
        checkRowSize(row);
        if (schema.primaryKey) {
            int64_t key = row[*schema.primaryKey].asInt();
            if (table.store->contains(key) || !batchKeys.insert(key).second) {
                throw SqlError(duplicateKeyError(table, key));
            }
        }
        rows.push_back(std::move(row));
    }

    for (Row& row : rows) {
        int64_t key = table.keyForNewRow(row);
        table.store->insert(key, std::move(row));
    }
    return messageResult("INSERT " + std::to_string(rows.size()));
}

QueryResult Database::select(SelectStmt& stmt) {
    Table& table = catalog_.getTable(stmt.table);
    OperatorPtr plan = planSelect(stmt, table);

    QueryResult result;
    result.hasRows = true;
    if (stmt.explain) {
        result.columns = {"QUERY PLAN"};
        for (const std::string& line : explainPlan(*plan)) result.rows.push_back({Value::text(line)});
        return result;
    }

    if (stmt.items.empty()) {
        for (const Column& column : table.schema.columns) result.columns.push_back(column.name);
    } else {
        for (const SelectItem& item : stmt.items) {
            result.columns.push_back(item.alias.empty() ? exprToString(*item.expr) : item.alias);
        }
    }
    Tuple tuple;
    while (plan->next(tuple)) result.rows.push_back(std::move(tuple.row));
    return result;
}

// Reads every matching row before writing any. Updating while scanning could
// revisit a row whose key moved forward (the "Halloween problem").
QueryResult Database::update(UpdateStmt& stmt) {
    Table& table = catalog_.getTable(stmt.table);
    const Schema& schema = table.schema;

    std::vector<size_t> targets;
    std::set<size_t> assigned;
    for (auto& [column, expr] : stmt.assignments) {
        auto index = schema.indexOf(column);
        if (!index) throw SqlError("no such column: " + column);
        if (!assigned.insert(*index).second) throw SqlError("column " + column + " assigned twice");
        bindColumns(*expr, schema);
        targets.push_back(*index);
    }

    std::vector<Tuple> matches;
    OperatorPtr source = planRowSource(table, stmt.where.get());
    for (Tuple tuple; source->next(tuple);) matches.push_back(tuple);

    // Compute new rows; every right-hand side sees the row's old values.
    std::vector<Tuple> updated;
    std::set<int64_t> oldKeys, newKeys;
    for (const Tuple& match : matches) {
        Tuple next{match.key, match.row};
        for (size_t i = 0; i < targets.size(); ++i) {
            Value value = evaluate(*stmt.assignments[i].second, match.row);
            checkType(schema.columns[targets[i]], value);
            next.row[targets[i]] = std::move(value);
        }
        checkRowSize(next.row);
        next.key = table.keyForUpdatedRow(next.row, match.key);
        oldKeys.insert(match.key);
        if (!newKeys.insert(next.key).second) throw SqlError(duplicateKeyError(table, next.key));
        updated.push_back(std::move(next));
    }
    for (size_t i = 0; i < updated.size(); ++i) {
        int64_t key = updated[i].key;
        if (key != matches[i].key && !oldKeys.count(key) && table.store->contains(key)) {
            throw SqlError(duplicateKeyError(table, key));
        }
    }

    // Apply: remove rows whose key changes first, so keys can swap.
    for (size_t i = 0; i < updated.size(); ++i) {
        if (updated[i].key != matches[i].key) table.store->erase(matches[i].key);
    }
    for (size_t i = 0; i < updated.size(); ++i) {
        if (updated[i].key == matches[i].key) {
            table.store->replace(updated[i].key, std::move(updated[i].row));
        } else {
            table.store->insert(updated[i].key, std::move(updated[i].row));
        }
    }
    return messageResult("UPDATE " + std::to_string(updated.size()));
}

QueryResult Database::remove(DeleteStmt& stmt) {
    Table& table = catalog_.getTable(stmt.table);
    std::vector<int64_t> keys;
    OperatorPtr source = planRowSource(table, stmt.where.get());
    for (Tuple tuple; source->next(tuple);) keys.push_back(tuple.key);
    for (int64_t key : keys) table.store->erase(key);
    return messageResult("DELETE " + std::to_string(keys.size()));
}

}  // namespace jerryql
