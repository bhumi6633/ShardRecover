#include "shardrecover/thread_pool.hpp"

#include <atomic>
#include <cstddef>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void test_basic_execution()
{
    shardrecover::ThreadPool pool(4);
    auto integer = pool.submit([] { return 42; });
    auto text = pool.submit([] { return std::string("complete"); });
    check(integer.get() == 42, "integer task returned the wrong value");
    check(text.get() == "complete", "string task returned the wrong value");
    check(pool.worker_count() == 4, "pool reported the wrong worker count");
    check(shardrecover::ThreadPool::available_hardware_threads() >= 1,
          "hardware thread helper returned zero");
}

void test_many_tasks_execute_once()
{
    shardrecover::ThreadPool pool(3);
    std::vector<std::atomic<unsigned int>> executions(200);
    std::vector<std::future<void>> futures;
    futures.reserve(executions.size());
    for (std::size_t index = 0; index < executions.size(); ++index) {
        futures.push_back(pool.submit([&, index] { ++executions[index]; }));
    }
    for (auto& future : futures) {
        future.get();
    }
    for (const auto& count : executions) {
        check(count.load() == 1, "a queued task did not execute exactly once");
    }
}

void test_one_worker_and_exceptions()
{
    shardrecover::ThreadPool pool(1);
    auto failure = pool.submit([]() -> int { throw std::runtime_error("task failure"); });
    bool observed = false;
    try {
        static_cast<void>(failure.get());
    } catch (const std::runtime_error& error) {
        observed = std::string_view(error.what()) == "task failure";
    }
    check(observed, "task exception did not propagate through its future");
    check(pool.submit([] { return 7; }).get() == 7,
          "worker did not continue after a task exception");
}

void test_invalid_worker_count()
{
    bool rejected = false;
    try {
        const shardrecover::ThreadPool pool(0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "zero-worker pool was accepted");
}

void test_destruction_drains_queue()
{
    std::atomic<unsigned int> completed{0};
    std::vector<std::future<void>> futures;
    {
        shardrecover::ThreadPool pool(2);
        for (int index = 0; index < 100; ++index) {
            futures.push_back(pool.submit([&completed] { ++completed; }));
        }
    }
    for (auto& future : futures) {
        future.get();
    }
    check(completed.load() == futures.size(),
          "pool destruction abandoned queued work");
}

}  // namespace

int main()
{
    try {
        test_basic_execution();
        test_many_tasks_execute_once();
        test_one_worker_and_exceptions();
        test_invalid_worker_count();
        test_destruction_drains_queue();
        std::cout << "All thread pool tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Thread pool test failure: " << error.what() << '\n';
        return 1;
    }
}
