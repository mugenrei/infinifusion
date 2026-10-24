#pragma once

#include "core/value.hpp"
#include <memory>
#include <string>
#include <vector>

namespace infinifusion::core {

struct PathSegment {
    enum class Kind { Field, Index } kind = Kind::Field;
    std::string field;
    std::uint32_t index = 0;
    bool operator==(const PathSegment&) const = default;
};

struct PropertyPath {
    std::vector<PathSegment> segments;
    static Result<PropertyPath> parse(std::string_view text);
    std::string string() const;
};

struct ResourceLocator {
    std::string provider;
    std::string target;
    std::string row;
    std::string class_guard;
};

enum class IdentityTracking { ObservedOnly, ObjectSerial, VerifiedStructuralEpoch };

struct TargetIdentity {
    std::uint64_t epoch = 0;
    std::uint64_t object_index = 0;
    std::uint64_t object_serial = 0;
    std::uint64_t structural_epoch = 0;
    bool operator==(const TargetIdentity&) const = default;
};

struct Region {
    TargetIdentity identity;

    std::string storage_key;
    std::uint64_t offset = 0;
    std::vector<std::uint8_t> byte_mask;
    FieldType type;
    std::string canonical_path;

    std::string provider_certificate;
    IdentityTracking identity_tracking = IdentityTracking::ObservedOnly;
    bool restorable = false;

    bool whole_value = false;

    bool byte_sequence = false;
};

class Provider {
public:
    virtual ~Provider() = default;
    virtual std::string_view id() const noexcept = 0;

    virtual bool change_driven() const noexcept { return false; }
    virtual Result<Region> bind(const ResourceLocator&, const PropertyPath&) = 0;
    virtual Result<Region> bind_owned(const ResourceLocator& locator, const PropertyPath& path,
                                      std::string_view) { return bind(locator, path); }

    virtual Error validate(const Region&) = 0;
    virtual Result<EncodedValue> prepare(const Region& region, const Value& value) {
        return encode_value(value, region.type);
    }
    virtual Result<Value> decode(const Region& region, std::span<const std::uint8_t> bytes) {
        return decode_value(bytes, region.type);
    }

    virtual Result<std::vector<std::uint8_t>> read(const Region&) = 0;

    virtual Error write(const Region&, std::span<const std::uint8_t>) = 0;
};

}
