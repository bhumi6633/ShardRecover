#pragma once

#include "shardrecover/png/types.hpp"

#include <cstddef>
#include <span>

namespace shardrecover::png {

class Analyzer {
public:
    static AnalysisResult analyze(std::span<const std::byte> bytes);
};

}  // namespace shardrecover::png
