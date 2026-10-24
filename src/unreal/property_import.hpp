#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace infinifusion::unreal {

enum class ImportKind { Scalar, Struct, Array, Set, Map };
struct ImportSchema {
    ImportKind kind{ImportKind::Scalar};
    std::size_t fixed_count{1};
    std::uintptr_t inner{}, key{}, value{};
};
struct ImportCallbacks {
    std::function<ImportSchema(std::uintptr_t)> describe;

    std::function<std::uintptr_t(std::uintptr_t, std::string_view)> member;

    std::function<void(std::uintptr_t, std::string_view)> scalar_validation;
};

[[nodiscard]] std::vector<std::string> prepare_property_import(
    std::uintptr_t property, std::string_view text, const ImportCallbacks& callbacks);

}
