#pragma once

#include "ast.h"
#include "catalog.h"
#include "executor.h"

namespace jerryql {

// The primary-key range implied by a WHERE clause. Only top-level AND-ed
// comparisons between the primary key and an INT literal narrow the range
// (e.g. "id >= 10 AND id < 20 AND name = 'x'" -> [10, 19]); anything else,
// such as OR, leaves it as the full range. The full WHERE clause is still
// applied by a Filter, so this only decides how much of the table is read.
KeyRange primaryKeyRange(const Expr* where, const Schema& schema);

// Rows of one table matching `where`: a primary-key lookup or range, an
// index scan, or a full scan, plus a Filter. Column references may be
// qualified by the table's name. Binds `where`. Used by UPDATE and DELETE.
OperatorPtr planRowSource(const Table& table, Expr* where);

struct PlannedSelect {
    OperatorPtr plan;
    std::vector<std::string> columns;  // result column names
};

// Full SELECT plan: FROM/JOIN source (with WHERE) -> Aggregate -> HAVING ->
// Sort -> Limit -> Projection. `tables` are the FROM table followed by each
// JOIN's table, in order. Binds every expression of the statement.
PlannedSelect planSelect(SelectStmt& stmt, const std::vector<const Table*>& tables);

}  // namespace jerryql
