#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace shardrecover::png {

std::uint32_t compute_chunk_crc(const std::array<char, 4>& type,
                                std::span<const std::byte> data) noexcept;

}  // namespace shardrecover::png
