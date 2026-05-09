#pragma once

#include <memory>
#include <stdexcept>

#include "StorageService.h"
#include "PlaidService.h"
#include "ISecretStore.h"
#include "IHttpClient.h"

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

    void set_http_client(std::shared_ptr<IHttpClient> service) {
        http_client_ = service;
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

    std::shared_ptr<IHttpClient> get_http_client() {
        return http_client_;
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

    IHttpClient& http_client() {
        if (!http_client_) throw std::runtime_error("HTTP client not initialized");
        return *http_client_;
    }

    bool has_storage() const { return storage_ != nullptr; }
    bool has_plaid() const { return plaid_ != nullptr; }
    bool has_secret_store() const { return secret_store_ != nullptr; }
    bool has_http_client() const { return http_client_ != nullptr; }

private:
    std::shared_ptr<IStorageService> storage_;
    std::shared_ptr<IPlaidService> plaid_;
    std::shared_ptr<ISecretStore> secret_store_;
    std::shared_ptr<IHttpClient> http_client_;
};
