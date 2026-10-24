#pragma once
#include <string_view>

namespace infinifusion::unreal {

constexpr bool reflected_name_equal(std::string_view first, std::string_view second) noexcept {
    if (first.size() != second.size()) return false;
    const auto fold = [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
    };
    for (std::size_t i = 0; i < first.size(); ++i)
        if (fold(static_cast<unsigned char>(first[i])) != fold(static_cast<unsigned char>(second[i]))) return false;
    return true;
}
}
