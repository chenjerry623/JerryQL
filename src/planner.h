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

// Scan (narrowed by primaryKeyRange) plus Filter for the WHERE clause.
// Binds column names in `where`. Used by SELECT, UPDATE and DELETE.
OperatorPtr planRowSource(const Table& table, Expr* where);

// Full SELECT plan: row source -> Sort -> Limit -> Projection.
// Binds column names in every expression of the statement.
OperatorPtr planSelect(SelectStmt& stmt, const Table& table);

}  // namespace jerryql
