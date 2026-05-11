#include <gtest/gtest.h>
#include "../src/services/ConsolidationService.h"
#include "../src/models/Transaction.h"
#include "../src/models/Account.h"

#include <algorithm>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static Transaction make_tx(
    const std::string& plaid_id,
    const std::string& account_id,
    const std::string& date,
    double amount,
    const std::string& description = "Test"
) {
    Transaction tx;
    tx.id = "tx_" + plaid_id;
    tx.plaid_transaction_id = plaid_id;
    tx.account_id = account_id;
    tx.date = date;
    tx.amount = amount;
    tx.description = description;
    tx.category_id = "cat_other_expense";
    tx.pending = false;
    return tx;
}

// Produce a deterministic serialisation of a transaction list for hashing.
// We sort by (plaid_transaction_id, account_id, date, amount) so ordering
// differences do not produce false inequality.
static std::string serialise(std::vector<Transaction> txs) {
    std::sort(txs.begin(), txs.end(), [](const Transaction& a, const Transaction& b) {
        if (a.plaid_transaction_id != b.plaid_transaction_id)
            return a.plaid_transaction_id < b.plaid_transaction_id;
        if (a.account_id != b.account_id)
            return a.account_id < b.account_id;
        if (a.date != b.date)
            return a.date < b.date;
        return a.amount < b.amount;
    });

    std::ostringstream oss;
    for (const auto& tx : txs) {
        oss << tx.plaid_transaction_id << "|"
            << tx.account_id << "|"
            << tx.date << "|"
            << std::fixed << std::setprecision(2) << tx.amount << "|"
            << tx.description << "|"
            << tx.category_id << "\n";
    }
    return oss.str();
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class ConsolidationServiceTest : public ::testing::Test {
protected:
    ConsolidationService& svc = ConsolidationService::instance();
};

// ---------------------------------------------------------------------------
// 1. Cross-account transfer — both halves must survive (NOT deduped)
// ---------------------------------------------------------------------------

TEST_F(ConsolidationServiceTest, CrossAccountTransferNotDeduped) {
    // A wire transfer between two owned accounts produces two Plaid transactions
    // with distinct transaction IDs.  ConsolidationService must preserve both.
    Transaction debit = make_tx(
        "plaid_wire_debit",          // unique Plaid ID on the sending side
        "acc_checking",
        "2026-04-15",
        -1000.00,
        "TRANSFER TO SAVINGS"
    );

    Transaction credit = make_tx(
        "plaid_wire_credit",         // unique Plaid ID on the receiving side
        "acc_savings",
        "2026-04-15",
        1000.00,
        "TRANSFER FROM CHECKING"
    );

    std::vector<Transaction> existing;
    auto result = svc.merge_transactions(existing, {debit, credit});

    EXPECT_EQ(result.new_transactions, 2)
        << "Both halves of the intra-account transfer must be treated as new";
    EXPECT_EQ(result.duplicate_transactions, 0)
        << "Transfer halves must NOT be deduped — they have distinct Plaid IDs";
    ASSERT_EQ(existing.size(), 2)
        << "DataStore must contain both the debit and credit legs";

    // Verify each leg is individually identifiable.
    auto debit_it = std::find_if(existing.begin(), existing.end(),
        [](const Transaction& t){ return t.plaid_transaction_id == "plaid_wire_debit"; });
    auto credit_it = std::find_if(existing.begin(), existing.end(),
        [](const Transaction& t){ return t.plaid_transaction_id == "plaid_wire_credit"; });

    ASSERT_NE(debit_it, existing.end())   << "Debit leg must be present";
    ASSERT_NE(credit_it, existing.end())  << "Credit leg must be present";
    EXPECT_EQ(debit_it->account_id, "acc_checking");
    EXPECT_EQ(credit_it->account_id, "acc_savings");
    EXPECT_DOUBLE_EQ(debit_it->amount, -1000.00);
    EXPECT_DOUBLE_EQ(credit_it->amount,  1000.00);
}

// ---------------------------------------------------------------------------
// 2. Duplicate on same account — second import must be absorbed (deduped)
// ---------------------------------------------------------------------------

TEST_F(ConsolidationServiceTest, DuplicateOnSameAccountIsDeduped) {
    // Simulates a Plaid sync that returns the same transaction twice
    // (e.g., a webhook retry or back-to-back syncs).
    Transaction original = make_tx(
        "plaid_tx_abc123",
        "acc_checking",
        "2026-04-10",
        -75.00,
        "STARBUCKS COFFEE"
    );

    std::vector<Transaction> existing = {original};

    // Second import: identical Plaid ID, same account.
    Transaction reimport = original;
    reimport.id = "tx_reimport";   // different internal ID (shouldn't matter)

    auto result = svc.merge_transactions(existing, {reimport});

    EXPECT_EQ(result.duplicate_transactions, 1)
        << "Re-import of the same Plaid transaction must be counted as a duplicate";
    EXPECT_EQ(result.new_transactions, 0);
    ASSERT_EQ(existing.size(), 1)
        << "DataStore must contain only one copy after dedup";
}

// ---------------------------------------------------------------------------
// 3. Idempotency — f(f(x)) == f(x)
// ---------------------------------------------------------------------------

TEST_F(ConsolidationServiceTest, IdempotencyRunningTwiceProducesSameState) {
    // Build a deterministic set of 100 transactions across 5 accounts.
    // Amounts and dates are whole values to avoid floating-point surprises.
    std::vector<Transaction> base;
    const int kCount = 100;
    const int kAccounts = 5;

    for (int i = 0; i < kCount; ++i) {
        std::string acc = "acc_idem_" + std::to_string(i % kAccounts);
        int month = (i % 5) + 1;
        int day   = (i % 28) + 1;
        std::ostringstream date_ss;
        date_ss << "2026-"
                << std::setw(2) << std::setfill('0') << month << "-"
                << std::setw(2) << std::setfill('0') << day;

        base.push_back(make_tx(
            "plaid_idem_" + std::to_string(i),
            acc,
            date_ss.str(),
            static_cast<double>(-(i + 1) * 10),
            "Merchant " + std::to_string(i)
        ));
    }

    // First consolidation pass.
    std::vector<Transaction> state1;
    svc.merge_transactions(state1, base);

    std::string snapshot1 = serialise(state1);

    // Second consolidation pass on the result of the first.
    // If idempotent, no new transactions should be added.
    std::vector<Transaction> state2 = state1;
    auto result2 = svc.merge_transactions(state2, state1);

    std::string snapshot2 = serialise(state2);

    EXPECT_EQ(result2.new_transactions, 0)
        << "Second pass must not add any transactions (idempotency)";
    EXPECT_EQ(result2.duplicate_transactions, static_cast<size_t>(kCount))
        << "All transactions from pass 1 must be seen as duplicates in pass 2";
    EXPECT_EQ(snapshot1, snapshot2)
        << "Serialised state must be identical after two consolidation passes";
}

// ---------------------------------------------------------------------------
// 4. Hash stability — two independent runs on fresh input yield same result
// ---------------------------------------------------------------------------

TEST_F(ConsolidationServiceTest, HashStabilityAcrossRuns) {
    // Build the same deterministic input independently for each "run".
    auto build_input = []() -> std::vector<Transaction> {
        std::vector<Transaction> txs;
        for (int i = 0; i < 50; ++i) {
            std::string acc = "acc_hs_" + std::to_string(i % 3);
            int month = (i % 4) + 1;
            int day   = (i % 20) + 1;
            std::ostringstream date_ss;
            date_ss << "2026-"
                    << std::setw(2) << std::setfill('0') << month << "-"
                    << std::setw(2) << std::setfill('0') << day;

            txs.push_back(make_tx(
                "plaid_hs_" + std::to_string(i),
                acc,
                date_ss.str(),
                static_cast<double>(-(i + 1) * 5),
                "Stable Merchant " + std::to_string(i)
            ));
        }
        return txs;
    };

    // Run 1.
    std::vector<Transaction> out1;
    svc.merge_transactions(out1, build_input());

    // Run 2 — completely independent: fresh input, fresh output vector.
    std::vector<Transaction> out2;
    svc.merge_transactions(out2, build_input());

    ASSERT_EQ(out1.size(), out2.size())
        << "Both independent runs must produce the same number of transactions";

    EXPECT_EQ(serialise(out1), serialise(out2))
        << "Serialised output must be identical across two independent runs "
           "on the same deterministic input (hash stability)";
}
