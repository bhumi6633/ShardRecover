#include "shardrecover/binary_file.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
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
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto base = std::filesystem::temp_directory_path()
                          / ("shardrecover-binary-file-tests-" + std::to_string(timestamp));

        for (int suffix = 0; suffix < 100; ++suffix) {
            auto candidate = base;
            candidate += "-" + std::to_string(suffix);

            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = std::move(candidate);
                return;
            }
            if (error) {
                throw std::runtime_error("Failed to create temporary directory: " + error.message());
            }
        }

        throw std::runtime_error("Failed to find an unused temporary directory name");
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write_binary_file(const std::filesystem::path& path, const std::vector<std::byte>& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to create test file: " + path.string());
    }

    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    output.close();

    if (!output) {
        throw std::runtime_error("Failed to write test file: " + path.string());
    }
}

void check_load_fails(const std::filesystem::path& path)
{
    try {
        static_cast<void>(shardrecover::BinaryFile::load(path));
    } catch (const std::exception& error) {
        check(std::string_view(error.what()).find(path.string()) != std::string_view::npos,
              "load failure did not identify the supplied path");
        return;
    }

    throw std::runtime_error("BinaryFile::load unexpectedly succeeded");
}

void test_normal_file(const std::filesystem::path& directory)
{
    const auto path = directory / "normal.bin";
    const std::vector<std::byte> expected{
        std::byte{0x00}, std::byte{0x01}, std::byte{0x7f}, std::byte{0x80},
        std::byte{0xfe}, std::byte{0xff}, std::byte{0x42}, std::byte{0x0a},
    };
    write_binary_file(path, expected);

    const auto file = shardrecover::BinaryFile::load(path);
    check(file.size() == expected.size(), "normal file size was incorrect");
    check(!file.empty(), "normal file was reported as empty");
    check(file.bytes().size() == expected.size(), "byte view size was incorrect");
    check(std::equal(file.bytes().begin(), file.bytes().end(), expected.begin()),
          "normal file bytes were not preserved exactly");
}

void test_empty_file(const std::filesystem::path& directory)
{
    const auto path = directory / "empty.bin";
    write_binary_file(path, {});

    const auto file = shardrecover::BinaryFile::load(path);
    check(file.size() == 0, "empty file size was not zero");
    check(file.empty(), "empty file was not reported as empty");
    check(file.bytes().empty(), "empty file exposed a non-empty byte view");
}

void test_small_file(const std::filesystem::path& directory)
{
    const auto path = directory / "one-byte.bin";
    const std::vector<std::byte> expected{std::byte{0xa5}};
    write_binary_file(path, expected);

    const auto file = shardrecover::BinaryFile::load(path);
    check(file.size() == 1, "one-byte file size was incorrect");
    check(!file.empty(), "one-byte file was reported as empty");
    check(file.bytes().front() == expected.front(), "one-byte file content was incorrect");
}

void test_nonexistent_path(const std::filesystem::path& directory)
{
    check_load_fails(directory / "does-not-exist.bin");
}

void test_directory_path(const std::filesystem::path& directory)
{
    const auto path = directory / "not-a-file";
    std::filesystem::create_directory(path);
    check_load_fails(path);
}

void test_supplied_path_is_preserved(const std::filesystem::path& directory)
{
    const auto path = directory / "preserved.bin";
    write_binary_file(path, {std::byte{0x31}, std::byte{0x32}});

    const auto supplied_path = directory / "." / "preserved.bin";
    const auto file = shardrecover::BinaryFile::load(supplied_path);
    check(file.path() == supplied_path, "BinaryFile did not preserve the supplied path");
}

struct TestCase {
    std::string_view name;
    void (*run)(const std::filesystem::path&);
};

}  // namespace

int main()
{
    try {
        const TemporaryDirectory temporary_directory;
        const std::array tests{
            TestCase{"normal file contents and size", test_normal_file},
            TestCase{"zero-byte file", test_empty_file},
            TestCase{"very small file", test_small_file},
            TestCase{"nonexistent path rejection", test_nonexistent_path},
            TestCase{"directory rejection", test_directory_path},
            TestCase{"supplied path preservation", test_supplied_path_is_preserved},
        };

        std::size_t failures = 0;
        for (const auto& test : tests) {
            try {
                test.run(temporary_directory.path());
                std::cout << "[PASS] " << test.name << '\n';
            } catch (const std::exception& error) {
                ++failures;
                std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
            }
        }

        if (failures != 0) {
            std::cerr << failures << " test case(s) failed\n";
            return 1;
        }

        std::cout << tests.size() << " test cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FATAL] " << error.what() << '\n';
        return 1;
    }
}
