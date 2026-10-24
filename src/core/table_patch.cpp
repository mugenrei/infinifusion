#include "core/table_patch.hpp"
#include <algorithm>
#include <stdexcept>

namespace infinifusion::core {
namespace {

bool utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        std::uint32_t code = first, minimum = 0;
        unsigned more = 0;
        if (first >= 0xC2 && first <= 0xDF) { code &= 0x1F; more = 1; minimum = 0x80; }
        else if (first >= 0xE0 && first <= 0xEF) { code &= 0x0F; more = 2; minimum = 0x800; }
        else if (first >= 0xF0 && first <= 0xF4) { code &= 7; more = 3; minimum = 0x10000; }
        else if (first >= 0x80) return false;
        if (text.size() - i < more) return false;
        for (unsigned j = 0; j < more; ++j) {
            const auto byte = static_cast<unsigned char>(text[i++]);
            if ((byte & 0xC0) != 0x80) return false;
            code = (code << 6) | (byte & 0x3F);
        }
        if (!code || code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
    }
    return true;
}
bool name(std::string_view text) {

    if (text.empty() || text.size() > 256 || !utf8(text)) return false;
    if (text.size() == 4 && (text[0] == 'n' || text[0] == 'N') &&
        (text[1] == 'o' || text[1] == 'O') && (text[2] == 'n' || text[2] == 'N') &&
        (text[3] == 'e' || text[3] == 'E')) return false;
    return std::none_of(text.begin(), text.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':' ||
               c == '.' || c == '"' || c == '\'' || c == '[' || c == ']';
    });
}
bool object_path(std::string_view text) {
    if (text.size() < 4 || text.size() > 2048 || text.front() != '/' || !utf8(text)) return false;
    const auto dot = text.find('.');
    const auto slash = text.rfind('/');
    if (dot == text.npos || dot <= slash + 1 || dot + 1 == text.size() ||
        text.find('.', dot + 1) != text.npos || text.find("//") != text.npos) return false;
    return std::none_of(text.begin(), text.end(), [](unsigned char c) {
        return c <= 0x20 || c == 0x7F || c == '\\' || c == ':' || c == '*' ||
               c == '?' || c == '"' || c == '\'' || c == '[' || c == ']' ||
               c == '(' || c == ')' || c == '<' || c == '>' || c == '|' || c == ',' || c == '=';
    });
}
bool operation(std::string_view text) {
    return text == "update" || text == "upsert" || text == "remove" ||
           (text.starts_with("clone:") && name(text.substr(6)));
}
Result<std::string> property(const PropertyPath& path) {
    auto parsed = PropertyPath::parse(path.string());
    if (!parsed || parsed->segments != path.segments || !utf8(path.string()))
        return make_error(ErrorCode::InvalidArgument, "Malformed table definition property");
    if (path.segments.size() == 1 && path.segments[0].field == "Operation") return path.string();
    if (path.segments.size() == 2 && (path.segments[0].field == "Fields" || path.segments[0].field == "ExpectedFields") &&
        path.segments[1].kind == PathSegment::Kind::Field) return path.string();
    return make_error(ErrorCode::PathNotFound, "Table definition requires Operation, Fields.<rootfield> or ExpectedFields.<rootfield>");
}
struct Payload { std::string guard, text; };
Result<Payload> payload(std::span<const std::uint8_t> bytes, bool is_operation) {
    if (bytes.size() != TablePatchProvider::storage_bytes)
        return make_error(ErrorCode::InvalidArgument, "Table definition payload width mismatch");
    if (bytes[0] == 0) {
        if (!std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == 0; }))
            return make_error(ErrorCode::InvalidArgument, "Inactive table definition must be zero");
        return make_error(ErrorCode::NotFound, "Table definition is inactive");
    }
    if (bytes[0] != 1) return make_error(ErrorCode::InvalidArgument, "Invalid table definition active marker");
    const auto length_at = [&](std::size_t offset) {
        return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
               (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
    };
    const auto guard_length = length_at(1), length = length_at(5);
    constexpr auto offset = TablePatchProvider::value_offset;
    if (guard_length > TablePatchProvider::max_guard_bytes || length > TablePatchProvider::max_payload_bytes ||
        !std::all_of(bytes.begin() + 9 + guard_length, bytes.begin() + offset, [](auto b) { return b == 0; }) ||
        !std::all_of(bytes.begin() + offset + length, bytes.end(), [](auto b) { return b == 0; }))
        return make_error(ErrorCode::InvalidArgument, "Invalid table definition length or padding");
    Payload result{{bytes.begin() + 9, bytes.begin() + 9 + guard_length},
                   {bytes.begin() + offset, bytes.begin() + offset + length}};
    if ((!result.guard.empty() && !object_path(result.guard)) || !utf8(result.text) ||
        (is_operation && !operation(result.text)))
        return make_error(ErrorCode::InvalidArgument, "Invalid table definition text or operation");
    return result;
}
std::string part(std::string_view value) { return std::to_string(value.size()) + ':' + std::string(value); }
std::string ascii_identity(std::string_view value) {
    std::string key(value);
    for (auto& c : key) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return key;
}
}
TablePatchProvider::TablePatchProvider(std::string provider_id) : id_(std::move(provider_id)) {
    if (id_ != "unreal.table_definition" && id_ != "unreal.message_definition")
        throw std::invalid_argument("Unsupported logical definition provider id");
}
Result<Region> TablePatchProvider::bind(const ResourceLocator& locator, const PropertyPath& path) {
    if (locator.provider != id() || !object_path(locator.target) || !name(locator.row) ||
        (!locator.class_guard.empty() && !object_path(locator.class_guard)))
        return make_error(ErrorCode::InvalidArgument, "Table definition requires exact asset/RowStruct paths and a row name");
    auto selected = property(path); if (!selected) return selected.error();

    const auto row_key = part(locator.target) + part(ascii_identity(locator.row));
    const auto key = row_key + part(ascii_identity(selected.value()));
    auto found = slots_.find(key);
    if (found == slots_.end()) {
        if (slots_.size() >= max_slots) return make_error(ErrorCode::LimitExceeded, "1024 stable table definition entries exhausted");
        auto display = row_display_.find(row_key);
        Slot next{locator.target, display == row_display_.end() ? locator.row : display->second,
                  selected.value(), {1, slots_.size() + 1, 0, 1},
                  std::vector<std::uint8_t>(storage_bytes, 0)};
        found = slots_.emplace(key, std::move(next)).first;
        if (display == row_display_.end()) {
            try { row_display_.emplace(row_key, locator.row); }
            catch (...) { slots_.erase(found); throw; }
        }
    }
    Region region;
    region.identity = found->second.identity;
    region.storage_key = key;
    region.byte_mask.assign(storage_bytes, 0xFF);
    region.type.kind = ScalarKind::UnsignedInt; region.type.size = 1;
    region.canonical_path = found->second.property;
    region.provider_certificate = locator.class_guard;
    region.identity_tracking = IdentityTracking::VerifiedStructuralEpoch;
    region.restorable = true; region.whole_value = true;
    return region;
}
Result<const TablePatchProvider::Slot*> TablePatchProvider::slot(const Region& region) const {
    auto found = slots_.find(region.storage_key);
    if (found == slots_.end()) return make_error(ErrorCode::InvalidArgument, "Unknown table definition storage certificate");
    if (region.identity != found->second.identity || region.offset || !region.whole_value ||
        region.byte_sequence || !region.restorable || region.identity_tracking != IdentityTracking::VerifiedStructuralEpoch ||
        region.canonical_path != found->second.property || region.byte_mask.size() != storage_bytes ||
        (!region.provider_certificate.empty() && !object_path(region.provider_certificate)) ||
        !std::all_of(region.byte_mask.begin(), region.byte_mask.end(), [](auto b) { return b == 0xFF; }) ||
        region.type.kind != ScalarKind::UnsignedInt || region.type.size != 1 ||
        region.type.bool_field_mask != 0xFF || region.type.bool_true_bits != 1 || region.type.enum_signed ||
        region.type.enum_numeric_allowed || !region.type.enum_type.empty() || !region.type.enum_entries.empty())
        return make_error(ErrorCode::InvalidArgument, "Invalid table definition storage certificate");
    return &found->second;
}
Error TablePatchProvider::validate(const Region& region) {
    auto checked = slot(region); return checked ? Error{} : checked.error();
}
Result<EncodedValue> TablePatchProvider::prepare(const Region& region, const Value& value) {
    if (auto error = validate(region); !error.ok()) return error;
    if (value.kind() != ValueKind::String) return make_error(ErrorCode::TypeMismatch, "Table definition requires import text string");
    const auto& text = value.as_string();
    if (text.size() > max_payload_bytes) return make_error(ErrorCode::LimitExceeded, "Table import text exceeds 4096 UTF-8 bytes");
    if (!utf8(text) || (region.canonical_path == "Operation" && !operation(text)))
        return make_error(ErrorCode::InvalidArgument, "Invalid table definition UTF-8 or operation");
    EncodedValue result{std::vector<std::uint8_t>(storage_bytes, 0), region.byte_mask};
    result.bytes[0] = 1;
    const auto length = static_cast<std::uint32_t>(text.size());
    const auto guard_length = static_cast<std::uint32_t>(region.provider_certificate.size());
    for (unsigned i = 0; i < 4; ++i) {
        result.bytes[1 + i] = static_cast<std::uint8_t>(guard_length >> (8 * i));
        result.bytes[5 + i] = static_cast<std::uint8_t>(length >> (8 * i));
    }
    std::copy(region.provider_certificate.begin(), region.provider_certificate.end(), result.bytes.begin() + 9);
    std::copy(text.begin(), text.end(), result.bytes.begin() + value_offset);
    return result;
}
Result<Value> TablePatchProvider::decode(const Region& region, std::span<const std::uint8_t> bytes) {
    if (auto error = validate(region); !error.ok()) return error;
    auto text = payload(bytes, region.canonical_path == "Operation");
    if (!text) return text.error();
    return Value::string(std::move(text.value().text));
}
Result<std::vector<std::uint8_t>> TablePatchProvider::read(const Region& region) {
    auto checked = slot(region); if (!checked) return checked.error();
    return checked.value()->storage;
}
Error TablePatchProvider::write(const Region& region, std::span<const std::uint8_t> bytes) {
    if (auto error = validate(region); !error.ok()) return error;
    auto checked = payload(bytes, region.canonical_path == "Operation");
    if (!checked && checked.error().code != ErrorCode::NotFound) return checked.error();
    std::copy(bytes.begin(), bytes.end(), slots_.at(region.storage_key).storage.begin());
    return {};
}
std::vector<TablePatchProvider::Definition> TablePatchProvider::definitions() const {
    std::map<std::pair<std::string, std::string>, Definition> rows;
    for (const auto& [key, slot] : slots_) {
        if (!slot.storage[0]) continue;
        auto text = payload(slot.storage, slot.property == "Operation");

        auto [found, inserted] = rows.try_emplace({slot.asset, ascii_identity(slot.row)});
        auto& row = found->second;
        if (inserted) { row.asset = slot.asset; row.row = slot.row; }
        if (!text->guard.empty() && std::find(row.guards.begin(), row.guards.end(), text->guard) == row.guards.end())
            row.guards.push_back(std::move(text.value().guard));
        if (slot.property == "Operation") row.operation = std::move(text.value().text);
        else if (slot.property.starts_with("ExpectedFields."))
            row.expected_fields.emplace(slot.property.substr(15), std::move(text.value().text));
        else row.fields.emplace(slot.property.substr(7), std::move(text.value().text));
    }
    std::vector<Definition> result; result.reserve(rows.size());
    for (auto& [key, row] : rows) {
        std::sort(row.guards.begin(), row.guards.end());
        if (!row.guards.empty()) row.struct_guard = row.guards.front();
        result.push_back(std::move(row));
    }
    return result;
}
}
