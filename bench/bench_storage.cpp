// Storage benchmark: primary-key lookups and range scans vs. full scans,
// end to end through the SQL layer (parse + plan + execute) on a database file.
//
//   ./build/jerryql_bench --rows 1000000 --db /tmp/bench.db
//
// Table: t(id INT PRIMARY KEY, k INT, payload TEXT), with k = id. A query on
// id can use the B+tree; the same query on k must scan the whole table.
// Prints a Markdown table and writes raw per-query latencies as CSV.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "database.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    int64_t rows = 1000000;
    int lookups = 10000;
    int ranges = 2000;
    int fullScans = 20;
    size_t poolPages = 1024;
    std::string dbPath = "/tmp/jerryql_bench.db";
    std::string csvPath = "bench_latencies.csv";
    unsigned seed = 1;
};

struct Series {
    std::string name;
    std::vector<double> micros;  // one entry per query
};

double percentile(std::vector<double> values, double p) {
    std::sort(values.begin(), values.end());
    size_t index = size_t(p * double(values.size() - 1) + 0.5);
    return values[std::min(index, values.size() - 1)];
}

double elapsedMicros(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

// Runs one query, checks it returned the expected number of rows, and
// returns its latency in microseconds.
double timeQuery(jerryql::Database& db, const std::string& sql, size_t expectedRows) {
    auto start = Clock::now();
    jerryql::QueryResult result = db.execute(sql);
    double micros = elapsedMicros(start);
    if (result.rows.size() != expectedRows) {
        std::cerr << "unexpected row count " << result.rows.size() << " for: " << sql << "\n";
        std::exit(1);
    }
    return micros;
}

double loadTable(jerryql::Database& db, const Options& options) {
    db.execute("CREATE TABLE t (id INT PRIMARY KEY, k INT, payload TEXT)");
    const std::string payload(50, 'p');
    const int64_t batch = 1000;
    auto start = Clock::now();
    for (int64_t first = 0; first < options.rows; first += batch) {
        std::string sql = "INSERT INTO t VALUES ";
        for (int64_t id = first; id < std::min(first + batch, options.rows); ++id) {
            if (id != first) sql += ", ";
            sql += "(" + std::to_string(id) + ", " + std::to_string(id) + ", '" + payload + "')";
        }
        db.execute(sql);
    }
    return elapsedMicros(start) / 1e6;
}

Series pointLookups(jerryql::Database& db, const Options& options, const std::string& name,
                    int count, std::mt19937_64& rng) {
    std::uniform_int_distribution<int64_t> pick(0, options.rows - 1);
    Series series{name, {}};
    for (int i = 0; i < count; ++i) {
        series.micros.push_back(
            timeQuery(db, "SELECT * FROM t WHERE id = " + std::to_string(pick(rng)), 1));
    }
    return series;
}

Series rangeScans(jerryql::Database& db, const Options& options, std::mt19937_64& rng) {
    std::uniform_int_distribution<int64_t> pick(0, options.rows - 100);
    Series series{"PK range scan, 100 rows (warm)", {}};
    for (int i = 0; i < options.ranges; ++i) {
        int64_t lo = pick(rng);
        series.micros.push_back(timeQuery(db,
            "SELECT * FROM t WHERE id >= " + std::to_string(lo) + " AND id < " + std::to_string(lo + 100),
            100));
    }
    return series;
}

Series fullScans(jerryql::Database& db, const Options& options, std::mt19937_64& rng) {
    std::uniform_int_distribution<int64_t> pick(0, options.rows - 1);
    Series series{"Full scan, same lookup on unindexed k (warm)", {}};
    for (int i = 0; i < options.fullScans; ++i) {
        series.micros.push_back(
            timeQuery(db, "SELECT * FROM t WHERE k = " + std::to_string(pick(rng)), 1));
    }
    return series;
}

Options parseArgs(int argc, char** argv) {
    Options options;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string flag = argv[i], value = argv[i + 1];
        if (flag == "--rows") options.rows = std::stoll(value);
        else if (flag == "--lookups") options.lookups = std::stoi(value);
        else if (flag == "--ranges") options.ranges = std::stoi(value);
        else if (flag == "--full-scans") options.fullScans = std::stoi(value);
        else if (flag == "--pool-pages") options.poolPages = std::stoul(value);
        else if (flag == "--db") options.dbPath = value;
        else if (flag == "--csv") options.csvPath = value;
        else if (flag == "--seed") options.seed = unsigned(std::stoul(value));
        else {
            std::cerr << "unknown flag " << flag << "\n";
            std::exit(2);
        }
    }
    return options;
}

void writeCsv(const std::vector<Series>& all, const std::string& path) {
    std::ofstream out(path);
    out << "series,query,micros\n";
    for (const Series& s : all) {
        for (size_t i = 0; i < s.micros.size(); ++i) {
            out << '"' << s.name << "\"," << i << "," << std::fixed << std::setprecision(2) << s.micros[i] << "\n";
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options options = parseArgs(argc, argv);
    std::remove(options.dbPath.c_str());
    std::mt19937_64 rng(options.seed);
    jerryql::DatabaseOptions dbOptions{options.poolPages};
    std::vector<Series> all;
    double loadSeconds = 0;
    double indexBuildSeconds = 0;
    uint32_t pages = 0;

    {
        jerryql::Database db(options.dbPath, dbOptions);
        loadSeconds = loadTable(db, options);
        pages = db.pager().pageCount();
    }
    {
        // Cold: a fresh process-level buffer pool. The OS page cache is still
        // warm (dropping it needs root), so "cold" means pool misses, not disk reads.
        jerryql::Database db(options.dbPath, dbOptions);
        all.push_back(pointLookups(db, options, "PK point lookup (cold buffer pool)", 1000, rng));
        pointLookups(db, options, "warm-up", options.lookups, rng);
        all.push_back(pointLookups(db, options, "PK point lookup (warm)", options.lookups, rng));
        all.push_back(rangeScans(db, options, rng));
        all.push_back(fullScans(db, options, rng));

        // Same lookup on k again, after indexing it.
        auto indexStart = Clock::now();
        db.execute("CREATE INDEX t_k ON t (k)");
        indexBuildSeconds = elapsedMicros(indexStart) / 1e6;
        std::uniform_int_distribution<int64_t> anyRow(0, options.rows - 1);
        Series indexed{"Secondary index lookup on k (warm)", {}};
        for (int i = 0; i < options.lookups; ++i) {
            indexed.micros.push_back(
                timeQuery(db, "SELECT * FROM t WHERE k = " + std::to_string(anyRow(rng)), 1));
        }
        all.push_back(std::move(indexed));
        const jerryql::BufferPoolStats& stats = db.pager().pool().stats();
        std::cout << "buffer pool: " << options.poolPages << " pages, hits " << stats.hits
                  << ", misses " << stats.misses << "\n";
    }

    std::cout << "rows: " << options.rows << ", file pages: " << pages << " ("
              << std::fixed << std::setprecision(1) << double(pages) * 4096 / 1e6 << " MB)"
              << ", load: " << std::setprecision(2) << loadSeconds << " s ("
              << std::setprecision(0) << double(options.rows) / loadSeconds
              << " rows/s, 1000-row INSERT statements, fsync per statement)\n\n";
    std::cout << "| Query | Runs | Median (us) | p95 (us) |\n|---|---:|---:|---:|\n";
    for (const Series& s : all) {
        std::cout << "| " << s.name << " | " << s.micros.size() << " | " << std::setprecision(1)
                  << percentile(s.micros, 0.5) << " | " << percentile(s.micros, 0.95) << " |\n";
    }
    double pk = percentile(all[1].micros, 0.5), scan = percentile(all[3].micros, 0.5);
    double secondary = percentile(all[4].micros, 0.5);
    std::cout << "\nPK lookup vs full scan (median, warm): " << std::setprecision(0) << scan / pk
              << "x faster\n"
              << "Secondary index lookup vs full scan (median, warm): " << scan / secondary << "x faster\n"
              << "CREATE INDEX over " << options.rows << " rows: " << std::setprecision(2)
              << indexBuildSeconds << " s\n";
    writeCsv(all, options.csvPath);
    std::remove(options.dbPath.c_str());
    return 0;
}
