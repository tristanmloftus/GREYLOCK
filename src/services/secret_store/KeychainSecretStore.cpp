// Apple-only. Only compiled when __APPLE__ is defined (CMake enforces this).
#ifdef __APPLE__

#include "KeychainSecretStore.h"
#include "../../utils/Logger.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

// Helper: convert a std::string_view to a CFStringRef (caller must CFRelease).
static CFStringRef make_cfstring(std::string_view sv) {
    return CFStringCreateWithBytes(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(sv.data()),
        static_cast<CFIndex>(sv.size()),
        kCFStringEncodingUTF8,
        false);
}

bool KeychainSecretStore::put(std::string_view key, std::span<const std::byte> value) {
    // Attempt to delete any existing item first so we can do a clean add.
    // (SecItemUpdate is more correct but add-after-delete is simpler and reliable
    // for a CLI tool that does not run under a code-signed bundle.)
    remove(key);

    CFStringRef service_ref = make_cfstring(kServiceName);
    CFStringRef account_ref = make_cfstring(key);
    CFDataRef data_ref = CFDataCreate(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(value.data()),
        static_cast<CFIndex>(value.size()));

    if (!service_ref || !account_ref || !data_ref) {
        if (service_ref) CFRelease(service_ref);
        if (account_ref) CFRelease(account_ref);
        if (data_ref) CFRelease(data_ref);
        Logger::instance().error("KeychainSecretStore: CFString/CFData allocation failed");
        return false;
    }

    const void* keys[] = {
        kSecClass,
        kSecAttrService,
        kSecAttrAccount,
        kSecValueData,
        kSecAttrAccessible,
    };
    const void* values[] = {
        kSecClassGenericPassword,
        service_ref,
        account_ref,
        data_ref,
        kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
    };

    CFDictionaryRef query = CFDictionaryCreate(kCFAllocatorDefault,
        keys, values, 5,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    OSStatus status = SecItemAdd(query, NULL);

    CFRelease(query);
    CFRelease(data_ref);
    CFRelease(account_ref);
    CFRelease(service_ref);

    if (status != errSecSuccess) {
        Logger::instance().error("KeychainSecretStore: SecItemAdd failed, OSStatus=" +
            std::to_string(static_cast<int>(status)));
        return false;
    }

    Logger::instance().info("KeychainSecretStore: Stored secret for key: " + std::string(key));
    return true;
}

std::optional<std::vector<std::byte>> KeychainSecretStore::get(std::string_view key) {
    CFStringRef service_ref = make_cfstring(kServiceName);
    CFStringRef account_ref = make_cfstring(key);

    if (!service_ref || !account_ref) {
        if (service_ref) CFRelease(service_ref);
        if (account_ref) CFRelease(account_ref);
        Logger::instance().error("KeychainSecretStore: CFString allocation failed");
        return std::nullopt;
    }

    const void* keys[] = {
        kSecClass,
        kSecAttrService,
        kSecAttrAccount,
        kSecMatchLimit,
        kSecReturnData,
    };
    const void* values[] = {
        kSecClassGenericPassword,
        service_ref,
        account_ref,
        kSecMatchLimitOne,
        kCFBooleanTrue,
    };

    CFDictionaryRef query = CFDictionaryCreate(kCFAllocatorDefault,
        keys, values, 5,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    CFDataRef result_data = NULL;
    OSStatus status = SecItemCopyMatching(query,
        reinterpret_cast<CFTypeRef*>(&result_data));

    CFRelease(query);
    CFRelease(account_ref);
    CFRelease(service_ref);

    if (status == errSecItemNotFound) {
        return std::nullopt;
    }
    if (status != errSecSuccess || result_data == NULL) {
        Logger::instance().error("KeychainSecretStore: SecItemCopyMatching failed, OSStatus=" +
            std::to_string(static_cast<int>(status)));
        return std::nullopt;
    }

    CFIndex len = CFDataGetLength(result_data);
    const UInt8* ptr = CFDataGetBytePtr(result_data);

    std::vector<std::byte> result(static_cast<size_t>(len));
    std::memcpy(result.data(), ptr, static_cast<size_t>(len));
    CFRelease(result_data);

    Logger::instance().debug("KeychainSecretStore: Retrieved secret for key: " + std::string(key));
    return result;
}

bool KeychainSecretStore::remove(std::string_view key) {
    CFStringRef service_ref = make_cfstring(kServiceName);
    CFStringRef account_ref = make_cfstring(key);

    if (!service_ref || !account_ref) {
        if (service_ref) CFRelease(service_ref);
        if (account_ref) CFRelease(account_ref);
        Logger::instance().error("KeychainSecretStore: CFString allocation failed");
        return false;
    }

    const void* keys[] = {
        kSecClass,
        kSecAttrService,
        kSecAttrAccount,
    };
    const void* values[] = {
        kSecClassGenericPassword,
        service_ref,
        account_ref,
    };

    CFDictionaryRef query = CFDictionaryCreate(kCFAllocatorDefault,
        keys, values, 3,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    OSStatus status = SecItemDelete(query);

    CFRelease(query);
    CFRelease(account_ref);
    CFRelease(service_ref);

    // errSecItemNotFound is acceptable — key was already absent.
    if (status != errSecSuccess && status != errSecItemNotFound) {
        Logger::instance().error("KeychainSecretStore: SecItemDelete failed, OSStatus=" +
            std::to_string(static_cast<int>(status)));
        return false;
    }

    Logger::instance().info("KeychainSecretStore: Removed secret for key: " + std::string(key));
    return true;
}

#endif // __APPLE__
