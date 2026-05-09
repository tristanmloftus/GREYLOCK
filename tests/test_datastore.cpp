#include <gtest/gtest.h>
#include "../src/models/DataStore.h"
#include "../src/models/Account.h"
#include "../src/models/Transaction.h"
#include "../src/models/Entity.h"
#include "../src/services/DiscoveryService.h"
#include "../src/services/PlaidService.h"
#include "../src/services/ConsolidationService.h"
#include "../src/services/SecurityService.h"
#include "../src/utils/Validator.h"
#include "../src/utils/SyntheticGenerator.h"
#include "../src/utils/ConfigManager.h"

#include <sstream>
#include <iomanip>
#include <algorithm>
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

TEST_F(DataStoreTest, DiscoveryServiceMapToSupplier) {
    auto& discovery = DiscoveryService::instance();

    auto ticker1 = discovery.mapToSupplier("AMAZON WEB SERVICES");
    ASSERT_TRUE(ticker1.has_value());
    EXPECT_EQ(*ticker1, "AMZN");

    auto ticker2 = discovery.mapToSupplier("Google Cloud Platform invoice");
    ASSERT_TRUE(ticker2.has_value());
    EXPECT_EQ(*ticker2, "GOOGL");

    auto ticker3 = discovery.mapToSupplier("Microsoft Azure subscription");
    ASSERT_TRUE(ticker3.has_value());
    EXPECT_EQ(*ticker3, "MSFT");

    auto ticker4 = discovery.mapToSupplier("Netflix monthly charge");
    ASSERT_TRUE(ticker4.has_value());
    EXPECT_EQ(*ticker4, "NFLX");

    auto no_match = discovery.mapToSupplier("Random unknown merchant xyz");
    ASSERT_FALSE(no_match.has_value());
}

TEST_F(DataStoreTest, DiscoveryServiceCalculateVelocity) {
    auto& discovery = DiscoveryService::instance();

    Account acc;
    acc.name = "Test";
    acc.type = AccountType::Checking;
    acc.balance = 1000.0;
    data_store.add_account(acc);
    std::string acc_id = data_store.accounts[0].id;

    Transaction tx1;
    tx1.account_id = acc_id;
    tx1.date = "2026-04-15";
    tx1.amount = -50.0;
    tx1.description = "Food";
    tx1.category_id = "cat_food";
    data_store.add_transaction(tx1);

    Transaction tx2;
    tx2.account_id = acc_id;
    tx2.date = "2026-05-15";
    tx2.amount = -100.0;
    tx2.description = "More Food";
    tx2.category_id = "cat_food";
    data_store.add_transaction(tx2);

    auto velocity = discovery.calculateVelocity(data_store.transactions, data_store.categories);
    ASSERT_FALSE(velocity.empty());

    auto food_vel = std::find_if(velocity.begin(), velocity.end(),
        [](const VelocityResult& v) { return v.category_id == "cat_food"; });
    ASSERT_NE(food_vel, velocity.end());
    EXPECT_EQ(food_vel->previous_month_spend, 50.0);
    EXPECT_EQ(food_vel->current_month_spend, 100.0);
}

TEST_F(DataStoreTest, DiscoveryServiceEmptyDescription) {
    auto& discovery = DiscoveryService::instance();
    auto result = discovery.mapToSupplier("");
    ASSERT_FALSE(result.has_value());
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
        Account{"acc1", "Checking", "", AccountType::Checking, 1000.0, "", "", "", true},
        Account{"acc2", "Savings", "", AccountType::Savings, 5000.0, "", "", "", true},
        Account{"acc3", "Credit", "", AccountType::CreditCard, -500.0, "", "", "", true}
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

TEST_F(DataStoreTest, PlaidServiceStubCreation) {
    auto service = create_plaid_service(true);
    ASSERT_TRUE(service != nullptr);
    ASSERT_TRUE(service->is_stub());
    ASSERT_TRUE(service->initialize("test", "secret"));
}

TEST_F(DataStoreTest, PlaidServiceCreateLinkToken) {
    auto service = create_plaid_service(true);
    service->initialize("client_id", "secret");
    std::string token = service->create_link_token();
    ASSERT_EQ(token, "link-sandbox-xxxxx");
}

TEST_F(DataStoreTest, PlaidServiceExchangePublicToken) {
    auto service = create_plaid_service(true);
    service->initialize("client_id", "secret");
    auto access_token = service->exchange_public_token("public_token");
    ASSERT_TRUE(access_token.has_value());
    ASSERT_EQ(*access_token, "access-sandbox-xxxxx");
}

TEST_F(DataStoreTest, PlaidServiceGetAccounts) {
    auto service = create_plaid_service(true);
    service->initialize("client_id", "secret");
    auto accounts = service->get_accounts("access_token");
    ASSERT_TRUE(accounts.empty());
}

TEST_F(DataStoreTest, PlaidServiceGetTransactions) {
    auto service = create_plaid_service(true);
    service->initialize("client_id", "secret");
    auto txs = service->get_transactions("access_token", "2026-01-01", "2026-05-01");
    ASSERT_TRUE(txs.empty());
}

TEST_F(DataStoreTest, DiscoveryServiceVelocityWithMultipleCategories) {
    auto& discovery = DiscoveryService::instance();

    Account acc;
    acc.name = "Test";
    acc.type = AccountType::Checking;
    acc.balance = 1000.0;
    data_store.add_account(acc);
    std::string acc_id = data_store.accounts[0].id;

    std::vector<Transaction> txs = {
        Transaction{"", acc_id, "2026-04-01", -100.0, "Food", "cat_food", false, "", "", ""},
        Transaction{"", acc_id, "2026-04-15", -50.0, "Gas", "cat_gas", false, "", "", ""},
        Transaction{"", acc_id, "2026-05-01", -200.0, "Food", "cat_food", false, "", "", ""},
        Transaction{"", acc_id, "2026-05-10", -75.0, "Gas", "cat_gas", false, "", "", ""}
    };

    for (auto& tx : txs) {
        tx.id = "tx_" + std::to_string(rand());
        data_store.transactions.push_back(tx);
    }

    auto velocity = discovery.calculateVelocity(data_store.transactions, data_store.categories);
    ASSERT_FALSE(velocity.empty());

    auto food_vel = std::find_if(velocity.begin(), velocity.end(),
        [](const VelocityResult& v) { return v.category_id == "cat_food"; });
    ASSERT_NE(food_vel, velocity.end());
    EXPECT_EQ(food_vel->previous_month_spend, 100.0);
    EXPECT_EQ(food_vel->current_month_spend, 200.0);
    EXPECT_EQ(food_vel->percent_change, 100.0);
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

TEST_F(DataStoreTest, PlaidServiceInitializeWithEnvironment) {
    auto service = create_plaid_service(true);
    ASSERT_TRUE(service->initialize("test", "key", PlaidEnvironment::Development));
    ASSERT_TRUE(service->initialize("test", "key", PlaidEnvironment::Production));
    ASSERT_TRUE(service->initialize("test", "key", PlaidEnvironment::Sandbox));
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

TEST_F(DataStoreTest, DiscoveryServiceDynamicMonthVelocity) {
    auto& discovery = DiscoveryService::instance();

    Account acc;
    acc.name = "Test Dynamic";
    acc.type = AccountType::Checking;
    acc.balance = 1000.0;
    data_store.add_account(acc);
    std::string acc_id = data_store.accounts[0].id;

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    std::ostringstream prev_ss;
    prev_ss << (tm.tm_year + 1900) << "-" << std::setw(2) << std::setfill('0') << (tm.tm_mon);
    std::string prev_month = prev_ss.str();

    std::ostringstream curr_ss;
    curr_ss << (tm.tm_year + 1900) << "-" << std::setw(2) << std::setfill('0') << (tm.tm_mon + 1);
    std::string curr_month = curr_ss.str();

    std::vector<Transaction> txs = {
        Transaction{"", acc_id, prev_month + "-01", -100.0, "Prev", "cat_restaurants", false, "", "", ""},
        Transaction{"", acc_id, curr_month + "-01", -200.0, "Curr", "cat_restaurants", false, "", "", ""}
    };

    for (auto& tx : txs) {
        tx.id = "tx_" + std::to_string(rand());
        data_store.transactions.push_back(tx);
    }

    auto velocity = discovery.calculateVelocity(data_store.transactions, data_store.categories);
    auto cat_vel = std::find_if(velocity.begin(), velocity.end(),
        [](const VelocityResult& v) { return v.category_id == "cat_restaurants"; });

    if (cat_vel != velocity.end()) {
        EXPECT_EQ(cat_vel->current_month_spend, 200.0);
    }
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

TEST_F(DataStoreTest, DiscoveryServiceMoMCalculation) {
    auto& discovery = DiscoveryService::instance();

    Account acc;
    acc.name = "Test MoM";
    acc.type = AccountType::Checking;
    acc.balance = 1000.0;
    data_store.add_account(acc);
    std::string acc_id = data_store.accounts[0].id;

    std::vector<Transaction> txs = {
        Transaction{"", acc_id, "2026-03-01", -50.0, "Mar Food", "cat_food", false, "", "", ""},
        Transaction{"", acc_id, "2026-04-01", -100.0, "Apr Food", "cat_food", false, "", "", ""},
        Transaction{"", acc_id, "2026-04-15", -50.0, "Apr Gas", "cat_gas", false, "", "", ""},
        Transaction{"", acc_id, "2026-05-01", -200.0, "May Food", "cat_food", false, "", "", ""},
        Transaction{"", acc_id, "2026-05-10", -75.0, "May Gas", "cat_gas", false, "", "", ""},
        Transaction{"", acc_id, "2026-05-20", 3000.0, "Salary", "cat_salary", false, "", "", ""}
    };

    for (auto& tx : txs) {
        tx.id = "tx_" + std::to_string(rand());
        data_store.transactions.push_back(tx);
    }

    auto velocity = discovery.calculateVelocity(data_store.transactions, data_store.categories);
    auto food_vel = std::find_if(velocity.begin(), velocity.end(),
        [](const VelocityResult& v) { return v.category_id == "cat_food"; });

    if (food_vel != velocity.end()) {
        EXPECT_EQ(food_vel->current_month_spend, 200.0);
        EXPECT_EQ(food_vel->previous_month_spend, 100.0);
    }

    auto gas_vel = std::find_if(velocity.begin(), velocity.end(),
        [](const VelocityResult& v) { return v.category_id == "cat_gas"; });

    if (gas_vel != velocity.end()) {
        EXPECT_EQ(gas_vel->current_month_spend, 75.0);
        EXPECT_EQ(gas_vel->previous_month_spend, 50.0);
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}