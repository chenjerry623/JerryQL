// JerryQL command-line shell.
//
//   jerryql                    interactive prompt (or reads SQL piped on stdin)
//   jerryql script.sql         run a script and exit
//   jerryql --echo script.sql  also print each statement before its result

#include <unistd.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "database.h"
#include "shell.h"

namespace {

bool endsStatement(const std::string& buffer) {
    size_t last = buffer.find_last_not_of(" \t\r\n");
    return last != std::string::npos && buffer[last] == ';';
}

bool isBlank(const std::string& text) {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

int runFile(jerryql::Database& db, const std::string& path, const jerryql::ScriptOptions& options) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << "cannot open " << path << "\n";
        return 2;
    }
    std::stringstream contents;
    contents << file.rdbuf();
    return jerryql::runScript(db, contents.str(), std::cout, options) == 0 ? 0 : 1;
}

// Reads lines until a statement ends with ';', then runs the buffer.
int runInteractive(jerryql::Database& db, const jerryql::ScriptOptions& options) {
    const bool interactive = isatty(STDIN_FILENO);
    if (interactive) {
        std::cout << "JerryQL - a small SQL engine in C++. Type .help for commands, .quit to exit.\n";
    }
    std::string buffer, line;
    while (true) {
        if (interactive) std::cout << (buffer.empty() ? "jerryql> " : "    ...> ") << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (buffer.empty() && (line == ".quit" || line == ".exit")) break;
        if (buffer.empty() && jerryql::runDotCommand(db, line, std::cout)) continue;
        buffer += line + "\n";
        if (endsStatement(buffer)) {
            jerryql::runScript(db, buffer, std::cout, options);
            buffer.clear();
        }
    }
    if (!isBlank(buffer)) jerryql::runScript(db, buffer, std::cout, options);
    if (interactive) std::cout << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    jerryql::ScriptOptions options;
    std::string scriptPath;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--echo") {
            options.echo = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "usage: jerryql [--echo] [script.sql]\n";
            return 0;
        } else {
            scriptPath = arg;
        }
    }

    jerryql::Database db;
    if (!scriptPath.empty()) return runFile(db, scriptPath, options);
    return runInteractive(db, options);
}
