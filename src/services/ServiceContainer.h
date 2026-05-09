#pragma once

#include <memory>
#include <stdexcept>

#include "StorageService.h"
#include "PlaidService.h"

class ServiceContainer {
public:
    ServiceContainer() = default;

    void set_storage(std::shared_ptr<IStorageService> service) {
        storage_ = service;
    }

    void set_plaid(std::shared_ptr<IPlaidService> service) {
        plaid_ = service;
    }

    std::shared_ptr<IStorageService> get_storage() {
        return storage_;
    }

    std::shared_ptr<IPlaidService> get_plaid() {
        return plaid_;
    }

    IStorageService& storage() {
        if (!storage_) throw std::runtime_error("Storage service not initialized");
        return *storage_;
    }

    IPlaidService& plaid() {
        if (!plaid_) throw std::runtime_error("Plaid service not initialized");
        return *plaid_;
    }

    bool has_storage() const { return storage_ != nullptr; }
    bool has_plaid() const { return plaid_ != nullptr; }

private:
    std::shared_ptr<IStorageService> storage_;
    std::shared_ptr<IPlaidService> plaid_;
};