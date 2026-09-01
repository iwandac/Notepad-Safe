#include "DedicatedThread.h"

#include <thread>
#include <utility>

struct DedicatedThread::Impl {
    std::thread thread;
};

DedicatedThread::DedicatedThread()
    : impl_(std::make_unique<Impl>())
{
}

DedicatedThread::~DedicatedThread()
{
    join();
}

bool DedicatedThread::start(std::function<void()> worker)
{
    if (!worker || impl_->thread.joinable())
        return false;
    try {
        impl_->thread = std::thread([worker = std::move(worker)]() mutable {
            try {
                worker();
            } catch (...) {
                // Infrastructure workers must always reach a joinable terminal
                // state. The owner receives explicit startup status separately.
            }
        });
    } catch (...) {
        return false;
    }
    return true;
}

void DedicatedThread::join()
{
    if (impl_->thread.joinable())
        impl_->thread.join();
}

bool DedicatedThread::joinable() const
{
    return impl_->thread.joinable();
}
