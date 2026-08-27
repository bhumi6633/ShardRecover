#include "fragment_command.hpp"

#include "shardrecover/binary_file.hpp"
#include "shardrecover/damage.hpp"
#include "shardrecover/fragment_emitter.hpp"
#include "shardrecover/fragment_generator.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
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

std::uint64_t parse_seed(std::string_view value)
{
    std::uint64_t seed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seed);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("Invalid seed: '" + std::string(value) + "'");
    }
    return seed;
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

std::uint64_t random_seed()
{
    std::random_device source;
    return (static_cast<std::uint64_t>(source()) << 32U)
           ^ static_cast<std::uint64_t>(source());
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
    std::size_t duplicate_count = 0;
    std::size_t noise_count = 0;
    std::size_t drop_count = 0;
    std::size_t corrupt_byte_count = 0;
    std::optional<std::uint64_t> requested_seed;
    bool has_size = false;
    bool has_overlap = false;
    bool has_output = false;
    bool shuffle = false;
    bool opaque_names = false;
    bool has_duplicates = false;
    bool has_noise = false;
    bool has_drop = false;
    bool has_corrupt_bytes = false;

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
        } else if (option == "--shuffle") {
            if (shuffle) {
                throw std::runtime_error("--shuffle may only be specified once");
            }
            shuffle = true;
        } else if (option == "--opaque-names") {
            if (opaque_names) {
                throw std::runtime_error("--opaque-names may only be specified once");
            }
            opaque_names = true;
        } else if (option == "--seed") {
            if (requested_seed.has_value()) {
                throw std::runtime_error("--seed may only be specified once");
            }
            if (++index >= argc) {
                throw std::runtime_error("--seed requires a value");
            }
            requested_seed = parse_seed(argv[index]);
        } else if (option == "--duplicates") {
            if (has_duplicates) {
                throw std::runtime_error("--duplicates may only be specified once");
            }
            if (++index >= argc) {
                throw std::runtime_error("--duplicates requires a value");
            }
            duplicate_count = parse_byte_count(argv[index], "duplicate count");
            has_duplicates = true;
        } else if (option == "--noise") {
            if (has_noise) {
                throw std::runtime_error("--noise may only be specified once");
            }
            if (++index >= argc) {
                throw std::runtime_error("--noise requires a value");
            }
            noise_count = parse_byte_count(argv[index], "noise count");
            has_noise = true;
        } else if (option == "--drop") {
            if (has_drop) {
                throw std::runtime_error("--drop may only be specified once");
            }
            if (++index >= argc) {
                throw std::runtime_error("--drop requires a value");
            }
            drop_count = parse_byte_count(argv[index], "drop count");
            has_drop = true;
        } else if (option == "--corrupt-bytes") {
            if (has_corrupt_bytes) {
                throw std::runtime_error("--corrupt-bytes may only be specified once");
            }
            if (++index >= argc) {
                throw std::runtime_error("--corrupt-bytes requires a value");
            }
            corrupt_byte_count = parse_byte_count(argv[index], "corrupt byte count");
            has_corrupt_bytes = true;
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
    const auto original_fragments = FragmentGenerator::generate(input.bytes(), fragment_size, overlap);

    const bool uses_damage = duplicate_count != 0 || noise_count != 0 || drop_count != 0
                             || corrupt_byte_count != 0;
    const bool uses_randomness = shuffle || opaque_names || uses_damage;
    if (requested_seed.has_value() && !uses_randomness) {
        throw std::runtime_error("--seed requires randomized emission or dataset damage");
    }
    const auto seed = uses_randomness
                          ? requested_seed.value_or(random_seed())
                          : std::uint64_t{0};
    const auto damage = DamageSimulator::apply(
        original_fragments,
        DamageConfig{duplicate_count, noise_count, drop_count, corrupt_byte_count, seed});
    auto fragments = damage.fragments;
    if (shuffle) {
        FragmentEmitter::shuffle(fragments, seed);
    }

    const auto filenames = opaque_names
                               ? FragmentEmitter::opaque_filenames(fragments.size(), seed)
                               : std::vector<std::string>{};

    std::error_code error;
    std::filesystem::create_directories(output_path, error);
    if (error) {
        throw std::runtime_error("Failed to create output directory '" + output_path.string()
                                 + "': " + error.message());
    }
    if (!std::filesystem::is_directory(output_path, error) || error) {
        throw std::runtime_error("Output path is not a directory: '" + output_path.string() + "'");
    }

    std::size_t last_index = 0;
    for (const auto& fragment : fragments) {
        last_index = std::max(last_index, fragment.index);
    }
    const auto name_width = std::max<std::size_t>(4, std::to_string(last_index).size());
    for (std::size_t position = 0; position < fragments.size(); ++position) {
        const auto& fragment = fragments[position];
        const auto filename = opaque_names
                                  ? filenames[position]
                                  : fragment_filename(fragment.index, name_width);
        write_fragment(output_path / filename, fragment);
    }

    std::cout << "Input: " << input_path.string() << '\n'
              << "Input size: " << input.size() << " bytes\n"
              << "Fragment size: " << fragment_size << " bytes\n"
              << "Overlap: " << overlap << " bytes\n"
              << "Stride: " << fragment_size - overlap << " bytes\n"
              << "Original fragments: " << damage.original_fragment_count << '\n'
              << "Dropped: " << damage.dropped_original_ids.size() << '\n'
              << "Duplicates added: " << damage.duplicates.size() << '\n'
              << "Noise fragments added: " << damage.noise_fragment_ids.size() << '\n'
              << "Corrupted bytes: " << damage.corruptions.size() << '\n'
              << "Fragments emitted: " << fragments.size() << '\n'
              << "Shuffled: " << (shuffle ? "yes" : "no") << '\n'
              << "Opaque names: " << (opaque_names ? "yes" : "no") << '\n';
    if (uses_randomness) {
        std::cout << "Seed: " << seed << '\n';
    }
    std::cout
              << "Output: " << output_path.string() << '\n';
    return 0;
}

}  // namespace shardrecover::cli
