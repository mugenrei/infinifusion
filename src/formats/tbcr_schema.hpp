#pragma once

#include "core/value.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace infinifusion::formats::tbcr {

struct FieldSchema {
    std::string_view property;
    std::string_view type;
    std::uint32_t offset;
    std::uint32_t count;
    std::uint32_t min_count;
};
struct TableSchema {
    std::uint32_t index;
    std::uint32_t stride;
    std::string_view alias;
    std::span<const FieldSchema> fields;
};
struct AssetSchema {
    std::string_view path;
    std::string_view alias;
    std::span<const TableSchema> tables;
};
struct Write {
    std::string property;
    std::uint32_t offset = 0;
    std::string type;
    std::vector<std::uint8_t> bytes;
};

const AssetSchema* asset(std::string_view selector) noexcept;
std::string canonical_asset(std::string_view selector);
const TableSchema* table(const AssetSchema& schema, std::string_view alias) noexcept;
const TableSchema* table(const AssetSchema& schema, std::uint32_t index) noexcept;

core::Result<Write> encode_raw(std::uint32_t offset, std::string_view type,
                               std::span<const core::Value> values);
core::Result<Write> raw_bytes(std::uint32_t offset, std::span<const std::uint8_t> bytes);

core::Result<Write> encode_named(const TableSchema& schema, std::string_view property,
                                 std::span<const core::Value> values);

core::Result<std::vector<std::uint8_t>> assemble_record(
    std::uint32_t stride, std::span<const Write> writes,
    std::span<const std::uint8_t> base = {});

}
