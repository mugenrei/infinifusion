#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace infinifusion::core {

enum class ErrorCode : std::uint32_t {
    None = 0,
    InvalidArgument = 1,
    DuplicateOwner = 2,
    StaleToken = 3,
    UnknownOwner = 4,
    StaleRevision = 5,
    RevisionConflict = 6,
    TypeMismatch = 7,
    OutOfRange = 8,
    NotFinite = 9,
    UnknownEnumSymbol = 10,
    PathNotFound = 11,
    AmbiguousPath = 12,
    GuardMismatch = 13,
    CapabilityUnavailable = 14,
    ExpectationFailed = 15,
    LifetimeUnverifiable = 16,
    LimitExceeded = 17,
    QueueFull = 18,
    WriteFailed = 19,
    RestoreFailed = 20,
    ReadFailed = 21,
    TargetExpired = 22,
    GroupInvalid = 23,
    GroupTooLarge = 24,
    Faulted = 25,
    Cancelled = 26,
    NotFound = 27,
    ProviderUnknown = 28,
    ExternalChange = 29,
    InvalidHandle = 30,
    AbiMismatch = 31,
    NotReady = 32,
    Internal = 33,
};

std::string_view to_string(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::None;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::None; }
    static Error none() { return {}; }
};

inline Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

template <class T>
class Result {
public:
    Result(T value) : data_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : data_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return data_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & { return std::get<0>(data_); }
    const T& value() const& { return std::get<0>(data_); }
    T&& value() && { return std::get<0>(std::move(data_)); }
    T* operator->() { return &std::get<0>(data_); }
    const T* operator->() const { return &std::get<0>(data_); }

    const Error& error() const { return std::get<1>(data_); }

private:
    std::variant<T, Error> data_;
};

}
