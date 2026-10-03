// Crash-injection harness for the write-ahead log.
//
//   jerryql_crashtest --mode kill --runs 1000
//     Forks a child that runs bank transfers against a real database file
//     and reports each acknowledged COMMIT/ROLLBACK through a pipe. The
//     parent SIGKILLs it at a random moment, reopens the file (running
//     recovery) and checks every acknowledged commit survived and nothing is
//     half-applied. 25 consecutive kills share one database, so recovery
//     also runs on already-recovered files.
//
//   jerryql_crashtest --mode powerloss --runs 1000
//     In-process. Both files are FaultFiles that hold unsynced writes in a
//     simulated volatile cache. Power is cut at a random file operation; the
//     disk image keeps a random subset of unsynced writes (some torn). This
//     checks the fsync ordering, which SIGKILL cannot: after a SIGKILL the OS
//     still writes everything the process handed it.
//
// Prints "N runs, M corrupted" and exits non-zero if M > 0.

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#include "bank_workload.h"
#include "fault_file.h"

using namespace jerryql;
using namespace jerryql::crash;

namespace {

struct Options {
    std::string mode = "powerloss";
    int runs = 200;
    unsigned seed = 1;
    std::string dir = "/tmp/jerryql_crash";
    int maxKillDelayMs = 300;
    int transactionsPerRun = 60;
    bool unsafeNoFsync = false;  // to show the harness catches a missing fsync
};

bool g_unsafeNoFsync = false;

struct Totals {
    int runs = 0;
    int corrupted = 0;
    uint64_t acknowledgedCommits = 0;   // distinct acknowledged commits
    uint64_t commitChecks = 0;          // (run, acknowledged commit) pairs checked present
    uint64_t unacknowledgedCommits = 0; // committed but the ack was lost in the crash
    uint64_t recoveredFrames = 0;
    uint64_t discardedFrames = 0;
    int runsWithRecovery = 0;
};

DatabaseOptions harnessOptions() {
    DatabaseOptions options;
    options.poolPages = 32;          // small, so transactions evict dirty pages
    options.checkpointFrames = 200;  // frequent checkpoints, so crashes land in them too
    options.syncOnCommit = !g_unsafeNoFsync;
    return options;
}

void report(const Totals& totals, const std::string& mode) {
    std::cout << "\nmode: " << mode << "\n"
              << totals.runs << " runs, " << totals.corrupted << " corrupted\n"
              << "distinct acknowledged commits: " << totals.acknowledgedCommits
              << " (re-checked after every later crash in the same series: "
              << totals.commitChecks << " checks)\n"
              << "commits that survived although the crash came before their acknowledgement: "
              << totals.unacknowledgedCommits << "\n"
              << "runs where recovery replayed log frames: " << totals.runsWithRecovery << "\n"
              << "log frames replayed: " << totals.recoveredFrames
              << ", uncommitted frames discarded: " << totals.discardedFrames << "\n";
}

// ---------- SIGKILL mode ----------

// Child: run transactions forever, acknowledging through the pipe.
[[noreturn]] void childLoop(const std::string& path, int ackFd, int64_t firstTxn, unsigned seed) {
    auto ack = [&](char kind, int64_t txn) {
        std::string line = std::string(1, kind) + " " + std::to_string(txn) + "\n";
        if (::write(ackFd, line.data(), line.size()) < 0) _exit(3);
    };
    try {
        Database db(path, harnessOptions());
        if (setupBank(db)) ack('S', 0);
        std::mt19937_64 rng(seed);
        for (int64_t txn = firstTxn;; ++txn) {
            ack('A', txn);
            Outcome outcome = runTransaction(db, rng, txn);
            if (outcome == Outcome::Committed) ack('C', txn);
            if (outcome == Outcome::RolledBack) ack('R', txn);
        }
    } catch (const std::exception& e) {
        std::cerr << "child error: " << e.what() << "\n";
        _exit(4);
    }
}

void readAcks(int fd, Acknowledged& acks) {
    std::string buffer;
    char chunk[4096];
    ssize_t n;
    while ((n = ::read(fd, chunk, sizeof(chunk))) > 0) buffer.append(chunk, size_t(n));
    std::istringstream lines(buffer);
    char kind;
    int64_t txn;
    while (lines >> kind >> txn) {
        if (kind == 'S') acks.setup = true;
        if (kind == 'A') acks.maxAttempted = std::max(acks.maxAttempted, txn);
        if (kind == 'C') acks.committed.insert(txn);
        if (kind == 'R') acks.rolledBack.insert(txn);
    }
}

void removeDatabase(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
}

// After a successful check, commits that survived without an acknowledgement
// are durable too, so they join the set that every later crash must keep.
bool verifyAndRecord(Database& db, Acknowledged& acks, Totals& totals, const std::string& label) {
    VerifyResult result = verifyBank(db, acks);
    const PagerStats& stats = db.pager().stats();
    totals.recoveredFrames += stats.recoveredFrames;
    totals.discardedFrames += stats.discardedFrames;
    if (stats.recoveredFrames > 0) ++totals.runsWithRecovery;
    totals.commitChecks += acks.committed.size();
    totals.unacknowledgedCommits += result.unacknowledgedCommits.size();
    if (!result.error.empty()) {
        std::cout << label << ": CORRUPTED: " << result.error << "\n";
        return false;
    }
    acks.committed.insert(result.unacknowledgedCommits.begin(), result.unacknowledgedCommits.end());
    return true;
}

void runKillMode(const Options& options, Totals& totals) {
    std::filesystem::create_directories(options.dir);
    const std::string path = options.dir + "/bank.db";
    std::mt19937_64 rng(options.seed);
    Acknowledged acks;
    for (int run = 0; run < options.runs; ++run) {
        if (run % 25 == 0) {  // start a new series on a fresh file
            removeDatabase(path);
            acks = Acknowledged();
        }
        int pipeFds[2];
        if (::pipe(pipeFds) != 0) throw std::runtime_error("pipe failed");
        pid_t pid = ::fork();
        if (pid < 0) throw std::runtime_error("fork failed");
        if (pid == 0) {
            ::close(pipeFds[0]);
            childLoop(path, pipeFds[1], acks.maxAttempted + 1, unsigned(rng()));
        }
        ::close(pipeFds[1]);
        int delayMs = int(rng() % unsigned(options.maxKillDelayMs)) + 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        ::kill(pid, SIGKILL);
        int status = 0;
        ::waitpid(pid, &status, 0);
        size_t acknowledgedBefore = acks.committed.size();
        readAcks(pipeFds[0], acks);
        ::close(pipeFds[0]);
        totals.acknowledgedCommits += acks.committed.size() - acknowledgedBefore;
        // A commit acknowledged now may already have been counted as an
        // unacknowledged survivor after the previous crash; that's fine.
        if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)) {
            std::cout << "run " << run << ": child exited on its own (status " << status << ")\n";
        }

        ++totals.runs;
        bool ok = false;
        try {
            Database db(path, harnessOptions());  // runs recovery
            ok = verifyAndRecord(db, acks, totals, "run " + std::to_string(run));
        } catch (const std::exception& e) {
            std::cout << "run " << run << ": CORRUPTED: reopening failed: " << e.what() << "\n";
        }
        if (!ok) {
            ++totals.corrupted;
            std::string keep = options.dir + "/failed_run_" + std::to_string(run);
            std::filesystem::copy_file(path, keep + ".db", std::filesystem::copy_options::overwrite_existing);
            if (std::filesystem::exists(path + "-wal")) {
                std::filesystem::copy_file(path + "-wal", keep + ".db-wal",
                                           std::filesystem::copy_options::overwrite_existing);
            }
            removeDatabase(path);
            acks = Acknowledged();
        }
        if ((run + 1) % 50 == 0) {
            std::cout << "  " << (run + 1) << " runs, " << totals.corrupted << " corrupted, "
                      << acks.committed.size() << " commits acknowledged in this series\n" << std::flush;
        }
    }
    removeDatabase(path);
}

// ---------- Simulated power-loss mode ----------

// Runs setup plus `transactions` transfers on fresh fault-injecting disks,
// recording acknowledgements, until the power cut (or the end).
struct PowerLossRun {
    std::shared_ptr<PowerSwitch> power = std::make_shared<PowerSwitch>();
    std::shared_ptr<FaultDisk> dbDisk = std::make_shared<FaultDisk>();
    std::shared_ptr<FaultDisk> walDisk = std::make_shared<FaultDisk>();
    Acknowledged acks;
    bool cut = false;

    void execute(int transactions, unsigned workloadSeed) {
        try {
            Database db(std::make_unique<FaultFile>(dbDisk, power),
                        std::make_unique<FaultFile>(walDisk, power), harnessOptions());
            if (setupBank(db)) acks.setup = true;
            std::mt19937_64 rng(workloadSeed);
            for (int64_t txn = 1; txn <= transactions; ++txn) {
                acks.maxAttempted = txn;
                Outcome outcome = runTransaction(db, rng, txn);
                if (outcome == Outcome::Committed) acks.committed.insert(txn);
                if (outcome == Outcome::RolledBack) acks.rolledBack.insert(txn);
            }
        } catch (const PowerCut&) {
            cut = true;
        }
    }
};

void runPowerLossMode(const Options& options, Totals& totals) {
    std::mt19937_64 rng(options.seed);
    // Count the file operations of a full run, to pick cut points across it.
    PowerLossRun calibration;
    calibration.execute(options.transactionsPerRun, 12345);
    const int64_t operations = calibration.power->operations;
    std::cout << "a full run performs about " << operations << " file operations\n";

    int cuts = 0;
    for (int run = 0; run < options.runs; ++run) {
        PowerLossRun crashRun;
        crashRun.power->operationsLeft = int64_t(rng() % uint64_t(operations));
        crashRun.execute(options.transactionsPerRun, unsigned(rng()));
        if (crashRun.cut) ++cuts;
        totals.acknowledgedCommits += crashRun.acks.committed.size();

        auto dbImage = std::make_unique<MemoryFile>();
        auto walImage = std::make_unique<MemoryFile>();
        dbImage->bytes() = crashRun.dbDisk->crashImage(rng);
        walImage->bytes() = crashRun.walDisk->crashImage(rng);

        ++totals.runs;
        bool ok = false;
        std::string label = "run " + std::to_string(run) + " (cut at operation " +
                            std::to_string(crashRun.power->operations) + ")";
        try {
            Database db(std::move(dbImage), std::move(walImage), harnessOptions());
            ok = verifyAndRecord(db, crashRun.acks, totals, label);
        } catch (const std::exception& e) {
            std::cout << label << ": CORRUPTED: reopening failed: " << e.what() << "\n";
        }
        if (!ok) ++totals.corrupted;
        if ((run + 1) % 200 == 0) {
            std::cout << "  " << (run + 1) << " runs, " << totals.corrupted << " corrupted\n" << std::flush;
        }
    }
    std::cout << "power was cut mid-run in " << cuts << " of " << options.runs << " runs\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string flag = argv[i], value = argv[i + 1];
        if (flag == "--mode") options.mode = value;
        else if (flag == "--runs") options.runs = std::stoi(value);
        else if (flag == "--seed") options.seed = unsigned(std::stoul(value));
        else if (flag == "--dir") options.dir = value;
        else if (flag == "--max-kill-delay-ms") options.maxKillDelayMs = std::stoi(value);
        else if (flag == "--transactions") options.transactionsPerRun = std::stoi(value);
        else if (flag == "--unsafe-no-fsync") options.unsafeNoFsync = value == "1";
        else {
            std::cerr << "unknown flag " << flag << "\n";
            return 2;
        }
    }
    g_unsafeNoFsync = options.unsafeNoFsync;
    std::cout << "seed " << options.seed << (g_unsafeNoFsync ? " (UNSAFE: commit fsync disabled)" : "") << "\n";
    Totals totals;
    if (options.mode == "kill") {
        runKillMode(options, totals);
    } else if (options.mode == "powerloss") {
        runPowerLossMode(options, totals);
    } else {
        std::cerr << "--mode must be kill or powerloss\n";
        return 2;
    }
    report(totals, options.mode);
    return totals.corrupted == 0 ? 0 : 1;
}
