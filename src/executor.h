#pragma once

#include <memory>
#include <string>
#include <unordered_map>
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
    // The build / inner side of a join, printed after child() in EXPLAIN.
    virtual const Operator* secondChild() const { return nullptr; }
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

// ---------- Joins ----------
// Each produces left row ++ right row for every pair that satisfies `on`
// (bound against the combined row; null = always true).

// For each left row, tries every right row. The right input is read once
// and kept in memory. Used when the join condition has no usable equality.
class NestedLoopJoinOperator : public Operator {
public:
    NestedLoopJoinOperator(OperatorPtr left, OperatorPtr right, const Expr* on, std::string description);
    bool next(Tuple& out) override;
    std::string describe() const override { return description_; }
    const Operator* child() const override { return left_.get(); }
    const Operator* secondChild() const override { return right_.get(); }

private:
    OperatorPtr left_, right_;
    const Expr* on_;
    std::string description_;
    std::vector<Row> rightRows_;
    bool built_ = false;
    Tuple current_;
    bool haveCurrent_ = false;
    size_t position_ = 0;
};

// Equality join: builds a hash table of the right input keyed by one of its
// columns, then probes it with an expression over each left row.
class HashJoinOperator : public Operator {
public:
    HashJoinOperator(OperatorPtr left, OperatorPtr right, const Expr& leftKey, size_t rightColumn,
                     const Expr* on, std::string description);
    bool next(Tuple& out) override;
    std::string describe() const override { return description_; }
    const Operator* child() const override { return left_.get(); }
    const Operator* secondChild() const override { return right_.get(); }

private:
    OperatorPtr left_, right_;
    const Expr& leftKey_;
    size_t rightColumn_;
    const Expr* on_;
    std::string description_;
    std::unordered_map<std::string, std::vector<Row>> buckets_;
    bool built_ = false;
    Tuple current_;
    const std::vector<Row>* matches_ = nullptr;
    size_t position_ = 0;
};

// Equality join where the right side's column is its primary key or has a
// secondary index: for each left row, looks the matching right rows up
// directly instead of reading the whole right table.
class IndexNestedLoopJoinOperator : public Operator {
public:
    // index == nullptr means "look up by primary key".
    IndexNestedLoopJoinOperator(OperatorPtr left, const TableStore& rightStore, BTree* index,
                                const Expr& leftKey, const Expr* on, std::string description);
    bool next(Tuple& out) override;
    std::string describe() const override { return description_; }
    const Operator* child() const override { return left_.get(); }

private:
    void lookUp(const Value& key);

    OperatorPtr left_;
    const TableStore& rightStore_;
    BTree* index_;
    const Expr& leftKey_;
    const Expr* on_;
    std::string description_;
    Tuple current_;
    std::vector<Row> matches_;
    size_t position_ = 0;
};

// The plan as indented lines, root first.
std::vector<std::string> explainPlan(const Operator& root);

}  // namespace jerryql
