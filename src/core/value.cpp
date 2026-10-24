#include "core/value.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace infinifusion::core {

std::string_view to_string(ErrorCode code) noexcept {
#define IF_ERROR_NAME(name) case ErrorCode::name: return #name
    switch (code) {
    IF_ERROR_NAME(None); IF_ERROR_NAME(InvalidArgument); IF_ERROR_NAME(DuplicateOwner);
    IF_ERROR_NAME(StaleToken); IF_ERROR_NAME(UnknownOwner); IF_ERROR_NAME(StaleRevision);
    IF_ERROR_NAME(RevisionConflict); IF_ERROR_NAME(TypeMismatch); IF_ERROR_NAME(OutOfRange);
    IF_ERROR_NAME(NotFinite); IF_ERROR_NAME(UnknownEnumSymbol); IF_ERROR_NAME(PathNotFound);
    IF_ERROR_NAME(AmbiguousPath); IF_ERROR_NAME(GuardMismatch); IF_ERROR_NAME(CapabilityUnavailable);
    IF_ERROR_NAME(ExpectationFailed); IF_ERROR_NAME(LifetimeUnverifiable); IF_ERROR_NAME(LimitExceeded);
    IF_ERROR_NAME(QueueFull); IF_ERROR_NAME(WriteFailed); IF_ERROR_NAME(RestoreFailed);
    IF_ERROR_NAME(ReadFailed); IF_ERROR_NAME(TargetExpired); IF_ERROR_NAME(GroupInvalid);
    IF_ERROR_NAME(GroupTooLarge); IF_ERROR_NAME(Faulted); IF_ERROR_NAME(Cancelled);
    IF_ERROR_NAME(NotFound); IF_ERROR_NAME(ProviderUnknown); IF_ERROR_NAME(ExternalChange);
    IF_ERROR_NAME(InvalidHandle); IF_ERROR_NAME(AbiMismatch); IF_ERROR_NAME(NotReady);
    IF_ERROR_NAME(Internal);
    }
#undef IF_ERROR_NAME
    return "Unknown";
}

std::string_view to_string(ValueKind kind) noexcept {
    switch (kind) {
    case ValueKind::Boolean: return "Boolean";
    case ValueKind::SignedInteger: return "SignedInteger";
    case ValueKind::UnsignedInteger: return "UnsignedInteger";
    case ValueKind::Float: return "Float";
    case ValueKind::String: return "String";
    case ValueKind::EnumSymbol: return "EnumSymbol";
    case ValueKind::Bytes: return "Bytes";
    }
    return "Unknown";
}

bool Value::operator==(const Value& other) const noexcept {
    if (kind() != other.kind()) return false;
    if (kind() == ValueKind::Float)
        return std::bit_cast<std::uint64_t>(as_float()) == std::bit_cast<std::uint64_t>(other.as_float());
    return data_ == other.data_;
}

std::string Value::debug_string() const {
    std::ostringstream out;
    switch (kind()) {
    case ValueKind::Boolean: return as_bool() ? "true" : "false";
    case ValueKind::SignedInteger: return std::to_string(as_signed());
    case ValueKind::UnsignedInteger: return std::to_string(as_unsigned());
    case ValueKind::Float: out.precision(17); out << as_float(); return out.str();
    case ValueKind::String: return as_string();
    case ValueKind::EnumSymbol: return as_enum().enum_type + "::" + as_enum().symbol;
    case ValueKind::Bytes:
        out << "bytes[";
        for (std::size_t i = 0; i < as_bytes().size(); ++i) {
            if (i) out << ',';
            out << static_cast<unsigned>(as_bytes()[i]);
        }
        out << ']'; return out.str();
    }
    return {};
}

namespace {
Error descriptor_error(const FieldType& type) {
    if (type.kind == ScalarKind::Bool) {
        if (type.size != 1 || !type.bool_field_mask || !type.bool_true_bits ||
            (type.bool_true_bits & ~type.bool_field_mask))
            return make_error(ErrorCode::InvalidArgument, "Invalid bool storage descriptor");
        return {};
    }
    if (type.kind == ScalarKind::Float) {
        if (type.size != 4 && type.size != 8)
            return make_error(ErrorCode::InvalidArgument, "Float width must be 4 or 8");
    } else if (type.kind == ScalarKind::SignedInt || type.kind == ScalarKind::UnsignedInt ||
               type.kind == ScalarKind::Enum) {
        if (type.size != 1 && type.size != 2 && type.size != 4 && type.size != 8)
            return make_error(ErrorCode::InvalidArgument, "Integer width must be 1, 2, 4 or 8");
    } else {
        return make_error(ErrorCode::InvalidArgument, "Unknown scalar kind");
    }
    return {};
}

Result<std::uint64_t> integer_bits(const Value& value, bool is_signed, unsigned size) {
    const unsigned bits = size * 8;
    if (value.kind() != ValueKind::SignedInteger && value.kind() != ValueKind::UnsignedInteger)
        return make_error(ErrorCode::TypeMismatch, "Integer storage requires integer input");
    if (is_signed) {
        const auto max = bits == 64 ? std::numeric_limits<std::int64_t>::max() :
            static_cast<std::int64_t>((std::uint64_t{1} << (bits - 1)) - 1);
        const auto min = bits == 64 ? std::numeric_limits<std::int64_t>::min() : -max - 1;
        if (value.kind() == ValueKind::UnsignedInteger) {
            if (value.as_unsigned() > static_cast<std::uint64_t>(max))
                return make_error(ErrorCode::OutOfRange, "Unsigned input does not fit signed destination");
            return value.as_unsigned();
        }
        if (value.as_signed() < min || value.as_signed() > max)
            return make_error(ErrorCode::OutOfRange, "Signed input does not fit destination");
        return static_cast<std::uint64_t>(value.as_signed());
    }
    if (value.kind() == ValueKind::SignedInteger && value.as_signed() < 0)
        return make_error(ErrorCode::OutOfRange, "Negative input does not fit unsigned destination");
    const auto number = value.kind() == ValueKind::UnsignedInteger ? value.as_unsigned() :
        static_cast<std::uint64_t>(value.as_signed());
    if (bits != 64 && number >= (std::uint64_t{1} << bits))
        return make_error(ErrorCode::OutOfRange, "Unsigned input does not fit destination");
    return number;
}
}

Result<EncodedValue> encode_value(const Value& input, const FieldType& type) {
    if (auto error = descriptor_error(type); !error.ok()) return error;
    EncodedValue output{std::vector<std::uint8_t>(type.size), std::vector<std::uint8_t>(type.size, 0xFF)};
    std::uint64_t bits = 0;
    if (type.kind == ScalarKind::Bool) {
        if (input.kind() != ValueKind::Boolean)
            return make_error(ErrorCode::TypeMismatch, "Bool storage requires Boolean input");
        output.bytes[0] = input.as_bool() ? type.bool_true_bits : 0;
        output.mask[0] = type.bool_field_mask;
        return output;
    }
    if (type.kind == ScalarKind::Float) {
        if (input.kind() != ValueKind::Float && input.kind() != ValueKind::SignedInteger && input.kind() != ValueKind::UnsignedInteger)
            return make_error(ErrorCode::TypeMismatch, "Float storage requires numeric input");
        const double numeric = input.kind() == ValueKind::Float ? input.as_float() :
            input.kind() == ValueKind::SignedInteger ? static_cast<double>(input.as_signed()) : static_cast<double>(input.as_unsigned());
        if (!std::isfinite(numeric)) return make_error(ErrorCode::NotFinite, "Float input is not finite");
        if (type.size == 4) {
            const float narrowed = static_cast<float>(numeric);
            if (!std::isfinite(narrowed)) return make_error(ErrorCode::OutOfRange, "Float narrowing overflow");
            bits = std::bit_cast<std::uint32_t>(narrowed);
        } else bits = std::bit_cast<std::uint64_t>(numeric);
    } else {
        Value number = input;
        if (type.kind == ScalarKind::Enum) {
            if (input.kind() == ValueKind::EnumSymbol) {
                const auto& symbol = input.as_enum();
                if (!symbol.enum_type.empty() && symbol.enum_type != type.enum_type)
                    return make_error(ErrorCode::TypeMismatch, "Enum type identity mismatch");
                const EnumEntry* match = nullptr;
                std::size_t count = 0;
                for (const auto& entry : type.enum_entries) if (entry.name == symbol.symbol) { match = &entry; ++count; }
                if (!count) {
                    for (const auto& entry : type.enum_entries) {
                        const auto colon = entry.name.rfind("::");
                        if (colon != std::string::npos && entry.name.substr(colon + 2) == symbol.symbol) { match = &entry; ++count; }
                    }
                }
                if (!count) return make_error(ErrorCode::UnknownEnumSymbol, "Unknown enum symbol");
                if (count != 1) return make_error(ErrorCode::AmbiguousPath, "Ambiguous enum symbol");
                number = Value::signed_integer(match->value);
            } else if (!type.enum_numeric_allowed) {
                return make_error(ErrorCode::TypeMismatch, "Enum requires a tagged symbol");
            }
        }
        auto encoded = integer_bits(number, type.kind == ScalarKind::SignedInt ||
            (type.kind == ScalarKind::Enum && type.enum_signed), type.size);
        if (!encoded) return encoded.error();
        bits = encoded.value();
    }
    for (unsigned i = 0; i < type.size; ++i) output.bytes[i] = static_cast<std::uint8_t>(bits >> (i * 8));
    return output;
}

Result<Value> decode_value(std::span<const std::uint8_t> bytes, const FieldType& type) {
    if (auto error = descriptor_error(type); !error.ok()) return error;
    if (bytes.size() != type.size) return make_error(ErrorCode::InvalidArgument, "Storage byte width mismatch");
    if (type.kind == ScalarKind::Bool) return Value::boolean((bytes[0] & type.bool_field_mask) != 0);
    std::uint64_t bits = 0;
    for (unsigned i = 0; i < type.size; ++i) bits |= std::uint64_t{bytes[i]} << (i * 8);
    if (type.kind == ScalarKind::Float) {
        const double number = type.size == 4 ? static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(bits))) :
            std::bit_cast<double>(bits);
        if (!std::isfinite(number)) return make_error(ErrorCode::NotFinite, "Live float is not finite");
        return Value::floating(number);
    }
    const bool sign = type.kind == ScalarKind::SignedInt || (type.kind == ScalarKind::Enum && type.enum_signed);
    if (sign && type.size != 8 && (bits & (std::uint64_t{1} << (type.size * 8 - 1))))
        bits |= (~std::uint64_t{0}) << (type.size * 8);
    const auto signed_value = std::bit_cast<std::int64_t>(bits);
    if (type.kind == ScalarKind::Enum) {
        const EnumEntry* match = nullptr;
        std::size_t count = 0;
        for (const auto& entry : type.enum_entries) if (static_cast<std::uint64_t>(entry.value) == bits) { match = &entry; ++count; }
        if (count == 1) return Value::enum_symbol(type.enum_type, match->name);
    }
    return sign ? Value::signed_integer(signed_value) : Value::unsigned_integer(bits);
}

}
