#include "shardrecover/fragment_emitter.hpp"

#include <algorithm>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>

namespace shardrecover {

void FragmentEmitter::shuffle(std::vector<Fragment>& fragments, std::uint64_t seed)
{
    std::mt19937_64 engine(seed);
    std::shuffle(fragments.begin(), fragments.end(), engine);
}

std::vector<std::string> FragmentEmitter::opaque_filenames(std::size_t count, std::uint64_t seed)
{
    std::mt19937_64 engine(seed);
    std::vector<std::string> filenames;
    filenames.reserve(count);

    std::unordered_set<std::uint64_t> identifiers;
    identifiers.reserve(count);

    while (filenames.size() < count) {
        const auto identifier = engine();
        if (!identifiers.insert(identifier).second) {
            continue;
        }

        std::ostringstream filename;
        filename << "shard_" << std::hex << std::setfill('0') << std::setw(16) << identifier << ".bin";
        filenames.push_back(filename.str());
    }

    return filenames;
}

}  // namespace shardrecover
