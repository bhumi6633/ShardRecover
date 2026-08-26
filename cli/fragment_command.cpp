#include "fragment_command.hpp"

#include "shardrecover/binary_file.hpp"
#include "shardrecover/fragment_generator.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace shardrecover::cli {
namespace {

std::size_t parse_byte_count(std::string_view value, std::string_view name)
{
    std::size_t size = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), size);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("Invalid " + std::string(name) + ": '" + std::string(value) + "'");
    }
    return size;
}

std::string fragment_filename(std::size_t index, std::size_t width)
{
    std::ostringstream name;
    name << "fragment_" << std::setfill('0') << std::setw(static_cast<int>(width)) << index << ".bin";
    return name.str();
}

void write_fragment(const std::filesystem::path& path, const Fragment& fragment)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to create fragment file: '" + path.string() + "'");
    }

    if (!fragment.data.empty()) {
        output.write(reinterpret_cast<const char*>(fragment.data.data()),
                     static_cast<std::streamsize>(fragment.data.size()));
    }
    output.close();

    if (!output) {
        throw std::runtime_error("Failed to write fragment file: '" + path.string() + "'");
    }
}

}  // namespace

int run_fragment_command(int argc, char* argv[])
{
    if (argc == 0) {
        throw std::runtime_error("fragment requires an input file");
    }

    const std::filesystem::path input_path{argv[0]};
    std::filesystem::path output_path;
    std::size_t fragment_size = 0;
    std::size_t overlap = 0;
    bool has_size = false;
    bool has_overlap = false;
    bool has_output = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        if (option == "--size") {
            if (has_size) {
                throw std::runtime_error("--size may only be specified once");
            }
            if (++index >= argc) {
                throw std::runtime_error("--size requires a value");
            }
            fragment_size = parse_byte_count(argv[index], "fragment size");
            if (fragment_size == 0) {
                throw std::runtime_error("Fragment size must be greater than zero");
            }
            has_size = true;
        } else if (option == "--overlap") {
            if (has_overlap) {
                throw std::runtime_error("--overlap may only be specified once");
            }
            if (++index >= argc) {
                throw std::runtime_error("--overlap requires a value");
            }
            overlap = parse_byte_count(argv[index], "overlap");
            has_overlap = true;
        } else if (option == "--output") {
            if (has_output) {
                throw std::runtime_error("--output may only be specified once");
            }
            if (++index >= argc) {
                throw std::runtime_error("--output requires a directory");
            }
            output_path = argv[index];
            has_output = true;
        } else {
            throw std::runtime_error("Unexpected argument: '" + std::string(option) + "'");
        }
    }

    if (!has_size) {
        throw std::runtime_error("Missing required option: --size <bytes>");
    }
    if (!has_output) {
        throw std::runtime_error("Missing required option: --output <directory>");
    }

    const auto input = BinaryFile::load(input_path);
    const auto fragments = FragmentGenerator::generate(input.bytes(), fragment_size, overlap);

    std::error_code error;
    std::filesystem::create_directories(output_path, error);
    if (error) {
        throw std::runtime_error("Failed to create output directory '" + output_path.string()
                                 + "': " + error.message());
    }
    if (!std::filesystem::is_directory(output_path, error) || error) {
        throw std::runtime_error("Output path is not a directory: '" + output_path.string() + "'");
    }

    const auto last_index = fragments.empty() ? 0 : fragments.size() - 1;
    const auto name_width = std::max<std::size_t>(4, std::to_string(last_index).size());
    for (const auto& fragment : fragments) {
        write_fragment(output_path / fragment_filename(fragment.index, name_width), fragment);
    }

    std::cout << "Input: " << input_path.string() << '\n'
              << "Input size: " << input.size() << " bytes\n"
              << "Fragment size: " << fragment_size << " bytes\n"
              << "Overlap: " << overlap << " bytes\n"
              << "Stride: " << fragment_size - overlap << " bytes\n"
              << "Fragments written: " << fragments.size() << '\n'
              << "Output: " << output_path.string() << '\n';
    return 0;
}

}  // namespace shardrecover::cli
