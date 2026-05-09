#pragma once

#include <memory>
#include <stdexcept>

#include "StorageService.h"
#include "PlaidService.h"
#include "ISecretStore.h"
#include "IHttpClient.h"
#include "BackendClient.h"
#include "AuthService.h"

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

    void set_backend_client(std::shared_ptr<BackendClient> service) {
        backend_client_ = service;
    }

    // [3.B EXTENSION: AuthService slot]
    void set_auth_service(std::shared_ptr<AuthService> service) {
        auth_service_ = service;
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

    std::shared_ptr<BackendClient> get_backend_client() {
        return backend_client_;
    }

    std::shared_ptr<AuthService> get_auth_service() {
        return auth_service_;
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

    BackendClient& backend_client() {
        if (!backend_client_) throw std::runtime_error("Backend client not initialized");
        return *backend_client_;
    }

    AuthService& auth_service() {
        if (!auth_service_) throw std::runtime_error("Auth service not initialized");
        return *auth_service_;
    }

    bool has_storage() const { return storage_ != nullptr; }
    bool has_plaid() const { return plaid_ != nullptr; }
    bool has_secret_store() const { return secret_store_ != nullptr; }
    bool has_http_client() const { return http_client_ != nullptr; }
    bool has_backend_client() const { return backend_client_ != nullptr; }
    bool has_auth_service() const { return auth_service_ != nullptr; }

private:
    std::shared_ptr<IStorageService> storage_;
    std::shared_ptr<IPlaidService> plaid_;
    std::shared_ptr<ISecretStore> secret_store_;
    std::shared_ptr<IHttpClient> http_client_;
    std::shared_ptr<BackendClient> backend_client_;
    std::shared_ptr<AuthService> auth_service_;
};
