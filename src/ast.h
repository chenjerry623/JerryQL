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

enum class ExprKind { Literal, Column, Unary, Binary };
enum class UnaryOp { Negate, Not };
enum class BinaryOp { Add, Subtract, Multiply, Divide, Eq, Ne, Lt, Le, Gt, Ge, And, Or };

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    ExprKind kind = ExprKind::Literal;
    Value value;                         // Literal
    std::string column;                  // Column: name as written (lowercased)
    std::optional<size_t> columnIndex;   // Column: set by bindColumns()
    UnaryOp unaryOp = UnaryOp::Negate;   // Unary
    BinaryOp binaryOp = BinaryOp::Add;   // Binary
    ExprPtr left;                        // Unary operand, or Binary left side
    ExprPtr right;                       // Binary right side
};

ExprPtr makeLiteral(Value value);
ExprPtr makeColumn(std::string name);
ExprPtr makeUnary(UnaryOp op, ExprPtr operand);
ExprPtr makeBinary(BinaryOp op, ExprPtr left, ExprPtr right);

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
                               UpdateStmt, DeleteStmt, TransactionStmt>;

}  // namespace jerryql
