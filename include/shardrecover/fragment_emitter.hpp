#pragma once

#include "shardrecover/fragment_generator.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace shardrecover {

class FragmentEmitter {
public:
    static void shuffle(std::vector<Fragment>& fragments, std::uint64_t seed);
    static std::vector<std::string> opaque_filenames(std::size_t count, std::uint64_t seed);
};

}  // namespace shardrecover
