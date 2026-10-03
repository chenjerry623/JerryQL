#include "database.h"

#include <set>

#include "expr_eval.h"
#include "parser.h"
#include "planner.h"
#include "sql_error.h"
#include "storage/btree.h"
#include "storage/btree_store.h"
#include "storage/index_key.h"
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
    : Database(std::make_unique<MemoryFile>(), std::make_unique<MemoryFile>(), options) {}

namespace {

std::unique_ptr<File> openWalFile(const std::string& dbPath) {
    std::string walPath = dbPath + "-wal";
    bool existed = fileExists(walPath);
    auto file = std::make_unique<PosixFile>(walPath);
    if (!existed) syncDirectoryOf(walPath);  // make the new file's name durable
    return file;
}

std::unique_ptr<File> openDbFile(const std::string& path) {
    bool existed = fileExists(path);
    auto file = std::make_unique<PosixFile>(path);
    if (!existed) syncDirectoryOf(path);
    return file;
}

}  // namespace

Database::Database(const std::string& path, DatabaseOptions options)
    : Database(openDbFile(path), openWalFile(path), options) {}

Database::Database(std::unique_ptr<File> dbFile, std::unique_ptr<File> walFile,
                   DatabaseOptions options)
    : pager_(std::make_unique<Pager>(std::move(dbFile), std::move(walFile), options)),
      catalog_(std::make_unique<Catalog>(*pager_)) {
    pager_->commit();  // a new database gets its header and schema tree right away
}

Database::~Database() {
    if (!pager_) return;  // moved from
    try {
        if (inTransaction_) pager_->rollback();
        pager_->checkpoint();
    } catch (const std::exception&) {
        // Destructors must not throw. Committed work is already in the log.
    }
}

void Database::rollbackAndReload() {
    pager_->rollback();
    catalog_ = std::make_unique<Catalog>(*pager_);  // table list may have changed
    inTransaction_ = false;
}

QueryResult Database::execute(const std::string& sql) {
    Statement statement = parseOne(sql);
    return execute(statement);
}

QueryResult Database::execute(Statement& statement) {
    if (auto* s = std::get_if<SelectStmt>(&statement)) return select(*s);
    if (auto* s = std::get_if<TransactionStmt>(&statement)) return transaction(*s);
    return runWrite(statement);
}

// Validation errors (SqlError) are raised before a statement writes anything.
// Any other exception may leave a statement half-applied, so the whole
// transaction is rolled back.
QueryResult Database::runWrite(Statement& statement) {
    QueryResult result;
    try {
        if (auto* s = std::get_if<CreateTableStmt>(&statement)) {
            result = createTable(*s);
        } else if (auto* s = std::get_if<DropTableStmt>(&statement)) {
            result = dropTable(*s);
        } else if (auto* s = std::get_if<InsertStmt>(&statement)) {
            result = insert(*s);
        } else if (auto* s = std::get_if<CreateIndexStmt>(&statement)) {
            result = createIndex(*s);
        } else if (auto* s = std::get_if<DropIndexStmt>(&statement)) {
            result = dropIndex(*s);
        } else if (auto* s = std::get_if<UpdateStmt>(&statement)) {
            result = update(*s);
        } else {
            result = remove(std::get<DeleteStmt>(statement));
        }
        if (!inTransaction_) pager_->commit();
    } catch (const SqlError&) {
        throw;
    } catch (const std::exception& e) {
        bool wasInTransaction = inTransaction_;
        rollbackAndReload();
        throw SqlError(std::string(e.what()) +
                       (wasInTransaction ? " (transaction rolled back)" : " (statement rolled back)"));
    }
    return result;
}

QueryResult Database::transaction(const TransactionStmt& stmt) {
    switch (stmt.action) {
        case TransactionAction::Begin:
            if (inTransaction_) throw SqlError("a transaction is already open");
            inTransaction_ = true;
            return messageResult("BEGIN");
        case TransactionAction::Commit:
            if (!inTransaction_) throw SqlError("no transaction is open");
            try {
                pager_->commit();
            } catch (const std::exception& e) {
                rollbackAndReload();
                throw SqlError(std::string("commit failed: ") + e.what() + " (transaction rolled back)");
            }
            inTransaction_ = false;
            return messageResult("COMMIT");
        case TransactionAction::Rollback:
            if (!inTransaction_) throw SqlError("no transaction is open");
            rollbackAndReload();
            return messageResult("ROLLBACK");
    }
    throw SqlError("unknown transaction statement");
}

QueryResult Database::createTable(const CreateTableStmt& stmt) {
    catalog_->createTable(stmt.table, schemaFromDefinition(stmt));
    return messageResult("CREATE TABLE");
}

QueryResult Database::dropTable(const DropTableStmt& stmt) {
    if (!catalog_->dropTable(stmt.table)) throw SqlError("no such table: " + stmt.table);
    return messageResult("DROP TABLE");
}

// Validates every row (types, duplicate keys) before inserting any, so a
// failing multi-row INSERT leaves the table unchanged.
QueryResult Database::insert(InsertStmt& stmt) {
    Table& table = catalog_->getTable(stmt.table);
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
        table.checkIndexable(row);
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
        table.insertRow(key, row);
    }
    return messageResult("INSERT " + std::to_string(rows.size()));
}

QueryResult Database::select(SelectStmt& stmt) {
    Table& table = catalog_->getTable(stmt.table);
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
    Table& table = catalog_->getTable(stmt.table);
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
        table.checkIndexable(next.row);
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
        if (updated[i].key != matches[i].key) table.eraseRow(matches[i].key, matches[i].row);
    }
    for (size_t i = 0; i < updated.size(); ++i) {
        if (updated[i].key == matches[i].key) {
            table.replaceRow(updated[i].key, matches[i].row, updated[i].row);
        } else {
            table.insertRow(updated[i].key, updated[i].row);
        }
    }
    return messageResult("UPDATE " + std::to_string(updated.size()));
}

QueryResult Database::remove(DeleteStmt& stmt) {
    Table& table = catalog_->getTable(stmt.table);
    std::vector<Tuple> matches;
    OperatorPtr source = planRowSource(table, stmt.where.get());
    for (Tuple tuple; source->next(tuple);) matches.push_back(tuple);
    source.reset();  // release the cursor's pinned page before writing
    for (const Tuple& match : matches) table.eraseRow(match.key, match.row);
    return messageResult("DELETE " + std::to_string(matches.size()));
}

std::string Database::checkIntegrity() {
    try {
        for (const std::string& name : catalog_->tableNames()) {
            Table& table = catalog_->getTable(name);
            static_cast<BTreeStore&>(*table.store).tree().check();
            for (Index& index : table.indexes) {
                index.tree->check();
                if (index.tree->size() != table.store->size()) {
                    return "index " + index.name + " has " + std::to_string(index.tree->size()) +
                           " entries but " + name + " has " + std::to_string(table.store->size()) + " rows";
                }
                BTreeCursor cursor = index.tree->scan("", std::nullopt);
                std::string entry, payload;
                while (cursor.next(entry, payload)) {
                    int64_t key = primaryKeyOfEntry(entry);
                    std::optional<Row> row = table.store->get(key);
                    if (!row) return "index " + index.name + " points to missing row " + std::to_string(key);
                    if (encodeValueKey((*row)[index.column]) != entry.substr(0, entry.size() - 8)) {
                        return "index " + index.name + " has a stale value for row " + std::to_string(key);
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

QueryResult Database::createIndex(const CreateIndexStmt& stmt) {
    catalog_->createIndex(stmt.index, stmt.table, stmt.column);
    return messageResult("CREATE INDEX");
}

QueryResult Database::dropIndex(const DropIndexStmt& stmt) {
    if (!catalog_->dropIndex(stmt.index)) throw SqlError("no such index: " + stmt.index);
    return messageResult("DROP INDEX");
}

}  // namespace jerryql
