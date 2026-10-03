#include "executor.h"

#include <algorithm>
#include <map>
#include <stdexcept>

#include "expr_eval.h"
#include "storage/index_key.h"
#include "sql_error.h"

namespace jerryql {

// ---------- Scan ----------

ScanOperator::ScanOperator(const TableStore& store, KeyRange range, std::string description)
    : cursor_(store.scan(range)), description_(std::move(description)) {}

bool ScanOperator::next(Tuple& out) {
    return cursor_->next(out.key, out.row);
}

// ---------- Index scan ----------

IndexScanOperator::IndexScanOperator(const TableStore& store, BTree& index, std::string lo,
                                     std::optional<std::string> hi, std::string description)
    : store_(store), cursor_(index.scan(lo, std::move(hi))), description_(std::move(description)) {}

bool IndexScanOperator::next(Tuple& out) {
    std::string_view entry, payload;
    if (!cursor_.nextRaw(entry, payload)) return false;
    out.key = primaryKeyOfEntry(entry);
    std::optional<Row> row = store_.get(out.key);
    if (!row) throw std::logic_error("index entry points to a missing row");
    out.row = std::move(*row);
    return true;
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

LimitOperator::LimitOperator(OperatorPtr child, int64_t limit, int64_t offset)
    : child_(std::move(child)), limit_(limit), offset_(offset) {}

bool LimitOperator::next(Tuple& out) {
    for (; offset_ > 0; --offset_) {
        if (!child_->next(out)) return false;
    }
    if (produced_ >= limit_) return false;
    if (!child_->next(out)) return false;
    ++produced_;
    return true;
}

std::string LimitOperator::describe() const {
    return "LIMIT " + std::to_string(limit_) + (offset_ > 0 ? " OFFSET " + std::to_string(offset_) : "");
}

// ---------- Aggregate ----------

AggregateOperator::AggregateOperator(OperatorPtr child, std::vector<const Expr*> keys,
                                     std::vector<AggregateSpec> aggregates)
    : child_(std::move(child)), keys_(std::move(keys)), aggregates_(std::move(aggregates)) {}

bool AggregateOperator::KeyLess::operator()(const std::vector<Value>& a,
                                            const std::vector<Value>& b) const {
    for (size_t i = 0; i < a.size(); ++i) {
        int c = compareValues(a[i], b[i]);
        if (c != 0) return c < 0;
    }
    return false;
}

void AggregateOperator::accumulate(State& state, const AggregateSpec& spec, const Row& row) const {
    ++state.count;
    if (spec.func == AggregateFunc::Count) return;
    Value value = evaluate(*spec.argument, row);
    if (spec.func == AggregateFunc::Sum || spec.func == AggregateFunc::Avg) {
        if (!value.isInt()) throw SqlError(spec.text + " needs INT values");
        if (__builtin_add_overflow(state.sum, value.asInt(), &state.sum)) {
            throw SqlError("integer overflow in " + spec.text);
        }
        return;
    }
    if (!state.hasValue) {
        state.best = std::move(value);
        state.hasValue = true;
        return;
    }
    int c = compareValues(value, state.best);
    if ((spec.func == AggregateFunc::Min && c < 0) || (spec.func == AggregateFunc::Max && c > 0)) {
        state.best = std::move(value);
    }
}

// Without NULL there is no value for MIN/MAX/AVG of zero rows; that's an error.
Value AggregateOperator::finish(const State& state, const AggregateSpec& spec) const {
    switch (spec.func) {
        case AggregateFunc::Count: return Value::integer(state.count);
        case AggregateFunc::Sum: return Value::integer(state.sum);
        case AggregateFunc::Avg:
            if (state.count == 0) throw SqlError(spec.text + " of no rows (JerryQL has no NULL yet)");
            return Value::integer(state.sum / state.count);
        case AggregateFunc::Min:
        case AggregateFunc::Max:
            if (!state.hasValue) throw SqlError(spec.text + " of no rows (JerryQL has no NULL yet)");
            return state.best;
    }
    return Value();
}

void AggregateOperator::materialize() {
    std::map<std::vector<Value>, std::vector<State>, KeyLess> groups;
    if (keys_.empty()) groups[{}] = std::vector<State>(aggregates_.size());
    Tuple tuple;
    std::vector<Value> key;
    while (child_->next(tuple)) {
        key.clear();
        for (const Expr* expr : keys_) key.push_back(evaluate(*expr, tuple.row));
        auto it = groups.find(key);
        if (it == groups.end()) it = groups.emplace(key, std::vector<State>(aggregates_.size())).first;
        for (size_t i = 0; i < aggregates_.size(); ++i) accumulate(it->second[i], aggregates_[i], tuple.row);
    }
    for (const auto& [groupKey, states] : groups) {
        Row row = groupKey;
        for (size_t i = 0; i < aggregates_.size(); ++i) row.push_back(finish(states[i], aggregates_[i]));
        results_.push_back(std::move(row));
    }
    materialized_ = true;
}

bool AggregateOperator::next(Tuple& out) {
    if (!materialized_) materialize();
    if (position_ >= results_.size()) return false;
    out.key = int64_t(position_);
    out.row = results_[position_++];
    return true;
}

std::string AggregateOperator::describe() const {
    std::string text = "AGGREGATE ";
    for (size_t i = 0; i < aggregates_.size(); ++i) text += (i ? ", " : "") + aggregates_[i].text;
    if (!keys_.empty()) {
        text += " GROUP BY ";
        for (size_t i = 0; i < keys_.size(); ++i) text += (i ? ", " : "") + exprToString(*keys_[i]);
    }
    return text;
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
