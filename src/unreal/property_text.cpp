#include "unreal/property_text.hpp"
#include <charconv>
#include <cmath>
#include <string_view>
#include <type_traits>
#include <unordered_set>

namespace infinifusion::unreal {
namespace {
using core::ErrorCode;

bool identifier(std::string_view value) {
    const auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (value.empty() || !alpha(value.front())) return false;
    for (const char c : value) if (!alpha(c) && !(c >= '0' && c <= '9')) return false;
    return true;
}

bool enum_symbol(std::string_view value) {
    for (;;) {
        const auto separator = value.find("::");
        if (!identifier(value.substr(0, separator))) return false;
        if (separator == std::string_view::npos) return true;
        value.remove_prefix(separator + 2);
    }
}

bool object_path(std::string_view path) {
    const auto dot = path.find('.');
    if (path.empty() || path.front() != '/' || dot == std::string_view::npos) return false;
    const auto package = path.substr(0, dot);
    const auto object = path.substr(dot + 1);
    if (package.find('/', 1) == std::string_view::npos || package.back() == '/' ||
        package.find("//") != std::string_view::npos || object.empty()) return false;
    constexpr std::string_view package_forbidden = "\\:*?\"<>|' ,.&!~\n\r\t@#(){}[]=;^%$`";
    constexpr std::string_view object_forbidden = "\\*?\"<>|' ,/&!~\n\r\t@#(){}[]=;^%$`";
    for (const auto c : package)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F || package_forbidden.find(c) != std::string_view::npos) return false;
    for (const auto c : object)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F || object_forbidden.find(c) != std::string_view::npos) return false;

    bool component_start = true, had_colon = false;
    for (const auto c : object) {
        if (c == '.' || c == ':') {
            if (component_start || (c == ':' && had_colon) || (c == '.' && !had_colon)) return false;
            had_colon |= c == ':';
            component_start = true;
        } else component_start = false;
    }
    return !component_start;
}

class Writer {
public:
    explicit Writer(const PropertyTextLimits& limits) : limits_(limits) {}

    core::Result<std::string> run(const PropertyValue& value) {

        if (!limits_.max_depth || limits_.max_depth > 64 || !limits_.max_nodes || limits_.max_nodes > 65536 ||
            !limits_.max_input_bytes || limits_.max_input_bytes > 4 * 1024 * 1024 ||
            !limits_.max_output_bytes || limits_.max_output_bytes > 8 * 1024 * 1024)
            return core::make_error(ErrorCode::LimitExceeded, "property text limits exceed codec bounds or are zero");
        if (!write(value, 1)) return error_;
        return std::move(output_);
    }

private:
    bool fail(ErrorCode code, const char* message) {
        error_ = core::make_error(code, message);
        return false;
    }

    bool append(std::string_view value) {
        if (value.size() > limits_.max_output_bytes - output_.size())
            return fail(ErrorCode::LimitExceeded, "property import text exceeds output byte limit");
        output_.append(value);
        return true;
    }

    bool payload(std::string_view value) {
        if (value.size() > limits_.max_input_bytes - input_bytes_)
            return fail(ErrorCode::LimitExceeded, "property values exceed input byte limit");
        input_bytes_ += value.size();

        for (std::size_t i = 0; i < value.size();) {
            const auto lead = static_cast<unsigned char>(value[i++]);
            std::uint32_t code = lead, minimum = 0;
            unsigned continuation = 0;
            if (lead >= 0xC2 && lead <= 0xDF) { code &= 0x1F; continuation = 1; minimum = 0x80; }
            else if (lead >= 0xE0 && lead <= 0xEF) { code &= 0x0F; continuation = 2; minimum = 0x800; }
            else if (lead >= 0xF0 && lead <= 0xF4) { code &= 7; continuation = 3; minimum = 0x10000; }
            else if (lead >= 0x80) return fail(ErrorCode::InvalidArgument, "property value has invalid UTF-8 lead byte");
            if (value.size() - i < continuation) return fail(ErrorCode::InvalidArgument, "property value has truncated UTF-8");
            for (unsigned j = 0; j < continuation; ++j) {
                const auto byte = static_cast<unsigned char>(value[i++]);
                if ((byte & 0xC0) != 0x80) return fail(ErrorCode::InvalidArgument, "property value has invalid UTF-8 continuation");
                code = (code << 6) | (byte & 0x3F);
            }
            if (!code || code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
                return fail(ErrorCode::InvalidArgument, "property value has invalid UTF-8 scalar or embedded NUL");
        }
        return true;
    }

    bool quote(std::string_view value) {
        return payload(value) && quote_validated(value);
    }

    bool quote_validated(std::string_view value) {
        if (!append("\"")) return false;
        for (const char c : value) {

            switch (c) {
            case '\\': if (!append("\\\\")) return false; break;
            case '"': if (!append("\\\"")) return false; break;
            case '\'': if (!append("\\'")) return false; break;
            case '\n': if (!append("\\n")) return false; break;
            case '\r': if (!append("\\r")) return false; break;
            case '\t': if (!append("\\t")) return false; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
                    const auto byte = static_cast<unsigned char>(c);
                    const char escaped[]{'\\', static_cast<char>('0' + (byte >> 6)),
                        static_cast<char>('0' + ((byte >> 3) & 7)), static_cast<char>('0' + (byte & 7))};
                    if (!append(std::string_view(escaped, sizeof escaped))) return false;
                } else if (!append(std::string_view(&c, 1))) return false;
            }
        }
        return append("\"");
    }

    template<class T> bool number(T value) {
        char buffer[1024];
        std::to_chars_result result;
        if constexpr (std::is_same_v<T, double>) {
            if (!std::isfinite(value)) return fail(ErrorCode::NotFinite, "property floating value must be finite");

            result = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::fixed);
        } else result = std::to_chars(buffer, buffer + sizeof buffer, value);
        if (result.ec != std::errc{}) return fail(ErrorCode::Internal, "property number formatting failed");
        return append(std::string_view(buffer, static_cast<std::size_t>(result.ptr - buffer)));
    }

    bool reference(const std::optional<std::string>& path) {
        if (!path) return append("None");
        if (!payload(*path)) return false;
        if (!object_path(*path)) return fail(ErrorCode::InvalidArgument, "reference requires a qualified object path without engine grammar");
        return quote_validated(*path);
    }

    bool children(std::size_t count, std::size_t multiplier = 1) {
        if (count > (limits_.max_nodes - nodes_) / multiplier)
            return fail(ErrorCode::LimitExceeded, "property values exceed node limit");
        return true;
    }

    bool sequence(const std::vector<PropertyValue>& elements, std::size_t depth, bool array) {
        if (!children(elements.size())) return false;

        if (array && elements.empty()) return true;
        if (!append("(")) return false;
        bool first = true;
        for (const auto& item : elements) {
            if (!first && !append(",")) return false;
            first = false;
            if (!write(item, depth + 1)) return false;
        }
        return append(")");
    }

    bool structure(const PropertyStruct& value, std::size_t depth) {
        if (!children(value.fields.size()) || !append("(")) return false;
        std::unordered_set<std::string> names;
        bool first = true;
        for (const auto& [name, field] : value.fields) {
            if (!payload(name)) return false;
            if (!identifier(name)) return fail(ErrorCode::InvalidArgument, "struct field must be an ASCII identifier");
            auto folded = name;
            for (char& c : folded) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (!names.insert(std::move(folded)).second)
                return fail(ErrorCode::InvalidArgument, "struct field is duplicated (case-insensitive)");
            if (!first && !append(",")) return false;
            first = false;
            if (!append(name) || !append("=") || !write(field, depth + 1)) return false;
        }
        return append(")");
    }

    bool map(const PropertyMap& value, std::size_t depth) {
        if (!children(value.entries.size(), 2) || !append("(")) return false;
        bool first = true;
        for (const auto& [key, item] : value.entries) {
            if (!first && !append(",")) return false;
            first = false;
            if (!append("(") || !write(key, depth + 1) || !append(",") ||
                !write(item, depth + 1) || !append(")")) return false;
        }
        return append(")");
    }

    bool write(const PropertyValue& value, std::size_t depth) {
        if (depth > limits_.max_depth) return fail(ErrorCode::LimitExceeded, "property values exceed nesting limit");
        if (nodes_ == limits_.max_nodes) return fail(ErrorCode::LimitExceeded, "property values exceed node limit");
        ++nodes_;
        return std::visit([&](const auto& item) -> bool {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, bool>) return append(item ? "True" : "False");
            else if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, std::uint64_t> || std::is_same_v<T, double>) return number(item);
            else if constexpr (std::is_same_v<T, PropertyString> || std::is_same_v<T, PropertyName>) return quote(item.value);
            else if constexpr (std::is_same_v<T, PropertyEnum>) {
                if (!payload(item.symbol)) return false;
                if (!enum_symbol(item.symbol)) return fail(ErrorCode::InvalidArgument, "enum symbol must contain identifiers separated by ::");
                return quote_validated(item.symbol);
            } else if constexpr (std::is_same_v<T, PropertyObjectReference> || std::is_same_v<T, PropertySoftReference>) return reference(item.path);
            else if constexpr (std::is_same_v<T, PropertyInvariantText>) return append("INVTEXT(") && quote(item.value) && append(")");
            else if constexpr (std::is_same_v<T, PropertyLocalizedText>) {
                if (item.key.empty()) return fail(ErrorCode::InvalidArgument, "localized text requires a nonempty stable key");
                return append("NSLOCTEXT(") && quote(item.name_space) && append(",") && quote(item.key) && append(",") && quote(item.source) && append(")");
            } else if constexpr (std::is_same_v<T, PropertyStruct>) return structure(item, depth);
            else if constexpr (std::is_same_v<T, PropertyArray>) return sequence(item.elements, depth, true);
            else if constexpr (std::is_same_v<T, PropertySet>) return sequence(item.elements, depth, false);
            else return map(item, depth);
        }, value.value);
    }

    const PropertyTextLimits& limits_;
    std::size_t nodes_{}, input_bytes_{};
    std::string output_;
    core::Error error_;
};
}

core::Result<std::string> to_property_text(const PropertyValue& value, const PropertyTextLimits& limits) {
    return Writer(limits).run(value);
}

}
