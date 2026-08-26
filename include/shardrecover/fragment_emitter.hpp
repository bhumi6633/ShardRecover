#pragma once

#include "shardrecover/fragment_generator.hpp"

#include <cstdint>
#include <vector>

namespace shardrecover {

class FragmentEmitter {
public:
    static void shuffle(std::vector<Fragment>& fragments, std::uint64_t seed);
};

}  // namespace shardrecover
