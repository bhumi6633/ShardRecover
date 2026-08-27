#include "shardrecover/binary_file.hpp"
#include "shardrecover/mapped_file.hpp"
#include "shardrecover/thread_pool.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
                / ("shardrecover-mapped-file-tests-" + std::to_string(stamp));
        if (!std::filesystem::create_directory(path_)) {
            throw std::runtime_error("Failed to create mapped-file test directory");
        }
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, const std::vector<std::byte>& data)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!data.empty()) {
        output.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
    }
    output.close();
    if (!output) {
        throw std::runtime_error("Failed to write mapped-file fixture");
    }
}

void expect_failure(const std::filesystem::path& path)
{
    try {
        const shardrecover::MappedFile file(path);
    } catch (const std::exception& error) {
        check(std::string_view(error.what()).find(path.string()) != std::string_view::npos,
              "mapping failure did not identify its path");
        return;
    }
    throw std::runtime_error("invalid mapped-file path was accepted");
}

void test_contents_empty_and_one_byte(const std::filesystem::path& directory)
{
    const std::vector<std::byte> expected{
        std::byte{0x00}, std::byte{0xff}, std::byte{0x80}, std::byte{0x13}, std::byte{0x7a},
    };
    const auto path = directory / "binary.bin";
    write_file(path, expected);
    const shardrecover::MappedFile mapped(path);
    check(mapped.path() == path && mapped.size() == expected.size() && !mapped.empty(),
          "mapped-file metadata was incorrect");
    check(std::equal(mapped.bytes().begin(), mapped.bytes().end(), expected.begin()),
          "mapped-file bytes differed from the source");
    const auto buffered = shardrecover::BinaryFile::load(path);
    check(std::equal(mapped.bytes().begin(), mapped.bytes().end(), buffered.bytes().begin()),
          "buffered and mapped contents differed");

    const auto empty_path = directory / "empty.bin";
    write_file(empty_path, {});
    const shardrecover::MappedFile empty(empty_path);
    check(empty.empty() && empty.size() == 0 && empty.bytes().empty(),
          "empty mapping was not represented safely");

    const auto one_path = directory / "one.bin";
    write_file(one_path, {std::byte{0xa5}});
    const shardrecover::MappedFile one(one_path);
    check(one.size() == 1 && one.bytes().front() == std::byte{0xa5},
          "one-byte mapping was incorrect");
}

void test_errors_and_moves(const std::filesystem::path& directory)
{
    expect_failure(directory / "missing.bin");
    const auto subdirectory = directory / "subdirectory";
    std::filesystem::create_directory(subdirectory);
    expect_failure(subdirectory);

    const auto first_path = directory / "move-first.bin";
    const auto second_path = directory / "move-second.bin";
    write_file(first_path, {std::byte{1}, std::byte{2}});
    write_file(second_path, {std::byte{3}, std::byte{4}, std::byte{5}});
    shardrecover::MappedFile first(first_path);
    shardrecover::MappedFile moved(std::move(first));
    check(first.empty() && moved.size() == 2 && moved.bytes()[1] == std::byte{2},
          "move construction did not transfer mapping ownership");
    shardrecover::MappedFile assigned(second_path);
    assigned = std::move(moved);
    check(moved.empty() && assigned.path() == first_path && assigned.size() == 2,
          "move assignment did not transfer mapping ownership");
}

void test_repetition_large_file_and_parallel_reads(const std::filesystem::path& directory)
{
    std::vector<std::byte> data(1024 * 1024);
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index] = static_cast<std::byte>(index % 251);
    }
    const auto path = directory / "moderate.bin";
    write_file(path, data);
    for (int repetition = 0; repetition < 50; ++repetition) {
        const shardrecover::MappedFile mapped(path);
        check(mapped.size() == data.size() && mapped.bytes().front() == data.front()
                  && mapped.bytes().back() == data.back(),
              "repeated mapping exposed incorrect boundary bytes");
    }

    const shardrecover::MappedFile mapped(path);
    shardrecover::ThreadPool pool(4);
    std::vector<std::future<std::size_t>> futures;
    for (int task = 0; task < 16; ++task) {
        futures.push_back(pool.submit([view = mapped.bytes()] {
            std::size_t checksum = 0;
            for (const auto value : view) {
                checksum += std::to_integer<unsigned int>(value);
            }
            return checksum;
        }));
    }
    const auto expected = futures.front().get();
    for (std::size_t index = 1; index < futures.size(); ++index) {
        check(futures[index].get() == expected,
              "concurrent mapped reads produced different checksums");
    }
}

}  // namespace

int main()
{
    try {
        const TemporaryDirectory directory;
        test_contents_empty_and_one_byte(directory.path());
        test_errors_and_moves(directory.path());
        test_repetition_large_file_and_parallel_reads(directory.path());
        std::cout << "All mapped file tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Mapped file test failure: " << error.what() << '\n';
        return 1;
    }
}
