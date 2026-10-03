// C API for the WebAssembly build. web/index.html calls these through
// Emscripten's ccall; the engine runs entirely in the browser.

#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

#include "database.h"
#include "shell.h"

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

}  // namespace

extern "C" {

// Runs SQL (or one ".command") and returns the printed output.
// The caller must release the result with jerryql_free.
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

void jerryql_free(char* text) {
    std::free(text);
}

// Drops every table.
void jerryql_reset() {
    g_db = std::make_unique<jerryql::Database>();
}

}  // extern "C"
