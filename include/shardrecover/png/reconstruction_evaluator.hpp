#pragma once

#include "shardrecover/reconstruction.hpp"

namespace shardrecover::png {

class ReconstructionEvaluator final : public CandidateEvaluator {
public:
    FormatEvidence evaluate(std::span<const std::byte> candidate) const override;
};

}  // namespace shardrecover::png
