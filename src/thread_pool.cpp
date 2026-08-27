#include "shardrecover/thread_pool.hpp"

namespace shardrecover {

ThreadPool::ThreadPool(std::size_t worker_count)
{
    if (worker_count == 0) {
        throw std::invalid_argument("Thread pool worker count must be greater than zero");
    }

    workers_.reserve(worker_count);
    try {
        for (std::size_t index = 0; index < worker_count; ++index) {
            workers_.emplace_back([this] { run_worker(); });
        }
    } catch (...) {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        work_available_.notify_all();
        for (auto& worker : workers_) {
            worker.join();
        }
        throw;
    }
}

ThreadPool::~ThreadPool()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    work_available_.notify_all();
    for (auto& worker : workers_) {
        worker.join();
    }
}

std::size_t ThreadPool::worker_count() const noexcept
{
    return workers_.size();
}

std::size_t ThreadPool::available_hardware_threads() noexcept
{
    const auto reported = std::thread::hardware_concurrency();
    return reported == 0 ? 1 : reported;
}

void ThreadPool::run_worker()
{
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            work_available_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}

}  // namespace shardrecover
