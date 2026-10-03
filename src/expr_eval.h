#pragma once

#include "ast.h"
#include "catalog.h"

namespace jerryql {

// Resolves every column name in the expression to its index in the schema,
// so evaluation doesn't look names up per row. Throws on unknown columns.
void bindColumns(Expr& expr, const Schema& schema);

// Evaluates an expression against one row. Comparisons, AND, OR and NOT
// return INT 1 or 0. Throws SqlError on type errors, integer overflow,
// division by zero, or an unbound column reference (e.g. in VALUES).
Value evaluate(const Expr& expr, const Row& row);

// A WHERE result is true if it is a non-zero INT. TEXT is an error.
bool isTrue(const Value& value);

}  // namespace jerryql
