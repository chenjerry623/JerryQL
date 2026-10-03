#include "shell.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "parser.h"
#include "sql_error.h"

namespace jerryql {

namespace {

std::string horizontalRule(const std::vector<size_t>& widths) {
    std::string rule = "+";
    for (size_t w : widths) rule += std::string(w + 2, '-') + "+";
    return rule + "\n";
}

std::string padded(const std::string& text, size_t width, bool alignRight) {
    std::string padding(width - text.size(), ' ');
    return alignRight ? padding + text : text + padding;
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

const char* const kHelpText =
    "SQL statements end with ';'. Supported:\n"
    "  CREATE TABLE t (id INT PRIMARY KEY, name TEXT, ...)\n"
    "  DROP TABLE t\n"
    "  INSERT INTO t [(col, ...)] VALUES (...), (...)\n"
    "  SELECT * | expr [AS name], ... FROM t [WHERE ...] [ORDER BY expr [DESC], ...] [LIMIT n [OFFSET m]]\n"
    "  EXPLAIN SELECT ...      show the query plan\n"
    "  UPDATE t SET col = expr, ... [WHERE ...]\n"
    "  DELETE FROM t [WHERE ...]\n"
    "  BEGIN / COMMIT / ROLLBACK  group statements into one transaction\n"
    "Expressions: + - * /  = <> < <= > >=  AND OR NOT  'text'  123\n"
    "Shell commands:\n"
    "  .tables          list tables\n"
    "  .schema [table]  show CREATE TABLE statements\n"
    "  .help            this message\n";

}  // namespace

std::string formatResult(const QueryResult& result) {
    if (!result.hasRows) return result.message + "\n";

    std::vector<size_t> widths;
    for (const std::string& name : result.columns) widths.push_back(name.size());
    std::vector<std::vector<std::string>> cells;
    for (const Row& row : result.rows) {
        std::vector<std::string> line;
        for (size_t i = 0; i < row.size(); ++i) {
            line.push_back(row[i].toString());
            widths[i] = std::max(widths[i], line.back().size());
        }
        cells.push_back(std::move(line));
    }

    std::string out = horizontalRule(widths) + "|";
    for (size_t i = 0; i < result.columns.size(); ++i) {
        out += " " + padded(result.columns[i], widths[i], false) + " |";
    }
    out += "\n" + horizontalRule(widths);
    for (size_t r = 0; r < cells.size(); ++r) {
        out += "|";
        for (size_t i = 0; i < cells[r].size(); ++i) {
            out += " " + padded(cells[r][i], widths[i], result.rows[r][i].isInt()) + " |";
        }
        out += "\n";
    }
    if (!cells.empty()) out += horizontalRule(widths);
    size_t n = result.rows.size();
    return out + "(" + std::to_string(n) + (n == 1 ? " row)\n" : " rows)\n");
}

int runScript(Database& db, const std::string& sql, std::ostream& out,
              const ScriptOptions& options) {
    int failures = 0;
    Parser parser(sql);
    while (!parser.atEnd()) {
        ParsedStatement parsed;
        try {
            parsed = parser.next();
        } catch (const SqlError& e) {
            // Only a syntax error leaves the parser mid-statement; skip the rest of it.
            std::string text = parser.skipToNextStatement();
            if (options.echo) out << "jerryql> " << text << ";\n";
            out << "Error: " << e.what() << "\n" << (options.echo ? "\n" : "");
            ++failures;
            continue;
        }
        if (options.echo) out << "jerryql> " << parsed.text << ";\n";
        try {
            out << formatResult(db.execute(parsed.statement));
        } catch (const SqlError& e) {
            out << "Error: " << e.what() << "\n";
            ++failures;
        }
        if (options.echo) out << "\n";
    }
    return failures;
}

bool runDotCommand(Database& db, const std::string& line, std::ostream& out) {
    std::istringstream words(trim(line));
    std::string command, argument;
    words >> command >> argument;
    if (command.empty() || command[0] != '.') return false;
    for (char& c : argument) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (command == ".help") {
        out << kHelpText;
    } else if (command == ".tables") {
        for (const std::string& name : db.tableNames()) out << name << "\n";
    } else if (command == ".schema") {
        try {
            std::vector<std::string> names =
                argument.empty() ? db.tableNames() : std::vector<std::string>{argument};
            for (const std::string& name : names) out << db.table(name).toCreateSql() << ";\n";
        } catch (const SqlError& e) {
            out << "Error: " << e.what() << "\n";
        }
    } else {
        out << "Error: unknown command " << command << " (try .help)\n";
    }
    return true;
}

}  // namespace jerryql
