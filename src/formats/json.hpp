#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace smtvv::json
{
struct Number
{
    std::string text;
};

class Value
{
public:
    using Array = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>;
    using Storage =
        std::variant<std::nullptr_t, bool, Number, std::string, Array, Object>;

    Value() noexcept = default;
    explicit Value(bool value) : storage_(value) {}
    explicit Value(Number value) : storage_(std::move(value)) {}
    explicit Value(std::string value) : storage_(std::move(value)) {}
    explicit Value(Array value) : storage_(std::move(value)) {}
    explicit Value(Object value) : storage_(std::move(value)) {}

    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] const bool* boolean() const noexcept;
    [[nodiscard]] const Number* number() const noexcept;
    [[nodiscard]] const std::string* string() const noexcept;
    [[nodiscard]] const Array* array() const noexcept;
    [[nodiscard]] const Object* object() const noexcept;
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;

private:
    Storage storage_{nullptr};
};

struct Error
{
    std::string message;
    std::size_t offset{};
    std::size_t line{1};
    std::size_t column{1};
};

struct Result
{
    std::optional<Value> value;
    std::optional<Error> error;
};

[[nodiscard]] Result parse(std::string_view input);
[[nodiscard]] Result parse_file(const std::filesystem::path& path);
}
