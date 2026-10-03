// C API for the WebAssembly build. web/playground.html calls these through
// Emscripten's ccall; the engine runs entirely in the browser.
//
// Every function returning char* returns a malloc'd string the caller must
// release with jerryql_free.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

#include "database.h"
#include "parser.h"
#include "shell.h"
#include "sql_error.h"

namespace {

std::unique_ptr<jerryql::Database> g_db = std::make_unique<jerryql::Database>();

char* copyToHeap(const std::string& text) {
    char* out = static_cast<char*>(std::malloc(text.size() + 1));
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

bool isDotCommand(const std::string& input) {
    size_t first = input.find_first_not_of(" \t\r\n");
    return first != std::string::npos && input[first] == '.';
}

std::string jsonString(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out += char(c);
                }
        }
    }
    return out + "\"";
}

// Integers that JavaScript numbers can hold exactly go out as numbers; the
// rest as strings.
std::string jsonValue(const jerryql::Value& value) {
    if (!value.isInt()) return jsonString(value.asText());
    int64_t v = value.asInt();
    const int64_t safe = (int64_t(1) << 53) - 1;
    return (v >= -safe && v <= safe) ? std::to_string(v) : jsonString(std::to_string(v));
}

std::string resultJson(const jerryql::QueryResult& result) {
    std::string out = "{\"ok\":true,\"hasRows\":" + std::string(result.hasRows ? "true" : "false") +
                      ",\"message\":" + jsonString(result.message) + ",\"columns\":[";
    for (size_t i = 0; i < result.columns.size(); ++i) {
        out += (i ? "," : "") + jsonString(result.columns[i]);
    }
    out += "],\"rows\":[";
    for (size_t r = 0; r < result.rows.size(); ++r) {
        out += r ? ",[" : "[";
        for (size_t c = 0; c < result.rows[r].size(); ++c) {
            out += (c ? "," : "") + jsonValue(result.rows[r][c]);
        }
        out += "]";
    }
    return out + "]}";
}

std::string errorJson(const std::string& message) {
    return "{\"ok\":false,\"error\":" + jsonString(message) + "}";
}

// Deterministic sample data: no randomness, so every visitor sees the same rows.
const char* const kCustomers[] = {"Ada", "Grace", "Linus", "Barbara", "Ken", "Margaret", "Edsger",
                                  "Donald", "Frances", "Dennis", "Radia", "John", "Shafi", "Leslie",
                                  "Jim", "Michael", "Tim", "Sophie", "Guido", "Bjarne"};
const char* const kCities[] = {"Vancouver", "Toronto", "Seattle", "San Francisco", "New York",
                               "London", "Berlin", "Tokyo", "Sydney", "Montreal"};
const char* const kStatuses[] = {"pending", "paid", "shipped", "delivered", "refunded"};

uint64_t mix(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    return x ^ (x >> 33);
}

}  // namespace

extern "C" {

// Runs SQL (or one ".command") and returns the printed output, as the CLI would.
char* jerryql_run(const char* input) {
    std::string sql = input ? input : "";
    std::ostringstream out;
    if (isDotCommand(sql)) {
        jerryql::runDotCommand(*g_db, sql, out);
    } else {
        jerryql::ScriptOptions options;
        options.echo = true;
        jerryql::runScript(*g_db, sql, out, options);
    }
    return copyToHeap(out.str());
}

// Runs one statement and returns its result as JSON:
// {"ok":true,"hasRows":..,"message":..,"columns":[..],"rows":[[..],..]}
// or {"ok":false,"error":".."}.
char* jerryql_query_json(const char* sql) {
    try {
        return copyToHeap(resultJson(g_db->execute(std::string(sql ? sql : ""))));
    } catch (const std::exception& e) {
        return copyToHeap(errorJson(e.what()));
    }
}

// Tables and storage size as JSON:
// {"pages":N,"bytes":N,"tables":[{"name":..,"sql":..,"rows":N,"primaryKey":".."|null},..]}
char* jerryql_tables_json() {
    std::string out = "{\"pages\":" + std::to_string(g_db->pager().pageCount()) +
                      ",\"bytes\":" + std::to_string(uint64_t(g_db->pager().pageCount()) * 4096) +
                      ",\"tables\":[";
    bool first = true;
    for (const std::string& name : g_db->tableNames()) {
        jerryql::Table& table = g_db->table(name);
        out += (first ? "" : ",");
        first = false;
        out += "{\"name\":" + jsonString(name) + ",\"sql\":" + jsonString(table.toCreateSql()) +
               ",\"rows\":" + std::to_string(table.store->size()) + ",\"primaryKey\":" +
               (table.schema.primaryKey ? jsonString(table.schema.columns[*table.schema.primaryKey].name)
                                        : std::string("null")) +
               ",\"indexes\":[";
        for (size_t i = 0; i < table.indexes.size(); ++i) {
            out += (i ? "," : "") + jsonString(table.indexSql(table.indexes[i]));
        }
        out += "]}";
    }
    return copyToHeap(out + "]}");
}

// Inserts sample orders [first, first + count) through ordinary SQL INSERT
// statements of 1,000 rows each. With first == 0 the table is (re)created.
// Returns "" on success or an error message.
char* jerryql_load_orders(int first, int count) {
    try {
        if (first == 0) {
            for (const std::string& name : g_db->tableNames()) {
                if (name == "orders") g_db->execute(std::string("DROP TABLE orders"));
            }
            g_db->execute(std::string(
                "CREATE TABLE orders (id INT PRIMARY KEY, customer TEXT, city TEXT, amount INT, status TEXT)"));
        }
        for (int start = first; start < first + count; start += 1000) {
            int end = std::min(start + 1000, first + count);
            std::string sql = "INSERT INTO orders VALUES ";
            for (int id = start; id < end; ++id) {
                uint64_t h = mix(uint64_t(id) + 1);
                if (id != start) sql += ", ";
                sql += "(" + std::to_string(id) + ", '" + kCustomers[h % 20] + "', '" +
                       kCities[(h >> 8) % 10] + "', " + std::to_string((h >> 16) % 100000) + ", '" +
                       kStatuses[(h >> 40) % 5] + "')";
            }
            g_db->execute(sql);
        }
        return copyToHeap("");
    } catch (const std::exception& e) {
        return copyToHeap(e.what());
    }
}

void jerryql_free(char* text) {
    std::free(text);
}

// Drops every table.
void jerryql_reset() {
    g_db = std::make_unique<jerryql::Database>();
}

}  // extern "C"
