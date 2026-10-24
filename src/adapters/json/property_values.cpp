#include "adapters/json/property_values.hpp"
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <set>
namespace infinifusion::json_mods {
namespace {
using J = smtvv::json::Value;
const J& member(const J& value, const char* key) {
    const auto* result = value.find(key);
    if (!result) throw std::runtime_error(std::string("missing property value member: ") + key);
    return *result;
}
std::string string(const J& value) {
    if (!value.string()) throw std::runtime_error("property value requires string");
    return *value.string();
}
void keys(const J& value, std::initializer_list<std::string_view> allowed) {
    std::set<std::string> seen;
    for (const auto& [key, ignored] : *value.object()) {
        if (!seen.insert(key).second) throw std::runtime_error("duplicate property value member: " + key);
        bool match = false;
        for (auto name : allowed) match |= key == name;
        if (!match) throw std::runtime_error("unknown property value member: " + key);
    }
}
std::optional<std::string> reference(const J& value) {
    if (value.is_null()) return {};
    return string(value);
}
}
unreal::PropertyValue property_value(const J& value, unsigned depth) {
    using namespace unreal;
    if (depth >= 32) throw std::runtime_error("property value nesting exceeds32");
    if (const auto* boolean = value.boolean()) return {{*boolean}};
    if (const auto* text = value.string()) return {{PropertyString{*text}}};
    if (const auto* number = value.number()) {
        const auto* begin = number->text.data(); const auto* end = begin + number->text.size();
        if (number->text.find_first_of(".eE") != std::string::npos) {
            double n{}; const auto r = std::from_chars(begin, end, n);
            if (r.ec != std::errc{} || r.ptr != end || !std::isfinite(n)) throw std::runtime_error("invalid finite property number");
            return {{n}};
        }
        if (number->text.starts_with('-')) {
            std::int64_t n{}; const auto r = std::from_chars(begin, end, n);
            if (r.ec != std::errc{} || r.ptr != end) throw std::runtime_error("property signed integer overflow");
            return {{n}};
        }
        std::uint64_t n{}; const auto r = std::from_chars(begin, end, n);
        if (r.ec != std::errc{} || r.ptr != end) throw std::runtime_error("property unsigned integer overflow");
        return {{n}};
    }
    if (const auto* array = value.array()) {
        PropertyArray out;
        if (array->size() > 4096) throw std::runtime_error("property array exceeds4096 elements");
        for (const auto& item : *array) out.elements.push_back(property_value(item, depth + 1));
        return {{std::move(out)}};
    }
    if (!value.object()) throw std::runtime_error("null property needs a typed reference wrapper");
    const auto* tag = value.find("$type");
    if (!tag) {
        PropertyStruct out;
        for (const auto& [name, field] : *value.object()) out.fields.emplace_back(name, property_value(field, depth + 1));
        return {{std::move(out)}};
    }
    const auto type = string(*tag);
    if (type == "localized_text") {
        keys(value, {"$type", "namespace", "key", "source"});
        return {{PropertyLocalizedText{string(member(value,"namespace")),string(member(value,"key")),string(member(value,"source"))}}};
    }
    keys(value, {"$type", "value"});
    const auto& input = member(value, "value");
    if (type == "name") return {{PropertyName{string(input)}}};
    if (type == "string") return {{PropertyString{string(input)}}};
    if (type == "enum") return {{PropertyEnum{string(input)}}};
    if (type == "text") return {{PropertyInvariantText{string(input)}}};
    if (type == "object" || type == "class") return {{PropertyObjectReference{reference(input)}}};
    if (type == "soft_object" || type == "soft_class") return {{PropertySoftReference{reference(input)}}};
    if (type == "struct") {
        if (!input.object() || input.find("$type")) throw std::runtime_error("struct value requires field object");
        return property_value(input, depth + 1);
    }
    if (type == "array" || type == "set") {
        auto nested = property_value(input, depth + 1);
        auto* array = std::get_if<PropertyArray>(&nested.value);
        if (!array) throw std::runtime_error("array/set requires array value");
        if (type == "array") return nested;
        return {{PropertySet{std::move(array->elements)}}};
    }
    if (type == "map") {
        if (!input.array() || input.array()->size() > 2048) throw std::runtime_error("map requires bounded [key,value] pairs");
        PropertyMap out;
        for (const auto& pair : *input.array()) {
            if (!pair.array() || pair.array()->size() != 2) throw std::runtime_error("map entry requires [key,value]");
            out.entries.emplace_back(property_value((*pair.array())[0],depth+1),property_value((*pair.array())[1],depth+1));
        }
        return {{std::move(out)}};
    }
    throw std::runtime_error("unknown property value type: " + type);
}
std::string property_text(const J& value) {
    unreal::PropertyTextLimits limits; limits.max_output_bytes = 4096;
    auto result = unreal::to_property_text(property_value(value), limits);
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result.value());
}
}
