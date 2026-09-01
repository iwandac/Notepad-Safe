#pragma once

#include <functional>
#include <memory>

// Owns one long-lived, dedicated worker without consuming an AsyncThreadPool
// slot. Creation and joining stay inside the async infrastructure boundary.
class DedicatedThread final {
public:
    DedicatedThread();
    ~DedicatedThread();

    DedicatedThread(const DedicatedThread &) = delete;
    DedicatedThread &operator=(const DedicatedThread &) = delete;

    bool start(std::function<void()> worker);
    void join();
    bool joinable() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
