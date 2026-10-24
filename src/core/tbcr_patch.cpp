#include "core/tbcr_patch.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>
#include <tuple>

namespace infinifusion::core {
namespace {
Result<std::uint32_t> decimal(std::string_view text) {
    std::uint32_t number{};
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return make_error(ErrorCode::InvalidArgument, "TBCR selector requires decimal digits");
    auto result = std::from_chars(text.data(), text.data() + text.size(), number);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return make_error(ErrorCode::OutOfRange, "TBCR selector overflow");
    return number;
}
Result<std::array<std::uint32_t, 3>> selectors(std::string_view row) {
    std::array<std::uint32_t, 3> values{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto colon = row.find(':');
        if ((i < 2 && colon == row.npos) || (i == 2 && colon != row.npos))
            return make_error(ErrorCode::InvalidArgument, "TBCR row requires table:record:stride");
        auto number = decimal(row.substr(0, colon)); if (!number) return number.error();
        values[i] = number.value();
        if (i < 2) row.remove_prefix(colon + 1);
    }
    if (!values[2] || values[2] > 4096)
        return make_error(ErrorCode::OutOfRange, "TBCR record stride must be 1..4096");
    return values;
}
Result<std::pair<std::uint32_t, std::uint32_t>> span(const PropertyPath& path) {
    const auto& s = path.segments;
    if (s.size() != 3 || s[0].kind != PathSegment::Kind::Field || s[0].field != "Bytes" ||
        s[1].kind != PathSegment::Kind::Index || s[2].kind != PathSegment::Kind::Index ||
        !s[1].field.empty() || !s[2].field.empty() || s[0].index != 0 || !s[2].index)
        return make_error(ErrorCode::PathNotFound, "TBCR property requires Bytes[offset][length]");
    return std::pair{s[1].index, s[2].index};
}
}
Result<Region> TbcrPatchProvider::bind(const ResourceLocator& locator, const PropertyPath& path) {
    return bind_owned(locator, path, {});
}
Result<Region> TbcrPatchProvider::bind_owned(const ResourceLocator& locator, const PropertyPath& path, std::string_view owner_id) {
    if (locator.provider != id() || locator.target.empty() || locator.target.size() > 2048 ||
        locator.target.find('\0') != std::string::npos || !locator.class_guard.empty())
        return make_error(ErrorCode::InvalidArgument, "TBCR requires an exact asset identity and no class guard");
    const bool append = locator.row.starts_with("append:");
    std::uint32_t table{}, record{}, stride{}, offset{}, length{};
    std::string declaration_id;
    if (append) {
        if (owner_id.empty() || owner_id.find('\0') != owner_id.npos)
            return make_error(ErrorCode::InvalidArgument, "TBCR append requires authoritative owner identity");
        auto row = std::string_view(locator.row).substr(7);
        const auto first = row.find(':');
        if (first == row.npos) return make_error(ErrorCode::InvalidArgument, "append requires table:stride:id");
        auto t = decimal(row.substr(0, first)); if (!t) return t.error();
        row.remove_prefix(first + 1);
        const auto second = row.find(':');
        if (second == row.npos) return make_error(ErrorCode::InvalidArgument, "append requires table:stride:id");
        auto width = decimal(row.substr(0, second)); if (!width) return width.error();
        declaration_id = row.substr(second + 1);
        if (declaration_id.empty() || declaration_id.size() > 128 || declaration_id.find('\0') != declaration_id.npos)
            return make_error(ErrorCode::InvalidArgument, "append id must be 1..128 bytes");
        table = t.value(); stride = width.value(); length = stride;
        if (!stride || stride > 4096) return make_error(ErrorCode::OutOfRange, "TBCR record stride must be 1..4096");
        if (path.string() != "Record" || path.segments.size() != 1 || path.segments[0].kind != PathSegment::Kind::Field || path.segments[0].index != 0)
            return make_error(ErrorCode::PathNotFound, "TBCR append requires complete Record property");
    } else {
        auto ids = selectors(locator.row); if (!ids) return ids.error();
        auto selected = span(path); if (!selected) return selected.error();
        table = ids.value()[0]; record = ids.value()[1]; stride = ids.value()[2];
        offset = selected->first; length = selected->second;
        if (offset >= stride || length > stride - offset)
            return make_error(ErrorCode::OutOfRange, "TBCR span exceeds record stride");
    }
    const auto part = [](std::string_view value) { return std::to_string(value.size()) + ':' + std::string(value); };

    const auto key = part(locator.target) + (append ? ":append:" + std::to_string(table) + ':' + part(owner_id) + ':' + part(declaration_id)
                                                   : ":" + std::to_string(table) + ':' + std::to_string(record));
    auto found = slots_.find(key);
    if (found != slots_.end() && found->second.record.record_size != stride)
        return make_error(ErrorCode::InvalidArgument, "TBCR record stride disagrees with stable storage certificate");
    if (found == slots_.end()) {
        if (slots_.size() >= 128) return make_error(ErrorCode::LimitExceeded, "128 stable TBCR records exhausted");
        Slot next;
        next.record = {locator.target, table, record, stride, {}, {}, append, append ? std::string(owner_id) : std::string{}, declaration_id};
        next.storage.assign(2 * stride, 0);
        next.identity = {1, slots_.size() + 1, 0, 1};
        found = slots_.emplace(key, std::move(next)).first;
    }
    Region region;
    region.identity = found->second.identity; region.storage_key = key;
    region.offset = 2ull * offset; region.byte_mask.assign(2ull * length, 0xFF);
    region.type.kind = ScalarKind::UnsignedInt; region.type.size = 1;
    region.canonical_path = append ? "Record" : "Bytes[" + std::to_string(offset) + "][" + std::to_string(length) + ']';
    region.identity_tracking = IdentityTracking::VerifiedStructuralEpoch;
    region.restorable = true; region.byte_sequence = true;
    return region;
}
Result<const TbcrPatchProvider::Slot*> TbcrPatchProvider::slot(const Region& region) const {
    auto found = slots_.find(region.storage_key);
    if (found == slots_.end()) return make_error(ErrorCode::InvalidArgument, "Unknown TBCR storage certificate");
    const auto& owned = found->second;
    auto path = PropertyPath::parse(region.canonical_path);
    if (!path) return path.error();
    auto selected = owned.record.append ? Result<std::pair<std::uint32_t, std::uint32_t>>(std::pair{0u, owned.record.record_size}) : span(path.value()); if (!selected) return selected.error();
    if (owned.record.append && region.canonical_path != "Record") return make_error(ErrorCode::InvalidArgument, "append certificate requires Record");
    const auto [offset, length] = selected.value();
    const auto width = region.byte_mask.size();
    const auto begin = 2ull * offset, end = begin + 2ull * length;
    if (region.identity != owned.identity || region.whole_value || !region.byte_sequence || !region.restorable ||
        region.identity_tracking != IdentityTracking::VerifiedStructuralEpoch ||
        region.type.kind != ScalarKind::UnsignedInt || region.type.size != 1 ||
        offset >= owned.record.record_size || length > owned.record.record_size - offset ||
        !width || region.offset < begin || region.offset >= end || width > end - region.offset ||
        !std::all_of(region.byte_mask.begin(), region.byte_mask.end(), [](auto b) { return b != 0; }))
        return make_error(ErrorCode::InvalidArgument, "Invalid TBCR logical storage certificate");
    return &owned;
}
Error TbcrPatchProvider::validate(const Region& region) {
    auto checked = slot(region); return checked ? Error{} : checked.error();
}
Result<EncodedValue> TbcrPatchProvider::prepare(const Region& region, const Value& value) {
    if (auto error = validate(region); !error.ok()) return error;
    auto selected = slots_.at(region.storage_key).record.append ? std::pair{0u, slots_.at(region.storage_key).record.record_size} : span(PropertyPath::parse(region.canonical_path).value()).value();
    if (region.offset != 2ull * selected.first || region.byte_mask.size() != 2ull * selected.second ||
        !std::all_of(region.byte_mask.begin(), region.byte_mask.end(), [](auto b) { return b == 0xFF; }))
        return make_error(ErrorCode::InvalidArgument, "TBCR preparation requires complete logical pairs");
    if (value.kind() != ValueKind::Bytes) return make_error(ErrorCode::TypeMismatch, "TBCR span requires bytes input");
    if (value.as_bytes().size() != selected.second) return make_error(ErrorCode::InvalidArgument, "TBCR bytes length mismatch");
    EncodedValue result{{}, region.byte_mask}; result.bytes.reserve(region.byte_mask.size());
    for (auto byte : value.as_bytes()) { result.bytes.push_back(1); result.bytes.push_back(byte); }
    return result;
}
Result<Value> TbcrPatchProvider::decode(const Region& region, std::span<const std::uint8_t> copied) {
    if (auto error = validate(region); !error.ok()) return error;
    auto selected = slots_.at(region.storage_key).record.append ? std::pair{0u, slots_.at(region.storage_key).record.record_size} : span(PropertyPath::parse(region.canonical_path).value()).value();
    if (region.offset != 2ull * selected.first || copied.size() != region.byte_mask.size() ||
        copied.size() != 2ull * selected.second)
        return make_error(ErrorCode::InvalidArgument, "TBCR decoding requires complete logical pairs");
    std::vector<std::uint8_t> result; result.reserve(selected.second);
    for (std::size_t i = 0; i < copied.size(); i += 2) {
        if (copied[i] != 1) return make_error(ErrorCode::NotFound, "TBCR span has inactive bytes");
        result.push_back(copied[i + 1]);
    }
    return Value::bytes(std::move(result));
}
Result<std::vector<std::uint8_t>> TbcrPatchProvider::read(const Region& region) {
    auto checked = slot(region); if (!checked) return checked.error();
    const auto& storage = checked.value()->storage;
    return std::vector<std::uint8_t>(storage.begin() + region.offset, storage.begin() + region.offset + region.byte_mask.size());
}
Error TbcrPatchProvider::write(const Region& region, std::span<const std::uint8_t> copied) {
    auto checked = slot(region); if (!checked) return checked.error();
    if (copied.size() != region.byte_mask.size()) return make_error(ErrorCode::InvalidArgument, "TBCR write width mismatch");
    auto& storage = slots_.at(region.storage_key).storage;

    for (std::size_t i = 0; i < copied.size(); ++i) {
        auto& byte = storage[region.offset + i]; const auto mask = region.byte_mask[i];
        byte = static_cast<std::uint8_t>((byte & ~mask) | (copied[i] & mask));
    }
    return {};
}
std::vector<TbcrPatchProvider::Definition> TbcrPatchProvider::definitions() const {
    std::vector<Definition> result;
    for (const auto& [key, slot] : slots_) {
        Definition copied = slot.record; bool active = false;
        copied.bytes.reserve(copied.record_size); copied.active_mask.reserve(copied.record_size);
        for (std::size_t i = 0; i < slot.storage.size(); i += 2) {
            copied.active_mask.push_back(slot.storage[i] == 1 ? 0xFF : 0);
            copied.bytes.push_back(slot.storage[i + 1]); active |= slot.storage[i] == 1;
        }
        if (active && copied.append && !std::all_of(copied.active_mask.begin(), copied.active_mask.end(), [](auto byte) { return byte == 0xFF; }))
            throw std::runtime_error("TBCR append pending: active record is incomplete");
        if (active) result.push_back(std::move(copied));
    }
    std::sort(result.begin(), result.end(), [](const Definition& a, const Definition& b) {
        return std::tie(a.owner_id, a.append_id, a.table_index, a.asset, a.record_index) < std::tie(b.owner_id, b.append_id, b.table_index, b.asset, b.record_index);
    });
    return result;
}
}
