#pragma once

#include <cstdint>

namespace Asset
{
    // Shared "no such element" marker for every optional index field below (material slots,
    // node mesh/skin references, joint parents, ...) - single source so every struct agrees
    // on what "not present" looks like.
    constexpr int32_t kInvalidIndex = -1;
}
