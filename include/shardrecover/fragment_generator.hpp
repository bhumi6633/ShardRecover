#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace shardrecover {

struct Fragment {
    std::size_t index;
    std::size_t offset;
    std::vector<std::byte> data;
};

class FragmentGenerator {
public:
    static std::vector<Fragment> generate(std::span<const std::byte> bytes,
                                          std::size_t fragment_size);
};

}  // namespace shardrecover
