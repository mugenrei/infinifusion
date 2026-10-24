#include "json.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <unordered_set>

namespace smtvv::json
{
namespace
{
void append_utf8(std::string& output, std::uint32_t codepoint)
{
    if (codepoint <= 0x7F)
    {
        output.push_back(static_cast<char>(codepoint));
    }
    else if (codepoint <= 0x7FF)
    {
        output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    else if (codepoint <= 0xFFFF)
    {
        output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    else
    {
        output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

class Parser
{
public:
    explicit Parser(std::string_view input) : input_(input) {}

    Result run()
    {
        skip_space();
        auto value = parse_value();
        if (!value)
        {
            return {.error = error_};
        }
        skip_space();
        if (cursor_ != input_.size())
        {
            fail("unexpected content after the root value");
            return {.error = error_};
        }
        return {.value = std::move(*value)};
    }

private:
    std::optional<Value> parse_value()
    {
        if (cursor_ >= input_.size())
        {
            fail("expected a JSON value");
            return std::nullopt;
        }
        switch (input_[cursor_])
        {
        case 'n':
            return parse_literal("null", Value{});
        case 't':
            return parse_literal("true", Value{true});
        case 'f':
            return parse_literal("false", Value{false});
        case '"':
        {
            auto value = parse_string();
            return value ? std::optional<Value>(Value{std::move(*value)})
                         : std::nullopt;
        }
        case '[':
            return parse_array();
        case '{':
            return parse_object();
        default:
            if (input_[cursor_] == '-' ||
                (input_[cursor_] >= '0' && input_[cursor_] <= '9'))
            {
                return parse_number();
            }
            fail("expected a JSON value");
            return std::nullopt;
        }
    }

    std::optional<Value> parse_literal(std::string_view literal, Value value)
    {
        if (input_.substr(cursor_, literal.size()) != literal)
        {
            fail("invalid JSON literal");
            return std::nullopt;
        }
        cursor_ += literal.size();
        return value;
    }

    std::optional<std::uint32_t> parse_hex4()
    {
        if (input_.size() - cursor_ < 4)
        {
            fail("incomplete Unicode escape");
            return std::nullopt;
        }
        std::uint32_t value{};
        for (int index = 0; index < 4; ++index)
        {
            const char character = input_[cursor_++];
            value <<= 4;
            if (character >= '0' && character <= '9')
                value |= static_cast<std::uint32_t>(character - '0');
            else if (character >= 'a' && character <= 'f')
                value |= static_cast<std::uint32_t>(character - 'a' + 10);
            else if (character >= 'A' && character <= 'F')
                value |= static_cast<std::uint32_t>(character - 'A' + 10);
            else
            {
                fail("invalid Unicode escape");
                return std::nullopt;
            }
        }
        return value;
    }

    std::optional<std::string> parse_string()
    {
        if (!consume('"'))
        {
            fail("expected a string");
            return std::nullopt;
        }
        std::string output;
        while (cursor_ < input_.size())
        {
            const unsigned char character =
                static_cast<unsigned char>(input_[cursor_++]);
            if (character == '"')
                return output;
            if (character < 0x20)
            {
                fail("unescaped control character in string");
                return std::nullopt;
            }
            if (character != '\\')
            {
                output.push_back(static_cast<char>(character));
                continue;
            }
            if (cursor_ >= input_.size())
            {
                fail("incomplete escape sequence");
                return std::nullopt;
            }
            const char escaped = input_[cursor_++];
            switch (escaped)
            {
            case '"':
            case '\\':
            case '/':
                output.push_back(escaped);
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case 'u':
            {
                auto first = parse_hex4();
                if (!first)
                    return std::nullopt;
                std::uint32_t codepoint = *first;
                if (codepoint >= 0xD800 && codepoint <= 0xDBFF)
                {
                    if (input_.size() - cursor_ < 6 ||
                        input_[cursor_] != '\\' || input_[cursor_ + 1] != 'u')
                    {
                        fail("high surrogate is missing its low surrogate");
                        return std::nullopt;
                    }
                    cursor_ += 2;
                    auto second = parse_hex4();
                    if (!second || *second < 0xDC00 || *second > 0xDFFF)
                    {
                        fail("invalid low surrogate");
                        return std::nullopt;
                    }
                    codepoint =
                        0x10000 + ((codepoint - 0xD800) << 10) +
                        (*second - 0xDC00);
                }
                else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF)
                {
                    fail("unexpected low surrogate");
                    return std::nullopt;
                }
                append_utf8(output, codepoint);
                break;
            }
            default:
                fail("invalid escape sequence");
                return std::nullopt;
            }
        }
        fail("unterminated string");
        return std::nullopt;
    }

    std::optional<Value> parse_number()
    {
        const std::size_t begin = cursor_;
        consume('-');
        if (consume('0'))
        {
            if (cursor_ < input_.size() && input_[cursor_] >= '0' &&
                input_[cursor_] <= '9')
            {
                fail("leading zero in number");
                return std::nullopt;
            }
        }
        else
        {
            if (cursor_ >= input_.size() || input_[cursor_] < '1' ||
                input_[cursor_] > '9')
            {
                fail("invalid number");
                return std::nullopt;
            }
            while (cursor_ < input_.size() && input_[cursor_] >= '0' &&
                   input_[cursor_] <= '9')
                ++cursor_;
        }
        if (consume('.'))
        {
            const std::size_t fraction = cursor_;
            while (cursor_ < input_.size() && input_[cursor_] >= '0' &&
                   input_[cursor_] <= '9')
                ++cursor_;
            if (fraction == cursor_)
            {
                fail("fraction has no digits");
                return std::nullopt;
            }
        }
        if (cursor_ < input_.size() &&
            (input_[cursor_] == 'e' || input_[cursor_] == 'E'))
        {
            ++cursor_;
            if (cursor_ < input_.size() &&
                (input_[cursor_] == '+' || input_[cursor_] == '-'))
                ++cursor_;
            const std::size_t exponent = cursor_;
            while (cursor_ < input_.size() && input_[cursor_] >= '0' &&
                   input_[cursor_] <= '9')
                ++cursor_;
            if (exponent == cursor_)
            {
                fail("exponent has no digits");
                return std::nullopt;
            }
        }
        return Value{Number{std::string(input_.substr(begin, cursor_ - begin))}};
    }

    std::optional<Value> parse_array()
    {
        consume('[');
        skip_space();
        Value::Array values;
        if (consume(']'))
            return Value{std::move(values)};
        while (true)
        {
            skip_space();
            auto value = parse_value();
            if (!value)
                return std::nullopt;
            values.emplace_back(std::move(*value));
            skip_space();
            if (consume(']'))
                return Value{std::move(values)};
            if (!consume(','))
            {
                fail("expected ',' or ']' in array");
                return std::nullopt;
            }
        }
    }

    std::optional<Value> parse_object()
    {
        consume('{');
        skip_space();
        Value::Object values;
        std::unordered_set<std::string> keys;
        if (consume('}'))
            return Value{std::move(values)};
        while (true)
        {
            skip_space();
            auto key = parse_string();
            if (!key)
                return std::nullopt;
            if (!keys.insert(*key).second)
            {
                fail("duplicate object key '" + *key + "'");
                return std::nullopt;
            }
            skip_space();
            if (!consume(':'))
            {
                fail("expected ':' after object key");
                return std::nullopt;
            }
            skip_space();
            auto value = parse_value();
            if (!value)
                return std::nullopt;
            values.emplace_back(std::move(*key), std::move(*value));
            skip_space();
            if (consume('}'))
                return Value{std::move(values)};
            if (!consume(','))
            {
                fail("expected ',' or '}' in object");
                return std::nullopt;
            }
        }
    }

    void skip_space()
    {
        while (cursor_ < input_.size())
        {
            const char character = input_[cursor_];
            if (character != ' ' && character != '\t' &&
                character != '\r' && character != '\n')
                break;
            ++cursor_;
        }
    }

    bool consume(char expected)
    {
        if (cursor_ < input_.size() && input_[cursor_] == expected)
        {
            ++cursor_;
            return true;
        }
        return false;
    }

    void fail(std::string message)
    {
        if (error_)
            return;
        Error error;
        error.message = std::move(message);
        error.offset = cursor_;
        for (std::size_t index = 0; index < cursor_ && index < input_.size();
             ++index)
        {
            if (input_[index] == '\n')
            {
                ++error.line;
                error.column = 1;
            }
            else
            {
                ++error.column;
            }
        }
        error_ = std::move(error);
    }

    std::string_view input_;
    std::size_t cursor_{};
    std::optional<Error> error_;
};
}

bool Value::is_null() const noexcept
{
    return std::holds_alternative<std::nullptr_t>(storage_);
}

const bool* Value::boolean() const noexcept
{
    return std::get_if<bool>(&storage_);
}

const Number* Value::number() const noexcept
{
    return std::get_if<Number>(&storage_);
}

const std::string* Value::string() const noexcept
{
    return std::get_if<std::string>(&storage_);
}

const Value::Array* Value::array() const noexcept
{
    return std::get_if<Array>(&storage_);
}

const Value::Object* Value::object() const noexcept
{
    return std::get_if<Object>(&storage_);
}

const Value* Value::find(std::string_view key) const noexcept
{
    const auto* entries = object();
    if (!entries)
        return nullptr;
    for (auto iterator = entries->rbegin(); iterator != entries->rend();
         ++iterator)
    {
        if (iterator->first == key)
            return &iterator->second;
    }
    return nullptr;
}

Result parse(std::string_view input)
{
    if (input.size() >= 3 &&
        static_cast<unsigned char>(input[0]) == 0xEF &&
        static_cast<unsigned char>(input[1]) == 0xBB &&
        static_cast<unsigned char>(input[2]) == 0xBF)
    {
        input.remove_prefix(3);
    }
    return Parser(input).run();
}

Result parse_file(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return {.error = Error{
                    .message = "could not open file",
                    .offset = 0,
                    .line = 1,
                    .column = 1}};
    }
    std::string contents{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    return parse(contents);
}
}
