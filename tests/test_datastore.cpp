#include <gtest/gtest.h>
#include "../src/models/DataStore.h"
#include "../src/models/Account.h"
#include "../src/models/Transaction.h"
#include "../src/models/Entity.h"
#include "../src/services/PlaidService.h"
#include "../src/services/ConsolidationService.h"
#include "../src/utils/Validator.h"
#include "../src/utils/SyntheticGenerator.h"
#include "../src/utils/ConfigManager.h"

#include <chrono>

class DataStoreTest : public ::testing::Test {
protected:
    DataStore data_store;
};

TEST_F(DataStoreTest, EntityValidation) {
    Entity ent;
    ent.name = "Test Entity";
    ent.type = EntityType::Individual;
    data_store.add_entity(ent);

    Account acc;
    acc.name = "Test Account";
    acc.type = AccountType::Checking;
    acc.balance = 100.0;
    acc.entity_id = data_store.entities[0].id;

    Account& added = data_store.add_account(acc);
    ASSERT_EQ(added.entity_id, data_store.entities[0].id);
}

TEST_F(DataStoreTest, AccountWithInvalidEntityIdCleared) {
    Account acc;
    acc.name = "Test Account";
    acc.type = AccountType::Checking;
    acc.balance = 100.0;
    acc.entity_id = "nonexistent_entity_id";

    Account& added = data_store.add_account(acc);
    ASSERT_TRUE(added.entity_id.empty());
    ASSERT_FALSE(data_store.get_last_error().empty());
}

TEST_F(DataStoreTest, TransactionDateValidation) {
    Account acc;
    acc.name = "Test";
    acc.type = AccountType::Checking;
    acc.balance = 100.0;
    data_store.add_account(acc);

    std::string acc_id = data_store.accounts[0].id;

    Transaction tx;
    tx.account_id = acc_id;
    tx.date = "invalid-date";
    tx.amount = -25.50;
    tx.description = "Test";

    data_store.add_transaction(tx);
    ASSERT_FALSE(data_store.get_last_error().empty());
}

TEST_F(DataStoreTest, TransactionValidDateAccepted) {
    Account acc;
    acc.name = "Test";
    acc.type = AccountType::Checking;
    acc.balance = 100.0;
    data_store.add_account(acc);

    std::string acc_id = data_store.accounts[0].id;

    Transaction tx;
    tx.account_id = acc_id;
    tx.date = "2026-05-08";
    tx.amount = -25.50;
    tx.description = "Test";

    data_store.add_transaction(tx);
    ASSERT_EQ(data_store.transactions.size(), 1);
}

TEST_F(DataStoreTest, AddAccount) {
    Account acc;
    acc.name = "Test Checking";
    acc.type = AccountType::Checking;
    acc.balance = 500.0;
    acc.institution = "Test Bank";

    Account& added = data_store.add_account(acc);

    ASSERT_EQ(data_store.accounts.size(), 1);
    ASSERT_EQ(added.name, "Test Checking");
    ASSERT_EQ(added.type, AccountType::Checking);
    ASSERT_FALSE(added.id.empty());
}

TEST_F(DataStoreTest, RemoveAccount) {
    Account acc;
    acc.name = "Test Account";
    acc.type = AccountType::Checking;
    acc.balance = 100.0;
    data_store.add_account(acc);

    std::string acc_id = data_store.accounts[0].id;
    data_store.remove_account(acc_id);

    ASSERT_EQ(data_store.accounts.size(), 0);
}

TEST_F(DataStoreTest, GetAccountById) {
    Account acc;
    acc.name = "Test Account";
    acc.type = AccountType::Checking;
    acc.balance = 100.0;
    data_store.add_account(acc);

    std::string acc_id = data_store.accounts[0].id;
    auto result = data_store.get_account(acc_id);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ((*result)->name, "Test Account");
}

TEST_F(DataStoreTest, GetAccountByType) {
    Account acc1;
    acc1.name = "Checking";
    acc1.type = AccountType::Checking;
    acc1.balance = 100.0;
    data_store.add_account(acc1);

    Account acc2;
    acc2.name = "Savings";
    acc2.type = AccountType::Savings;
    acc2.balance = 200.0;
    data_store.add_account(acc2);

    auto checking = data_store.get_accounts_by_type(AccountType::Checking);
    ASSERT_EQ(checking.size(), 1);
    ASSERT_EQ(checking[0]->name, "Checking");
}

TEST_F(DataStoreTest, AddTransaction) {
    Account acc;
    acc.name = "Test";
    acc.type = AccountType::Checking;
    acc.balance = 100.0;
    data_store.add_account(acc);

    Transaction tx;
    tx.account_id = data_store.accounts[0].id;
    tx.date = "2026-05-01";
    tx.amount = -25.50;
    tx.description = "Test Purchase";
    tx.category_id = "cat_food";

    data_store.add_transaction(tx);

    ASSERT_EQ(data_store.transactions.size(), 1);
    ASSERT_EQ(data_store.transactions[0].description, "Test Purchase");
    ASSERT_TRUE(data_store.transactions[0].is_expense());
}

TEST_F(DataStoreTest, NetWorthCalculation) {
    Account acc1;
    acc1.name = "Checking";
    acc1.type = AccountType::Checking;
    acc1.balance = 1000.0;
    data_store.add_account(acc1);

    Account acc2;
    acc2.name = "Credit Card";
    acc2.type = AccountType::CreditCard;
    acc2.balance = 500.0;
    data_store.add_account(acc2);

    double net_worth = data_store.get_total_net_worth();
    ASSERT_EQ(net_worth, 500.0);
}

TEST_F(DataStoreTest, SpendingForMonth) {
    Account acc;
    acc.name = "Test";
    acc.type = AccountType::Checking;
    acc.balance = 1000.0;
    data_store.add_account(acc);

    std::string acc_id = data_store.accounts[0].id;

    Transaction tx1;
    tx1.account_id = acc_id;
    tx1.date = "2026-05-01";
    tx1.amount = -50.0;
    tx1.description = "Groceries";
    data_store.add_transaction(tx1);

    Transaction tx2;
    tx2.account_id = acc_id;
    tx2.date = "2026-05-15";
    tx2.amount = -30.0;
    tx2.description = "Gas";
    data_store.add_transaction(tx2);

    double spending = data_store.get_spending_for_month("2026-05");
    ASSERT_EQ(spending, 80.0);
}

TEST_F(DataStoreTest, IncomeForMonth) {
    Account acc;
    acc.name = "Test";
    acc.type = AccountType::Checking;
    acc.balance = 0.0;
    data_store.add_account(acc);

    std::string acc_id = data_store.accounts[0].id;

    Transaction tx;
    tx.account_id = acc_id;
    tx.date = "2026-05-01";
    tx.amount = 3000.0;
    tx.description = "Salary";
    data_store.add_transaction(tx);

    double income = data_store.get_income_for_month("2026-05");
    ASSERT_EQ(income, 3000.0);
}

TEST_F(DataStoreTest, DefaultCategoriesExist) {
    ASSERT_FALSE(data_store.categories.empty());

    auto food = data_store.get_category("cat_food");
    ASSERT_TRUE(food.has_value());
    ASSERT_EQ((*food)->name, "Food & Dining");
}

TEST_F(DataStoreTest, ValidatorValidDate) {
    ASSERT_TRUE(Validator::isValidDate("2026-05-09"));
    ASSERT_TRUE(Validator::isValidDate("2025-12-31"));
    ASSERT_FALSE(Validator::isValidDate("2026/05/09"));
    ASSERT_FALSE(Validator::isValidDate("invalid"));
    ASSERT_FALSE(Validator::isValidDate("2026-13-01"));
    ASSERT_FALSE(Validator::isValidDate("2026-02-30"));
}

TEST_F(DataStoreTest, ValidatorNegativeBalanceCreditCard) {
    Account acc;
    acc.name = "Credit Card";
    acc.type = AccountType::CreditCard;
    acc.balance = -500.0;

    ASSERT_TRUE(Validator::isValidAmount(-500.0, acc));
    ASSERT_TRUE(Validator::isValidAmount(100.0, acc));
}

TEST_F(DataStoreTest, ValidatorNegativeBalanceChecking) {
    Account acc;
    acc.name = "Checking";
    acc.type = AccountType::Checking;
    acc.balance = 100.0;

    ASSERT_TRUE(Validator::isValidAmount(500.0, acc));
    ASSERT_FALSE(Validator::isValidAmount(-500.0, acc));
}

TEST_F(DataStoreTest, ValidatorOrphanedCategories) {
    Category cat;
    cat.id = "cat_orphan";
    cat.name = "Orphan Cat";
    cat.type = CategoryType::Expense;
    cat.is_system = false;
    data_store.categories.push_back(cat);

    bool has_orphans = Validator::hasOrphanedCategories(data_store.categories, data_store.transactions);
    ASSERT_TRUE(has_orphans);

    Transaction tx;
    tx.category_id = "cat_orphan";
    data_store.add_transaction(tx);

    has_orphans = Validator::hasOrphanedCategories(data_store.categories, data_store.transactions);
    ASSERT_FALSE(has_orphans);
}

TEST_F(DataStoreTest, ValidatorValidateTransaction) {
    Account acc;
    acc.name = "Checking";
    acc.type = AccountType::Checking;
    acc.balance = 100.0;
    data_store.add_account(acc);
    std::string acc_id = data_store.accounts[0].id;

    Transaction tx;
    tx.id = "tx_valid";
    tx.account_id = acc_id;
    tx.date = "2026-05-09";
    tx.amount = 25.0;

    auto result = Validator::validateTransaction(tx, acc);
    ASSERT_TRUE(result.is_valid);
}

TEST_F(DataStoreTest, ValidatorValidateTransactionInvalidDate) {
    Account acc;
    acc.name = "Checking";
    acc.type = AccountType::Checking;
    acc.balance = 100.0;

    Transaction tx;
    tx.id = "tx_invalid";
    tx.account_id = "acc123";
    tx.date = "invalid-date";
    tx.amount = -25.0;

    auto result = Validator::validateTransaction(tx, acc);
    ASSERT_FALSE(result.is_valid);
    ASSERT_EQ(result.anomaly_type, "invalid_date");
}

TEST_F(DataStoreTest, ValidatorValidateAccountOrphanEntity) {
    Entity ent;
    ent.id = "entity1";
    ent.name = "Test Entity";
    data_store.add_entity(ent);

    Account acc;
    acc.id = "acc_orphan";
    acc.name = "Orphan Account";
    acc.type = AccountType::Checking;
    acc.entity_id = "nonexistent";

    auto result = Validator::validateAccount(acc, data_store.entities);
    ASSERT_FALSE(result.is_valid);
    ASSERT_EQ(result.anomaly_type, "orphan_entity");
}

TEST_F(DataStoreTest, SyntheticGeneratorTransactions) {
    auto txs = SyntheticGenerator::generateTransactions("acc_test", 100);
    ASSERT_EQ(txs.size(), 100);

    for (const auto& tx : txs) {
        ASSERT_FALSE(tx.id.empty());
        ASSERT_EQ(tx.account_id, "acc_test");
        ASSERT_TRUE(Validator::isValidDate(tx.date));
    }
}

TEST_F(DataStoreTest, SyntheticGeneratorAccounts) {
    auto accounts = SyntheticGenerator::generateAccounts(10);
    ASSERT_EQ(accounts.size(), 10);

    for (const auto& acc : accounts) {
        ASSERT_FALSE(acc.id.empty());
        ASSERT_FALSE(acc.name.empty());
    }
}

TEST_F(DataStoreTest, HighVolumeTransactionAddition) {
    Account acc;
    acc.name = "High Volume Test";
    acc.type = AccountType::Checking;
    acc.balance = 100000.0;
    data_store.add_account(acc);
    std::string acc_id = data_store.accounts[0].id;

    for (int i = 0; i < 500; ++i) {
        Transaction tx;
        tx.account_id = acc_id;
        tx.date = "2026-05-01";
        tx.amount = static_cast<double>(-i - 1);
        tx.description = "Test transaction " + std::to_string(i);
        data_store.add_transaction(tx);
    }

    ASSERT_EQ(data_store.transactions.size(), 500);
}

TEST_F(DataStoreTest, ValidatorValidateAllTransactions) {
    Account acc;
    acc.name = "Checking";
    acc.type = AccountType::Checking;
    acc.balance = 100.0;
    data_store.add_account(acc);
    std::string acc_id = data_store.accounts[0].id;

    for (int i = 0; i < 10; ++i) {
        Transaction tx;
        tx.account_id = acc_id;
        tx.date = "2026-05-01";
        tx.amount = 10.0;
        tx.description = "Test " + std::to_string(i);
        data_store.add_transaction(tx);
    }

    auto results = Validator::validateAllTransactions(data_store.transactions, data_store.accounts);
    ASSERT_EQ(results.size(), data_store.transactions.size());

    for (const auto& r : results) {
        ASSERT_TRUE(r.is_valid);
    }
}

TEST_F(DataStoreTest, ConsolidationServiceHashComputation) {
    auto& consolidation = ConsolidationService::instance();

    PlaidTransaction pt;
    pt.transaction_id = "tx123";
    pt.amount = 50.0;
    pt.date = "2026-05-01";

    std::string hash = consolidation.compute_transaction_hash(pt);
    ASSERT_FALSE(hash.empty());

    std::string hash2 = consolidation.compute_transaction_hash(pt);
    ASSERT_EQ(hash, hash2);
}

TEST_F(DataStoreTest, ConsolidationServiceNormalizeTransactions) {
    auto& consolidation = ConsolidationService::instance();

    std::vector<PlaidTransaction> plaid_txs = {
        {"tx1", "acc1", "2026-05-01", 50.0, "Test Purchase", "Food", false},
        {"tx2", "acc1", "2026-05-02", 100.0, "Amazon", "Shopping", false}
    };

    auto txs = consolidation.normalize_transactions(plaid_txs, "acc_test");
    ASSERT_EQ(txs.size(), 2);
    ASSERT_EQ(txs[0].amount, -50.0);
    ASSERT_EQ(txs[0].account_id, "acc_test");
    ASSERT_FALSE(txs[0].plaid_transaction_id.empty());
}

TEST_F(DataStoreTest, ConsolidationServiceMergeTransactions) {
    auto& consolidation = ConsolidationService::instance();

    std::vector<Transaction> existing;
    Transaction tx;
    tx.id = "tx1";
    tx.plaid_transaction_id = "plaid_tx1";
    tx.amount = -50.0;
    tx.date = "2026-05-01";
    existing.push_back(tx);

    std::vector<Transaction> incoming;
    Transaction tx2;
    tx2.plaid_transaction_id = "plaid_tx1";
    tx2.amount = -50.0;
    tx2.date = "2026-05-01";
    tx2.notes = "User note";
    incoming.push_back(tx2);

    Transaction tx3;
    tx3.plaid_transaction_id = "plaid_tx_new";
    tx3.amount = -75.0;
    tx3.date = "2026-05-02";
    incoming.push_back(tx3);

    auto result = consolidation.merge_transactions(existing, incoming);
    ASSERT_EQ(result.duplicate_transactions, 1);
    ASSERT_EQ(result.new_transactions, 1);
    ASSERT_EQ(result.updated_transactions, 0);
    ASSERT_EQ(existing.size(), 2);
}

TEST_F(DataStoreTest, ConsolidationServiceMergeAccounts) {
    auto& consolidation = ConsolidationService::instance();

    std::vector<PlaidAccount> plaid_accs = {
        {"acc123", "Checking", "depository", "checking", 1000.0},
        {"acc456", "Credit Card", "credit", "credit card", -500.0}
    };

    auto accounts = consolidation.merge_accounts({}, plaid_accs, "entity1");
    ASSERT_EQ(accounts.size(), 2);
}

TEST_F(DataStoreTest, ConsolidationServiceCalculateLiquidity) {
    auto& consolidation = ConsolidationService::instance();

    std::vector<Account> accounts = {
        Account{"acc1", "Checking", "", AccountType::Checking, 1000.0, "", ""},
        Account{"acc2", "Savings", "", AccountType::Savings, 5000.0, "", ""},
        Account{"acc3", "Credit", "", AccountType::CreditCard, -500.0, "", ""}
    };

    auto liq = consolidation.calculate_liquidity(accounts);
    ASSERT_EQ(liq.checking, 1000.0);
    ASSERT_EQ(liq.savings, 5000.0);
    ASSERT_EQ(liq.credit, -500.0);
}

TEST_F(DataStoreTest, ConfigManagerSingleton) {
    auto& config = ConfigManager::instance();
    config.load_env_file();
    auto client_id = config.get_plaid_client_id();
    auto secret = config.get_plaid_secret();
    bool has_creds = config.has_plaid_credentials();
    ASSERT_TRUE(has_creds || (!client_id.has_value() && !secret.has_value()));
}

// ---------------------------------------------------------------------------
// PlaidService tests — v0.2 account_id-based interface.
// The TUI never sees a Plaid access_token; all operations are account_id-keyed
// and proxied through the Greylock server (Q1=A).
// ---------------------------------------------------------------------------

TEST_F(DataStoreTest, PlaidServiceStubCreation) {
    auto service = create_plaid_service(true);
    ASSERT_TRUE(service != nullptr);
    ASSERT_TRUE(service->is_stub());
}

TEST_F(DataStoreTest, PlaidServiceStubLinkAccount) {
    // StubPlaidService::link_account succeeds trivially.
    auto service = create_plaid_service(true);
    ASSERT_TRUE(service->link_account("acc_123", "public-sandbox-token"));
}

TEST_F(DataStoreTest, PlaidServiceStubGetTransactionsReturnsEmpty) {
    // StubPlaidService returns empty transaction list (no network).
    auto service = create_plaid_service(true);
    auto txs = service->get_transactions("acc_123", "2026-01-01", "2026-05-01");
    ASSERT_TRUE(txs.empty());
}

TEST_F(DataStoreTest, PlaidServiceStubGetAccountsReturnsEmpty) {
    // StubPlaidService returns empty account list (no network).
    auto service = create_plaid_service(true);
    auto accounts = service->get_accounts("acc_123");
    ASSERT_TRUE(accounts.empty());
}

TEST_F(DataStoreTest, PlaidServiceStubUnlinkAccount) {
    auto service = create_plaid_service(true);
    ASSERT_TRUE(service->unlink_account("acc_123"));
}

TEST_F(DataStoreTest, HighVolumeMergeStressTest) {
    auto& consolidation = ConsolidationService::instance();

    std::vector<Transaction> existing;
    std::vector<Transaction> incoming;

    for (int i = 0; i < 500; ++i) {
        Transaction tx;
        tx.plaid_transaction_id = "plaid_" + std::to_string(i);
        tx.amount = -static_cast<double>(i + 1);
        tx.date = "2026-05-01";
        incoming.push_back(tx);

        if (i % 2 == 0) {
            Transaction dup = tx;
            existing.push_back(dup);
        }
    }

    auto result = consolidation.merge_transactions(existing, incoming);
    EXPECT_GE(result.new_transactions, 250);
    EXPECT_EQ(result.duplicate_transactions, 250);
}

TEST_F(DataStoreTest, ConsolidationServiceCategoryMapping) {
    auto& consolidation = ConsolidationService::instance();

    std::vector<PlaidTransaction> plaid_txs = {
        {"tx1", "acc1", "2026-05-01", 50.0, "Uber Eats", "Food and Drink", false},
        {"tx2", "acc1", "2026-05-02", 200.0, "Shell Gas", "Travel", false},
        {"tx3", "acc1", "2026-05-03", 3000.0, "ACME Corp", "Deposit", false},
        {"tx4", "acc1", "2026-05-04", 15.0, "Netflix", "Entertainment", false}
    };

    auto txs = consolidation.normalize_transactions(plaid_txs, "acc_test");
    ASSERT_EQ(txs.size(), 4);
    ASSERT_EQ(txs[0].category_id, "cat_food");
    ASSERT_EQ(txs[1].category_id, "cat_transport");
    ASSERT_EQ(txs[2].category_id, "cat_salary");
    ASSERT_EQ(txs[3].category_id, "cat_entertainment");
}

TEST_F(DataStoreTest, ConfigManagerStoragePath) {
    auto& config = ConfigManager::instance();
    ASSERT_FALSE(config.get_storage_path().empty());

    config.set_storage_path("test_data.json");
    ASSERT_EQ(config.get_storage_path(), "test_data.json");
}

TEST_F(DataStoreTest, ConfigManagerPlaidEnvironment) {
    auto& config = ConfigManager::instance();
    auto env = config.get_plaid_environment();
    if (env.has_value()) {
        bool valid = (*env == "sandbox" || *env == "development" || *env == "production");
        ASSERT_TRUE(valid);
    }
}

TEST_F(DataStoreTest, PlaidServiceStubTimeoutDoesNotCrash) {
    // set_timeout on stub must not crash.
    auto service = create_plaid_service(true);
    service->set_timeout(std::chrono::seconds{60});
    ASSERT_TRUE(service->is_stub());
}

TEST_F(DataStoreTest, HighVolumeStressTest) {
    Account acc;
    acc.name = "Stress Account";
    acc.type = AccountType::Checking;
    acc.balance = 1000000.0;
    data_store.add_account(acc);
    std::string acc_id = data_store.accounts[0].id;

    for (int i = 0; i < 1000; ++i) {
        Transaction tx;
        tx.id = "stress_" + std::to_string(i);
        tx.account_id = acc_id;
        tx.date = "2026-05-01";
        tx.amount = -static_cast<double>(i + 1) * 1.5;
        tx.description = "Stress transaction " + std::to_string(i);
        tx.category_id = "cat_other_expense";
        data_store.add_transaction(tx);
    }

    ASSERT_EQ(data_store.transactions.size(), 1000);

    auto accounts_for_type = data_store.get_accounts_by_type(AccountType::Checking);
    ASSERT_FALSE(accounts_for_type.empty());

    auto spending = data_store.get_spending_for_month("2026-05");
    ASSERT_GT(spending, 0);
}

TEST_F(DataStoreTest, ConsolidationServiceRoundTripEmptyMerge) {
    auto& consolidation = ConsolidationService::instance();

    std::vector<Transaction> existing;
    std::vector<Transaction> incoming;

    auto result = consolidation.merge_transactions(existing, incoming);
    EXPECT_EQ(result.new_transactions, 0);
    EXPECT_EQ(result.duplicate_transactions, 0);
    EXPECT_EQ(result.updated_transactions, 0);
    EXPECT_TRUE(existing.empty());
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}