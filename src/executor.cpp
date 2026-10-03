#include "executor.h"

#include <algorithm>

#include "expr_eval.h"

namespace jerryql {

// ---------- Scan ----------

ScanOperator::ScanOperator(const TableStore& store, KeyRange range, std::string description)
    : cursor_(store.scan(range)), description_(std::move(description)) {}

bool ScanOperator::next(Tuple& out) {
    return cursor_->next(out.key, out.row);
}

// ---------- Filter ----------

FilterOperator::FilterOperator(OperatorPtr child, const Expr& predicate)
    : child_(std::move(child)), predicate_(predicate) {}

bool FilterOperator::next(Tuple& out) {
    while (child_->next(out)) {
        if (isTrue(evaluate(predicate_, out.row))) return true;
    }
    return false;
}

std::string FilterOperator::describe() const {
    return "FILTER " + exprToString(predicate_);
}

// ---------- Sort ----------

SortOperator::SortOperator(OperatorPtr child, std::vector<SortKey> keys)
    : child_(std::move(child)), keys_(std::move(keys)) {}

void SortOperator::materialize() {
    // Evaluate each tuple's sort keys once, then sort indices by them.
    std::vector<Tuple> input;
    std::vector<std::vector<Value>> keyValues;
    Tuple tuple;
    while (child_->next(tuple)) {
        std::vector<Value> values;
        for (const SortKey& key : keys_) values.push_back(evaluate(*key.expr, tuple.row));
        keyValues.push_back(std::move(values));
        input.push_back(std::move(tuple));
    }

    std::vector<size_t> order(input.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    auto lessThan = [&](size_t a, size_t b) {
        for (size_t k = 0; k < keys_.size(); ++k) {
            int c = compareValues(keyValues[a][k], keyValues[b][k]);
            if (c != 0) return keys_[k].descending ? c > 0 : c < 0;
        }
        return false;
    };
    std::stable_sort(order.begin(), order.end(), lessThan);

    sorted_.reserve(input.size());
    for (size_t i : order) sorted_.push_back(std::move(input[i]));
    materialized_ = true;
}

bool SortOperator::next(Tuple& out) {
    if (!materialized_) materialize();
    if (position_ >= sorted_.size()) return false;
    out = sorted_[position_++];
    return true;
}

std::string SortOperator::describe() const {
    std::string text = "SORT BY ";
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (i > 0) text += ", ";
        text += exprToString(*keys_[i].expr) + (keys_[i].descending ? " DESC" : "");
    }
    return text;
}

// ---------- Limit ----------

LimitOperator::LimitOperator(OperatorPtr child, int64_t limit)
    : child_(std::move(child)), limit_(limit) {}

bool LimitOperator::next(Tuple& out) {
    if (produced_ >= limit_) return false;
    if (!child_->next(out)) return false;
    ++produced_;
    return true;
}

std::string LimitOperator::describe() const {
    return "LIMIT " + std::to_string(limit_);
}

// ---------- Projection ----------

ProjectionOperator::ProjectionOperator(OperatorPtr child, std::vector<const Expr*> exprs)
    : child_(std::move(child)), exprs_(std::move(exprs)) {}

bool ProjectionOperator::next(Tuple& out) {
    Tuple input;
    if (!child_->next(input)) return false;
    out.key = input.key;
    out.row.clear();
    for (const Expr* expr : exprs_) out.row.push_back(evaluate(*expr, input.row));
    return true;
}

std::string ProjectionOperator::describe() const {
    std::string text = "PROJECT ";
    for (size_t i = 0; i < exprs_.size(); ++i) {
        if (i > 0) text += ", ";
        text += exprToString(*exprs_[i]);
    }
    return text;
}

// ---------- EXPLAIN ----------

std::vector<std::string> explainPlan(const Operator& root) {
    std::vector<std::string> lines;
    std::string indent;
    for (const Operator* op = &root; op != nullptr; op = op->child()) {
        lines.push_back(indent + (indent.empty() ? "" : "-> ") + op->describe());
        indent += "  ";
    }
    return lines;
}

}  // namespace jerryql
