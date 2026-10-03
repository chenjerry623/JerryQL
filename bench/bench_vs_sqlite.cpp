// JerryQL vs SQLite on the same machine, data and SQL.
//
//   ./build/jerryql_vs_sqlite --rows 1000000 --dir /tmp
//
// Both engines get the same SQL strings, the same 4 KiB pages, a 4 MiB page
// cache, write-ahead logging with a checkpoint every ~1000 pages, and a table
// clustered on its integer primary key (SQLite: INTEGER PRIMARY KEY makes the
// key the rowid). JerryQL has no prepared statements, so the main SQLite
// numbers also parse every statement; SQLite with prepared statements is
// reported separately as a reference point.
//
// Two durability settings, run separately:
//   durable: SQLite synchronous=FULL   vs JerryQL fsync on every commit (default)
//   relaxed: SQLite synchronous=NORMAL vs JerryQL syncOnCommit=false
// In both pairs, commits fsync the log (durable) or don't (relaxed), and
// checkpoints fsync the database file.

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "database.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    int64_t rows = 1000000;
    int lookups = 20000;
    int ranges = 2000;
    int fullScans = 10;
    int singleInserts = 500;
    int singleUpdates = 1000;
    std::string dir = "/tmp";
    std::string mode = "durable";  // or "relaxed"
    std::string csvPath = "latencies.csv";
    unsigned seed = 1;
};

double microsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

double percentile(std::vector<double> values, double p) {
    std::sort(values.begin(), values.end());
    size_t index = size_t(p * double(values.size() - 1) + 0.5);
    return values[std::min(index, values.size() - 1)];
}

uint64_t fileSize(const std::string& path) {
    struct stat info;
    return ::stat(path.c_str(), &info) == 0 ? uint64_t(info.st_size) : 0;
}

void fail(const std::string& message) {
    std::cerr << message << "\n";
    std::exit(1);
}

// ---------- Engines ----------

class Engine {
public:
    virtual ~Engine() = default;
    virtual std::string name() const = 0;
    virtual void open() = 0;
    virtual void close() = 0;
    virtual void exec(const std::string& sql) = 0;
    // Runs a query and materializes every row; returns the row count.
    virtual size_t query(const std::string& sql) = 0;
    virtual uint64_t bytesOnDisk() const = 0;
};

class JerryEngine : public Engine {
public:
    JerryEngine(std::string path, bool durable) : path_(std::move(path)), durable_(durable) {}
    std::string name() const override { return "JerryQL"; }
    void open() override {
        jerryql::DatabaseOptions options;
        options.poolPages = 1024;          // 4 MiB
        options.checkpointFrames = 1000;
        options.syncOnCommit = durable_;
        db_ = std::make_unique<jerryql::Database>(path_, options);
    }
    void close() override { db_.reset(); }
    void exec(const std::string& sql) override { db_->execute(sql); }
    size_t query(const std::string& sql) override { return db_->execute(sql).rows.size(); }
    uint64_t bytesOnDisk() const override { return fileSize(path_) + fileSize(path_ + "-wal"); }

private:
    std::string path_;
    bool durable_;
    std::unique_ptr<jerryql::Database> db_;
};

class SqliteEngine : public Engine {
public:
    SqliteEngine(std::string path, bool durable) : path_(std::move(path)), durable_(durable) {}
    ~SqliteEngine() override { close(); }
    std::string name() const override { return "SQLite"; }
    void open() override {
        if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) fail("sqlite3_open failed");
        exec("PRAGMA page_size = 4096");
        exec("PRAGMA journal_mode = WAL");
        exec(std::string("PRAGMA synchronous = ") + (durable_ ? "FULL" : "NORMAL"));
        exec("PRAGMA cache_size = -4096");  // KiB: 4 MiB, same as JerryQL's pool
        exec("PRAGMA wal_autocheckpoint = 1000");
    }
    void close() override {
        if (lookup_) sqlite3_finalize(lookup_);
        lookup_ = nullptr;
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
    }
    void exec(const std::string& sql) override {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            fail(std::string("sqlite: ") + (error ? error : "?") + " in " + sql.substr(0, 80));
        }
    }
    size_t query(const std::string& sql) override {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) fail("prepare failed");
        size_t rows = materialize(stmt);
        sqlite3_finalize(stmt);
        return rows;
    }
    // Reference only: the same point lookup with a prepared, reused statement.
    size_t preparedLookup(int64_t id) {
        if (!lookup_ && sqlite3_prepare_v2(db_, "SELECT * FROM t WHERE id = ?", -1, &lookup_, nullptr) != SQLITE_OK) {
            fail("prepare failed");
        }
        sqlite3_reset(lookup_);
        sqlite3_bind_int64(lookup_, 1, id);
        return materialize(lookup_);
    }
    uint64_t bytesOnDisk() const override { return fileSize(path_) + fileSize(path_ + "-wal"); }

private:
    // Copies every row into the same Row-of-Values type JerryQL returns, so
    // both engines pay identical result-building costs (integers stay
    // integers; text is copied once).
    static size_t materialize(sqlite3_stmt* stmt) {
        size_t rows = 0;
        jerryql::Row row;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int columns = sqlite3_column_count(stmt);
            row.clear();
            for (int c = 0; c < columns; ++c) {
                if (sqlite3_column_type(stmt, c) == SQLITE_INTEGER) {
                    row.push_back(jerryql::Value::integer(sqlite3_column_int64(stmt, c)));
                } else {
                    const unsigned char* text = sqlite3_column_text(stmt, c);
                    int length = sqlite3_column_bytes(stmt, c);
                    row.push_back(jerryql::Value::text(
                        std::string(reinterpret_cast<const char*>(text ? text : reinterpret_cast<const unsigned char*>("")), size_t(length))));
                }
            }
            ++rows;
        }
        return rows;
    }

    std::string path_;
    bool durable_;
    sqlite3* db_ = nullptr;
    sqlite3_stmt* lookup_ = nullptr;
};

// ---------- Workloads ----------

struct Series {
    std::string workload;
    std::string engine;
    std::vector<double> micros;
};

std::string insertBatchSql(int64_t first, int64_t count) {
    static const std::string payload(50, 'p');
    std::string sql = "INSERT INTO t VALUES ";
    for (int64_t id = first; id < first + count; ++id) {
        if (id != first) sql += ", ";
        sql += "(" + std::to_string(id) + ", " + std::to_string(id) + ", '" + payload + "')";
    }
    return sql;
}

void expectRows(size_t actual, size_t expected, const std::string& sql) {
    if (actual != expected) {
        fail("expected " + std::to_string(expected) + " rows, got " + std::to_string(actual) + ": " + sql);
    }
}

struct EngineResults {
    std::vector<Series> series;
    double loadSeconds = 0;
    uint64_t bytesAfterLoad = 0;
};

EngineResults runWorkloads(Engine& engine, const Options& options) {
    EngineResults results;
    std::mt19937_64 rng(options.seed);  // same query sequence for both engines
    auto add = [&](const std::string& workload) -> std::vector<double>& {
        results.series.push_back({workload, engine.name(), {}});
        return results.series.back().micros;
    };
    std::uniform_int_distribution<int64_t> anyRow(0, options.rows - 1);

    engine.open();
    engine.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, k INTEGER, payload TEXT)");
    {
        std::vector<double>& batches = add("Bulk load: 1,000-row transaction");
        auto loadStart = Clock::now();
        for (int64_t first = 0; first < options.rows; first += 1000) {
            std::string sql = insertBatchSql(first, std::min<int64_t>(1000, options.rows - first));
            auto start = Clock::now();
            engine.exec("BEGIN");
            engine.exec(sql);
            engine.exec("COMMIT");
            batches.push_back(microsSince(start));
        }
        results.loadSeconds = microsSince(loadStart) / 1e6;
    }
    engine.close();  // closing checkpoints both engines
    results.bytesAfterLoad = engine.bytesOnDisk();

    engine.open();
    {
        std::vector<double>& cold = add("Point lookup, cold cache");
        for (int i = 0; i < 1000; ++i) {
            std::string sql = "SELECT * FROM t WHERE id = " + std::to_string(anyRow(rng));
            auto start = Clock::now();
            size_t rows = engine.query(sql);
            cold.push_back(microsSince(start));
            expectRows(rows, 1, sql);
        }
    }
    for (int i = 0; i < 2000; ++i) engine.query("SELECT * FROM t WHERE id = " + std::to_string(anyRow(rng)));
    {
        std::vector<double>& warm = add("Point lookup, warm");
        for (int i = 0; i < options.lookups; ++i) {
            std::string sql = "SELECT * FROM t WHERE id = " + std::to_string(anyRow(rng));
            auto start = Clock::now();
            size_t rows = engine.query(sql);
            warm.push_back(microsSince(start));
            expectRows(rows, 1, sql);
        }
    }
    if (auto* sqlite = dynamic_cast<SqliteEngine*>(&engine)) {
        std::vector<double>& prepared = add("Point lookup, warm (SQLite prepared statement, reference)");
        for (int i = 0; i < options.lookups; ++i) {
            int64_t id = anyRow(rng);
            auto start = Clock::now();
            size_t rows = sqlite->preparedLookup(id);
            prepared.push_back(microsSince(start));
            expectRows(rows, 1, "prepared lookup");
        }
    }
    {
        std::uniform_int_distribution<int64_t> rangeStart(0, options.rows - 100);
        std::vector<double>& ranges = add("Range scan, 100 rows");
        for (int i = 0; i < options.ranges; ++i) {
            int64_t lo = rangeStart(rng);
            std::string sql = "SELECT * FROM t WHERE id >= " + std::to_string(lo) + " AND id < " +
                              std::to_string(lo + 100);
            auto start = Clock::now();
            size_t rows = engine.query(sql);
            ranges.push_back(microsSince(start));
            expectRows(rows, 100, sql);
        }
    }
    {
        std::vector<double>& scans = add("Full scan, unindexed column");
        for (int i = 0; i < options.fullScans; ++i) {
            std::string sql = "SELECT * FROM t WHERE k = " + std::to_string(anyRow(rng));
            auto start = Clock::now();
            size_t rows = engine.query(sql);
            scans.push_back(microsSince(start));
            expectRows(rows, 1, sql);
        }
    }
    {
        std::vector<double>& inserts = add("Single-row INSERT, own transaction");
        for (int i = 0; i < options.singleInserts; ++i) {
            int64_t id = options.rows + i;
            std::string sql = "INSERT INTO t VALUES (" + std::to_string(id) + ", " + std::to_string(id) +
                              ", '" + std::string(50, 'q') + "')";
            auto start = Clock::now();
            engine.exec(sql);
            inserts.push_back(microsSince(start));
        }
    }
    {
        std::vector<double>& updates = add("Single-row UPDATE by key, own transaction");
        for (int i = 0; i < options.singleUpdates; ++i) {
            std::string sql = "UPDATE t SET k = k + 1 WHERE id = " + std::to_string(anyRow(rng));
            auto start = Clock::now();
            engine.exec(sql);
            updates.push_back(microsSince(start));
        }
    }
    engine.close();
    return results;
}

Options parseArgs(int argc, char** argv) {
    Options options;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string flag = argv[i], value = argv[i + 1];
        if (flag == "--rows") options.rows = std::stoll(value);
        else if (flag == "--lookups") options.lookups = std::stoi(value);
        else if (flag == "--ranges") options.ranges = std::stoi(value);
        else if (flag == "--full-scans") options.fullScans = std::stoi(value);
        else if (flag == "--single-inserts") options.singleInserts = std::stoi(value);
        else if (flag == "--single-updates") options.singleUpdates = std::stoi(value);
        else if (flag == "--dir") options.dir = value;
        else if (flag == "--mode") options.mode = value;
        else if (flag == "--csv") options.csvPath = value;
        else if (flag == "--seed") options.seed = unsigned(std::stoul(value));
        else fail("unknown flag " + flag);
    }
    if (options.mode != "durable" && options.mode != "relaxed") fail("--mode must be durable or relaxed");
    return options;
}

void removeFiles(const std::string& path) {
    for (const char* suffix : {"", "-wal", "-shm", "-journal"}) std::remove((path + suffix).c_str());
}

std::string formatMicros(double us) {
    std::ostringstream out;
    out << std::fixed;
    if (us >= 10000) out << std::setprecision(1) << us / 1000 << " ms";
    else if (us >= 100) out << std::setprecision(0) << us << " µs";
    else out << std::setprecision(1) << us << " µs";
    return out.str();
}

}  // namespace

int main(int argc, char** argv) {
    Options options = parseArgs(argc, argv);
    const bool durable = options.mode == "durable";
    const std::string jerryPath = options.dir + "/vs_jerryql.db";
    const std::string sqlitePath = options.dir + "/vs_sqlite.db";
    removeFiles(jerryPath);
    removeFiles(sqlitePath);

    JerryEngine jerry(jerryPath, durable);
    SqliteEngine sqlite(sqlitePath, durable);
    EngineResults jr = runWorkloads(jerry, options);
    EngineResults sr = runWorkloads(sqlite, options);

    std::cout << "SQLite " << sqlite3_libversion() << ", mode: " << options.mode
              << (durable ? " (SQLite synchronous=FULL, JerryQL fsync per commit)"
                          : " (SQLite synchronous=NORMAL, JerryQL no fsync on commit)")
              << ", rows: " << options.rows << "\n\n";
    std::cout << "| Workload | Runs | JerryQL median | JerryQL p95 | SQLite median | SQLite p95 | JerryQL ÷ SQLite (median time) |\n"
              << "|---|---:|---:|---:|---:|---:|---:|\n";
    std::map<std::string, const Series*> sqliteByWorkload;
    for (const Series& s : sr.series) sqliteByWorkload[s.workload] = &s;
    for (const Series& j : jr.series) {
        const Series* s = sqliteByWorkload.at(j.workload);
        double jm = percentile(j.micros, 0.5), sm = percentile(s->micros, 0.5);
        std::cout << "| " << j.workload << " | " << j.micros.size() << " | " << formatMicros(jm) << " | "
                  << formatMicros(percentile(j.micros, 0.95)) << " | " << formatMicros(sm) << " | "
                  << formatMicros(percentile(s->micros, 0.95)) << " | " << std::fixed
                  << std::setprecision(2) << jm / sm << "× |\n";
    }
    for (const Series& s : sr.series) {
        if (s.workload.find("reference") != std::string::npos) {
            std::cout << "| " << s.workload << " | " << s.micros.size() << " | | | "
                      << formatMicros(percentile(s.micros, 0.5)) << " | "
                      << formatMicros(percentile(s.micros, 0.95)) << " | |\n";
        }
    }
    std::cout << std::setprecision(2) << "\nBulk load total: JerryQL " << jr.loadSeconds << " s ("
              << std::setprecision(0) << double(options.rows) / jr.loadSeconds << " rows/s), SQLite "
              << std::setprecision(2) << sr.loadSeconds << " s (" << std::setprecision(0)
              << double(options.rows) / sr.loadSeconds << " rows/s)\n"
              << std::setprecision(1) << "Size on disk after load: JerryQL " << double(jr.bytesAfterLoad) / 1e6
              << " MB, SQLite " << double(sr.bytesAfterLoad) / 1e6 << " MB\n";

    std::ofstream csv(options.csvPath);
    csv << "engine,workload,run,micros\n";
    for (const EngineResults* r : {&jr, &sr}) {
        for (const Series& s : r->series) {
            for (size_t i = 0; i < s.micros.size(); ++i) {
                csv << s.engine << ",\"" << s.workload << "\"," << i << "," << std::fixed
                    << std::setprecision(2) << s.micros[i] << "\n";
            }
        }
    }
    removeFiles(jerryPath);
    removeFiles(sqlitePath);
    return 0;
}
