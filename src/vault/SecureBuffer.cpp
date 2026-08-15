#include "SecureBuffer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

#ifdef Q_OS_WIN
#  include <Windows.h>
#endif

SecureBuffer::SecureBuffer(const void *data, qsizetype size)
{
    assign(data, size);
}

SecureBuffer::~SecureBuffer()
{
    clear();
}

SecureBuffer::SecureBuffer(SecureBuffer &&other) noexcept
    : bytes_(std::move(other.bytes_))
    , memoryLocked_(other.memoryLocked_)
{
    other.bytes_.clear();
    other.memoryLocked_ = false;
}

SecureBuffer &SecureBuffer::operator=(SecureBuffer &&other) noexcept
{
    if (this != &other) {
        clear();
        bytes_ = std::move(other.bytes_);
        memoryLocked_ = other.memoryLocked_;
        other.bytes_.clear();
        other.memoryLocked_ = false;
    }
    return *this;
}

void SecureBuffer::assign(const void *data, qsizetype size)
{
    if (size < 0 || (size > 0 && data == nullptr))
        throw std::invalid_argument("SecureBuffer received invalid input");

    clear();
    if (size == 0)
        return;
    const auto *first = static_cast<const unsigned char *>(data);
    bytes_.assign(first, first + size);
#ifdef Q_OS_WIN
    memoryLocked_ = ::VirtualLock(bytes_.data(), bytes_.size()) != FALSE;
#endif
}

void SecureBuffer::clear() noexcept
{
    if (!bytes_.empty())
        secureZero(bytes_.data(), bytes_.size());
#ifdef Q_OS_WIN
    if (memoryLocked_ && !bytes_.empty())
        ::VirtualUnlock(bytes_.data(), bytes_.size());
#endif
    memoryLocked_ = false;
    bytes_.clear();
}

bool SecureBuffer::equals(const void *data, qsizetype size) const noexcept
{
    if (size < 0 || size != this->size() || (size > 0 && data == nullptr))
        return false;
    const auto *other = static_cast<const unsigned char *>(data);
    unsigned char difference = 0;
    for (qsizetype i = 0; i < size; ++i)
        difference |= static_cast<unsigned char>(bytes_[static_cast<size_t>(i)] ^ other[i]);
    return difference == 0;
}

void SecureBuffer::secureZero(void *data, size_t size) noexcept
{
#ifdef Q_OS_WIN
    ::SecureZeroMemory(data, size);
#else
    volatile unsigned char *cursor = static_cast<volatile unsigned char *>(data);
    while (size-- > 0)
        *cursor++ = 0;
#endif
}
