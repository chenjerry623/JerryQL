#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast.h"
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

// Stops after a fixed number of tuples.
class LimitOperator : public Operator {
public:
    LimitOperator(OperatorPtr child, int64_t limit);
    bool next(Tuple& out) override;
    std::string describe() const override;
    const Operator* child() const override { return child_.get(); }

private:
    OperatorPtr child_;
    int64_t limit_;
    int64_t produced_ = 0;
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
