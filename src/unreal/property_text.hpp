#pragma once

#include "core/result.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace infinifusion::unreal {

struct PropertyValue;
struct PropertyString { std::string value; };
struct PropertyName { std::string value; };
struct PropertyEnum { std::string symbol; };

struct PropertyObjectReference { std::optional<std::string> path; };
struct PropertySoftReference { std::optional<std::string> path; };
struct PropertyInvariantText { std::string value; };
struct PropertyLocalizedText { std::string name_space, key, source; };
struct PropertyStruct { std::vector<std::pair<std::string, PropertyValue>> fields; };
struct PropertyArray { std::vector<PropertyValue> elements; };
struct PropertySet { std::vector<PropertyValue> elements; };
struct PropertyMap { std::vector<std::pair<PropertyValue, PropertyValue>> entries; };

struct PropertyValue {
    using Storage = std::variant<bool, std::int64_t, std::uint64_t, double,
        PropertyString, PropertyName, PropertyEnum, PropertyObjectReference,
        PropertySoftReference, PropertyInvariantText, PropertyLocalizedText,
        PropertyStruct, PropertyArray, PropertySet, PropertyMap>;
    Storage value{false};
};

struct PropertyTextLimits {

    std::size_t max_depth{32};
    std::size_t max_nodes{4096};
    std::size_t max_input_bytes{256 * 1024};
    std::size_t max_output_bytes{1024 * 1024};
};

[[nodiscard]] core::Result<std::string> to_property_text(
    const PropertyValue& value, const PropertyTextLimits& limits = {});

}
