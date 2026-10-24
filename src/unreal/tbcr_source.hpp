#pragma once
#include "core/tbcr_patch.hpp"
#include "formats/tbcr.hpp"
#include "unreal/tbcr_publication.hpp"
#include <cstring>
#include <climits>
#include <stdexcept>
#include <tuple>

namespace infinifusion::unreal {
struct TbcrSourceImage {
    smtvv::tbcr::Bytes bytes;
    std::uint32_t update_records{}, append_records{};
};

inline TbcrSourceImage compose_tbcr_source(std::span<const std::uint8_t> base,
    std::string_view asset, std::span<const core::TbcrPatchProvider::Definition> definitions) {
    auto document=smtvv::tbcr::Document::parse(base);
    std::vector<smtvv::tbcr::RecordPatch> patches;
    std::vector<const core::TbcrPatchProvider::Definition*> appends;
    for (const auto& definition:definitions) if (definition.asset==asset) {
        if (definition.table_index>=document.sections().size())
            throw std::runtime_error("TBCR definition selects a nonexistent table");
        const auto& section=document.sections()[definition.table_index];
        if (section.record_stride!=definition.record_size || definition.bytes.size()!=definition.record_size ||
            definition.active_mask.size()!=definition.record_size)
            throw std::runtime_error("TBCR definition/live record stride mismatch");
        if (definition.append) {
            if (!std::all_of(definition.active_mask.begin(),definition.active_mask.end(),[](auto byte){return byte==0xFF;}))
                throw std::runtime_error("TBCR append record is not completely initialized");
            appends.push_back(&definition);
        } else patches.push_back({definition.table_index,definition.record_index,definition.bytes,definition.active_mask});
    }
    TbcrSourceImage result;
    const auto edits=document.prepare_updates(patches);
    result.update_records=static_cast<std::uint32_t>(patches.size());
    result.bytes=document.serialize();
    for (const auto& edit:edits) std::copy(edit.after.begin(),edit.after.end(),result.bytes.begin()+edit.offset);
    document=smtvv::tbcr::Document::parse(result.bytes);
    std::sort(appends.begin(),appends.end(),[](const auto* a,const auto* b) {
        return std::tie(a->owner_id,a->append_id,a->table_index)<std::tie(b->owner_id,b->append_id,b->table_index);
    });
    for (const auto* append:appends) document.append(append->table_index,append->bytes);
    result.append_records=static_cast<std::uint32_t>(appends.size());
    result.bytes=document.serialize();
    return result;
}

struct TbcrBufferHeader {
    void* data{};
    std::int32_t num{}, max{};
    bool operator==(const TbcrBufferHeader&) const = default;
};
static_assert(sizeof(TbcrBufferHeader)==16);
struct TbcrBufferResult {
    TbcrPublication state{TbcrPublication::Rejected};
    bool cleanup_certain{true};
};

template<class Valid, class Allocate, class Release, class Read, class Write, class ReadHeader, class WriteHeader>
TbcrBufferResult publish_tbcr_buffer(const TbcrBufferHeader& old,
    std::span<const std::uint8_t> candidate, std::span<std::uint8_t> scratch,
    Valid valid, Allocate allocate, Release release, Read read, Write write,
    ReadHeader read_header, WriteHeader write_header) noexcept {
    if (candidate.empty() || candidate.size()>INT32_MAX || scratch.size()!=candidate.size() || !valid()) return {};
    void* replacement{};
    if (!allocate(candidate.size(),&replacement)) return {TbcrPublication::Rejected,false};
    if (!replacement) return {};
    const auto discard=[&]() noexcept {return release(replacement);};
    if (!write(replacement,candidate) || !read(replacement,scratch) ||
        !std::equal(candidate.begin(),candidate.end(),scratch.begin()) || !valid())
        return {TbcrPublication::Rejected,discard()};
    const TbcrBufferHeader next{replacement,static_cast<std::int32_t>(candidate.size()),static_cast<std::int32_t>(candidate.size())};
    TbcrBufferHeader observed{};
    const bool changed=write_header(next);
    if (!changed || !read_header(observed) || observed!=next) {

        if (!write_header(old) || !read_header(observed) || observed!=old || !valid())
            return {TbcrPublication::RecoveryUncertain,false};
        return {TbcrPublication::Recovered,discard()};
    }
    return {TbcrPublication::Applied,!old.data || release(old.data)};
}
}
