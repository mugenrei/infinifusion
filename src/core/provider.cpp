#include "core/provider.hpp"
#include <limits>

namespace infinifusion::core {
namespace {
bool name_start(unsigned char c) { return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80; }
bool name_continue(unsigned char c) { return name_start(c) || (c >= '0' && c <= '9'); }
}

Result<PropertyPath> PropertyPath::parse(std::string_view text) {
    if (text.empty() || text.size() > 1024) return make_error(ErrorCode::InvalidArgument, "Path length must be 1..1024");
    PropertyPath path;
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (!name_start(static_cast<unsigned char>(text[pos]))) return make_error(ErrorCode::InvalidArgument, "Expected property name");
        const auto start = pos++;
        while (pos < text.size() && name_continue(static_cast<unsigned char>(text[pos]))) ++pos;
        path.segments.push_back({PathSegment::Kind::Field, std::string(text.substr(start, pos - start)), 0});
        while (pos < text.size() && text[pos] == '[') {
            ++pos;
            if (pos == text.size() || text[pos] < '0' || text[pos] > '9') return make_error(ErrorCode::InvalidArgument, "Expected unsigned array index");
            std::uint32_t index = 0;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
                const unsigned digit = static_cast<unsigned>(text[pos++] - '0');
                if (index > (std::numeric_limits<std::uint32_t>::max() - digit) / 10)
                    return make_error(ErrorCode::OutOfRange, "Array index overflow");
                index = index * 10 + digit;
            }
            if (pos == text.size() || text[pos++] != ']') return make_error(ErrorCode::InvalidArgument, "Expected closing index bracket");
            path.segments.push_back({PathSegment::Kind::Index, {}, index});
            if (path.segments.size() > 64) return make_error(ErrorCode::LimitExceeded, "Path exceeds 64 segments");
        }
        if (path.segments.size() > 64) return make_error(ErrorCode::LimitExceeded, "Path exceeds 64 segments");
        if (pos == text.size()) break;
        if (text[pos++] != '.' || pos == text.size()) return make_error(ErrorCode::InvalidArgument, "Expected field separator");
    }
    return path;
}

std::string PropertyPath::string() const {
    std::string output;
    for (const auto& segment : segments) {
        if (segment.kind == PathSegment::Kind::Field) {
            if (!output.empty()) output += '.';
            output += segment.field;
        } else output += '[' + std::to_string(segment.index) + ']';
    }
    return output;
}
}
