#pragma once

#include <QtGlobal>

#include <cstddef>
#include <vector>

// Process-memory holder for passwords, TOTP seeds and secure-note bodies.
// It is deliberately non-copyable and has no QByteArray/QString conversion.
class SecureBuffer final {
public:
    SecureBuffer() = default;
    SecureBuffer(const void *data, qsizetype size);
    ~SecureBuffer();

    SecureBuffer(const SecureBuffer &) = delete;
    SecureBuffer &operator=(const SecureBuffer &) = delete;
    SecureBuffer(SecureBuffer &&other) noexcept;
    SecureBuffer &operator=(SecureBuffer &&other) noexcept;

    void assign(const void *data, qsizetype size);
    void clear() noexcept;

    bool empty() const noexcept { return bytes_.empty(); }
    qsizetype size() const noexcept { return static_cast<qsizetype>(bytes_.size()); }
    const unsigned char *data() const noexcept { return bytes_.data(); }
    bool equals(const void *data, qsizetype size) const noexcept;
    bool isMemoryLocked() const noexcept { return memoryLocked_; }

private:
    static void secureZero(void *data, size_t size) noexcept;
    std::vector<unsigned char> bytes_;
    bool memoryLocked_ = false;
};
