#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace shardrecover {

class ThreadPool {
public:
    explicit ThreadPool(std::size_t worker_count);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    template <typename Function>
    auto submit(Function&& function) -> std::future<std::invoke_result_t<Function>>
    {
        using Result = std::invoke_result_t<Function>;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::forward<Function>(function));
        auto future = task->get_future();
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                throw std::runtime_error("Cannot submit work to a stopping thread pool");
            }
            tasks_.emplace([task] { (*task)(); });
        }
        work_available_.notify_one();
        return future;
    }

    std::size_t worker_count() const noexcept;
    static std::size_t available_hardware_threads() noexcept;

private:
    void run_worker();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable work_available_;
    bool stopping_ = false;
};

}  // namespace shardrecover
