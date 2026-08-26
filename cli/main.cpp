#include "analyze_command.hpp"
#include "fragment_command.hpp"
#include "shardrecover/binary_file.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace {

void print_help()
{
    std::cout << "Usage:\n"
                 "  shardrecover [--help] [--version]\n"
                 "  shardrecover inspect <file>\n"
                 "  shardrecover analyze <fragment-directory> [--min-overlap <bytes>] [--top <count>]\n"
                 "  shardrecover fragment <input> --size <bytes> [--overlap <bytes>]\n"
                 "      [--shuffle] [--opaque-names] [--seed <integer>] --output <directory>\n"
                 "\n"
                 "Commands:\n"
                 "  analyze      Find directional byte overlaps between fragment files\n"
                 "  inspect      Show file size and a hexadecimal byte preview\n"
                 "  fragment     Split a file into fixed-size binary fragments\n"
                 "\n"
                 "Planned commands:\n"
                 "  reconstruct\n";
}

void inspect_file(const std::filesystem::path& path)
{
    const auto file = shardrecover::BinaryFile::load(path);

    std::cout << "File: " << path.string() << '\n';
    std::cout << "Size: " << file.size() << " bytes\n";
    std::cout << "Preview:";

    if (file.empty()) {
        std::cout << " <empty>\n";
        return;
    }

    constexpr std::size_t preview_size = 16;
    const auto bytes = file.bytes();
    const auto count = std::min(bytes.size(), preview_size);
    for (std::size_t index = 0; index < count; ++index) {
        std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0')
                  << std::to_integer<unsigned int>(bytes[index]);
    }
    std::cout << std::dec << '\n';
}

}  // namespace

int main(int argc, char* argv[])
{
    if (argc == 1) {
        print_help();
        return 0;
    }

    const std::string_view argument{argv[1]};

    if (argc == 2 && argument == "--help") {
        print_help();
        return 0;
    }

    if (argc == 2 && argument == "--version") {
        std::cout << "ShardRecover 0.1.0\n";
        return 0;
    }

    if (argument == "inspect") {
        if (argc != 3) {
            std::cerr << "Error: inspect requires exactly one file path\n"
                         "Usage: shardrecover inspect <file>\n";
            return 1;
        }

        try {
            inspect_file(argv[2]);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Error: " << error.what() << '\n';
            return 1;
        }
    }

    if (argument == "fragment") {
        try {
            return shardrecover::cli::run_fragment_command(argc - 2, argv + 2);
        } catch (const std::exception& error) {
            std::cerr << "Error: " << error.what() << '\n'
                      << "Usage: shardrecover fragment <input> --size <bytes> "
                         "[--overlap <bytes>] [--shuffle] [--opaque-names] "
                         "[--seed <integer>] --output <directory>\n";
            return 1;
        }
    }

    if (argument == "analyze") {
        try {
            return shardrecover::cli::run_analyze_command(argc - 2, argv + 2);
        } catch (const std::exception& error) {
            std::cerr << "Error: " << error.what() << '\n'
                      << "Usage: shardrecover analyze <fragment-directory> "
                         "[--min-overlap <bytes>] [--top <count>]\n";
            return 1;
        }
    }

    std::cerr << "Error: unknown argument '" << argument << "'\n";
    return 1;
}
