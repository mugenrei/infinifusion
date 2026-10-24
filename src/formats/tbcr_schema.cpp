#include "formats/tbcr_schema.hpp"

#include <algorithm>
#include <array>

namespace infinifusion::formats::tbcr {
namespace {
using core::ErrorCode;
constexpr std::array<FieldSchema, 3> fusion_fields{{
    {"sort_id", "u16", 0, 1, 1},
    {"ingredients", "u16", 2, 4, 1},
    {"result_id", "u16", 10, 1, 1},
}};
const std::array<TableSchema, 1> unite_tables{{{4, 12, "special_fusion", fusion_fields}}};

constexpr std::array<FieldSchema, 32> nkm_base_fields{{
    {"name_id", "u32", 4, 1, 1},
    {"race", "i8", 8, 1, 1},
    {"alignment_2", "i8", 16, 1, 1},
    {"level", "i32", 20, 1, 1},
    {"is_recruitable", "i8", 24, 1, 1},
    {"hp", "i32", 48, 1, 1},
    {"sp", "i32", 52, 1, 1},
    {"hp_growth", "i32", 56, 1, 1},
    {"sp_growth", "i32", 60, 1, 1},
    {"stats.strength", "i32", 64, 1, 1},
    {"stats.vitality", "i32", 68, 1, 1},
    {"stats.magic", "i32", 72, 1, 1},
    {"stats.agility", "i32", 76, 1, 1},
    {"stats.luck", "i32", 80, 1, 1},
    {"stats.strength_growth", "i32", 84, 1, 1},
    {"stats.vitality_growth", "i32", 88, 1, 1},
    {"stats.magic_growth", "i32", 92, 1, 1},
    {"stats.agility_growth", "i32", 96, 1, 1},
    {"stats.luck_growth", "i32", 100, 1, 1},
    {"gift_ev", "i8", 104, 1, 1},
    {"gift_type_val", "i8", 105, 1, 1},
    {"talk_type", "i8", 108, 1, 1},
    {"backup_talk_type", "i8", 109, 1, 1},

    {"skills", "i32", 132, 12, 12},
    {"auto_skills", "i32", 180, 12, 12},
    {"unique_skill", "i32", 276, 1, 1},
    {"affinities.ice", "i32", 288, 1, 1},
    {"affinities.thunder", "i32", 292, 1, 1},
    {"affinities.dark", "i32", 304, 1, 1},
    {"skill_affinities.ice", "i32", 400, 1, 1},
    {"skill_affinities.elec", "i32", 404, 1, 1},
    {"skill_affinities.support", "i32", 428, 1, 1},
}};
const std::array<TableSchema, 1> nkm_tables{{{0, 464, "base", nkm_base_fields}}};
const std::array<AssetSchema, 2> assets{{
    {"/Game/Blueprints/Gamedata/BinTable/Unite/UniteTable.UniteTable", "unite", unite_tables},
    {"/Game/Blueprints/Gamedata/BinTable/Devil/NKMBaseTable.NKMBaseTable", "nkm_base", nkm_tables},
}};

core::Result<core::FieldType> primitive(std::string_view name) {
    core::FieldType result;
    if (name == "u8" || name == "bytes") { result.kind = core::ScalarKind::UnsignedInt; result.size = 1; }
    else if (name == "i8") { result.kind = core::ScalarKind::SignedInt; result.size = 1; }
    else if (name == "u16") { result.kind = core::ScalarKind::UnsignedInt; result.size = 2; }
    else if (name == "i16") { result.kind = core::ScalarKind::SignedInt; result.size = 2; }
    else if (name == "u32") { result.kind = core::ScalarKind::UnsignedInt; result.size = 4; }
    else if (name == "i32") { result.kind = core::ScalarKind::SignedInt; result.size = 4; }
    else if (name == "f32") { result.kind = core::ScalarKind::Float; result.size = 4; }
    else return core::make_error(ErrorCode::TypeMismatch, "Unknown TBCR raw codec: " + std::string(name));
    return result;
}
}

const AssetSchema* asset(std::string_view selector) noexcept {
    for (const auto& item : assets)
        if (selector == item.path || selector == item.alias) return &item;
    return nullptr;
}
std::string canonical_asset(std::string_view selector) {
    const auto* item = asset(selector);
    return std::string(item ? item->path : selector);
}
const TableSchema* table(const AssetSchema& schema, std::string_view alias) noexcept {
    for (const auto& item : schema.tables) if (item.alias == alias) return &item;
    return nullptr;
}
const TableSchema* table(const AssetSchema& schema, std::uint32_t index) noexcept {
    for (const auto& item : schema.tables) if (item.index == index) return &item;
    return nullptr;
}

core::Result<Write> encode_raw(std::uint32_t offset, std::string_view type,
                              std::span<const core::Value> values) {
    auto descriptor = primitive(type);
    if (!descriptor) return descriptor.error();
    if (values.empty() || (type != "bytes" && values.size() != 1))
        return core::make_error(ErrorCode::InvalidArgument, "TBCR raw codec has invalid value count");
    if (values.size() > UINT32_MAX / descriptor->size)
        return core::make_error(ErrorCode::LimitExceeded, "TBCR encoded write exceeds record offset range");
    Write output{{}, offset, std::string(type), {}};
    output.bytes.reserve(values.size() * descriptor->size);
    for (const auto& value : values) {
        auto encoded = core::encode_value(value, descriptor.value());
        if (!encoded) return encoded.error();
        output.bytes.insert(output.bytes.end(), encoded->bytes.begin(), encoded->bytes.end());
    }
    return output;
}
core::Result<Write> raw_bytes(std::uint32_t offset, std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return core::make_error(ErrorCode::InvalidArgument, "TBCR byte write is empty");
    if (bytes.size() > UINT32_MAX)
        return core::make_error(ErrorCode::LimitExceeded, "TBCR byte write exceeds record offset range");
    return Write{{}, offset, "bytes", {bytes.begin(), bytes.end()}};
}
core::Result<Write> encode_named(const TableSchema& schema, std::string_view property,
                                std::span<const core::Value> values) {
    const FieldSchema* field = nullptr;
    for (const auto& item : schema.fields) if (item.property == property) { field = &item; break; }
    if (!field) return core::make_error(ErrorCode::PathNotFound, "Unknown TBCR field: " + std::string(property));
    if (values.size() < field->min_count || values.size() > field->count)
        return core::make_error(ErrorCode::InvalidArgument, "TBCR named field has invalid value count");
    auto descriptor = primitive(field->type);
    if (!descriptor) return descriptor.error();
    Write output{std::string(property), field->offset, std::string(field->type), {}};
    for (const auto& value : values) {
        auto encoded = core::encode_value(value, descriptor.value());
        if (!encoded) return encoded.error();
        output.bytes.insert(output.bytes.end(), encoded->bytes.begin(), encoded->bytes.end());
    }

    if (values.size() != field->count && property != "ingredients")
        return core::make_error(ErrorCode::InvalidArgument, "TBCR field has no omitted-element default");
    output.bytes.resize(static_cast<std::size_t>(field->count) * descriptor->size, 0);
    if (output.offset > schema.stride || output.bytes.size() > schema.stride - output.offset)
        return core::make_error(ErrorCode::OutOfRange, "TBCR schema field exceeds record stride");
    return output;
}

core::Result<std::vector<std::uint8_t>> assemble_record(
    std::uint32_t stride, std::span<const Write> writes, std::span<const std::uint8_t> base) {
    if (!stride || writes.empty())
        return core::make_error(ErrorCode::InvalidArgument, "TBCR record requires nonzero stride and writes");
    if (!base.empty() && base.size() != stride)
        return core::make_error(ErrorCode::InvalidArgument, "TBCR base record size differs from stride");
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    ranges.reserve(writes.size());
    for (const auto& write : writes) {
        if (write.bytes.empty()) return core::make_error(ErrorCode::InvalidArgument, "TBCR write is empty");
        if (write.offset > stride || write.bytes.size() > stride - write.offset)
            return core::make_error(ErrorCode::OutOfRange, "TBCR write exceeds record stride");
        ranges.emplace_back(write.offset, write.offset + static_cast<std::uint32_t>(write.bytes.size()));
    }
    std::sort(ranges.begin(), ranges.end());
    std::uint32_t previous_end = 0;
    bool gap = false;
    for (const auto& range : ranges) {
        if (range.first < previous_end)
            return core::make_error(ErrorCode::InvalidArgument, "TBCR writes overlap");
        if (range.first != previous_end) gap = true;
        previous_end = range.second;
    }
    if (base.empty() && (gap || previous_end != stride))
        return core::make_error(ErrorCode::InvalidArgument, "TBCR append leaves uninitialized record bytes");
    std::vector<std::uint8_t> output(stride);
    if (!base.empty()) std::copy(base.begin(), base.end(), output.begin());
    for (const auto& write : writes)
        std::copy(write.bytes.begin(), write.bytes.end(), output.begin() + write.offset);
    return output;
}
}
