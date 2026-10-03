#include "expr_eval.h"

#include <limits>

#include "sql_error.h"

namespace jerryql {

void bindColumns(Expr& expr, const Schema& schema) {
    if (expr.kind == ExprKind::Aggregate) {
        throw SqlError("aggregate " + exprToString(expr) +
                       " is only allowed in SELECT, HAVING and ORDER BY");
    }
    if (expr.kind == ExprKind::Column) {
        auto index = schema.resolve(expr.table, expr.column);
        if (!index) throw SqlError("no such column: " + exprToString(expr));
        expr.columnIndex = index;
    }
    if (expr.left) bindColumns(*expr.left, schema);
    if (expr.right) bindColumns(*expr.right, schema);
}

bool isTrue(const Value& value) {
    if (!value.isInt()) throw SqlError("expected a condition but got TEXT '" + value.asText() + "'");
    return value.asInt() != 0;
}

namespace {

Value fromBool(bool b) {
    return Value::integer(b ? 1 : 0);
}

int64_t requireInt(const Value& value, const char* opName) {
    if (!value.isInt()) throw SqlError(std::string("operator ") + opName + " needs INT operands");
    return value.asInt();
}

Value arithmetic(BinaryOp op, const Value& lhs, const Value& rhs) {
    const std::string symbol = binaryOpSymbol(op);
    int64_t a = requireInt(lhs, symbol.c_str());
    int64_t b = requireInt(rhs, symbol.c_str());
    int64_t result = 0;
    bool overflow = false;
    switch (op) {
        case BinaryOp::Add: overflow = __builtin_add_overflow(a, b, &result); break;
        case BinaryOp::Subtract: overflow = __builtin_sub_overflow(a, b, &result); break;
        case BinaryOp::Multiply: overflow = __builtin_mul_overflow(a, b, &result); break;
        case BinaryOp::Divide:
            if (b == 0) throw SqlError("division by zero");
            overflow = (a == std::numeric_limits<int64_t>::min() && b == -1);
            if (!overflow) result = a / b;
            break;
        default: break;
    }
    if (overflow) throw SqlError("integer overflow in " + lhs.toString() + " " + symbol + " " +
                                 rhs.toString());
    return Value::integer(result);
}

bool comparison(BinaryOp op, const Value& lhs, const Value& rhs) {
    int c = compareValues(lhs, rhs);
    switch (op) {
        case BinaryOp::Eq: return c == 0;
        case BinaryOp::Ne: return c != 0;
        case BinaryOp::Lt: return c < 0;
        case BinaryOp::Le: return c <= 0;
        case BinaryOp::Gt: return c > 0;
        case BinaryOp::Ge: return c >= 0;
        default: return false;
    }
}

// Columns and literals are returned by reference instead of copied (a copy
// of a TEXT value allocates); anything else is computed into `scratch`.
const Value& evaluateRef(const Expr& expr, const Row& row, Value& scratch) {
    if (expr.kind == ExprKind::Literal) return expr.value;
    if (expr.kind == ExprKind::Column && expr.columnIndex) return row[*expr.columnIndex];
    scratch = evaluate(expr, row);
    return scratch;
}

Value evaluateUnary(const Expr& expr, const Row& row) {
    Value operand = evaluate(*expr.left, row);
    if (expr.unaryOp == UnaryOp::Not) return fromBool(!isTrue(operand));
    return arithmetic(BinaryOp::Subtract, Value::integer(0), operand);
}

Value evaluateBinary(const Expr& expr, const Row& row) {
    // AND / OR short-circuit, so the right side may never be evaluated.
    if (expr.binaryOp == BinaryOp::And) {
        return fromBool(isTrue(evaluate(*expr.left, row)) && isTrue(evaluate(*expr.right, row)));
    }
    if (expr.binaryOp == BinaryOp::Or) {
        return fromBool(isTrue(evaluate(*expr.left, row)) || isTrue(evaluate(*expr.right, row)));
    }
    Value leftScratch, rightScratch;
    const Value& lhs = evaluateRef(*expr.left, row, leftScratch);
    const Value& rhs = evaluateRef(*expr.right, row, rightScratch);
    switch (expr.binaryOp) {
        case BinaryOp::Add:
        case BinaryOp::Subtract:
        case BinaryOp::Multiply:
        case BinaryOp::Divide:
            return arithmetic(expr.binaryOp, lhs, rhs);
        default:
            return fromBool(comparison(expr.binaryOp, lhs, rhs));
    }
}

}  // namespace

Value evaluate(const Expr& expr, const Row& row) {
    switch (expr.kind) {
        case ExprKind::Literal:
            return expr.value;
        case ExprKind::Column:
            if (!expr.columnIndex) {
                throw SqlError("column " + expr.column + " cannot be used here");
            }
            return row[*expr.columnIndex];
        case ExprKind::Unary:
            return evaluateUnary(expr, row);
        case ExprKind::Binary:
            return evaluateBinary(expr, row);
        case ExprKind::Aggregate:
            break;  // the planner replaces aggregates before evaluation
    }
    throw SqlError("unknown expression");
}

}  // namespace jerryql
