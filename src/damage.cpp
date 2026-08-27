#include "shardrecover/damage.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace shardrecover {
namespace {

std::size_t next_dataset_id(const std::vector<Fragment>& originals)
{
    std::size_t next = 0;
    for (const auto& fragment : originals) {
        if (fragment.index == std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument("Fragment identity cannot be extended safely");
        }
        next = std::max(next, fragment.index + 1);
    }
    return next;
}

}  // namespace

DamageResult DamageSimulator::apply(const std::vector<Fragment>& originals,
                                    const DamageConfig& config)
{
    if (config.drop_count > originals.size()) {
        throw std::invalid_argument("Drop count exceeds original fragment count");
    }

    DamageResult result;
    result.original_fragment_count = originals.size();
    std::mt19937_64 engine(config.seed);

    std::vector<std::size_t> positions(originals.size());
    for (std::size_t index = 0; index < positions.size(); ++index) {
        positions[index] = index;
    }
    std::shuffle(positions.begin(), positions.end(), engine);
    positions.resize(config.drop_count);
    std::sort(positions.begin(), positions.end());

    std::unordered_set<std::size_t> dropped_positions(positions.begin(), positions.end());
    for (std::size_t position = 0; position < originals.size(); ++position) {
        const auto& fragment = originals[position];
        if (dropped_positions.contains(position)) {
            result.dropped_original_ids.push_back(fragment.index);
        } else {
            result.fragments.push_back(fragment);
            result.surviving_original_ids.push_back(fragment.index);
        }
    }

    const auto survivor_count = result.fragments.size();
    if (config.duplicate_count != 0 && survivor_count == 0) {
        throw std::invalid_argument("Cannot duplicate when no original fragments survive");
    }
    std::size_t next_id = next_dataset_id(originals);
    if (config.duplicate_count > std::numeric_limits<std::size_t>::max() - next_id
        || config.noise_count > std::numeric_limits<std::size_t>::max()
                                    - next_id - config.duplicate_count) {
        throw std::invalid_argument("Requested damage creates too many fragment identities");
    }

    if (config.duplicate_count != 0) {
        std::uniform_int_distribution<std::size_t> select_survivor(0, survivor_count - 1);
        for (std::size_t count = 0; count < config.duplicate_count; ++count) {
            const auto source_position = select_survivor(engine);
            auto duplicate = result.fragments[source_position];
            const auto source_id = duplicate.index;
            duplicate.index = next_id++;
            result.duplicates.push_back(DuplicateRecord{duplicate.index, source_id});
            result.fragments.push_back(std::move(duplicate));
        }
    }

    if (config.noise_count != 0) {
        if (survivor_count == 0) {
            throw std::invalid_argument("Cannot size noise when no original fragments survive");
        }
        auto minimum_size = result.fragments.front().data.size();
        auto maximum_size = minimum_size;
        for (std::size_t index = 1; index < survivor_count; ++index) {
            minimum_size = std::min(minimum_size, result.fragments[index].data.size());
            maximum_size = std::max(maximum_size, result.fragments[index].data.size());
        }
        if (maximum_size == 0) {
            throw std::invalid_argument("Cannot generate nonempty noise from zero-byte fragments");
        }
        minimum_size = std::max<std::size_t>(1, minimum_size);
        std::uniform_int_distribution<std::size_t> select_size(minimum_size, maximum_size);
        std::uniform_int_distribution<unsigned int> select_byte(0, 255);
        for (std::size_t count = 0; count < config.noise_count; ++count) {
            Fragment noise{next_id++, 0, std::vector<std::byte>{}};
            noise.data.resize(select_size(engine));
            for (auto& value : noise.data) {
                value = static_cast<std::byte>(select_byte(engine));
            }
            result.noise_fragment_ids.push_back(noise.index);
            result.fragments.push_back(std::move(noise));
        }
    }

    std::size_t eligible_bytes = 0;
    const auto real_fragment_count = survivor_count + result.duplicates.size();
    for (std::size_t index = 0; index < real_fragment_count; ++index) {
        const auto size = result.fragments[index].data.size();
        if (size > std::numeric_limits<std::size_t>::max() - eligible_bytes) {
            throw std::invalid_argument("Eligible corruption byte count overflows size_t");
        }
        eligible_bytes += size;
    }
    if (config.corrupt_byte_count > eligible_bytes) {
        throw std::invalid_argument("Corruption count exceeds eligible real fragment bytes");
    }

    std::unordered_set<std::size_t> selected_positions;
    selected_positions.reserve(config.corrupt_byte_count);
    if (config.corrupt_byte_count != 0) {
        std::uniform_int_distribution<std::size_t> select_position(0, eligible_bytes - 1);
        while (selected_positions.size() < config.corrupt_byte_count) {
            selected_positions.insert(select_position(engine));
        }
    }
    std::vector<std::size_t> ordered_positions(selected_positions.begin(), selected_positions.end());
    std::sort(ordered_positions.begin(), ordered_positions.end());
    std::uniform_int_distribution<unsigned int> select_delta(1, 255);
    for (const auto flat_position : ordered_positions) {
        auto remaining = flat_position;
        for (std::size_t index = 0; index < real_fragment_count; ++index) {
            auto& fragment = result.fragments[index];
            if (remaining >= fragment.data.size()) {
                remaining -= fragment.data.size();
                continue;
            }
            const auto original = fragment.data[remaining];
            const auto corrupted = original ^ static_cast<std::byte>(select_delta(engine));
            fragment.data[remaining] = corrupted;
            result.corruptions.push_back(
                CorruptionRecord{fragment.index, remaining, original, corrupted});
            break;
        }
    }

    return result;
}

}  // namespace shardrecover
