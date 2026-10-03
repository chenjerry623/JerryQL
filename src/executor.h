#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast.h"
#include "storage/btree.h"
#include "table_store.h"

namespace jerryql {

// A row flowing through the plan, with the storage key it came from.
struct Tuple {
    int64_t key = 0;
    Row row;
};

// Pull-based ("Volcano") operator: each next() call produces one tuple.
// Operators form a chain, e.g. Projection <- Limit <- Sort <- Filter <- Scan.
class Operator {
public:
    virtual ~Operator() = default;
    virtual bool next(Tuple& out) = 0;
    virtual std::string describe() const = 0;  // one line for EXPLAIN
    virtual const Operator* child() const { return nullptr; }
};

using OperatorPtr = std::unique_ptr<Operator>;

// Reads rows from a table's store, restricted to a key range.
class ScanOperator : public Operator {
public:
    ScanOperator(const TableStore& store, KeyRange range, std::string description);
    bool next(Tuple& out) override;
    std::string describe() const override { return description_; }

private:
    std::unique_ptr<Cursor> cursor_;
    std::string description_;
};

// Walks a secondary index's entries in [lo, hi] and fetches each matching
// row from the table by its primary key.
class IndexScanOperator : public Operator {
public:
    IndexScanOperator(const TableStore& store, BTree& index, std::string lo,
                      std::optional<std::string> hi, std::string description);
    bool next(Tuple& out) override;
    std::string describe() const override { return description_; }

private:
    const TableStore& store_;
    BTreeCursor cursor_;
    std::string description_;
};

// Passes through tuples for which the predicate is true.
class FilterOperator : public Operator {
public:
    FilterOperator(OperatorPtr child, const Expr& predicate);
    bool next(Tuple& out) override;
    std::string describe() const override;
    const Operator* child() const override { return child_.get(); }

private:
    OperatorPtr child_;
    const Expr& predicate_;
};

struct SortKey {
    const Expr* expr;
    bool descending;
};

// Reads all input, then emits it ordered by the sort keys (stable).
class SortOperator : public Operator {
public:
    SortOperator(OperatorPtr child, std::vector<SortKey> keys);
    bool next(Tuple& out) override;
    std::string describe() const override;
    const Operator* child() const override { return child_.get(); }

private:
    void materialize();

    OperatorPtr child_;
    std::vector<SortKey> keys_;
    std::vector<Tuple> sorted_;
    size_t position_ = 0;
    bool materialized_ = false;
};

// Skips `offset` tuples, then stops after `limit` more.
class LimitOperator : public Operator {
public:
    LimitOperator(OperatorPtr child, int64_t limit, int64_t offset = 0);
    bool next(Tuple& out) override;
    std::string describe() const override;
    const Operator* child() const override { return child_.get(); }

private:
    OperatorPtr child_;
    int64_t limit_;
    int64_t offset_;
    int64_t produced_ = 0;
};

struct AggregateSpec {
    AggregateFunc func;
    ExprPtr argument;  // null for COUNT(*); bound to the input schema
    std::string text;  // e.g. "SUM(amount)", for EXPLAIN and headers
};

// GROUP BY: reads all input, groups it by the key expressions, and emits one
// row per group: the key values followed by each aggregate's result. Groups
// come out in key order (an ordered map, not a hash table). With no keys
// there is exactly one group, even for empty input, so COUNT(*) gives 0.
class AggregateOperator : public Operator {
public:
    AggregateOperator(OperatorPtr child, std::vector<const Expr*> keys,
                      std::vector<AggregateSpec> aggregates);
    bool next(Tuple& out) override;
    std::string describe() const override;
    const Operator* child() const override { return child_.get(); }

private:
    struct State {
        int64_t count = 0;
        int64_t sum = 0;
        bool hasValue = false;
        Value best;  // MIN / MAX so far
    };
    struct KeyLess {
        bool operator()(const std::vector<Value>& a, const std::vector<Value>& b) const;
    };

    void materialize();
    void accumulate(State& state, const AggregateSpec& spec, const Row& row) const;
    Value finish(const State& state, const AggregateSpec& spec) const;

    OperatorPtr child_;
    std::vector<const Expr*> keys_;
    std::vector<AggregateSpec> aggregates_;
    std::vector<Row> results_;
    size_t position_ = 0;
    bool materialized_ = false;
};

// Computes the SELECT list for each tuple.
class ProjectionOperator : public Operator {
public:
    ProjectionOperator(OperatorPtr child, std::vector<const Expr*> exprs);
    bool next(Tuple& out) override;
    std::string describe() const override;
    const Operator* child() const override { return child_.get(); }

private:
    OperatorPtr child_;
    std::vector<const Expr*> exprs_;
};

// The plan as indented lines, root first.
std::vector<std::string> explainPlan(const Operator& root);

}  // namespace jerryql
