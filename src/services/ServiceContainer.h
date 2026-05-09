#pragma once

#include <memory>
#include <stdexcept>

#include "StorageService.h"
#include "PlaidService.h"
#include "ISecretStore.h"

class ServiceContainer {
public:
    ServiceContainer() = default;

    void set_storage(std::shared_ptr<IStorageService> service) {
        storage_ = service;
    }

    void set_plaid(std::shared_ptr<IPlaidService> service) {
        plaid_ = service;
    }

    void set_secret_store(std::shared_ptr<ISecretStore> service) {
        secret_store_ = service;
    }

    std::shared_ptr<IStorageService> get_storage() {
        return storage_;
    }

    std::shared_ptr<IPlaidService> get_plaid() {
        return plaid_;
    }

    std::shared_ptr<ISecretStore> get_secret_store() {
        return secret_store_;
    }

    IStorageService& storage() {
        if (!storage_) throw std::runtime_error("Storage service not initialized");
        return *storage_;
    }

    IPlaidService& plaid() {
        if (!plaid_) throw std::runtime_error("Plaid service not initialized");
        return *plaid_;
    }

    ISecretStore& secret_store() {
        if (!secret_store_) throw std::runtime_error("Secret store not initialized");
        return *secret_store_;
    }

    bool has_storage() const { return storage_ != nullptr; }
    bool has_plaid() const { return plaid_ != nullptr; }
    bool has_secret_store() const { return secret_store_ != nullptr; }

private:
    std::shared_ptr<IStorageService> storage_;
    std::shared_ptr<IPlaidService> plaid_;
    std::shared_ptr<ISecretStore> secret_store_;
};
