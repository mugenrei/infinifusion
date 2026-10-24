#pragma once

#include "core/result.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace infinifusion::core {

enum class ValueKind : std::uint8_t {
    Boolean = 0,
    SignedInteger = 1,
    UnsignedInteger = 2,
    Float = 3,
    String = 4,
    EnumSymbol = 5,
    Bytes = 7,
};

std::string_view to_string(ValueKind kind) noexcept;

struct EnumSymbolValue {
    std::string enum_type;
    std::string symbol;
    bool operator==(const EnumSymbolValue&) const = default;
};

class Value {
public:
    Value() : data_(false) {}

    static Value boolean(bool value) { return Value(Data(std::in_place_index<0>, value)); }
    static Value signed_integer(std::int64_t value) { return Value(Data(std::in_place_index<1>, value)); }
    static Value unsigned_integer(std::uint64_t value) { return Value(Data(std::in_place_index<2>, value)); }
    static Value floating(double value) { return Value(Data(std::in_place_index<3>, value)); }
    static Value string(std::string value) { return Value(Data(std::in_place_index<4>, std::move(value))); }
    static Value enum_symbol(std::string enum_type, std::string symbol) {
        return Value(Data(std::in_place_index<5>, EnumSymbolValue{std::move(enum_type), std::move(symbol)}));
    }

    static Value bytes(std::vector<std::uint8_t> value) {
        return Value(Data(std::in_place_index<6>, std::move(value)));
    }
    [[nodiscard]] const std::vector<std::uint8_t>& as_bytes() const { return std::get<6>(data_); }

    [[nodiscard]] ValueKind kind() const noexcept { return data_.index()==6 ? ValueKind::Bytes : static_cast<ValueKind>(data_.index()); }
    [[nodiscard]] bool as_bool() const { return std::get<0>(data_); }
    [[nodiscard]] std::int64_t as_signed() const { return std::get<1>(data_); }
    [[nodiscard]] std::uint64_t as_unsigned() const { return std::get<2>(data_); }
    [[nodiscard]] double as_float() const { return std::get<3>(data_); }
    [[nodiscard]] const std::string& as_string() const { return std::get<4>(data_); }
    [[nodiscard]] const EnumSymbolValue& as_enum() const { return std::get<5>(data_); }

    bool operator==(const Value& other) const noexcept;

    [[nodiscard]] std::string debug_string() const;

private:
    using Data = std::variant<bool, std::int64_t, std::uint64_t, double, std::string, EnumSymbolValue, std::vector<std::uint8_t>>;
    explicit Value(Data data) : data_(std::move(data)) {}
    Data data_;
};

enum class ScalarKind : std::uint8_t {
    Bool = 0,
    SignedInt = 1,
    UnsignedInt = 2,
    Float = 3,
    Enum = 4,
};

struct EnumEntry {
    std::string name;
    std::int64_t value = 0;
};

struct FieldType {
    ScalarKind kind = ScalarKind::SignedInt;
    std::uint8_t size = 4;

    std::uint8_t bool_field_mask = 0xFF;
    std::uint8_t bool_true_bits = 0x01;

    bool enum_signed = false;
    bool enum_numeric_allowed = false;
    std::string enum_type;
    std::vector<EnumEntry> enum_entries;
};

struct EncodedValue {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> mask;
};

Result<EncodedValue> encode_value(const Value& value, const FieldType& type);

Result<Value> decode_value(std::span<const std::uint8_t> bytes, const FieldType& type);

}
