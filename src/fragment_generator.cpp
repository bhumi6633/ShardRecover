#include "shardrecover/fragment_generator.hpp"

#include <algorithm>
#include <stdexcept>

namespace shardrecover {

std::vector<Fragment> FragmentGenerator::generate(std::span<const std::byte> bytes,
                                                  std::size_t fragment_size)
{
    if (fragment_size == 0) {
        throw std::invalid_argument("Fragment size must be greater than zero");
    }

    std::vector<Fragment> fragments;
    const auto fragment_count = bytes.size() / fragment_size
                                + (bytes.size() % fragment_size != 0 ? 1 : 0);
    fragments.reserve(fragment_count);

    for (std::size_t offset = 0, index = 0; offset < bytes.size(); ++index) {
        const auto size = std::min(fragment_size, bytes.size() - offset);
        const auto fragment_bytes = bytes.subspan(offset, size);
        fragments.push_back(Fragment{index,
                                     offset,
                                     {fragment_bytes.begin(), fragment_bytes.end()}});
        offset += size;
    }

    return fragments;
}

}  // namespace shardrecover
