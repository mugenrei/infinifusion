#pragma once
#include "formats/tbcr.hpp"
#include <algorithm>

namespace infinifusion::unreal {
enum class TbcrPublication { Unchanged, Applied, Rejected, Recovered, RecoveryUncertain };

template<class Valid, class Read, class Write>
TbcrPublication publish_tbcr_records(std::span<const smtvv::tbcr::RecordEdit> edits,
    std::span<std::uint8_t> scratch, Valid valid, Read read, Write write) noexcept
{
    if (!valid()) return TbcrPublication::Rejected;
    for (const auto& edit : edits) {
        if (edit.before.size() != edit.after.size() || edit.before.size() > scratch.size())
            return TbcrPublication::Rejected;
        auto observed = scratch.first(edit.before.size());
        if (!read(edit.offset, observed) || !std::equal(observed.begin(), observed.end(), edit.before.begin()))
            return TbcrPublication::Rejected;
    }
    if (!valid()) return TbcrPublication::Rejected;
    if (edits.empty()) return TbcrPublication::Unchanged;
    std::size_t attempted{};
    bool success = true;
    for (const auto& edit : edits) {
        if (!valid()) { success = false; break; }
        ++attempted;
        auto observed = scratch.first(edit.after.size());
        if (!write(edit.offset, edit.after) || !read(edit.offset, observed) ||
            !std::equal(observed.begin(), observed.end(), edit.after.begin())) {
            success = false; break;
        }
    }
    if (success && valid()) return TbcrPublication::Applied;
    if (!attempted) return TbcrPublication::Rejected;
    bool recovered = valid();
    for (std::size_t i = 0; i < attempted; ++i) {
        const auto& edit = edits[i];
        auto observed = scratch.first(edit.before.size());

        const bool restored = valid() && write(edit.offset, edit.before) && read(edit.offset, observed) &&
            std::equal(observed.begin(), observed.end(), edit.before.begin());
        recovered = restored && recovered;
    }
    return recovered && valid() ? TbcrPublication::Recovered : TbcrPublication::RecoveryUncertain;
}
}
