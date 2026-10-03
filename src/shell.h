#pragma once

#include <ostream>
#include <string>

#include "database.h"

namespace jerryql {

// Renders a result as a bordered text table followed by "(N rows)", or as
// its message (e.g. "INSERT 3") for statements that return no rows.
std::string formatResult(const QueryResult& result);

struct ScriptOptions {
    bool echo = false;  // print each statement before its result
};

// Runs every statement in `sql`, writing results and "Error: ..." lines to
// `out`. An error in one statement doesn't stop the following ones.
// Returns the number of statements that failed.
int runScript(Database& db, const std::string& sql, std::ostream& out,
              const ScriptOptions& options = {});

// Handles shell commands that start with '.', e.g. ".tables". Returns false
// if `line` isn't one.
bool runDotCommand(Database& db, const std::string& line, std::ostream& out);

}  // namespace jerryql
