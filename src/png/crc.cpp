#include "shardrecover/png/crc.hpp"

#include <cstddef>
#include <cstdint>

namespace shardrecover::png {
namespace {

std::uint32_t update_crc(std::uint32_t crc, std::byte value) noexcept
{
    crc ^= std::to_integer<std::uint8_t>(value);
    for (int bit = 0; bit < 8; ++bit) {
        const auto mask = 0U - (crc & 1U);
        crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
    return crc;
}

}  // namespace

std::uint32_t compute_chunk_crc(const std::array<char, 4>& type,
                                std::span<const std::byte> data) noexcept
{
    std::uint32_t crc = 0xffffffffU;
    for (const auto value : type) {
        crc = update_crc(crc, static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    for (const auto value : data) {
        crc = update_crc(crc, value);
    }
    return crc ^ 0xffffffffU;
}

}  // namespace shardrecover::png
