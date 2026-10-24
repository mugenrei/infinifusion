#include "unreal/property_import.hpp"
#include <stdexcept>
#include <unordered_set>

namespace infinifusion::unreal {
namespace {
constexpr std::size_t max_bytes = 65536, max_depth = 32, max_nodes = 4096;

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }
[[noreturn]] void limit(const char* reason) { throw std::length_error(reason); }
bool whitespace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
std::string_view trim(std::string_view text) {
    while (!text.empty() && whitespace(text.front())) text.remove_prefix(1);
    while (!text.empty() && whitespace(text.back())) text.remove_suffix(1);
    return text;
}
bool identifier(std::string_view text) {
    const auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (text.empty() || !alpha(text.front())) return false;
    for (char c : text) if (!alpha(c) && !(c >= '0' && c <= '9')) return false;
    return true;
}

void scan(std::string_view text) {
    if (text.size() > max_bytes) limit("property import exceeds 65536 input bytes");
    std::size_t depth{};
    bool quoted = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (!c) invalid("property import contains NUL");
        if (quoted) {
            if (c == '\n' || c == '\r') invalid("property import contains a raw quoted newline");
            if (c == '\\') {
                if (++i == text.size() || !text[i]) invalid("property import ends in a quoted escape");
                if (text[i] == '\n' || text[i] == '\r') invalid("property import contains a raw quoted newline");
            } else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '(') { if (++depth > max_depth) limit("property import nesting exceeds 32"); }
        else if (c == ')') { if (!depth) invalid("property import has an unmatched closing parenthesis"); --depth; }
    }
    if (quoted) invalid("property import has an unterminated quote");
    if (depth) invalid("property import has an unmatched opening parenthesis");
}

std::vector<std::string_view> split(std::string_view body, char delimiter) {
    std::vector<std::string_view> result;
    std::size_t start{}, depth{};
    bool quoted = false;
    for (std::size_t i = 0; i < body.size(); ++i) {
        char c = body[i];
        if (quoted) {
            if (c == '\\') ++i;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (c == delimiter && !depth) {
            if (result.size() == max_nodes) limit("property import tuple exceeds 4096 entries");
            result.push_back(trim(body.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (result.size() == max_nodes) limit("property import tuple exceeds 4096 entries");
    result.push_back(trim(body.substr(start)));
    return result;
}

std::string_view tuple(std::string_view text) {
    if (text.empty() || text.front() != '(') invalid("property import requires a tuple");

    std::size_t depth{};
    bool quoted = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quoted) { if (c == '\\') ++i; else if (c == '"') quoted = false; }
        else if (c == '"') quoted = true;
        else if (c == '(') ++depth;
        else if (c == ')' && !--depth && i != text.size() - 1)
            invalid("property import has trailing tuple tokens");
    }
    return trim(text.substr(1, text.size() - 2));
}

void literal(std::string_view text, bool native_struct) {
    if (text.empty()) invalid("property import has an empty scalar/native literal");
    if (text.front() == '"') {
        for (std::size_t i = 1; i < text.size(); ++i) {
            if (text[i] == '\\') ++i;
            else if (text[i] == '"') {
                if (i != text.size() - 1) invalid("property import has trailing quoted tokens");
                return;
            }
        }
    }
    const auto open = text.find('(');
    if (open != std::string_view::npos) {

        if (!identifier(trim(text.substr(0, open)))) invalid("property import has an invalid literal call");
        (void)tuple(text.substr(open));
        return;
    }
    if (!native_struct)
        for (char c : text)
            if (whitespace(c) || c == ',' || c == '=' || c == '"')
                invalid("property import has multiple scalar tokens");
}

class Prepare {
public:
    explicit Prepare(const ImportCallbacks& callbacks) : callbacks_(callbacks) {}
    std::vector<std::string> run(std::uintptr_t property, std::string_view text) {
        scan(text);
        if (!callbacks_.describe) invalid("property import requires describe callback");
        auto result = property_values(property, trim(text), 1);
        std::size_t bytes{};
        for (const auto& element : result) {
            if (element.size() > max_bytes - bytes) limit("property import exceeds 65536 output bytes");
            bytes += element.size();
        }
        return result;
    }

private:
    void node(std::size_t depth) {
        if (depth > max_depth) limit("property import nesting exceeds 32");
        if (++nodes_ > max_nodes) limit("property import values exceed 4096 nodes");
    }
    ImportSchema schema(std::uintptr_t property) {
        if (!property) invalid("property import requires a valid property token");
        auto result = callbacks_.describe(property);
        if (!result.fixed_count) invalid("property import static count is zero");
        if (result.fixed_count > max_nodes) limit("property import static count exceeds 4096");
        switch (result.kind) {
        case ImportKind::Scalar: case ImportKind::Struct: break;
        case ImportKind::Array: case ImportKind::Set:
            if (!result.inner) invalid("property import container has no element schema");
            break;
        case ImportKind::Map:
            if (!result.key || !result.value) invalid("property import map has no key/value schema");
            break;
        default: invalid("property import has an invalid schema kind");
        }
        return result;
    }
    static void append(std::string& target, std::string_view text) {
        if (text.size() > max_bytes - target.size()) limit("property import exceeds 65536 output bytes");
        target.append(text);
    }

    std::vector<std::string> property_values(std::uintptr_t property, std::string_view text, std::size_t depth) {
        auto type = schema(property);
        if (type.fixed_count == 1) return {element(property, type, text, depth)};
        node(depth);
        auto entries = split(tuple(text), ',');
        if (entries.size() != type.fixed_count) invalid("property import static cardinality differs from reflection");
        std::vector<std::string> result;
        result.reserve(entries.size());
        for (auto entry : entries) result.push_back(element(property, type, entry, depth + 1));
        return result;
    }

    std::string nested(std::uintptr_t property, std::string_view text, std::size_t depth) {
        auto result = property_values(property, text, depth);
        if (result.size() == 1) return std::move(result.front());

        std::string joined{"("};
        for (std::size_t i = 0; i < result.size(); ++i) {
            if (i) append(joined, ",");
            append(joined, result[i]);
        }
        append(joined, ")");
        return joined;
    }

    std::string structure(std::uintptr_t property, std::string_view text, std::size_t depth) {
        if (text.empty() || text.front() != '(') { literal(text, true); return std::string(text); }
        auto body = tuple(text);
        if (body.empty()) return "()";
        if (!callbacks_.member) invalid("property import struct requires member callback");
        std::unordered_set<std::string> names;
        std::unordered_set<std::uintptr_t> properties;
        std::string result{"("};
        bool first = true;
        for (auto assignment : split(body, ',')) {
            auto equal = assignment.find('=');
            if (equal == std::string_view::npos || !identifier(trim(assignment.substr(0, equal))))
                invalid("property import struct requires named field assignments");
            auto name = std::string(trim(assignment.substr(0, equal)));
            auto folded = name;
            for (char& c : folded) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (!names.insert(std::move(folded)).second) invalid("property import struct field is duplicated");
            auto field = callbacks_.member(property, name);
            if (!field) invalid("property import struct field is unknown");
            if (!properties.insert(field).second) invalid("property import struct field aliases a duplicate property");
            auto values = property_values(field, trim(assignment.substr(equal + 1)), depth + 1);
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (!first) append(result, ",");
                first = false;
                append(result, name);
                if (values.size() > 1) { append(result, "("); append(result, std::to_string(i)); append(result, ")"); }
                append(result, "=");
                append(result, values[i]);
            }
        }
        append(result, ")");
        return result;
    }

    std::string sequence(const ImportSchema& type, std::string_view text, std::size_t depth) {
        if (type.kind == ImportKind::Array && text.empty()) return {};
        auto body = tuple(text);
        if (type.kind == ImportKind::Set && body.empty()) return "()";
        std::string result{"("};
        auto entries = split(body, ',');
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i) append(result, ",");
            append(result, nested(type.inner, entries[i], depth + 1));
        }
        append(result, ")");
        return result;
    }

    std::string map(const ImportSchema& type, std::string_view text, std::size_t depth) {
        auto body = tuple(text);
        if (body.empty()) return "()";
        std::string result{"("};
        bool first = true;
        for (auto pair : split(body, ',')) {
            auto entries = split(tuple(pair), ',');
            if (entries.size() != 2) invalid("property import map pair requires key and value");
            if (!first) append(result, ",");
            first = false;
            append(result, "("); append(result, nested(type.key, entries[0], depth + 1));
            append(result, ","); append(result, nested(type.value, entries[1], depth + 1)); append(result, ")");
        }
        append(result, ")");
        return result;
    }

    std::string element(std::uintptr_t property, const ImportSchema& type, std::string_view text, std::size_t depth) {
        node(depth);
        switch (type.kind) {
        case ImportKind::Scalar:
            literal(text, false);
            if (callbacks_.scalar_validation) callbacks_.scalar_validation(property, text);
            return std::string(text);
        case ImportKind::Struct: return structure(property, text, depth);
        case ImportKind::Array: case ImportKind::Set: return sequence(type, text, depth);
        case ImportKind::Map: return map(type, text, depth);
        }
        invalid("property import has an invalid schema kind");
    }

    const ImportCallbacks& callbacks_;
    std::size_t nodes_{};
};
}

std::vector<std::string> prepare_property_import(std::uintptr_t property, std::string_view text, const ImportCallbacks& callbacks) {
    return Prepare(callbacks).run(property, text);
}

}
