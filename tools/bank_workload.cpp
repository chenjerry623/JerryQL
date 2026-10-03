#include "bank_workload.h"

#include <map>
#include <stdexcept>

#include "sql_error.h"
#include "storage/btree_store.h"

namespace jerryql::crash {

namespace {

int64_t randomBetween(std::mt19937_64& rng, int64_t lo, int64_t hi) {
    return std::uniform_int_distribution<int64_t>(lo, hi)(rng);
}

std::string text(size_t length, char fill) {
    return "'" + std::string(length, fill) + "'";
}

bool tablesExist(Database& db) {
    return db.tableNames().size() == 3;
}

void transfer(Database& db, std::mt19937_64& rng, int64_t txnId) {
    int64_t src = randomBetween(rng, 1, kAccounts);
    int64_t dst = randomBetween(rng, 1, kAccounts - 1);
    if (dst >= src) ++dst;
    int64_t amount = randomBetween(rng, 1, 200);
    db.execute("UPDATE accounts SET balance = balance - " + std::to_string(amount) +
               " WHERE id = " + std::to_string(src));
    db.execute("UPDATE accounts SET balance = balance + " + std::to_string(amount) +
               " WHERE id = " + std::to_string(dst));
    db.execute("INSERT INTO ledger VALUES (" + std::to_string(txnId) + ", " + std::to_string(src) +
               ", " + std::to_string(dst) + ", " + std::to_string(amount) + ", " +
               text(size_t(randomBetween(rng, 0, 400)), 'n') + ")");
}

// Big rows inserted and deleted in bulk: splits, emptied leaves, page reuse,
// and (with a small buffer pool) dirty-page eviction mid-transaction.
void churnScratch(Database& db, std::mt19937_64& rng) {
    int64_t base = randomBetween(rng, 0, 400);
    std::string sql = "INSERT INTO scratch VALUES ";
    int rows = int(randomBetween(rng, 5, 40));
    for (int i = 0; i < rows; ++i) {
        sql += (i ? ", (" : "(") + std::to_string(base * 100 + i) + ", " +
               text(size_t(randomBetween(rng, 100, 900)), 's') + ")";
    }
    try {
        db.execute(sql);
    } catch (const SqlError&) {
        // duplicate key: those rows are already there; validation changed nothing
    }
    int64_t cut = randomBetween(rng, 0, 40000);
    db.execute("DELETE FROM scratch WHERE k >= " + std::to_string(cut) + " AND k < " +
               std::to_string(cut + randomBetween(rng, 0, 3000)));
}

}  // namespace

bool setupBank(Database& db) {
    if (tablesExist(db)) return false;
    db.execute("BEGIN");
    db.execute("CREATE TABLE accounts (id INT PRIMARY KEY, balance INT)");
    db.execute("CREATE TABLE ledger (txn INT PRIMARY KEY, src INT, dst INT, amount INT, note TEXT)");
    db.execute("CREATE TABLE scratch (k INT PRIMARY KEY, body TEXT)");
    db.execute("CREATE INDEX ledger_src ON ledger (src)");
    db.execute("CREATE INDEX ledger_amount ON ledger (amount)");
    std::string sql = "INSERT INTO accounts VALUES ";
    for (int id = 1; id <= kAccounts; ++id) {
        sql += (id > 1 ? ", (" : "(") + std::to_string(id) + ", " + std::to_string(kInitialBalance) + ")";
    }
    db.execute(sql);
    db.execute("COMMIT");
    return true;
}

Outcome runTransaction(Database& db, std::mt19937_64& rng, int64_t txnId) {
    int64_t kind = randomBetween(rng, 0, 19);
    if (kind < 3) {  // autocommit statements, not tracked
        churnScratch(db, rng);
        return Outcome::Untracked;
    }
    db.execute("BEGIN");
    transfer(db, rng, txnId);
    if (kind < 6) churnScratch(db, rng);  // larger multi-table transaction
    if (kind >= 17) {
        db.execute("ROLLBACK");
        return Outcome::RolledBack;
    }
    db.execute("COMMIT");
    return Outcome::Committed;
}

VerifyResult verifyBank(Database& db, const Acknowledged& acks) {
    VerifyResult result;
    if (!tablesExist(db)) {
        if (acks.setup || !acks.committed.empty()) result.error = "tables missing after acknowledged setup";
        else if (!db.tableNames().empty()) result.error = "partial setup: some tables exist";
        return result;
    }
    std::string problem = db.checkIntegrity();
    if (!problem.empty()) {
        result.error = "integrity check failed: " + problem;
        return result;
    }
    if (db.table("ledger").indexes.size() != 2) {
        result.error = "ledger indexes missing after recovery";
        return result;
    }

    std::map<int64_t, int64_t> expected;
    for (int id = 1; id <= kAccounts; ++id) expected[id] = kInitialBalance;
    QueryResult ledger = db.execute("SELECT txn, src, dst, amount FROM ledger");
    result.ledgerRows = ledger.rows.size();
    std::set<int64_t> present;
    for (const Row& row : ledger.rows) {
        int64_t txn = row[0].asInt();
        present.insert(txn);
        expected[row[1].asInt()] -= row[3].asInt();
        expected[row[2].asInt()] += row[3].asInt();
        if (txn < 1 || txn > acks.maxAttempted) {
            result.error = "ledger has transaction " + std::to_string(txn) + " that was never attempted";
            return result;
        }
        if (acks.rolledBack.count(txn)) {
            result.error = "rolled-back transaction " + std::to_string(txn) + " is in the ledger";
            return result;
        }
        if (!acks.committed.count(txn)) result.unacknowledgedCommits.insert(txn);
    }
    for (int64_t txn : acks.committed) {
        if (!present.count(txn)) {
            result.error = "acknowledged commit " + std::to_string(txn) + " is missing";
            return result;
        }
    }

    QueryResult accounts = db.execute("SELECT id, balance FROM accounts");
    if (accounts.rows.size() != size_t(kAccounts)) {
        result.error = "accounts table has " + std::to_string(accounts.rows.size()) + " rows";
        return result;
    }
    int64_t total = 0;
    for (const Row& row : accounts.rows) {
        total += row[1].asInt();
        if (row[1].asInt() != expected[row[0].asInt()]) {
            result.error = "account " + row[0].toString() + " balance " + row[1].toString() +
                           " doesn't match the ledger (" + std::to_string(expected[row[0].asInt()]) +
                           "): a transaction was partially applied";
            return result;
        }
    }
    if (total != kAccounts * kInitialBalance) result.error = "total balance changed";
    return result;
}

}  // namespace jerryql::crash
