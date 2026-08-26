#include "shardrecover/fragment_emitter.hpp"

#include <algorithm>
#include <random>

namespace shardrecover {

void FragmentEmitter::shuffle(std::vector<Fragment>& fragments, std::uint64_t seed)
{
    std::mt19937_64 engine(seed);
    std::shuffle(fragments.begin(), fragments.end(), engine);
}

}  // namespace shardrecover
