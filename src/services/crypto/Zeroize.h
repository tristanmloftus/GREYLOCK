#pragma once

// Zeroize.h — RAII zeroization wrappers for sensitive plaintext buffers.
//
// Design:
//   ZeroizingBuffer wraps std::vector<std::byte> and calls sodium_memzero
//   in its destructor before the vector's memory is freed. This guarantees
//   that key material or decrypted secrets are scrubbed even when exceptions
//   unwind the stack.
//
//   withZeroized<T> is a scope-guard helper that allocates a buffer, calls a
//   user-provided callable with that buffer, and then zeroes the buffer on
//   scope exit — mirroring the TypeScript `withDecryptedToken` pattern.
//
// GUARDRAIL F-3: sodium_init() must have been called before any code that
// uses sodium_memzero reaches production paths. Callers are responsible for
// calling sodium_init() at startup (typically in main()). These helpers do
// NOT call sodium_init() themselves to avoid initialization races.

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include <sodium.h>

namespace tf::crypto {

// ---------------------------------------------------------------------------
// ZeroizingBuffer
//
// A move-only wrapper around std::vector<std::byte> whose destructor calls
// sodium_memzero before deallocation so key material does not linger in
// freed heap pages.
//
// Move semantics: the moved-from state is explicitly made empty so the
// destructor's sodium_memzero call is a no-op (zero bytes to zero).
// ---------------------------------------------------------------------------
class ZeroizingBuffer {
public:
    ZeroizingBuffer() = default;

    explicit ZeroizingBuffer(std::size_t size)
        : data_(size) {}

    // Construct from a span (copies bytes in).
    explicit ZeroizingBuffer(std::span<const std::byte> src)
        : data_(src.begin(), src.end()) {}

    // Non-copyable — copying sensitive buffers widens the attack surface.
    ZeroizingBuffer(const ZeroizingBuffer&) = delete;
    ZeroizingBuffer& operator=(const ZeroizingBuffer&) = delete;

    // Move-construct: take ownership, leave moved-from empty.
    ZeroizingBuffer(ZeroizingBuffer&& other) noexcept
        : data_(std::move(other.data_)) {
        // other.data_ is now empty (vector move guarantees this); its
        // destructor will call sodium_memzero on 0 bytes — safe no-op.
    }

    // Move-assign: zeroize our current contents, then take ownership.
    ZeroizingBuffer& operator=(ZeroizingBuffer&& other) noexcept {
        if (this != &other) {
            if (!data_.empty()) {
                sodium_memzero(data_.data(), data_.size());
            }
            data_ = std::move(other.data_);
        }
        return *this;
    }

    ~ZeroizingBuffer() {
        if (!data_.empty()) {
            sodium_memzero(data_.data(), data_.size());
        }
        // vector destructor frees the (now zeroed) memory.
    }

    // Accessors
    std::byte*       data()       noexcept { return data_.data(); }
    const std::byte* data() const noexcept { return data_.data(); }
    std::size_t      size() const noexcept { return data_.size(); }
    bool             empty() const noexcept { return data_.empty(); }

    std::span<std::byte>       span()       noexcept { return data_; }
    std::span<const std::byte> span() const noexcept { return data_; }

    // Resize — new bytes are value-initialized to 0x00 by vector.
    void resize(std::size_t n) { data_.resize(n); }

    // Underlying vector (use sparingly; prefer span()).
    std::vector<std::byte>&       vec()       noexcept { return data_; }
    const std::vector<std::byte>& vec() const noexcept { return data_; }

private:
    std::vector<std::byte> data_;
};

// ---------------------------------------------------------------------------
// withZeroized<T>
//
// Allocates a ZeroizingBuffer of `size` bytes, passes it to `f`, returns
// f's result, and guarantees the buffer is zeroed when the scope exits
// (whether normally or via exception).
//
// Usage:
//   auto result = tf::crypto::withZeroized<SomeResult>(32, [&](ZeroizingBuffer& buf) {
//       // populate buf with sensitive bytes ...
//       return doSomethingWith(buf.span());
//   });
//   // buf is already zeroed here.
//
// The callable receives a non-const reference so it can populate the buffer.
// If `f` throws, the ZeroizingBuffer destructor still fires — guaranteed by
// the C++ standard for stack-allocated objects.
// ---------------------------------------------------------------------------
template <typename T = void, typename F>
auto withZeroized(std::size_t size, F&& f) -> std::invoke_result_t<F, ZeroizingBuffer&> {
    ZeroizingBuffer buf(size);
    return std::forward<F>(f)(buf);
}

} // namespace tf::crypto
