#include "ast.h"

namespace jerryql {

ExprPtr makeLiteral(Value value) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Literal;
    e->value = std::move(value);
    return e;
}

ExprPtr makeColumn(std::string name) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Column;
    e->column = std::move(name);
    return e;
}

ExprPtr makeUnary(UnaryOp op, ExprPtr operand) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Unary;
    e->unaryOp = op;
    e->left = std::move(operand);
    return e;
}

ExprPtr makeBinary(BinaryOp op, ExprPtr left, ExprPtr right) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Binary;
    e->binaryOp = op;
    e->left = std::move(left);
    e->right = std::move(right);
    return e;
}

ExprPtr makeAggregate(AggregateFunc func, ExprPtr argument) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Aggregate;
    e->aggregate = func;
    e->left = std::move(argument);
    return e;
}

ExprPtr cloneExpr(const Expr& expr) {
    auto copy = std::make_unique<Expr>();
    copy->kind = expr.kind;
    copy->value = expr.value;
    copy->column = expr.column;
    copy->columnIndex = expr.columnIndex;
    copy->unaryOp = expr.unaryOp;
    copy->binaryOp = expr.binaryOp;
    copy->aggregate = expr.aggregate;
    if (expr.left) copy->left = cloneExpr(*expr.left);
    if (expr.right) copy->right = cloneExpr(*expr.right);
    return copy;
}

std::string aggregateName(AggregateFunc func) {
    switch (func) {
        case AggregateFunc::Count: return "COUNT";
        case AggregateFunc::Sum: return "SUM";
        case AggregateFunc::Min: return "MIN";
        case AggregateFunc::Max: return "MAX";
        case AggregateFunc::Avg: return "AVG";
    }
    return "?";
}

bool containsAggregate(const Expr& expr) {
    if (expr.kind == ExprKind::Aggregate) return true;
    return (expr.left && containsAggregate(*expr.left)) || (expr.right && containsAggregate(*expr.right));
}

std::string binaryOpSymbol(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add: return "+";
        case BinaryOp::Subtract: return "-";
        case BinaryOp::Multiply: return "*";
        case BinaryOp::Divide: return "/";
        case BinaryOp::Eq: return "=";
        case BinaryOp::Ne: return "<>";
        case BinaryOp::Lt: return "<";
        case BinaryOp::Le: return "<=";
        case BinaryOp::Gt: return ">";
        case BinaryOp::Ge: return ">=";
        case BinaryOp::And: return "AND";
        case BinaryOp::Or: return "OR";
    }
    return "?";
}

namespace {

// Nested binary and NOT expressions get parentheses so the printed form is unambiguous.
std::string operandToString(const Expr& expr) {
    std::string s = exprToString(expr);
    bool needsParens = expr.kind == ExprKind::Binary ||
                       (expr.kind == ExprKind::Unary && expr.unaryOp == UnaryOp::Not);
    return needsParens ? "(" + s + ")" : s;
}

}  // namespace

std::string exprToString(const Expr& expr) {
    switch (expr.kind) {
        case ExprKind::Literal:
            return expr.value.toSqlLiteral();
        case ExprKind::Column:
            return expr.column;
        case ExprKind::Unary:
            return (expr.unaryOp == UnaryOp::Not ? "NOT " : "-") + operandToString(*expr.left);
        case ExprKind::Binary:
            return operandToString(*expr.left) + " " + binaryOpSymbol(expr.binaryOp) + " " +
                   operandToString(*expr.right);
        case ExprKind::Aggregate:
            return aggregateName(expr.aggregate) + "(" + (expr.left ? exprToString(*expr.left) : "*") + ")";
    }
    return "?";
}

}  // namespace jerryql
