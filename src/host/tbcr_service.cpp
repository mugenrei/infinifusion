#include "host/tbcr_service.hpp"
#include "formats/tbcr_schema.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace infinifusion::host {
namespace {
std::string copy(IF_String s) {
    if (s.length > 4096 || (s.length && !s.data)) throw std::invalid_argument("invalid string span");
    std::string result(s.length ? s.data : "", s.length);
    if (result.find('\0') != std::string::npos) throw std::invalid_argument("embedded NUL");
    return result;
}
template<size_t N> void put(char (&out)[N], std::string_view value) {
    const auto size = std::min(value.size(), N - 1);
    std::memcpy(out, value.data(), size); out[size] = 0;
}
core::Value scalar(const IF_Value& value) {
    if (value.size != sizeof(value)) throw std::invalid_argument("value size mismatch");
    switch (value.kind) {
    case IF_INT64: return core::Value::signed_integer(value.signed_value);
    case IF_UINT64: return core::Value::unsigned_integer(value.unsigned_value);
    case IF_FLOAT64: return core::Value::floating(value.float_value);
    default: throw std::invalid_argument("TBCR writes require numeric values");
    }
}
bool exact_asset_path(std::string_view path) {
    if (!path.starts_with("/Game/")) return false;
    const auto dot = path.find('.');
    if (dot == path.npos || path.find('.', dot + 1) != path.npos) return false;
    const auto package = path.substr(0, dot), object = path.substr(dot + 1);
    const auto slash = package.rfind('/');
    if (object.empty() || package.substr(slash + 1) != object) return false;
    const auto identifier = [](std::string_view part) {
        return !part.empty() && std::all_of(part.begin(), part.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
    };
    auto remaining = package.substr(1);
    while (!remaining.empty()) {
        const auto end = remaining.find('/');
        if (!identifier(remaining.substr(0, end))) return false;
        if (end == remaining.npos) return true;
        remaining.remove_prefix(end + 1);
    }
    return false;
}
}
uint32_t IF_CALL tbcr_compile_append(const IF_TbcrAppend* input, IF_TbcrCompiled* output) noexcept {
    if (!output || output->size != sizeof(*output)) return IF_INVALID_ARGUMENT;
    *output = {}; output->size = sizeof(*output);
    const auto fail = [&](uint32_t code, std::string_view reason) { put(output->message, reason); return code; };
    try {
        if (!input || input->size != sizeof(*input) || !input->writes ||
            !input->write_count || input->write_count > 128)
            return fail(IF_INVALID_ARGUMENT, "invalid append descriptor");
        namespace schema = formats::tbcr;
        const auto asset_name = copy(input->asset);
        const auto* asset = schema::asset(asset_name);
        const auto alias = copy(input->table_alias);
        const auto* table = asset ? (alias.empty() ? schema::table(*asset, input->table_index)
                                                 : schema::table(*asset, alias)) : nullptr;
        const auto asset_path = asset ? std::string(asset->path) : asset_name;
        if (!exact_asset_path(asset_path))
            return fail(IF_INVALID_ARGUMENT, "asset requires a known alias or exact /Game/...Asset.Asset path");
        if (asset_path.size() >= sizeof(output->asset))
            return fail(IF_LIMIT_EXCEEDED, "asset path exceeds compiled output capacity");
        const auto declaration_id = copy(input->id);
        if (declaration_id.empty() || declaration_id.size() > 128)
            return fail(IF_INVALID_ARGUMENT, "append id must be 1..128 bytes");
        if (!alias.empty() && !table)
            return fail(IF_CAPABILITY_UNAVAILABLE, "unknown table alias; raw tables require a numeric selector");
        if (table && input->record_size && input->record_size != table->stride)
            return fail(IF_GUARD_MISMATCH, "record_size differs from schema stride");
        const auto stride = table ? table->stride : input->record_size;
        if (!stride || stride > sizeof(output->bytes))
            return fail(IF_OUT_OF_RANGE, "record stride must be 1..4096");
        const auto table_index = table ? table->index : input->table_index;
        std::vector<schema::Write> writes;
        writes.reserve(input->write_count);
        for (uint32_t i = 0; i < input->write_count; ++i) {
            const auto& write = input->writes[i];
            if (write.size != sizeof(write) || write.reserved || !write.values ||
                !write.value_count || write.value_count > 4096)
                return fail(IF_INVALID_ARGUMENT, "invalid write descriptor");
            const auto property = copy(write.property), type = copy(write.type);
            if (!property.empty() && (!type.empty() || write.offset))
                return fail(IF_INVALID_ARGUMENT, "named write cannot also specify raw offset/type");
            if (!property.empty() && !table)
                return fail(IF_CAPABILITY_UNAVAILABLE, "named writes require a known table schema");
            if (property.empty() && !input->record_size)
                return fail(IF_INVALID_ARGUMENT, "raw writes require record_size");
            std::vector<core::Value> values;
            values.reserve(write.value_count);
            for (uint32_t j = 0; j < write.value_count; ++j) values.push_back(scalar(write.values[j]));
            auto encoded = property.empty() ? schema::encode_raw(write.offset, type, values)
                                           : schema::encode_named(*table, property, values);
            if (!encoded) return fail(static_cast<uint32_t>(encoded.error().code), encoded.error().message);
            writes.push_back(std::move(encoded.value()));
        }
        auto encoded = schema::assemble_record(stride, writes);
        if (!encoded) return fail(static_cast<uint32_t>(encoded.error().code), encoded.error().message);
        const auto& bytes = encoded.value();
        put(output->provider, "smtvv.tbcr"); put(output->asset, asset_path);
        put(output->row, "append:" + std::to_string(table_index) + ":" + std::to_string(stride) + ":" + declaration_id);
        put(output->property, "Record");
        output->byte_count = static_cast<uint32_t>(bytes.size());
        std::copy(bytes.begin(), bytes.end(), output->bytes);
        output->value.size = sizeof(IF_Value); output->value.kind = IF_BYTES;
        output->value.bytes = {output->bytes, output->byte_count};
        put(output->message, "prepared logical append; source materialization is a separate outcome");
        return IF_OK;
    } catch (const std::invalid_argument& e) { return fail(IF_INVALID_ARGUMENT, e.what()); }
      catch (const std::exception& e) { return fail(IF_INTERNAL, e.what()); }
      catch (...) { return fail(IF_INTERNAL, "TBCR preparation failed"); }
}
uint32_t IF_CALL tbcr_compile_update(const IF_TbcrUpdate* input, IF_TbcrCompiled* output) noexcept {
    if (!output || output->size != sizeof(*output)) return IF_INVALID_ARGUMENT;
    *output = {}; output->size = sizeof(*output);
    const auto fail = [&](uint32_t code, std::string_view reason) { put(output->message, reason); return code; };
    try {
        if (!input || input->size != sizeof(*input) || !input->write)
            return fail(IF_INVALID_ARGUMENT, "invalid update descriptor");
        namespace schema = formats::tbcr;
        const auto asset_name = copy(input->asset);
        const auto* asset = schema::asset(asset_name);
        const auto asset_path = asset ? std::string(asset->path) : asset_name;
        if (!exact_asset_path(asset_path))
            return fail(IF_INVALID_ARGUMENT, "asset requires a known alias or exact /Game/...Asset.Asset path");
        if (asset_path.size() >= sizeof(output->asset))
            return fail(IF_LIMIT_EXCEEDED, "asset path exceeds compiled output capacity");
        const auto alias = copy(input->table_alias);
        const auto* table = asset ? (alias.empty() ? schema::table(*asset, input->table_index) : schema::table(*asset, alias)) : nullptr;
        if (!alias.empty() && !table)
            return fail(IF_CAPABILITY_UNAVAILABLE, "unknown table alias; raw tables require a numeric selector");
        if (table && input->record_size && input->record_size != table->stride)
            return fail(IF_GUARD_MISMATCH, "record_size differs from schema stride");
        const auto stride = table ? table->stride : input->record_size;
        if (!stride || stride > sizeof(output->bytes))
            return fail(IF_OUT_OF_RANGE, "record stride must be 1..4096");
        const auto table_index = table ? table->index : input->table_index;
        const auto& write = *input->write;
        if (write.size != sizeof(write) || write.reserved || !write.values || !write.value_count || write.value_count > 4096)
            return fail(IF_INVALID_ARGUMENT, "invalid write descriptor");
        const auto property = copy(write.property), type = copy(write.type);
        if (!property.empty() && (!type.empty() || write.offset))
            return fail(IF_INVALID_ARGUMENT, "named write cannot also specify raw offset/type");
        if (!property.empty() && !table)
            return fail(IF_CAPABILITY_UNAVAILABLE, "named writes require a known table schema");
        if (property.empty() && !input->record_size)
            return fail(IF_INVALID_ARGUMENT, "raw writes require record_size");
        std::vector<core::Value> values;
        values.reserve(write.value_count);
        for (uint32_t j = 0; j < write.value_count; ++j) values.push_back(scalar(write.values[j]));
        auto encoded = property.empty() ? schema::encode_raw(write.offset, type, values)
                                       : schema::encode_named(*table, property, values);
        if (!encoded) return fail(static_cast<uint32_t>(encoded.error().code), encoded.error().message);
        if (encoded->offset >= stride || encoded->bytes.size() > stride - encoded->offset)
            return fail(IF_OUT_OF_RANGE, "write exceeds record stride");
        put(output->provider, "smtvv.tbcr"); put(output->asset, asset_path);
        put(output->row, std::to_string(table_index) + ":" + std::to_string(input->record_index) + ":" + std::to_string(stride));
        put(output->property, "Bytes[" + std::to_string(encoded->offset) + "][" + std::to_string(encoded->bytes.size()) + "]");
        output->byte_offset = encoded->offset; output->byte_count = static_cast<uint32_t>(encoded->bytes.size());
        std::copy(encoded->bytes.begin(), encoded->bytes.end(), output->bytes);
        output->value.size = sizeof(IF_Value); output->value.kind = IF_BYTES;
        output->value.bytes = {output->bytes, output->byte_count};
        put(output->message, "prepared logical span; consumer application is a separate startup outcome");
        return IF_OK;
    } catch (const std::invalid_argument& e) { return fail(IF_INVALID_ARGUMENT, e.what()); }
      catch (const std::exception& e) { return fail(IF_INTERNAL, e.what()); }
      catch (...) { return fail(IF_INTERNAL, "TBCR update preparation failed"); }
}

}
