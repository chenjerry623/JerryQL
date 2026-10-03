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

OperatorPtr planSelect(SelectStmt& stmt, const Table& table) {
    for (SelectItem& item : stmt.items) bindColumns(*item.expr, table.schema);
    for (OrderItem& item : stmt.orderBy) bindColumns(*item.expr, table.schema);
    if (stmt.limit && *stmt.limit < 0) throw SqlError("LIMIT must not be negative");

    OperatorPtr plan = planRowSource(table, stmt.where.get());
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
