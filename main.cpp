#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <memory>

using namespace std;

/*
 JerryQL — lightweight SQL-like CLI database
 -------------------------------------------------
 Supported Commands:
   CREATE TABLE <name> <col1> <col2> ...
   INSERT <table> <val1> <val2> ...
   SELECT <cols|*> FROM <table> [WHERE <col>=<val>]
   UPDATE <table> SET <col>=<val> [WHERE <col>=<val>]
   DELETE FROM <table> [WHERE <col>=<val>]
   DROP TABLE <name>
   DESCRIBE <table>
   SHOW <table>
   MEMORY / STATS
   HELP
   EXIT
 -------------------------------------------------
*/

struct Table {
    vector<string> columns;
    vector<vector<string>> rows;
};

class Database {
public:
    // === BASIC TABLE OPS ===
    bool createTable(const string &name, const vector<string> &cols) {
        if (tables.count(name)) return false;
        auto table = make_unique<Table>();
        table->columns = cols;
        tables[name] = std::move(table); // move unique_ptr ownership into map
        return true;
    }

    bool insertRow(const string &name, const vector<string> &values) {
        auto it = tables.find(name);
        if (it == tables.end()) return false;
        Table &t = *it->second;
        if (values.size() != t.columns.size()) return false;
        t.rows.push_back(values);
        return true;
    }

    bool dropTable(const string &name) {
        auto it = tables.find(name);
        if (it == tables.end()) return false;
        tables.erase(it); // unique_ptr auto-frees
        return true;
    }

    void describeTable(const string &name) const {
        auto it = tables.find(name);
        if (it == tables.end()) { cout << "[ERR] Table not found.\n"; return; }
        const Table &t = *it->second;
        cout << "Table: " << name << "\nColumns: ";
        for (auto &c : t.columns) cout << c << " ";
        cout << "\nRows: " << t.rows.size() << "\n";
    }

    // === SHOW ===
    void showTable(const string &name) const {
        auto it = tables.find(name);
        if (it == tables.end()) { cout << "[ERR] Table not found.\n"; return; }
        const Table &t = *it->second;

        if (t.columns.empty()) { cout << "[ERR] Table has no columns.\n"; return; }

        vector<size_t> widths(t.columns.size());
        for (size_t i = 0; i < t.columns.size(); ++i) widths[i] = t.columns[i].size();
        for (auto &r : t.rows)
            for (size_t i = 0; i < r.size(); ++i)
                widths[i] = max(widths[i], r[i].size());

        string rule; for (auto w : widths) rule += string(w + 3, '-');
        cout << rule << "\n";
        for (size_t i = 0; i < t.columns.size(); ++i)
            cout << left << setw(widths[i]) << t.columns[i] << " | ";
        cout << "\n" << rule << "\n";

        for (auto &r : t.rows) {
            for (size_t i = 0; i < r.size(); ++i)
                cout << left << setw(widths[i]) << r[i] << " | ";
            cout << "\n";
        }
        cout << rule << "\n";
    }

    // === SELECT ===
    void selectFrom(const string &table, const vector<string> &selectCols,
                    const string &whereCol = "", const string &whereVal = "") const {
        auto it = tables.find(table);
        if (it == tables.end()) { cout << "[ERR] Table not found.\n"; return; }
        const Table &t = *it->second;

        // resolve selected columns
        vector<int> colIdx;
        if (selectCols.size() == 1 && selectCols[0] == "*") {
            for (int i = 0; i < (int)t.columns.size(); ++i) colIdx.push_back(i);
        } else {
            for (auto &sc : selectCols) {
                auto jt = find(t.columns.begin(), t.columns.end(), sc);
                if (jt == t.columns.end()) { cout << "[ERR] Unknown column: " << sc << "\n"; return; }
                colIdx.push_back((int)(jt - t.columns.begin()));
            }
        }

        // optional WHERE
        int whereIdx = -1;
        if (!whereCol.empty()) {
            auto wk = find(t.columns.begin(), t.columns.end(), whereCol);
            if (wk == t.columns.end()) { cout << "[ERR] Unknown WHERE column.\n"; return; }
            whereIdx = (int)(wk - t.columns.begin());
        }

        // widths
        vector<size_t> widths; widths.reserve(colIdx.size());
        for (int idx : colIdx) widths.push_back(t.columns[idx].size());
        for (auto &r : t.rows)
            for (size_t i = 0; i < colIdx.size(); ++i)
                widths[i] = max(widths[i], r[colIdx[i]].size());

        string rule; for (auto w : widths) rule += string(w + 3, '-');
        cout << rule << "\n";
        for (size_t i = 0; i < colIdx.size(); ++i)
            cout << left << setw(widths[i]) << t.columns[colIdx[i]] << " | ";
        cout << "\n" << rule << "\n";

        for (auto &r : t.rows) {
            if (whereIdx != -1 && r[whereIdx] != whereVal) continue;
            for (size_t i = 0; i < colIdx.size(); ++i)
                cout << left << setw(widths[i]) << r[colIdx[i]] << " | ";
            cout << "\n";
        }
        cout << rule << "\n";
    }

    // === UPDATE ===
    void update(const string &table, const string &setCol, const string &setVal,
                const string &whereCol = "", const string &whereVal = "") {
        auto it = tables.find(table);
        if (it == tables.end()) { cout << "[ERR] Table not found.\n"; return; }
        Table &t = *it->second;

        auto setIt = find(t.columns.begin(), t.columns.end(), setCol);
        if (setIt == t.columns.end()) { cout << "[ERR] Unknown column.\n"; return; }
        int setIdx = (int)(setIt - t.columns.begin());

        int whereIdx = -1;
        if (!whereCol.empty()) {
            auto wk = find(t.columns.begin(), t.columns.end(), whereCol);
            if (wk == t.columns.end()) { cout << "[ERR] Unknown WHERE column.\n"; return; }
            whereIdx = (int)(wk - t.columns.begin());
        }

        int count = 0;
        for (auto &r : t.rows) {
            if (whereIdx == -1 || r[whereIdx] == whereVal) {
                r[setIdx] = setVal;
                ++count;
            }
        }
        cout << "OK - " << count << " row(s) updated.\n";
    }

    // === DELETE ===
    void deleteFrom(const string &table, const string &whereCol = "", const string &whereVal = "") {
        auto it = tables.find(table);
        if (it == tables.end()) { cout << "[ERR] Table not found.\n"; return; }
        Table &t = *it->second;

        if (whereCol.empty()) {
            int count = (int)t.rows.size();
            t.rows.clear();
            cout << "OK - " << count << " row(s) deleted.\n";
            return;
        }

        auto wk = find(t.columns.begin(), t.columns.end(), whereCol);
        if (wk == t.columns.end()) { cout << "[ERR] Unknown column.\n"; return; }
        int wIdx = (int)(wk - t.columns.begin());

        int before = (int)t.rows.size();
        t.rows.erase(remove_if(t.rows.begin(), t.rows.end(),
                               [&](const vector<string> &r){ return r[wIdx] == whereVal; }),
                     t.rows.end());
        int deleted = before - (int)t.rows.size();
        cout << "OK - " << deleted << " row(s) deleted.\n";
    }

    // === PERSISTENCE ===
    bool save(const string &path) const {
        ofstream ofs(path, ios::trunc);
        if (!ofs) return false;
        for (auto &[name, ptr] : tables) {
            const Table &t = *ptr;
            ofs << "TABLE " << name;
            for (auto &c : t.columns) ofs << " " << c;
            ofs << "\n";
            for (auto &r : t.rows) {
                ofs << "ROW " << name;
                for (auto &v : r) ofs << " " << escape(v);
                ofs << "\n";
            }
        }
        return true;
    }

    bool load(const string &path) {
        ifstream ifs(path);
        if (!ifs) return false;
        string type;
        while (ifs >> type) {
            if (type == "TABLE") {
                string name; ifs >> name;
                vector<string> cols; string rest; getline(ifs, rest);
                stringstream ss(rest); string tmp; while (ss >> tmp) cols.push_back(tmp);
                createTable(name, cols);
            } else if (type == "ROW") {
                string name; ifs >> name;
                vector<string> vals; string rest; getline(ifs, rest);
                stringstream ss(rest); string v; while (ss >> v) vals.push_back(unescape(v));
                insertRow(name, vals);
            }
        }
        return true;
    }

    // === MEMORY / STATS ===
    size_t memoryUsageBytes() const {
        size_t total = 0;
        for (auto &[_, tblPtr] : tables) {
            const Table &t = *tblPtr;
            total += sizeof(Table);
            for (auto &col : t.columns) total += col.capacity();
            for (auto &row : t.rows)
                for (auto &val : row) total += val.capacity();
        }
        return total;
    }
    size_t tableCount() const { return tables.size(); }

    // === UTIL ===
    static vector<string> tokenize(const string &line) {
        vector<string> out; string cur; bool inQuote = false;
        for (char c : line) {
            if (c == '"') { inQuote = !inQuote; continue; }
            if (isspace(c) && !inQuote) {
                if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            } else cur.push_back(c);
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

private:
    unordered_map<string, unique_ptr<Table>> tables;

    static string escape(const string &s) {
        string out;
        for (char c : s) out += (c == ' ' ? "\\s" : string(1, c));
        return out;
    }
    static string unescape(const string &s) {
        string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 's') { out += ' '; ++i; }
            else out += s[i];
        }
        return out;
    }
};

// === MAIN PROGRAM ===
int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    cout.setf(std::ios::unitbuf);

    const string DB_PATH = "data.db";
    Database db;
    db.load(DB_PATH);

    cout << "=====================================\n";
    cout << " JerryQL - SQL-like CLI Database\n";
    cout << "=====================================\n";
    cout << "Loaded: " << DB_PATH << "\n\n";
    cout << "Type HELP for a list of commands.\n\n";

    string line;
    while (true) {
        cout << "> ";
        if (!getline(cin, line)) break;
        if (line.empty()) continue;

        auto tokens = Database::tokenize(line);
        if (tokens.empty()) continue;

        string cmd = tokens[0];
        for (auto &c : cmd) c = toupper(c);

        if (cmd == "EXIT") {
            db.save(DB_PATH);
            cout << "Bye.\n";
            break;
        }
        else if (cmd == "HELP") {
            cout << "Commands:\n"
                 << "  CREATE TABLE <name> <cols...>\n"
                 << "  INSERT <table> <vals...>\n"
                 << "  SELECT <cols|*> FROM <table> [WHERE <col>=<val>]\n"
                 << "  UPDATE <table> SET <col>=<val> [WHERE <col>=<val>]\n"
                 << "  DELETE FROM <table> [WHERE <col>=<val>]\n"
                 << "  DROP TABLE <name>\n"
                 << "  DESCRIBE <table>\n"
                 << "  SHOW <table>\n"
                 << "  MEMORY / STATS\n"
                 << "  EXIT\n";
        }
        else if (cmd == "CREATE") {
            if (tokens.size() < 4 || tokens[1] != "TABLE") { cout << "[ERR] Usage: CREATE TABLE <name> <cols...>\n"; continue; }
            string name = tokens[2];
            vector<string> cols(tokens.begin() + 3, tokens.end());
            cout << (db.createTable(name, cols) ? "OK.\n" : "[ERR] Table exists.\n");
        }
        else if (cmd == "INSERT") {
            if (tokens.size() < 3) { cout << "[ERR] Usage: INSERT <table> <vals...>\n"; continue; }
            string name = tokens[1];
            vector<string> vals(tokens.begin() + 2, tokens.end());
            cout << (db.insertRow(name, vals) ? "OK.\n" : "[ERR] Insert failed.\n");
        }
        else if (cmd == "SELECT") {
            auto fromIt = find(tokens.begin(), tokens.end(), "FROM");
            if (fromIt == tokens.end() || fromIt + 1 == tokens.end()) {
                cout << "[ERR] Usage: SELECT <cols|*> FROM <table> [WHERE <col>=<val>]\n";
                continue;
            }
            vector<string> cols(tokens.begin() + 1, fromIt);
            string table = *(fromIt + 1);

            string whereCol, whereVal;
            auto whereIt = find(tokens.begin(), tokens.end(), "WHERE");
            if (whereIt != tokens.end() && whereIt + 1 != tokens.end()) {
                string cond = *(whereIt + 1);
                size_t pos = cond.find('=');
                if (pos != string::npos) {
                    whereCol = cond.substr(0, pos);
                    whereVal = cond.substr(pos + 1);
                }
            }
            db.selectFrom(table, cols, whereCol, whereVal);
        }
        else if (cmd == "UPDATE") {
            if (tokens.size() < 4 || tokens[2] != "SET") {
                cout << "[ERR] Usage: UPDATE <table> SET <col>=<val> [WHERE <col>=<val>]\n";
                continue;
            }
            string table = tokens[1];
            string setExpr = tokens[3];
            size_t eq = setExpr.find('=');
            if (eq == string::npos) { cout << "[ERR] Missing '='.\n"; continue; }
            string setCol = setExpr.substr(0, eq);
            string setVal = setExpr.substr(eq + 1);

            string whereCol, whereVal;
            auto itW = find(tokens.begin(), tokens.end(), "WHERE");
            if (itW != tokens.end() && itW + 1 < tokens.end()) {
                string cond = *(itW + 1);
                size_t pos = cond.find('=');
                if (pos != string::npos) {
                    whereCol = cond.substr(0, pos);
                    whereVal = cond.substr(pos + 1);
                }
            }
            db.update(table, setCol, setVal, whereCol, whereVal);
        }
        else if (cmd == "DELETE") {
            if (tokens.size() < 3 || tokens[1] != "FROM") {
                cout << "[ERR] Usage: DELETE FROM <table> [WHERE <col>=<val>]\n";
                continue;
            }
            string table = tokens[2];

            string whereCol, whereVal;
            auto itW = find(tokens.begin(), tokens.end(), "WHERE");
            if (itW != tokens.end() && itW + 1 < tokens.end()) {
                string cond = *(itW + 1);
                size_t pos = cond.find('=');
                if (pos != string::npos) {
                    whereCol = cond.substr(0, pos);
                    whereVal = cond.substr(pos + 1);
                }
            }
            db.deleteFrom(table, whereCol, whereVal);
        }
        else if (cmd == "DROP") {
            if (tokens.size() != 3 || tokens[1] != "TABLE") { cout << "[ERR] Usage: DROP TABLE <name>\n"; continue; }
            cout << (db.dropTable(tokens[2]) ? "OK.\n" : "[ERR] Table not found.\n");
        }
        else if (cmd == "DESCRIBE") {
            if (tokens.size() != 2) { cout << "[ERR] Usage: DESCRIBE <table>\n"; continue; }
            db.describeTable(tokens[1]);
        }
        else if (cmd == "SHOW") {
            if (tokens.size() != 2) { cout << "[ERR] Usage: SHOW <table>\n"; continue; }
            db.showTable(tokens[1]);
        }
        else if (cmd == "MEMORY" || cmd == "STATS") {
            cout << "Tables: " << db.tableCount() << "\n";
            cout << "Memory: " << db.memoryUsageBytes() << " bytes\n";
        }
        else {
            cout << "[ERR] Unknown command. Type HELP.\n";
        }
    }
    return 0;
}
