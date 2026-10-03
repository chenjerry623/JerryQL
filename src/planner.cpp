#include "planner.h"

#include <algorithm>
#include <limits>

#include "expr_eval.h"
#include "sql_error.h"

namespace jerryql {

namespace {

constexpr int64_t kMinKey = std::numeric_limits<int64_t>::min();
constexpr int64_t kMaxKey = std::numeric_limits<int64_t>::max();

void collectConjuncts(const Expr* expr, std::vector<const Expr*>& out) {
    if (expr == nullptr) return;
    if (expr->kind == ExprKind::Binary && expr->binaryOp == BinaryOp::And) {
        collectConjuncts(expr->left.get(), out);
        collectConjuncts(expr->right.get(), out);
    } else {
        out.push_back(expr);
    }
}

bool isIntLiteral(const Expr& expr) {
    return expr.kind == ExprKind::Literal && expr.value.isInt();
}

bool isColumn(const Expr& expr, const std::string& name) {
    return expr.kind == ExprKind::Column && expr.column == name;
}

// "5 < id" means "id > 5".
BinaryOp mirror(BinaryOp op) {
    switch (op) {
        case BinaryOp::Lt: return BinaryOp::Gt;
        case BinaryOp::Le: return BinaryOp::Ge;
        case BinaryOp::Gt: return BinaryOp::Lt;
        case BinaryOp::Ge: return BinaryOp::Le;
        default: return op;
    }
}

// Narrows `range` by "key <op> value". Returns false if op doesn't apply.
bool narrow(KeyRange& range, BinaryOp op, int64_t value) {
    switch (op) {
        case BinaryOp::Eq:
            range.lo = std::max(range.lo, value);
            range.hi = std::min(range.hi, value);
            break;
        case BinaryOp::Ge:
            range.lo = std::max(range.lo, value);
            break;
        case BinaryOp::Gt:
            if (value == kMaxKey) range.empty = true;
            else range.lo = std::max(range.lo, value + 1);
            break;
        case BinaryOp::Le:
            range.hi = std::min(range.hi, value);
            break;
        case BinaryOp::Lt:
            if (value == kMinKey) range.empty = true;
            else range.hi = std::min(range.hi, value - 1);
            break;
        default:
            return false;
    }
    if (range.lo > range.hi) range.empty = true;
    return true;
}

std::string describeScan(const Table& table, const KeyRange& range) {
    if (range.empty) return "EMPTY SCAN " + table.name + " (WHERE can never match)";
    if (range.isAll()) return "SEQ SCAN " + table.name;
    const std::string& pk = table.schema.columns[*table.schema.primaryKey].name;
    if (range.isPoint()) {
        return "PK LOOKUP " + table.name + " (" + pk + " = " + std::to_string(range.lo) + ")";
    }
    std::string bounds;
    if (range.lo != kMinKey) bounds += std::to_string(range.lo) + " <= ";
    bounds += pk;
    if (range.hi != kMaxKey) bounds += " <= " + std::to_string(range.hi);
    return "PK RANGE SCAN " + table.name + " (" + bounds + ")";
}

}  // namespace

KeyRange primaryKeyRange(const Expr* where, const Schema& schema) {
    KeyRange range;
    if (!schema.primaryKey) return range;
    const std::string& pk = schema.columns[*schema.primaryKey].name;

    std::vector<const Expr*> conjuncts;
    collectConjuncts(where, conjuncts);
    for (const Expr* c : conjuncts) {
        if (c->kind != ExprKind::Binary) continue;
        if (isColumn(*c->left, pk) && isIntLiteral(*c->right)) {
            narrow(range, c->binaryOp, c->right->value.asInt());
        } else if (isIntLiteral(*c->left) && isColumn(*c->right, pk)) {
            narrow(range, mirror(c->binaryOp), c->left->value.asInt());
        }
    }
    return range;
}

OperatorPtr planRowSource(const Table& table, Expr* where) {
    if (where) bindColumns(*where, table.schema);
    KeyRange range = primaryKeyRange(where, table.schema);
    OperatorPtr plan =
        std::make_unique<ScanOperator>(*table.store, range, describeScan(table, range));
    if (where) plan = std::make_unique<FilterOperator>(std::move(plan), *where);
    return plan;
}

namespace {

bool isAggregateQuery(const SelectStmt& stmt) {
    if (!stmt.groupBy.empty() || stmt.having) return true;
    for (const SelectItem& item : stmt.items) {
        if (containsAggregate(*item.expr)) return true;
    }
    for (const OrderItem& item : stmt.orderBy) {
        if (containsAggregate(*item.expr)) return true;
    }
    return false;
}

ExprPtr outputColumn(const std::string& text, size_t index) {
    ExprPtr ref = makeColumn(text);
    ref->columnIndex = index;
    return ref;
}

// Rewrites an expression evaluated after aggregation so it reads the
// aggregate operator's output row: [group keys..., aggregate results...].
// A subexpression that matches a GROUP BY key, or that is an aggregate, becomes
// a reference to that output column; anything else must be built from those.
class AggregateRewriter {
public:
    AggregateRewriter(const std::vector<ExprPtr>& groupBy, const Schema& schema) : schema_(schema) {
        for (const ExprPtr& key : groupBy) keyTexts_.push_back(exprToString(*key));
    }

    void rewrite(ExprPtr& expr) {
        std::string text = exprToString(*expr);
        for (size_t i = 0; i < keyTexts_.size(); ++i) {
            if (text == keyTexts_[i]) {
                expr = outputColumn(text, i);
                return;
            }
        }
        if (expr->kind == ExprKind::Aggregate) {
            expr = outputColumn(text, keyTexts_.size() + aggregateIndex(*expr, text));
            return;
        }
        if (expr->kind == ExprKind::Column) {
            throw SqlError("column " + expr->column + " must appear in GROUP BY or inside an aggregate");
        }
        if (expr->left) rewrite(expr->left);
        if (expr->right) rewrite(expr->right);
    }

    std::vector<AggregateSpec> takeAggregates() { return std::move(aggregates_); }

private:
    size_t aggregateIndex(Expr& aggregate, const std::string& text) {
        for (size_t i = 0; i < aggregates_.size(); ++i) {
            if (aggregates_[i].text == text) return i;  // SUM(x) used twice is computed once
        }
        if (aggregate.left) {
            if (containsAggregate(*aggregate.left)) throw SqlError("aggregates can't be nested: " + text);
            bindColumns(*aggregate.left, schema_);
        }
        aggregates_.push_back({aggregate.aggregate, std::move(aggregate.left), text});
        return aggregates_.size() - 1;
    }

    const Schema& schema_;
    std::vector<std::string> keyTexts_;
    std::vector<AggregateSpec> aggregates_;
};

// ORDER BY may name a SELECT alias, e.g. "SELECT city, COUNT(*) AS n ... ORDER BY n":
// the alias is replaced by a copy of the aliased expression. As in SQLite and
// PostgreSQL, an alias wins over a table column of the same name.
bool resolveOrderAliases(SelectStmt& stmt, std::vector<bool>& resolved) {
    bool any = false;
    resolved.assign(stmt.orderBy.size(), false);
    for (size_t i = 0; i < stmt.orderBy.size(); ++i) {
        OrderItem& order = stmt.orderBy[i];
        if (order.expr->kind != ExprKind::Column) continue;
        for (const SelectItem& item : stmt.items) {
            if (!item.alias.empty() && item.alias == order.expr->column) {
                order.expr = cloneExpr(*item.expr);
                resolved[i] = any = true;
                break;
            }
        }
    }
    return any;
}

// SELECT ... GROUP BY: row source -> Aggregate -> Filter (HAVING) -> Sort ->
// Limit -> Projection, with everything after the aggregate rewritten to read
// its output row.
OperatorPtr planAggregateSelect(SelectStmt& stmt, const Table& table) {
    if (stmt.items.empty()) throw SqlError("SELECT * can't be combined with GROUP BY or aggregates");
    for (ExprPtr& key : stmt.groupBy) bindColumns(*key, table.schema);

    AggregateRewriter rewriter(stmt.groupBy, table.schema);
    for (SelectItem& item : stmt.items) rewriter.rewrite(item.expr);
    if (stmt.having) rewriter.rewrite(stmt.having);
    std::vector<bool> aliased;
    resolveOrderAliases(stmt, aliased);  // copies are already rewritten
    for (size_t i = 0; i < stmt.orderBy.size(); ++i) {
        if (!aliased[i]) rewriter.rewrite(stmt.orderBy[i].expr);
    }

    std::vector<const Expr*> keys;
    for (const ExprPtr& key : stmt.groupBy) keys.push_back(key.get());
    OperatorPtr plan = planRowSource(table, stmt.where.get());
    plan = std::make_unique<AggregateOperator>(std::move(plan), std::move(keys), rewriter.takeAggregates());
    if (stmt.having) plan = std::make_unique<FilterOperator>(std::move(plan), *stmt.having);
    return plan;
}

}  // namespace

OperatorPtr planSelect(SelectStmt& stmt, const Table& table) {
    if (stmt.limit && *stmt.limit < 0) throw SqlError("LIMIT must not be negative");
    OperatorPtr plan;
    if (isAggregateQuery(stmt)) {
        plan = planAggregateSelect(stmt, table);
    } else {
        std::vector<bool> aliased;
        resolveOrderAliases(stmt, aliased);
        for (SelectItem& item : stmt.items) bindColumns(*item.expr, table.schema);
        for (OrderItem& item : stmt.orderBy) bindColumns(*item.expr, table.schema);
        plan = planRowSource(table, stmt.where.get());
    }
    if (!stmt.orderBy.empty()) {
        std::vector<SortKey> keys;
        for (const OrderItem& item : stmt.orderBy) keys.push_back({item.expr.get(), item.descending});
        plan = std::make_unique<SortOperator>(std::move(plan), std::move(keys));
    }
    if (stmt.limit) {
        plan = std::make_unique<LimitOperator>(std::move(plan), *stmt.limit, stmt.offset);
    }
    if (!stmt.items.empty()) {
        std::vector<const Expr*> exprs;
        for (const SelectItem& item : stmt.items) exprs.push_back(item.expr.get());
        plan = std::make_unique<ProjectionOperator>(std::move(plan), std::move(exprs));
    }
    return plan;
}

}  // namespace jerryql
