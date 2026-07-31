#pragma once

#include <cstddef>

namespace photopainter::product {

// Pure, host-testable policy for resolving categorized media against the
// read-only legacy flat layout during index reconstruction.
enum class MediaLocationDecision {
    kNone,
    kUseCategorized,
    kUseLegacy,
    kRejectCategorizedCollision,
};

constexpr MediaLocationDecision DecideMediaLocation(std::size_t categorized_count,
                                                     bool categorized_valid,
                                                     bool legacy_valid) {
    if (categorized_count > 1U) return MediaLocationDecision::kRejectCategorizedCollision;
    if (categorized_count == 1U && categorized_valid) return MediaLocationDecision::kUseCategorized;
    if (legacy_valid) return MediaLocationDecision::kUseLegacy;
    return MediaLocationDecision::kNone;
}

}  // namespace photopainter::product
