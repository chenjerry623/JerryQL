#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "value.h"

namespace jerryql {

// ---------- Expressions ----------

enum class ExprKind { Literal, Column, Unary, Binary, Aggregate };
enum class UnaryOp { Negate, Not };
enum class BinaryOp { Add, Subtract, Multiply, Divide, Eq, Ne, Lt, Le, Gt, Ge, And, Or };
enum class AggregateFunc { Count, Sum, Min, Max, Avg };

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    ExprKind kind = ExprKind::Literal;
    Value value;                         // Literal
    std::string column;                  // Column: name as written (lowercased)
    std::optional<size_t> columnIndex;   // Column: set by bindColumns()
    UnaryOp unaryOp = UnaryOp::Negate;   // Unary
    BinaryOp binaryOp = BinaryOp::Add;   // Binary
    AggregateFunc aggregate = AggregateFunc::Count;  // Aggregate
    ExprPtr left;                        // Unary operand, Binary left side, or
                                         // Aggregate argument (null for COUNT(*))
    ExprPtr right;                       // Binary right side
};

ExprPtr makeLiteral(Value value);
ExprPtr makeColumn(std::string name);
ExprPtr makeUnary(UnaryOp op, ExprPtr operand);
ExprPtr makeBinary(BinaryOp op, ExprPtr left, ExprPtr right);
ExprPtr makeAggregate(AggregateFunc func, ExprPtr argument);  // null argument = COUNT(*)

ExprPtr cloneExpr(const Expr& expr);  // deep copy, including bound column indexes

std::string aggregateName(AggregateFunc func);
bool containsAggregate(const Expr& expr);

std::string binaryOpSymbol(BinaryOp op);
// Readable SQL for an expression; used for result headers and EXPLAIN.
std::string exprToString(const Expr& expr);

// ---------- Statements ----------

struct ColumnDef {
    std::string name;
    Type type;
    bool primaryKey = false;
};

struct CreateTableStmt {
    std::string table;
    std::vector<ColumnDef> columns;
};

struct DropTableStmt {
    std::string table;
};

struct CreateIndexStmt {
    std::string index;
    std::string table;
    std::string column;
};

struct DropIndexStmt {
    std::string index;
};

struct InsertStmt {
    std::string table;
    std::vector<std::string> columns;         // empty = all columns in table order
    std::vector<std::vector<ExprPtr>> rows;   // one entry per VALUES tuple
};

struct SelectItem {
    ExprPtr expr;
    std::string alias;  // empty = use exprToString(expr)
};

struct OrderItem {
    ExprPtr expr;
    bool descending = false;
};

struct SelectStmt {
    bool explain = false;
    std::vector<SelectItem> items;  // empty = SELECT *
    std::string table;
    ExprPtr where;                  // may be null
    std::vector<ExprPtr> groupBy;
    ExprPtr having;                 // may be null
    std::vector<OrderItem> orderBy;
    std::optional<int64_t> limit;
    int64_t offset = 0;             // rows to skip before LIMIT applies
};

struct UpdateStmt {
    std::string table;
    std::vector<std::pair<std::string, ExprPtr>> assignments;
    ExprPtr where;  // may be null
};

struct DeleteStmt {
    std::string table;
    ExprPtr where;  // may be null
};

enum class TransactionAction { Begin, Commit, Rollback };

struct TransactionStmt {
    TransactionAction action;
};

using Statement = std::variant<CreateTableStmt, DropTableStmt, InsertStmt, SelectStmt,
                               UpdateStmt, DeleteStmt, TransactionStmt, CreateIndexStmt,
                               DropIndexStmt>;

}  // namespace jerryql
