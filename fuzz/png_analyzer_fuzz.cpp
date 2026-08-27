#include "shardrecover/png/analyzer.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace {

void require(bool condition)
{
    if (!condition) {
        __builtin_trap();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    constexpr std::size_t maximum_input_size = 4U * 1024U * 1024U;
    if (size > maximum_input_size) {
        return 0;
    }

    const auto bytes = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(data), size};
    const auto result = shardrecover::png::Analyzer::analyze(bytes);
    require(result.valid_crc_count <= result.chunks.size());
    require(result.invalid_crc_count <= result.chunks.size());
    require(result.valid_crc_count + result.invalid_crc_count == result.chunks.size());
    for (const auto& chunk : result.chunks) {
        require(chunk.offset <= bytes.size());
        require(chunk.data.size() == chunk.length);
        require(bytes.size() - chunk.offset >= 12);
        require(chunk.length <= bytes.size() - chunk.offset - 12);
    }
    for (const auto& issue : result.issues) {
        require(issue.offset <= bytes.size());
    }
    return 0;
}
