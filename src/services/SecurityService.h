#pragma once

// SecurityService.h — compatibility shim for v0.2.
//
// v0.1 used a free-function namespace SecurityService (Windows-only DPAPI +
// registry).  v0.2 replaces that with the ISecretStore interface and two
// platform implementations.  This header pulls in ISecretStore and the
// platform-appropriate concrete type so any existing #include "SecurityService.h"
// site continues to compile without changes.
//
// Call sites that used the old SecurityService:: free-functions should be
// migrated to ISecretStore* / std::shared_ptr<ISecretStore> obtained from
// ServiceContainer::get_secret_store().

#include "ISecretStore.h"

#ifdef _WIN32
#include "secret_store/DpapiSecretStore.h"
#elif defined(__APPLE__)
#include "secret_store/KeychainSecretStore.h"
#else
#include "secret_store/FileSecretStore.h"
#endif
