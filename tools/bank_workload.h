#pragma once

#include <cstdint>
#include <random>
#include <set>
#include <string>

#include "database.h"

namespace jerryql::crash {

// The workload both crash harnesses run, and the checks applied afterwards.
//
//   accounts(id INT PRIMARY KEY, balance INT)    50 accounts, 1000 each
//   ledger(txn INT PRIMARY KEY, src INT, dst INT, amount INT, note TEXT)
//   scratch(k INT PRIMARY KEY, body TEXT)        churn: big rows, splits, frees
//
// A transfer moves money between two accounts and records it in the ledger,
// in one transaction. After a crash, recovery must give:
//   - every transaction whose COMMIT returned (acknowledged) is in the ledger
//   - no acknowledged ROLLBACK is in the ledger
//   - balances equal 1000 + the replay of the ledger, so no transaction is
//     half-applied (and the total is still 50,000)
//   - every table's B+tree passes its structural check
constexpr int kAccounts = 50;
constexpr int64_t kInitialBalance = 1000;

enum class Outcome { Committed, RolledBack, Untracked };

// Creates the tables in one transaction unless they already exist.
// Returns true if it committed the setup.
bool setupBank(Database& db);

// Runs transaction `txnId`. A Committed or RolledBack result means COMMIT or
// ROLLBACK returned; the caller records that as an acknowledgement.
Outcome runTransaction(Database& db, std::mt19937_64& rng, int64_t txnId);

struct Acknowledged {
    bool setup = false;
    std::set<int64_t> committed;
    std::set<int64_t> rolledBack;
    int64_t maxAttempted = 0;
};

struct VerifyResult {
    std::string error;            // empty = consistent
    size_t ledgerRows = 0;
    std::set<int64_t> unacknowledgedCommits;  // committed, but the crash came before the ack
};

VerifyResult verifyBank(Database& db, const Acknowledged& acks);

}  // namespace jerryql::crash
