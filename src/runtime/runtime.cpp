#include "runtime.hpp"
#include "runtime/log_queue.hpp"
#include "runtime/build_profile.hpp"
#include "bootstrap/startup.hpp"
#ifdef INFINIFUSION_NATIVE_API
#include "host/api_host.hpp"
#include "host/game_services.hpp"
#include "formats/tbcr.hpp"
#include "formats/json.hpp"
#include "unreal/tbcr_publication.hpp"
#include "unreal/tbcr_source.hpp"
#include "host/client_session.hpp"
#include "adapters/json/json_mods.hpp"
#include "unreal/property_import.hpp"
#include "unreal/property_text.hpp"
#include "unreal/property_numeric.hpp"
#ifdef INFINIFUSION_LUA
#include "host/lua_client.hpp"
#endif
#endif

#include <Windows.h>
#include "unreal/reflected_name.hpp"
#include <intrin.h>
#include "unreal/blueprint_creation_trampoline.hpp"
#include "unreal/blueprint_template_trampoline.hpp"
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace smtvv
{
namespace
{
constexpr std::uint32_t kKnownExeTimestamp = 0x6790DFF5;
constexpr std::uint32_t kKnownImageSize = 0x14C6D000;
constexpr std::uintptr_t kKnownGObjectsRva = 0x043FA660;
constexpr std::uintptr_t kKnownAppendStringRva = 0x00EBB6F0;
constexpr std::uintptr_t kKnownGameEngineTickRva = 0x022627D0;
constexpr std::int32_t kElementsPerChunk = 0x10000;
constexpr std::size_t kObjectItemSize = 0x18;

constexpr std::string_view kGObjectsPattern =
    "48 8B ?? ?? ?? ?? ?? 48 8B ?? ?? 48 8D ?? ?? EB ?? 33 ?? 8B ?? ?? C1 ??";
constexpr std::string_view kAppendStringPattern =
    "48 89 ?? ?? ?? E8 ?? ?? ?? ?? 48 8B ?? ?? 48 85 ?? 75 ?? 48 8B ?? ?? ?? 48 8B ??";
constexpr std::string_view kGameEngineTickPattern =
    "44 88 44 24 18 55 53 57 41 54 41 57 48 8D 6C 24 C9 48 81 EC C0 00 00 00";
constexpr std::array<std::uint8_t, 17> kKnownGameEngineTickPrologue{
    0x44, 0x88, 0x44, 0x24, 0x18, 0x55, 0x53, 0x57, 0x41,
    0x54, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xC9};

struct FName
{
    std::int32_t comparison_index{};
    std::uint32_t number{};
};

struct FWeakObjectPtrData
{
    std::int32_t object_index{-1};
    std::int32_t object_serial_number{};
};

struct FUniqueObjectGuidData
{
    std::uint32_t a{};
    std::uint32_t b{};
    std::uint32_t c{};
    std::uint32_t d{};
};

struct FStringHeader
{
    wchar_t* data{};
    std::int32_t num{};
    std::int32_t max{};
};

struct UObjectArrayHeader
{
    void** objects{};
    std::uint8_t padding[8]{};
    std::int32_t max_elements{};
    std::int32_t num_elements{};
    std::int32_t max_chunks{};
    std::int32_t num_chunks{};
};

struct SparseContainerHeader
{
    void* data{};
    std::int32_t num_slots{};
    std::int32_t max_slots{};
    std::uint32_t inline_flags[4]{};
    const std::uint32_t* secondary_flags{};
    std::int32_t num_bits{};
    std::int32_t max_bits{};
    std::int32_t first_free_index{};
    std::int32_t num_free_indices{};
};
static_assert(sizeof(SparseContainerHeader) == 0x38);

struct Config
{
    bool enabled{false};
    std::uint32_t initial_delay_ms{15000};
    std::uint32_t repeat_interval_ms{10000};
    std::uint32_t repeat_count{6};
    std::filesystem::path output_directory{L"infinifusion_exports"};
    std::uint32_t max_tables{};
    std::uint32_t max_rows_per_table{};
    std::uint32_t max_array_elements{1024};
    std::uint32_t max_depth{8};
    bool include_empty_tables{false};
    bool write_raw_fallback{true};
    bool export_script_message_assets{true};
    bool export_object_properties{true};
    std::uint32_t max_script_message_assets{512};
    std::uint32_t max_object_exports{512};
};

struct JsonConfig
{
    bool enabled{true};
    std::filesystem::path mods_directory{L"../../Content/Paks/~mods/infinifusion"};
    std::uint32_t poll_interval_ms{1000};
    bool hot_reload{true};
    int reload_virtual_key{VK_F6};
    std::string reload_key_name{"F6"};
};

struct Runtime
{
    Config config;
    std::filesystem::path module_directory;
    std::filesystem::path session_directory;
    std::filesystem::path scan_directory;
    std::string scan_relative_directory;
    std::uint8_t* image_base{};
    std::uint32_t image_size{};
    std::uint32_t exe_timestamp{};
    UObjectArrayHeader* object_array{};
    using AppendStringFn = void (*)(const FName*, FStringHeader&);
    AppendStringFn append_string{};
    std::unordered_map<void*, bool> data_table_class_cache;
    std::unordered_map<std::uint64_t, std::string> fname_cache;
};

struct PathSegment
{
    std::string name;
    std::optional<std::size_t> index;
};

struct ResolvedLeaf
{
    void* property{};
    void* address{};
    std::string type;
    std::string canonical_path;

    std::vector<std::uintptr_t> layout_trace;
    bool dynamic_storage{};
};

struct PropertyInfo
{
    void* address{};
    std::string name;
    std::string type;
    std::int32_t array_dim{1};
    std::int32_t element_size{};
    std::int32_t offset{};
    std::uint64_t flags{};
};

struct TableSummary
{
    std::string path;
    std::string class_name;
    std::string row_struct_path;
    std::string file_name;
    std::int32_t row_count{};
    bool exported{};
    std::string error;
};

struct AssetExportSummary
{
    std::string path;
    std::string class_name;
    std::string file_name;
    std::string error;
    bool exported{};
};

struct RuntimeLog {
    using Queue = infinifusion::runtime::LogQueue<128, 4096>;
    Queue queue;
    std::ofstream file;
};

std::atomic<RuntimeLog*> g_runtime_log{};
bool g_debug_logging{};
using GameEngineTickFn = void(__fastcall*)(void*, float, std::uint8_t);

struct TickHookState
{
    GameEngineTickFn original{};
    void* target{};
    std::array<std::uint8_t, kKnownGameEngineTickPrologue.size()> original_bytes{};
};
TickHookState g_tick_hook;

std::string utc_timestamp(bool filename_safe = false)
{
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char buffer[64]{};
    if (filename_safe)
    {
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%04u%02u%02uT%02u%02u%02u.%03uZ",
                      st.wYear,
                      st.wMonth,
                      st.wDay,
                      st.wHour,
                      st.wMinute,
                      st.wSecond,
                      st.wMilliseconds);
    }
    else
    {
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                      st.wYear,
                      st.wMonth,
                      st.wDay,
                      st.wHour,
                      st.wMinute,
                      st.wSecond,
                      st.wMilliseconds);
    }
    return buffer;
}

void log_line(std::string_view level, std::string_view message)
{
    if (!infinifusion::runtime::log_enabled(level, g_debug_logging, message)) return;
    auto* sink = g_runtime_log.load(std::memory_order_acquire);
    if (!sink) return;
    RuntimeLog::Queue::Record record{};
    const auto written = std::snprintf(record.data(), record.size(), "[%s] [%.*s] %.*s\n",
        utc_timestamp().c_str(), static_cast<int>(std::min<std::size_t>(level.size(), 64)), level.data(),
        static_cast<int>(std::min<std::size_t>(message.size(), record.size())), message.data());
    if (written < 0 || static_cast<std::size_t>(written) >= record.size()) {
        constexpr char suffix[] = " [truncated]\n";
        std::memcpy(record.data() + record.size() - sizeof(suffix), suffix, sizeof(suffix));
    }
    sink->queue.push(record);
}

std::string narrow(const std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8,
                        WC_ERR_INVALID_CHARS,
                        value.data(),
                        static_cast<int>(value.size()),
                        result.data(),
                        required,
                        nullptr,
                        nullptr);
    return result;
}

std::wstring widen(const std::string_view value)
{
    if (value.empty())
        return {};
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (required <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            required) != required)
        return {};
    return result;
}

std::wstring trim(std::wstring value)
{
    const auto is_space = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
    };
    while (!value.empty() && is_space(value.front()))
    {
        value.erase(value.begin());
    }
    while (!value.empty() && is_space(value.back()))
    {
        value.pop_back();
    }
    return value;
}

std::wstring lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return value;
}

bool parse_bool(const std::wstring& value, bool fallback)
{
    const auto normalized = lower(trim(value));
    if (normalized == L"true" || normalized == L"yes" || normalized == L"on" || normalized == L"1")
    {
        return true;
    }
    if (normalized == L"false" || normalized == L"no" || normalized == L"off" || normalized == L"0")
    {
        return false;
    }
    return fallback;
}

std::wstring read_ini_string(const std::filesystem::path& ini,
                             const wchar_t* section,
                             const wchar_t* key,
                             const wchar_t* fallback)
{
    std::array<wchar_t, 32768> buffer{};
    GetPrivateProfileStringW(
        section, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), ini.c_str());
    return buffer.data();
}

std::uint32_t read_ini_u32(const std::filesystem::path& ini,
                           const wchar_t* section,
                           const wchar_t* key,
                           std::uint32_t fallback)
{
    const auto value = trim(read_ini_string(ini, section, key, L""));
    if (value.empty())
    {
        return fallback;
    }
    wchar_t* end{};
    errno = 0;
    const auto parsed = std::wcstoull(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != L'\0' ||
        parsed > std::numeric_limits<std::uint32_t>::max())
    {
        return fallback;
    }
    return static_cast<std::uint32_t>(parsed);
}

std::filesystem::path find_config_file(const std::filesystem::path& module_directory)
{
    return module_directory / L"infinifusion.ini";
}

Config read_config(const std::filesystem::path& module_directory)
{
    Config config;
    const auto ini = find_config_file(module_directory);
    if (!std::filesystem::exists(ini))
    {
        log_line("WARN", "Configuration file is missing; using built-in defaults.");
        return config;
    }

#ifdef INFINIFUSION_DEVELOPMENT
    config.enabled =
        parse_bool(read_ini_string(ini, L"Dumper", L"Enabled", L"false"), config.enabled);
    config.initial_delay_ms =
        read_ini_u32(ini, L"Dumper", L"InitialDelayMs", config.initial_delay_ms);
    config.repeat_interval_ms =
        read_ini_u32(ini, L"Dumper", L"RepeatIntervalMs", config.repeat_interval_ms);
    config.repeat_count = std::max<std::uint32_t>(
        1, read_ini_u32(ini, L"Dumper", L"RepeatCount", config.repeat_count));
    config.output_directory =
        trim(read_ini_string(
            ini, L"Dumper", L"OutputDirectory", config.output_directory.c_str()));
    config.max_tables =
        read_ini_u32(ini, L"Dumper", L"MaxTables", config.max_tables);
    config.max_rows_per_table =
        read_ini_u32(
            ini, L"Dumper", L"MaxRowsPerTable", config.max_rows_per_table);
    config.max_array_elements =
        read_ini_u32(
            ini, L"Dumper", L"MaxArrayElements", config.max_array_elements);
    config.max_depth = std::max<std::uint32_t>(
        1, read_ini_u32(ini, L"Dumper", L"MaxDepth", config.max_depth));
    config.include_empty_tables =
        parse_bool(
            read_ini_string(
                ini, L"Dumper", L"IncludeEmptyTables", L"false"),
            config.include_empty_tables);
    config.write_raw_fallback =
        parse_bool(
            read_ini_string(
                ini, L"Dumper", L"WriteRawFallback", L"true"),
            config.write_raw_fallback);
    config.export_script_message_assets =
        parse_bool(
            read_ini_string(
                ini, L"Dumper", L"ExportScriptMessageAssets", L"true"),
            config.export_script_message_assets);
    config.export_object_properties =
        parse_bool(
            read_ini_string(
                ini, L"Dumper", L"ExportObjectProperties", L"true"),
            config.export_object_properties);
    config.max_script_message_assets =
        read_ini_u32(
            ini, L"Dumper", L"MaxScriptMessageAssets",
            config.max_script_message_assets);
    config.max_object_exports =
        read_ini_u32(
            ini, L"Dumper", L"MaxObjectExports", config.max_object_exports);
#endif
    return config;
}

int parse_reload_virtual_key(const std::wstring& raw, std::string& label)
{
    const auto value = trim(raw);
    if (value.empty())
        return VK_F6;
    for (int key = VK_F1; key <= VK_F24; ++key)
    {
        const auto expected = L"F" + std::to_wstring(key - VK_F1 + 1);
        if (_wcsicmp(value.c_str(), expected.c_str()) == 0)
        {
            label = narrow(expected);
            return key;
        }
    }
    struct NamedKey
    {
        const wchar_t* name;
        int value;
    };
    constexpr NamedKey named_keys[] = {
        {L"Insert", VK_INSERT}, {L"Home", VK_HOME}, {L"End", VK_END},
        {L"PageUp", VK_PRIOR}, {L"PageDown", VK_NEXT}, {L"Pause", VK_PAUSE}};
    for (const auto& named : named_keys)
    {
        if (_wcsicmp(value.c_str(), named.name) == 0)
        {
            label = narrow(named.name);
            return named.value;
        }
    }
    wchar_t* end{};
    errno = 0;
    const auto numeric = std::wcstoul(value.c_str(), &end, 10);
    if (errno == 0 && end != value.c_str() && *end == L'\0' &&
        numeric > 0 && numeric <= 255)
    {
        label = std::to_string(numeric);
        return static_cast<int>(numeric);
    }
    label = "F6";
    return VK_F6;
}

JsonConfig read_json_config(const std::filesystem::path& module_directory)
{
    JsonConfig config;
    const auto ini = find_config_file(module_directory);
    config.enabled = parse_bool(read_ini_string(ini, L"Overlay", L"Enabled", L"true"), config.enabled);
    config.mods_directory = std::filesystem::path(trim(read_ini_string(ini, L"Overlay", L"ModsDirectory", config.mods_directory.c_str())));
    config.poll_interval_ms = std::max(100u, read_ini_u32(ini, L"Overlay", L"PollIntervalMs", config.poll_interval_ms));
    config.hot_reload = parse_bool(read_ini_string(ini, L"Overlay", L"HotReload", L"true"), config.hot_reload);
    config.reload_virtual_key = parse_reload_virtual_key(read_ini_string(ini, L"Overlay", L"ReloadKey", L"F6"), config.reload_key_name);
    return config;
}

bool is_readable_range(const void* address, std::size_t size)
{
    if (!address || size == 0)
    {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (begin > std::numeric_limits<std::uintptr_t>::max() - size)
    {
        return false;
    }
    const auto end = begin + size;
    auto cursor = begin;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        {
            return false;
        }
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & readable) == 0)
        {
            return false;
        }
        const auto region_end =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= cursor)
        {
            return false;
        }
        cursor = std::min(region_end, end);
    }
    return true;
}

#include "unreal/memory_copy.inc"

template <typename T>
std::optional<T> safe_read(const void* source)
{
    T value{};
    if (!safe_copy(source, &value, sizeof(value)))
    {
        return std::nullopt;
    }
    return value;
}

template <typename T>
std::optional<T> safe_read_at(const void* base, std::ptrdiff_t offset)
{
    return safe_read<T>(reinterpret_cast<const std::uint8_t*>(base) + offset);
}

std::string json_quote(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const unsigned char c : value)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buffer[8]{};
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            }
            else
            {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

std::string hex_value(std::uintptr_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << value;
    return stream.str();
}

std::string raw_hex(const void* address, std::size_t size)
{
    constexpr std::size_t kRawLimit = 512;
    const auto to_read = std::min(size, kRawLimit);
    std::vector<std::uint8_t> bytes(to_read);
    if (to_read == 0 || !safe_copy(address, bytes.data(), to_read))
    {
        return {};
    }
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string result;
    result.resize(to_read * 2);
    for (std::size_t i = 0; i < to_read; ++i)
    {
        result[i * 2] = kHex[bytes[i] >> 4];
        result[i * 2 + 1] = kHex[bytes[i] & 0x0F];
    }
    return result;
}

std::string read_fstring(const void* address)
{
    const auto header = safe_read<FStringHeader>(address);
    if (!header || header->num <= 0 || header->max < header->num || header->num > 1'048'576 ||
        !header->data)
    {
        return {};
    }
    std::vector<wchar_t> contents(static_cast<std::size_t>(header->num));
    if (!safe_copy(header->data, contents.data(), contents.size() * sizeof(wchar_t)))
    {
        return {};
    }
    std::size_t length = contents.size();
    if (length > 0 && contents.back() == L'\0')
    {
        --length;
    }
    return narrow(std::wstring_view(contents.data(), length));
}

bool call_append_string_safely(Runtime::AppendStringFn function,
                               const FName* input,
                               FStringHeader* destination) noexcept
{
    __try
    {
        function(input, *destination);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

std::string fname_to_string(Runtime& runtime, const FName& name)
{
    const auto cache_key = (static_cast<std::uint64_t>(name.number) << 32) |
                           static_cast<std::uint32_t>(name.comparison_index);
    if (const auto it = runtime.fname_cache.find(cache_key); it != runtime.fname_cache.end())
    {
        return it->second;
    }
    if (!runtime.append_string || name.comparison_index < 0)
    {
        return {};
    }

    std::array<wchar_t, 2048> buffer{};
    FStringHeader output{buffer.data(), 0, static_cast<std::int32_t>(buffer.size())};
    const bool succeeded = call_append_string_safely(runtime.append_string, &name, &output);
    if (!succeeded || output.num < 0 || output.num > output.max || output.max > 1'048'576 ||
        !output.data)
    {
        return {};
    }

    std::string result;
    if (output.data == buffer.data())
    {
        std::size_t length = static_cast<std::size_t>(output.num);
        if (length > 0 && buffer[length - 1] == L'\0')
        {
            --length;
        }
        result = narrow(std::wstring_view(buffer.data(), length));
    }
    else
    {
        result = read_fstring(&output);
    }
    if (!result.empty() && runtime.fname_cache.size() < 250'000)
    {
        runtime.fname_cache.emplace(cache_key, result);
    }
    return result;
}

std::string object_name(Runtime& runtime, const void* object)
{
    const auto name = safe_read_at<FName>(object, 0x18);
    return name ? fname_to_string(runtime, *name) : std::string{};
}

std::string class_name(Runtime& runtime, const void* object)
{
    const auto klass = safe_read_at<void*>(object, 0x10);
    return klass && *klass ? object_name(runtime, *klass) : std::string{};
}

std::string object_path(Runtime& runtime, const void* object)
{
    if (!object)
    {
        return {};
    }
    std::vector<std::string> parts;
    std::unordered_set<const void*> visited;
    const void* current = object;
    for (std::size_t depth = 0; current && depth < 64 && visited.insert(current).second; ++depth)
    {
        auto name = object_name(runtime, current);
        if (name.empty())
        {
            break;
        }
        parts.push_back(std::move(name));
        const auto outer = safe_read_at<void*>(current, 0x20);
        current = outer ? *outer : nullptr;
    }
    if (parts.empty())
    {
        return {};
    }
    std::reverse(parts.begin(), parts.end());
    std::string path = parts.front();
    for (std::size_t i = 1; i < parts.size(); ++i)
    {
        path.push_back('.');
        path += parts[i];
    }
    return path;
}

std::string field_class_name(Runtime& runtime, const void* field)
{
    const auto field_class = safe_read_at<void*>(field, 0x08);
    if (!field_class || !*field_class)
    {
        return {};
    }
    const auto name = safe_read<FName>(*field_class);
    return name ? fname_to_string(runtime, *name) : std::string{};
}

#include "unreal/property_chain.inc"

std::vector<void*> properties_for_struct(const void* struct_object)
{
    std::vector<void*> properties;
    if(!visit_property_names(struct_object,[&](void* field,const FName&){properties.push_back(field);return true;})) properties.clear();
    return properties;
}

PropertyInfo inspect_property(Runtime& runtime, void* property)
{
    PropertyInfo info;
    info.address = property;
    const auto header=safe_read<std::array<std::uint8_t,0x50>>(property);
    if(!header) return info;
    void* field_class{}; FName name{};
    std::memcpy(&field_class,header->data()+8,sizeof(field_class));
    if(field_class) if(const auto type_name=safe_read<FName>(field_class)) info.type=fname_to_string(runtime,*type_name);
    std::memcpy(&name,header->data()+0x28,sizeof(name));
    info.name=fname_to_string(runtime,name);
    std::memcpy(&info.array_dim,header->data()+0x38,sizeof(info.array_dim));
    std::memcpy(&info.element_size,header->data()+0x3C,sizeof(info.element_size));
    std::memcpy(&info.flags,header->data()+0x40,sizeof(info.flags));
    std::memcpy(&info.offset,header->data()+0x4C,sizeof(info.offset));
    return info;
}

std::string property_type_detail(Runtime& runtime, const PropertyInfo& info, std::uint32_t depth);
std::string friendly_property_name(std::string_view runtime_name);

bool scalar_leaf_type(std::string_view type)
{
    return type == "BoolProperty" || type == "ByteProperty" ||
           type == "EnumProperty" || type == "Int8Property" ||
           type == "Int16Property" || type == "IntProperty" ||
           type == "Int64Property" || type == "UInt8Property" ||
           type == "UInt16Property" || type == "UInt32Property" ||
           type == "UInt64Property" || type == "FloatProperty" ||
           type == "DoubleProperty";
}

std::string property_descriptor(Runtime& runtime, void* property, std::uint32_t depth)
{
    const auto info = inspect_property(runtime, property);
    std::ostringstream out;
    out << "{\"name\":" << json_quote(info.name) << ",\"type\":" << json_quote(info.type)
        << ",\"offset\":" << info.offset << ",\"offset_hex\":" << json_quote(hex_value(info.offset))
        << ",\"element_size\":" << info.element_size << ",\"array_dim\":" << info.array_dim
        << ",\"property_flags\":" << json_quote(hex_value(info.flags))
        << ",\"scalar_leaf\":"
        << (scalar_leaf_type(info.type) ? "true" : "false");
    const auto friendly = friendly_property_name(info.name);
    if (friendly != info.name)
        out << ",\"friendly_name\":" << json_quote(friendly);
    const auto detail = property_type_detail(runtime, info, depth);
    if (!detail.empty())
    {
        out << ",\"type_detail\":" << detail;
    }
    out << '}';
    return out.str();
}

std::string property_type_detail(Runtime& runtime, const PropertyInfo& info, std::uint32_t depth)
{
    if (!info.address || depth > runtime.config.max_depth)
    {
        return {};
    }
    if (info.type == "StructProperty")
    {
        const auto type = safe_read_at<void*>(info.address, 0x78).value_or(nullptr);
        if (type)
        {
            return "{\"struct_name\":" + json_quote(object_name(runtime, type)) +
                   ",\"struct_path\":" + json_quote(object_path(runtime, type)) + '}';
        }
    }
    else if (info.type == "ArrayProperty")
    {
        const auto inner = safe_read_at<void*>(info.address, 0x78).value_or(nullptr);
        if (inner)
        {
            return "{\"inner\":" + property_descriptor(runtime, inner, depth + 1) + '}';
        }
    }
    else if (info.type == "MapProperty")
    {
        const auto key = safe_read_at<void*>(info.address, 0x78).value_or(nullptr);
        const auto value = safe_read_at<void*>(info.address, 0x80).value_or(nullptr);
        if (key && value)
        {
            return "{\"key\":" + property_descriptor(runtime, key, depth + 1) +
                   ",\"value\":" + property_descriptor(runtime, value, depth + 1) + '}';
        }
    }
    else if (info.type == "SetProperty")
    {
        const auto element = safe_read_at<void*>(info.address, 0x78).value_or(nullptr);
        if (element)
        {
            return "{\"element\":" + property_descriptor(runtime, element, depth + 1) + '}';
        }
    }
    else if (info.type == "EnumProperty")
    {
        const auto underlying = safe_read_at<void*>(info.address, 0x78).value_or(nullptr);
        const auto enum_object = safe_read_at<void*>(info.address, 0x80).value_or(nullptr);
        std::ostringstream out;
        out << "{\"enum_name\":" << json_quote(object_name(runtime, enum_object));
        if (underlying)
        {
            out << ",\"underlying\":" << property_descriptor(runtime, underlying, depth + 1);
        }
        out << '}';
        return out.str();
    }
    else if (info.type == "ByteProperty")
    {
        const auto enum_object = safe_read_at<void*>(info.address, 0x78).value_or(nullptr);
        if (enum_object)
        {
            return "{\"enum_name\":" + json_quote(object_name(runtime, enum_object)) + '}';
        }
    }
    else if (info.type == "ClassProperty" || info.type == "SoftClassProperty")
    {
        const auto meta_class = safe_read_at<void*>(info.address, 0x80).value_or(nullptr);
        if (meta_class)
        {
            return "{\"meta_class\":" + json_quote(object_path(runtime, meta_class)) + '}';
        }
    }
    else if (info.type == "ObjectProperty" || info.type == "ObjectPtrProperty" ||
             info.type == "SoftObjectProperty" || info.type == "WeakObjectProperty")
    {
        const auto object_class = safe_read_at<void*>(info.address, 0x78).value_or(nullptr);
        if (object_class)
        {
            return "{\"object_class\":" + json_quote(object_path(runtime, object_class)) + '}';
        }
    }
    return {};
}

std::string fallback_value(Runtime& runtime,
                           std::string_view type,
                           const void* address,
                           std::size_t size,
                           std::string_view reason = {})
{
    std::ostringstream out;
    out << "{\"$type\":" << json_quote(type);
    if (!reason.empty())
    {
        out << ",\"$reason\":" << json_quote(reason);
    }
    out << ",\"$size\":" << size;
    if (runtime.config.write_raw_fallback)
    {
        out << ",\"$raw_hex\":" << json_quote(raw_hex(address, size));
        if (size > 512)
        {
            out << ",\"$raw_truncated\":true";
        }
    }
    out << '}';
    return out.str();
}

template <typename T>
std::string numeric_json(const void* address)
{
    const auto value = safe_read<T>(address);
    if (!value)
    {
        return "null";
    }
    if constexpr (std::is_floating_point_v<T>)
    {
        if (!std::isfinite(*value))
        {
            return json_quote(std::isnan(*value) ? "NaN" : (*value > 0 ? "Infinity" : "-Infinity"));
        }
        std::ostringstream out;
        out << std::setprecision(std::numeric_limits<T>::max_digits10) << *value;
        return out.str();
    }
    else if constexpr (std::is_same_v<T, std::int8_t> || std::is_same_v<T, std::uint8_t>)
    {
        return std::to_string(static_cast<int>(*value));
    }
    else
    {
        return std::to_string(*value);
    }
}

std::optional<std::int64_t> signed_property_value(std::string_view type, const void* address)
{
    if (type == "Int8Property")
    {
        const auto value = safe_read<std::int8_t>(address);
        return value ? std::optional<std::int64_t>(static_cast<std::int64_t>(*value)) : std::nullopt;
    }
    if (type == "Int16Property")
    {
        const auto value = safe_read<std::int16_t>(address);
        return value ? std::optional<std::int64_t>(static_cast<std::int64_t>(*value)) : std::nullopt;
    }
    if (type == "IntProperty")
    {
        const auto value = safe_read<std::int32_t>(address);
        return value ? std::optional<std::int64_t>(static_cast<std::int64_t>(*value)) : std::nullopt;
    }
    if (type == "Int64Property")
        return safe_read<std::int64_t>(address);
    return std::nullopt;
}

std::optional<std::uint64_t> unsigned_property_value(std::string_view type, const void* address)
{
    if (type == "ByteProperty" || type == "UInt8Property")
    {
        const auto value = safe_read<std::uint8_t>(address);
        return value ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(*value)) : std::nullopt;
    }
    if (type == "UInt16Property")
    {
        const auto value = safe_read<std::uint16_t>(address);
        return value ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(*value)) : std::nullopt;
    }
    if (type == "UInt32Property")
    {
        const auto value = safe_read<std::uint32_t>(address);
        return value ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(*value)) : std::nullopt;
    }
    if (type == "UInt64Property")
        return safe_read<std::uint64_t>(address);
    return std::nullopt;
}

std::string enum_label(Runtime& runtime, const void* enum_object, std::int64_t value)
{
    if (!enum_object)
    {
        return {};
    }
    const auto names = safe_read_at<FStringHeader>(enum_object, 0x40);
    if (!names || names->num < 0 || names->num > names->max || names->num > 65536 || !names->data)
    {
        return {};
    }
    struct EnumPair
    {
        FName name;
        std::int64_t value;
    };
    for (std::int32_t i = 0; i < names->num; ++i)
    {
        const auto pair = safe_read<EnumPair>(
            reinterpret_cast<const std::uint8_t*>(names->data) + static_cast<std::size_t>(i) * sizeof(EnumPair));
        if (pair && pair->value == value)
        {
            return fname_to_string(runtime, pair->name);
        }
    }
    return {};
}

bool slot_allocated(const SparseContainerHeader& header, std::int32_t index)
{
    if (index < 0 || index >= header.num_slots)
    {
        return false;
    }
    const auto word_index = static_cast<std::size_t>(index / 32);
    std::uint32_t word{};
    if (header.secondary_flags)
    {
        if (!safe_copy(header.secondary_flags + word_index, &word, sizeof(word)))
        {
            return false;
        }
    }
    else
    {
        if (word_index >= std::size(header.inline_flags))
        {
            return false;
        }
        word = header.inline_flags[word_index];
    }
    return (word & (1u << (index & 31))) != 0;
}

bool sparse_header_valid(const SparseContainerHeader& header)
{
    if (header.num_slots < 0 || header.max_slots < header.num_slots ||
        header.num_slots > 10'000'000 || header.num_bits != header.num_slots ||
        header.max_bits < header.num_bits || header.max_bits > 100'000'000 ||
        header.num_free_indices < 0 || header.num_free_indices > header.num_slots ||
        (header.num_slots > 0 && !header.data) ||
        (header.num_free_indices > 0 &&
         (header.first_free_index < 0 || header.first_free_index >= header.num_slots)))
    {
        return false;
    }

    const auto word_count =
        static_cast<std::size_t>((static_cast<std::uint32_t>(header.num_bits) + 31u) / 32u);
    if (!header.secondary_flags && word_count > std::size(header.inline_flags))
    {
        return false;
    }

    // Read allocation words in bounded blocks: one guarded read per block,
    // rather than one VirtualQuery/ReadProcessMemory pair per word.
    std::array<std::uint32_t, 256> flags{};
    std::int32_t allocated_count{};
    for (std::size_t word_index = 0; word_index < word_count; ++word_index)
    {
        std::uint32_t word{};
        if (header.secondary_flags)
        {
            const auto block_index = word_index % flags.size();
            if (block_index == 0 &&
                !safe_copy(header.secondary_flags + word_index, flags.data(),
                           std::min(flags.size(), word_count - word_index) * sizeof(word)))
            {
                return false;
            }
            word = flags[block_index];
        }
        else
        {
            word = header.inline_flags[word_index];
        }

        if (word_index + 1 == word_count && (header.num_bits & 31) != 0)
        {
            word &= (1u << (header.num_bits & 31)) - 1u;
        }
        allocated_count += static_cast<std::int32_t>(std::popcount(word));
    }
    return allocated_count == header.num_slots - header.num_free_indices;
}

std::size_t align_up(std::size_t value, std::size_t alignment)
{
    if (alignment <= 1)
    {
        return value;
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

std::size_t property_alignment(Runtime& runtime, void* property)
{
    const auto info = inspect_property(runtime, property);
    if (info.type == "BoolProperty" || info.type == "ByteProperty" || info.type == "Int8Property" ||
        info.type == "UInt8Property")
        return 1;
    if (info.type == "Int16Property" || info.type == "UInt16Property")
        return 2;
    if (info.type == "IntProperty" || info.type == "UInt32Property" || info.type == "FloatProperty" ||
        info.type == "NameProperty")
        return 4;
    if (info.type == "StructProperty")
    {
        const auto type = safe_read_at<void*>(property, 0x78).value_or(nullptr);
        const auto alignment = type ? safe_read_at<std::int16_t>(type, 0x5C).value_or(0) : 0;
        if (alignment > 0 && alignment <= 16 && (alignment & (alignment - 1)) == 0)
        {
            return static_cast<std::size_t>(alignment);
        }
    }
    if (info.type == "EnumProperty")
    {
        const auto underlying = safe_read_at<void*>(property, 0x78).value_or(nullptr);
        if (underlying)
        {
            return property_alignment(runtime, underlying);
        }
    }
    return info.element_size >= 8 ? 8 : std::max<std::size_t>(1, static_cast<std::size_t>(info.element_size));
}

std::string serialize_property_scalar(Runtime& runtime,
                                      void* property,
                                      const void* value_address,
                                      std::uint32_t depth);

std::string serialize_struct(Runtime& runtime,
                             const void* struct_type,
                             const void* data,
                             std::uint32_t depth)
{
    if (!struct_type || !data || depth > runtime.config.max_depth)
    {
        return fallback_value(runtime, "StructProperty", data, 0, "maximum depth or invalid struct");
    }
    const auto properties = properties_for_struct(struct_type);
    if (properties.empty())
    {
        const auto size = safe_read_at<std::int32_t>(struct_type, 0x58).value_or(0);
        return fallback_value(runtime,
                              object_name(runtime, struct_type),
                              data,
                              size > 0 ? static_cast<std::size_t>(size) : 0,
                              "struct exposes no child properties");
    }

    std::ostringstream out;
    out << '{';
    bool first = true;
    for (void* property : properties)
    {
        const auto info = inspect_property(runtime, property);
        if (info.name.empty() || info.offset < 0 || info.element_size <= 0 || info.array_dim <= 0)
        {
            continue;
        }
        if (!first)
        {
            out << ',';
        }
        first = false;
        out << json_quote(info.name) << ':';
        const auto* base = reinterpret_cast<const std::uint8_t*>(data) + info.offset;
        if (info.array_dim > 1)
        {
            out << '[';
            for (std::int32_t i = 0; i < info.array_dim; ++i)
            {
                if (i)
                    out << ',';
                out << serialize_property_scalar(
                    runtime,
                    property,
                    base + static_cast<std::size_t>(i) * info.element_size,
                    depth + 1);
            }
            out << ']';
        }
        else
        {
            out << serialize_property_scalar(runtime, property, base, depth + 1);
        }
    }
    out << '}';
    return out.str();
}

std::string serialize_array(Runtime& runtime,
                            void* property,
                            const void* value_address,
                            std::uint32_t depth)
{
    const auto header = safe_read<FStringHeader>(value_address);
    const auto inner = safe_read_at<void*>(property, 0x78).value_or(nullptr);
    if (!header || !inner || header->num < 0 || header->max < header->num || header->max > 100'000'000 ||
        (header->num > 0 && !header->data))
    {
        return fallback_value(runtime, "ArrayProperty", value_address, 0x10, "invalid TArray");
    }
    const auto inner_info = inspect_property(runtime, inner);
    if (inner_info.element_size <= 0 || inner_info.element_size > 16 * 1024 * 1024)
    {
        return fallback_value(runtime, "ArrayProperty", value_address, 0x10, "invalid inner element size");
    }
    const auto cap = runtime.config.max_array_elements == 0
                         ? static_cast<std::uint32_t>(header->num)
                         : runtime.config.max_array_elements;
    const auto emit_count = std::min<std::uint32_t>(static_cast<std::uint32_t>(header->num), cap);
    std::ostringstream out;
    out << "{\"count\":" << header->num << ",\"items\":[";
    for (std::uint32_t i = 0; i < emit_count; ++i)
    {
        if (i)
            out << ',';
        out << serialize_property_scalar(
            runtime,
            inner,
            reinterpret_cast<const std::uint8_t*>(header->data) +
                static_cast<std::size_t>(i) * inner_info.element_size,
            depth + 1);
    }
    out << ']';
    if (emit_count < static_cast<std::uint32_t>(header->num))
    {
        out << ",\"truncated\":true";
    }
    out << '}';
    return out.str();
}

std::string serialize_set(Runtime& runtime,
                          void* property,
                          const void* value_address,
                          std::uint32_t depth)
{
    const auto header = safe_read<SparseContainerHeader>(value_address);
    const auto element = safe_read_at<void*>(property, 0x78).value_or(nullptr);
    if (!header || !element || !sparse_header_valid(*header))
    {
        return fallback_value(runtime, "SetProperty", value_address, 0x50, "invalid TSet");
    }
    const auto element_info = inspect_property(runtime, element);
    const auto alignment = property_alignment(runtime, element);
    const auto payload_size = align_up(static_cast<std::size_t>(element_info.element_size), alignment);
    const auto set_element_size = align_up(payload_size + 8, std::max<std::size_t>(alignment, 4));
    const auto cap = runtime.config.max_array_elements == 0
                         ? std::numeric_limits<std::uint32_t>::max()
                         : runtime.config.max_array_elements;
    std::uint32_t emitted{};
    std::int32_t live_count{};
    std::ostringstream out;
    out << "{\"items\":[";
    for (std::int32_t i = 0; i < header->num_slots; ++i)
    {
        if (!slot_allocated(*header, i))
            continue;
        ++live_count;
        if (emitted >= cap)
            continue;
        if (emitted++)
            out << ',';
        out << serialize_property_scalar(
            runtime,
            element,
            reinterpret_cast<const std::uint8_t*>(header->data) +
                static_cast<std::size_t>(i) * set_element_size,
            depth + 1);
    }
    out << "],\"count\":" << live_count;
    if (emitted < static_cast<std::uint32_t>(live_count))
        out << ",\"truncated\":true";
    out << '}';
    return out.str();
}

std::string serialize_map(Runtime& runtime,
                          void* property,
                          const void* value_address,
                          std::uint32_t depth)
{
    const auto header = safe_read<SparseContainerHeader>(value_address);
    const auto key = safe_read_at<void*>(property, 0x78).value_or(nullptr);
    const auto value = safe_read_at<void*>(property, 0x80).value_or(nullptr);
    if (!header || !key || !value || !sparse_header_valid(*header))
    {
        return fallback_value(runtime, "MapProperty", value_address, 0x50, "invalid TMap");
    }
    const auto key_info = inspect_property(runtime, key);
    const auto value_info = inspect_property(runtime, value);
    const auto key_alignment = property_alignment(runtime, key);
    const auto value_alignment = property_alignment(runtime, value);
    const auto pair_alignment = std::max(key_alignment, value_alignment);
    const auto value_offset = align_up(static_cast<std::size_t>(key_info.element_size), value_alignment);
    const auto pair_size =
        align_up(value_offset + static_cast<std::size_t>(value_info.element_size), pair_alignment);
    const auto set_element_size = align_up(pair_size + 8, std::max<std::size_t>(pair_alignment, 4));
    const auto cap = runtime.config.max_array_elements == 0
                         ? std::numeric_limits<std::uint32_t>::max()
                         : runtime.config.max_array_elements;
    std::uint32_t emitted{};
    std::int32_t live_count{};
    std::ostringstream out;
    out << "{\"entries\":[";
    for (std::int32_t i = 0; i < header->num_slots; ++i)
    {
        if (!slot_allocated(*header, i))
            continue;
        ++live_count;
        if (emitted >= cap)
            continue;
        if (emitted++)
            out << ',';
        const auto* pair =
            reinterpret_cast<const std::uint8_t*>(header->data) + static_cast<std::size_t>(i) * set_element_size;
        out << "{\"key\":" << serialize_property_scalar(runtime, key, pair, depth + 1)
            << ",\"value\":"
            << serialize_property_scalar(runtime, value, pair + value_offset, depth + 1) << '}';
    }
    out << "],\"count\":" << live_count;
    if (emitted < static_cast<std::uint32_t>(live_count))
        out << ",\"truncated\":true";
    out << '}';
    return out.str();
}

std::string serialize_property_scalar(Runtime& runtime,
                                      void* property,
                                      const void* value_address,
                                      std::uint32_t depth)
{
    const auto info = inspect_property(runtime, property);
    if (depth > runtime.config.max_depth)
    {
        return fallback_value(runtime,
                              info.type,
                              value_address,
                              std::max(0, info.element_size),
                              "maximum depth reached");
    }
    if (info.type == "BoolProperty")
    {
        const auto byte_offset = safe_read_at<std::uint8_t>(property, 0x79).value_or(0);
        const auto byte_mask = safe_read_at<std::uint8_t>(property, 0x7A).value_or(0);
        const auto field_mask = safe_read_at<std::uint8_t>(property, 0x7B).value_or(0);
        const auto byte = safe_read<std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(value_address) + byte_offset);
        if (!byte)
            return "null";
        const auto mask = field_mask == 0xFF ? 0xFF : (byte_mask ? byte_mask : field_mask);
        return ((*byte & mask) != 0) ? "true" : "false";
    }
    if (info.type == "Int8Property")
        return numeric_json<std::int8_t>(value_address);
    if (info.type == "Int16Property")
        return numeric_json<std::int16_t>(value_address);
    if (info.type == "IntProperty")
        return numeric_json<std::int32_t>(value_address);
    if (info.type == "Int64Property")
        return numeric_json<std::int64_t>(value_address);
    if (info.type == "ByteProperty" || info.type == "UInt8Property")
    {
        const auto numeric = safe_read<std::uint8_t>(value_address);
        if (!numeric)
            return "null";
        const auto enum_object = safe_read_at<void*>(property, 0x78).value_or(nullptr);
        const auto label = enum_label(runtime, enum_object, *numeric);
        if (label.empty())
            return std::to_string(*numeric);
        return "{\"name\":" + json_quote(label) + ",\"value\":" + std::to_string(*numeric) + '}';
    }
    if (info.type == "UInt16Property")
        return numeric_json<std::uint16_t>(value_address);
    if (info.type == "UInt32Property")
        return numeric_json<std::uint32_t>(value_address);
    if (info.type == "UInt64Property")
        return numeric_json<std::uint64_t>(value_address);
    if (info.type == "FloatProperty")
        return numeric_json<float>(value_address);
    if (info.type == "DoubleProperty")
        return numeric_json<double>(value_address);
    if (info.type == "NameProperty")
    {
        const auto value = safe_read<FName>(value_address);
        return value ? json_quote(fname_to_string(runtime, *value)) : "null";
    }
    if (info.type == "StrProperty")
    {
        return json_quote(read_fstring(value_address));
    }
    if (info.type == "TextProperty")
    {
        const auto text_data = safe_read<void*>(value_address).value_or(nullptr);
        const auto display_string = text_data
                                        ? safe_read_at<void*>(text_data, 0x8).value_or(nullptr)
                                        : nullptr;
        return display_string ? json_quote(read_fstring(display_string)) : "null";
    }
    if (info.type == "ObjectProperty" || info.type == "ObjectPtrProperty" ||
        info.type == "ClassProperty")
    {
        const auto object = safe_read<void*>(value_address).value_or(nullptr);
        return object ? json_quote(object_path(runtime, object)) : "null";
    }
    if (info.type == "InterfaceProperty")
    {
        const auto object = safe_read<void*>(value_address).value_or(nullptr);
        return object ? json_quote(object_path(runtime, object)) : "null";
    }
    if (info.type == "WeakObjectProperty" || info.type == "LazyObjectProperty")
    {
        const auto weak = safe_read<FWeakObjectPtrData>(value_address);
        void* object = nullptr;
        if (weak && runtime.object_array)
        {
            const auto object_array = safe_read<UObjectArrayHeader>(runtime.object_array);
            if (object_array && weak->object_index >= 0 &&
                weak->object_index < object_array->num_elements)
            {
                const auto chunk_index = weak->object_index / kElementsPerChunk;
                const auto in_chunk = weak->object_index % kElementsPerChunk;
                if (chunk_index < object_array->num_chunks)
                {
                    const auto chunks = object_array->objects;
                    const auto chunk =
                        chunks ? safe_read<void*>(chunks + chunk_index).value_or(nullptr) : nullptr;
                    const auto* item =
                        chunk ? reinterpret_cast<const std::uint8_t*>(chunk) +
                                    static_cast<std::size_t>(in_chunk) * kObjectItemSize
                              : nullptr;
                    const auto serial = item ? safe_read_at<std::int32_t>(item, 0x10) : std::nullopt;
                    if (serial && *serial == weak->object_serial_number)
                    {
                        object = safe_read<void*>(item).value_or(nullptr);
                    }
                }
            }
        }
        if (info.type == "WeakObjectProperty")
        {
            return object ? json_quote(object_path(runtime, object)) : "null";
        }

        const auto guid = safe_read_at<FUniqueObjectGuidData>(value_address, 0x0C);
        std::ostringstream out;
        out << "{\"object\":"
            << (object ? json_quote(object_path(runtime, object)) : std::string{"null"})
            << ",\"guid\":";
        if (guid)
        {
            std::ostringstream value;
            value << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << guid->a
                  << '-' << std::setw(8) << guid->b << '-' << std::setw(8) << guid->c << '-'
                  << std::setw(8) << guid->d;
            out << json_quote(value.str());
        }
        else
        {
            out << "null";
        }
        out << '}';
        return out.str();
    }
    if (info.type == "SoftObjectProperty" || info.type == "SoftClassProperty")
    {
        const auto asset_name = safe_read_at<FName>(value_address, 0x10);
        const auto sub_path = read_fstring(reinterpret_cast<const std::uint8_t*>(value_address) + 0x18);
        std::string path = asset_name ? fname_to_string(runtime, *asset_name) : std::string{};
        if (!sub_path.empty())
        {
            path += ':';
            path += sub_path;
        }
        return path.empty() ? "null" : json_quote(path);
    }
    if (info.type == "StructProperty")
    {
        const auto struct_type = safe_read_at<void*>(property, 0x78).value_or(nullptr);
        return serialize_struct(runtime, struct_type, value_address, depth + 1);
    }
    if (info.type == "ArrayProperty")
    {
        return serialize_array(runtime, property, value_address, depth + 1);
    }
    if (info.type == "MapProperty")
    {
        return serialize_map(runtime, property, value_address, depth + 1);
    }
    if (info.type == "SetProperty")
    {
        return serialize_set(runtime, property, value_address, depth + 1);
    }
    if (info.type == "EnumProperty")
    {
        const auto underlying = safe_read_at<void*>(property, 0x78).value_or(nullptr);
        const auto enum_object = safe_read_at<void*>(property, 0x80).value_or(nullptr);
        if (!underlying)
            return fallback_value(runtime, info.type, value_address, info.element_size, "missing underlying property");
        const auto underlying_info = inspect_property(runtime, underlying);
        std::int64_t signed_value{};
        bool has_value = false;
        if (const auto signed_numeric = signed_property_value(underlying_info.type, value_address))
        {
            signed_value = *signed_numeric;
            has_value = true;
        }
        else if (const auto unsigned_numeric =
                     unsigned_property_value(underlying_info.type, value_address))
        {
            signed_value = static_cast<std::int64_t>(*unsigned_numeric);
            has_value = true;
        }
        if (!has_value)
            return serialize_property_scalar(runtime, underlying, value_address, depth + 1);
        const auto label = enum_label(runtime, enum_object, signed_value);
        std::ostringstream out;
        out << "{\"name\":" << (label.empty() ? "null" : json_quote(label))
            << ",\"value\":" << signed_value << '}';
        return out.str();
    }
    return fallback_value(runtime,
                          info.type.empty() ? "UnknownProperty" : info.type,
                          value_address,
                          info.element_size > 0 ? static_cast<std::size_t>(info.element_size) : 0);
}

std::vector<std::int32_t> parse_pattern(std::string_view pattern)
{
    std::vector<std::int32_t> bytes;
    for (std::size_t i = 0; i < pattern.size();)
    {
        while (i < pattern.size() && pattern[i] == ' ')
            ++i;
        if (i >= pattern.size())
            break;
        if (pattern[i] == '?')
        {
            bytes.push_back(-1);
            while (i < pattern.size() && pattern[i] == '?')
                ++i;
            continue;
        }
        if (i + 1 >= pattern.size())
            break;
        const auto hex_digit = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        const int high = hex_digit(pattern[i]);
        const int low_digit = hex_digit(pattern[i + 1]);
        if (high < 0 || low_digit < 0)
            break;
        bytes.push_back((high << 4) | low_digit);
        i += 2;
    }
    return bytes;
}

std::vector<std::uint8_t*> scan_pattern(std::uint8_t* base,
                                        std::size_t image_size,
                                        std::string_view pattern,
                                        std::size_t max_matches)
{
    const auto bytes = parse_pattern(pattern);
    std::vector<std::uint8_t*> matches;
    if (!base || bytes.empty() || image_size < bytes.size())
    {
        return matches;
    }

    std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(base);
    const auto image_end = cursor + image_size;
    while (cursor < image_end && matches.size() < max_matches)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi))
            break;
        const auto region_begin = std::max(cursor, reinterpret_cast<std::uintptr_t>(mbi.BaseAddress));
        const auto region_end =
            std::min(image_end, reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize);
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
            (mbi.Protect & readable) != 0 && region_end > region_begin &&
            region_end - region_begin >= bytes.size())
        {
            auto* region = reinterpret_cast<std::uint8_t*>(region_begin);
            const auto length = static_cast<std::size_t>(region_end - region_begin);
            for (std::size_t i = 0; i + bytes.size() <= length && matches.size() < max_matches; ++i)
            {
                bool found = true;
                for (std::size_t j = 0; j < bytes.size(); ++j)
                {
                    if (bytes[j] >= 0 && region[i + j] != static_cast<std::uint8_t>(bytes[j]))
                    {
                        found = false;
                        break;
                    }
                }
                if (found)
                    matches.push_back(region + i);
            }
        }
        if (region_end <= cursor)
            break;
        cursor = region_end;
    }
    return matches;
}

std::uint8_t* relative_target(std::uint8_t* displacement_address)
{
    const auto displacement = safe_read<std::int32_t>(displacement_address);
    return displacement ? displacement_address + sizeof(std::int32_t) + *displacement : nullptr;
}

bool read_pe_metadata(Runtime& runtime)
{
    const auto dos = safe_read<IMAGE_DOS_HEADER>(runtime.image_base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return false;
    const auto* nt_address = runtime.image_base + dos->e_lfanew;
    const auto signature = safe_read<DWORD>(nt_address);
    const auto file_header = safe_read<IMAGE_FILE_HEADER>(nt_address + sizeof(DWORD));
    const auto optional = safe_read<IMAGE_OPTIONAL_HEADER64>(
        nt_address + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER));
    if (!signature || *signature != IMAGE_NT_SIGNATURE || !file_header || !optional ||
        optional->Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;
    runtime.exe_timestamp = file_header->TimeDateStamp;
    runtime.image_size = optional->SizeOfImage;
    return runtime.image_size > 0;
}

bool resolve_runtime(Runtime& runtime)
{
    const auto gobject_matches =
        scan_pattern(runtime.image_base, runtime.image_size, kGObjectsPattern, 512);
    for (auto* match : gobject_matches)
    {
        auto* candidate = relative_target(match + 3);
        const auto rva = reinterpret_cast<std::uintptr_t>(candidate) -
                         reinterpret_cast<std::uintptr_t>(runtime.image_base);
        if (candidate && rva < runtime.image_size && (rva % alignof(void*)) == 0)
        {
            runtime.object_array = reinterpret_cast<UObjectArrayHeader*>(candidate);
            break;
        }
    }

    const auto append_matches =
        scan_pattern(runtime.image_base, runtime.image_size, kAppendStringPattern, 16);
    if (!append_matches.empty())
    {
        auto* candidate = relative_target(append_matches.front() + 6);
        const auto rva = reinterpret_cast<std::uintptr_t>(candidate) -
                         reinterpret_cast<std::uintptr_t>(runtime.image_base);
        if (candidate && rva < runtime.image_size)
        {
            runtime.append_string = reinterpret_cast<Runtime::AppendStringFn>(candidate);
        }
    }

    if ((!runtime.object_array || !runtime.append_string) &&
        runtime.exe_timestamp == kKnownExeTimestamp && runtime.image_size == kKnownImageSize)
    {
        log_line("WARN", "One or more signatures failed; using fingerprint-gated SMTVV build 17140961 RVAs.");
        if (!runtime.object_array)
            runtime.object_array =
                reinterpret_cast<UObjectArrayHeader*>(runtime.image_base + kKnownGObjectsRva);
        if (!runtime.append_string)
            runtime.append_string =
                reinterpret_cast<Runtime::AppendStringFn>(runtime.image_base + kKnownAppendStringRva);
    }

    if (!runtime.object_array || !runtime.append_string)
    {
        log_line("ERROR", "Could not resolve GUObjectArray and FName::AppendString.");
        return false;
    }
    std::ostringstream message;
    message << "Resolved GUObjectArray RVA "
            << hex_value(reinterpret_cast<std::uintptr_t>(runtime.object_array) -
                         reinterpret_cast<std::uintptr_t>(runtime.image_base))
            << ", FName::AppendString RVA "
            << hex_value(reinterpret_cast<std::uintptr_t>(runtime.append_string) -
                         reinterpret_cast<std::uintptr_t>(runtime.image_base))
            << ", pattern matches=" << gobject_matches.size() << '/' << append_matches.size();
    log_line("INFO", message.str());
    return true;
}

bool wait_for_objects(Runtime& runtime, std::uint32_t timeout_ms)
{
    const auto deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline)
    {
        const auto header = safe_read<UObjectArrayHeader>(runtime.object_array);
        if (header && header->objects && header->num_elements > 1000 &&
            header->num_elements <= header->max_elements && header->num_chunks > 0 &&
            header->num_chunks <= header->max_chunks && header->num_chunks < 4096)
        {
            const auto none = fname_to_string(runtime, FName{});
            if (!none.empty())
            {
                std::ostringstream message;
                message << "UE runtime ready with " << header->num_elements << " UObjects and FName[0]="
                        << none;
                log_line("INFO", message.str());
                return true;
            }
        }
        Sleep(250);
    }
    log_line("ERROR", "Timed out waiting for GUObjectArray/FName readiness.");
    return false;
}

void* object_at(Runtime& runtime, std::int32_t index)
{
    const auto header = safe_read<UObjectArrayHeader>(runtime.object_array);
    if (!header || !header->objects || index < 0 || index >= header->num_elements)
        return nullptr;
    const auto chunk_index = index / kElementsPerChunk;
    const auto within_chunk = index % kElementsPerChunk;
    if (chunk_index < 0 || chunk_index >= header->num_chunks)
        return nullptr;
    const auto chunk = safe_read<void*>(header->objects + chunk_index).value_or(nullptr);
    if (!chunk)
        return nullptr;
    return safe_read<void*>(reinterpret_cast<const std::uint8_t*>(chunk) +
                            static_cast<std::size_t>(within_chunk) * kObjectItemSize)
        .value_or(nullptr);
}

struct ObjectRegistryView
{
    UObjectArrayHeader header{};
    std::vector<void*> chunks;
};

std::optional<ObjectRegistryView> capture_object_registry(Runtime& runtime)
{
    const auto header = safe_read<UObjectArrayHeader>(runtime.object_array);
    if (!header || !header->objects || header->num_elements < 0 ||
        header->num_elements > header->max_elements || header->num_chunks <= 0 ||
        header->num_chunks > header->max_chunks || header->num_chunks >= 4096)
        return std::nullopt;

    ObjectRegistryView view;
    view.header = *header;
    view.chunks.resize(static_cast<std::size_t>(header->num_chunks));
    if (!safe_copy(
            header->objects,
            view.chunks.data(),
            view.chunks.size() * sizeof(void*)))
        return std::nullopt;
    return view;
}

bool read_pointer_direct(const void* address, void** value) noexcept
{
    if (!address || !value)
        return false;
    __try
    {
        *value = *reinterpret_cast<void* const*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *value = nullptr;
        return false;
    }
}

bool read_fname_direct(const void* object, FName* value) noexcept
{
    if (!object || !value)
        return false;
    __try
    {
        *value = *reinterpret_cast<const FName*>(
            reinterpret_cast<const std::uint8_t*>(object) + 0x18);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *value = {};
        return false;
    }
}

void* object_at(const ObjectRegistryView& view, std::int32_t index) noexcept
{
    if (index < 0 || index >= view.header.num_elements)
        return nullptr;
    const auto chunk_index = index / kElementsPerChunk;
    const auto within_chunk = index % kElementsPerChunk;
    if (chunk_index < 0 ||
        static_cast<std::size_t>(chunk_index) >= view.chunks.size())
        return nullptr;
    void* chunk = view.chunks[static_cast<std::size_t>(chunk_index)];
    if (!chunk)
        return nullptr;
    void* object{};
    const auto* item = reinterpret_cast<const std::uint8_t*>(chunk) +
                       static_cast<std::size_t>(within_chunk) * kObjectItemSize;
    return read_pointer_direct(item, &object) ? object : nullptr;
}

std::optional<ResolvedLeaf> resolve_leaf(Runtime& runtime,
                                         void* row_struct,
                                         void* row_data,
                                         std::string_view requested_path,
                                         std::string& error, bool capture_trace = false,
                                         bool allow_complete = false, void* standalone_root = nullptr);

void* find_row(Runtime& runtime,
               void* table,
               std::string_view requested_name,
               std::optional<FName> requested_key = std::nullopt);

bool is_data_table(Runtime& runtime, const void* object);

bool write_bytes_safely(void* destination,
                        const std::uint8_t* source,
                        std::size_t size) noexcept;

void* find_function_in_class(Runtime& runtime,
                             const void* object,
                             std::string_view owning_class_name,
                             std::string_view function_name)
{
    void* current =
        safe_read_at<void*>(object, 0x10).value_or(nullptr);
    std::unordered_set<void*> visited_classes;
    for (std::size_t depth = 0;
         current && depth < 128 && visited_classes.insert(current).second;
         ++depth)
    {
        if (object_name(runtime, current) == owning_class_name)
        {
            void* field =
                safe_read_at<void*>(current, 0x48).value_or(nullptr);
            std::unordered_set<void*> visited_fields;
            for (std::size_t field_count = 0;
                 field && field_count < 4096 &&
                 visited_fields.insert(field).second;
                 ++field_count)
            {
                if (object_name(runtime, field) == function_name &&
                    class_name(runtime, field) == "Function")
                {
                    return field;
                }
                field =
                    safe_read_at<void*>(field, 0x28).value_or(nullptr);
            }
            return nullptr;
        }
        current =
            safe_read_at<void*>(current, 0x40).value_or(nullptr);
    }
    return nullptr;
}

bool is_data_table(Runtime& runtime, const void* object)
{
    const auto klass = safe_read_at<void*>(object, 0x10).value_or(nullptr);
    if (!klass)
        return false;
    if (const auto it = runtime.data_table_class_cache.find(klass);
        it != runtime.data_table_class_cache.end())
    {
        return it->second;
    }
    bool result = false;
    std::unordered_set<void*> visited;
    void* current = klass;
    for (std::size_t depth = 0; current && depth < 128 && visited.insert(current).second; ++depth)
    {
        if (object_name(runtime, current) == "DataTable")
        {
            result = true;
            break;
        }
        current = safe_read_at<void*>(current, 0x40).value_or(nullptr);
    }
    runtime.data_table_class_cache.emplace(klass, result);
    return result;
}

bool is_asset_class(Runtime& runtime, const void* object, std::string_view qualified_class)
{
    void* current = safe_read_at<void*>(object, 0x10).value_or(nullptr);
    std::unordered_set<void*> visited;
    for (std::size_t depth = 0;
         current && depth < 128 && visited.insert(current).second;
         ++depth)
    {
        if (object_path(runtime, current) == qualified_class)
            return true;
        current = safe_read_at<void*>(current, 0x40).value_or(nullptr);
    }
    return false;
}

bool is_script_message_asset(Runtime& runtime, const void* object)
{
    return is_asset_class(runtime, object, "/Script/Project.ScriptMessageAsset");
}

bool is_writable_range(void* address, std::size_t size)
{
    if (!address || size == 0)
        return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (begin > std::numeric_limits<std::uintptr_t>::max() - size)
        return false;
    const auto end = begin + size;
    auto cursor = begin;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(
                reinterpret_cast<void*>(cursor),
                &information,
                sizeof(information)) != sizeof(information) ||
            information.State != MEM_COMMIT ||
            (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;
        const DWORD writable =
            PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
            PAGE_EXECUTE_WRITECOPY;
        if ((information.Protect & writable) == 0)
            return false;
        const auto region_end =
            reinterpret_cast<std::uintptr_t>(information.BaseAddress) +
            information.RegionSize;
        if (region_end <= cursor)
            return false;
        cursor = std::min(region_end, end);
    }
    return true;
}

bool write_bytes_safely(void* destination,
                        const std::uint8_t* source,
                        std::size_t size) noexcept
{
    if (!destination || !source || size == 0 ||
        !is_writable_range(destination, size))
        return false;
    __try
    {
        std::memcpy(destination, source, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

std::string friendly_property_name(std::string_view runtime_name)
{
    const auto last = runtime_name.rfind('_');
    if (last == std::string_view::npos || runtime_name.size() - last - 1 != 32)
        return std::string(runtime_name);
    for (std::size_t index = last + 1; index < runtime_name.size(); ++index)
    {
        if (!std::isxdigit(static_cast<unsigned char>(runtime_name[index])))
            return std::string(runtime_name);
    }
    const auto prior = runtime_name.rfind('_', last - 1);
    if (prior == std::string_view::npos || prior + 1 == last)
        return std::string(runtime_name);
    for (std::size_t index = prior + 1; index < last; ++index)
    {
        if (!std::isdigit(static_cast<unsigned char>(runtime_name[index])))
            return std::string(runtime_name);
    }
    return std::string(runtime_name.substr(0, prior));
}

std::optional<std::vector<PathSegment>> parse_property_path(
    std::string_view path,
    std::string& error)
{
    std::vector<PathSegment> segments;
    std::size_t cursor{};
    while (cursor < path.size())
    {
        const auto name_begin = cursor;
        while (cursor < path.size() && path[cursor] != '.' &&
               path[cursor] != '[')
            ++cursor;
        if (cursor == name_begin)
        {
            error = "empty property path segment";
            return std::nullopt;
        }
        PathSegment segment;
        segment.name = std::string(path.substr(name_begin, cursor - name_begin));
        if (cursor < path.size() && path[cursor] == '[')
        {
            ++cursor;
            const auto index_begin = cursor;
            while (cursor < path.size() &&
                   std::isdigit(static_cast<unsigned char>(path[cursor])))
                ++cursor;
            if (index_begin == cursor || cursor >= path.size() ||
                path[cursor] != ']')
            {
                error = "array index must be an unsigned integer in brackets";
                return std::nullopt;
            }
            std::size_t index{};
            const auto conversion = std::from_chars(
                path.data() + index_begin, path.data() + cursor, index);
            if (conversion.ec != std::errc{})
            {
                error = "array index is out of range";
                return std::nullopt;
            }
            segment.index = index;
            ++cursor;
        }
        segments.emplace_back(std::move(segment));
        if (cursor == path.size())
            break;
        if (path[cursor] != '.')
        {
            error = "expected '.' between property path segments";
            return std::nullopt;
        }
        ++cursor;
        if (cursor == path.size())
        {
            error = "property path cannot end with '.'";
            return std::nullopt;
        }
    }
    if (segments.empty())
    {
        error = "property path is empty";
        return std::nullopt;
    }
    return segments;
}

void* match_property(Runtime& runtime,
                     const void* struct_type,
                     std::string_view requested,
                     std::string& error)
{
    void* friendly_match{};
    void* exact_match{};
    std::string friendly_runtime_name;
    const auto walked=visit_property_names(struct_type,[&](void* property,const FName& name)
    {
        // Selection needs only the name. Decode layout/type/flags once the
        // caller has selected a field, instead of for every unrelated field.
        const auto runtime_name = fname_to_string(runtime, name);
        if (infinifusion::unreal::reflected_name_equal(runtime_name, requested))
        { exact_match=property; return false; }
        if (infinifusion::unreal::reflected_name_equal(friendly_property_name(runtime_name), requested))
        {
            if (friendly_match)
            {
                error =
                    "friendly property name '" + std::string(requested) +
                    "' is ambiguous between '" + friendly_runtime_name +
                    "' and '" + runtime_name + "'";
                return false;
            }
            friendly_match = property;
            friendly_runtime_name = runtime_name;
        }
        return true;
    });
    if(!walked) {error="reflected property chain unreadable or cyclic";return nullptr;}
    if(exact_match) return exact_match;
    if(!error.empty()) return nullptr;
    if (!friendly_match && error.empty())
        error = "property '" + std::string(requested) + "' was not found";
    return friendly_match;
}

std::optional<ResolvedLeaf> resolve_leaf(Runtime& runtime,
                                         void* row_struct,
                                         void* row_data,
                                         std::string_view requested_path,
                                         std::string& error, bool capture_trace,
                                         bool allow_complete, void* standalone_root)
{
    auto segments = parse_property_path(requested_path, error);
    if (!segments)
        return std::nullopt;
    void* current_struct = row_struct;
    auto* current_data = reinterpret_cast<std::uint8_t*>(row_data);
    std::string canonical;
    std::vector<std::uintptr_t> trace;
    bool dynamic_storage = false;
    if (!row_struct || !row_data) { error = "missing root structure/storage"; return std::nullopt; }
    for (std::size_t segment_index = 0;
         segment_index < segments->size();
         ++segment_index)
    {
        const auto& segment = (*segments)[segment_index];
        void* property =
            match_property(runtime, current_struct, segment.name, error);
        if (!property)
            return std::nullopt;
        auto info = inspect_property(runtime, property);
        if (info.offset < 0 || info.element_size <= 0 || info.array_dim <= 0)
        {
            error = "property '" + info.name + "' has invalid runtime layout";
            return std::nullopt;
        }
        const auto structure_size=safe_read_at<std::int32_t>(current_struct,0x58);
        const auto property_size=static_cast<std::uint64_t>(info.element_size)*info.array_dim;
        if(!structure_size || *structure_size<=0 || info.offset>*structure_size ||
           property_size>static_cast<std::uint64_t>(*structure_size-info.offset))
        {
            error="property '"+info.name+"' exceeds its reflected owner storage";
            return std::nullopt;
        }
        if (capture_trace) trace.insert(trace.end(), {reinterpret_cast<std::uintptr_t>(current_struct),
            reinterpret_cast<std::uintptr_t>(current_data), reinterpret_cast<std::uintptr_t>(property),
            static_cast<std::uintptr_t>(info.offset), static_cast<std::uintptr_t>(info.element_size),
            static_cast<std::uintptr_t>(info.array_dim)});
        if (!canonical.empty())
            canonical.push_back('.');
        canonical += info.name;
        const bool last = segment_index + 1 == segments->size();
        const bool complete = allow_complete && last && !segment.index;
        if (segment_index == 0 && standalone_root && property != standalone_root)
        {
            error = "standalone storage does not match the requested root property";
            return std::nullopt;
        }
        auto* value_address = segment_index == 0 && standalone_root
            ? current_data : current_data + info.offset;

        if (info.array_dim > 1 && !complete)
        {
            if (!segment.index)
            {
                error = "static array property '" + info.name +
                        "' requires an index";
                return std::nullopt;
            }
            if (*segment.index >= static_cast<std::size_t>(info.array_dim))
            {
                error = "static array index is outside '" + info.name + "'";
                return std::nullopt;
            }
            value_address += *segment.index *
                             static_cast<std::size_t>(info.element_size);
            canonical += '[' + std::to_string(*segment.index) + ']';
        }
        else if (segment.index && info.type != "ArrayProperty")
        {
            error = "non-array property '" + info.name +
                    "' cannot have an index";
            return std::nullopt;
        }

        if (info.type == "ArrayProperty" && !complete)
        {
            if (!segment.index)
            {
                error = "dynamic array property '" + info.name +
                        "' requires an existing element index";
                return std::nullopt;
            }
            const auto header = safe_read<FStringHeader>(value_address);
            void* inner =
                safe_read_at<void*>(property, 0x78).value_or(nullptr);
            if (!header || !inner || header->num < 0 ||
                header->max < header->num ||
                *segment.index >= static_cast<std::size_t>(header->num) ||
                !header->data)
            {
                error = "dynamic array index is outside '" + info.name + "'";
                return std::nullopt;
            }
            const auto inner_info = inspect_property(runtime, inner);
            if (inner_info.element_size <= 0)
            {
                error = "dynamic array '" + info.name +
                        "' has an invalid inner property";
                return std::nullopt;
            }
            dynamic_storage = true;
            if (capture_trace) trace.insert(trace.end(), {reinterpret_cast<std::uintptr_t>(value_address),
                reinterpret_cast<std::uintptr_t>(header->data), static_cast<std::uintptr_t>(header->num),
                static_cast<std::uintptr_t>(header->max), reinterpret_cast<std::uintptr_t>(inner),
                static_cast<std::uintptr_t>(inner_info.element_size)});
            value_address =
                reinterpret_cast<std::uint8_t*>(header->data) +
                *segment.index *
                    static_cast<std::size_t>(inner_info.element_size);
            canonical += '[' + std::to_string(*segment.index) + ']';
            property = inner;
            info = inner_info;
        }

        if (last)
        {
            if (!allow_complete && (info.type == "StructProperty" ||
                info.type == "ArrayProperty" ||
                info.type == "MapProperty" ||
                info.type == "SetProperty"))
            {
                error = "property path resolves to container type '" +
                        info.type + "'; select a supported leaf";
                return std::nullopt;
            }
            return ResolvedLeaf{
                .property = property,
                .address = value_address,
                .type = info.type,
                .canonical_path = std::move(canonical),
                .layout_trace = std::move(trace), .dynamic_storage = dynamic_storage};
        }

        if (info.type != "StructProperty")
        {
            error = "property '" + info.name +
                    "' is not a struct and cannot contain another segment";
            return std::nullopt;
        }
        current_struct =
            safe_read_at<void*>(property, 0x78).value_or(nullptr);
        if (!current_struct)
        {
            error = "struct property '" + info.name +
                    "' has no runtime struct type";
            return std::nullopt;
        }
        current_data = value_address;
    }
    error = "property path did not resolve";
    return std::nullopt;
}

void* find_row(Runtime& runtime,
               void* table,
               std::string_view requested_name,
               std::optional<FName> requested_key)
{
    const auto map =
        safe_read_at<SparseContainerHeader>(table, 0x30);
    if (!map || !sparse_header_valid(*map))
        return nullptr;
    constexpr std::size_t element_size = 0x18;
    constexpr std::size_t block_size = 256;
    std::array<std::uint8_t, element_size * block_size> pairs{};
    std::array<std::uint32_t, block_size / 32> flags{};
    bool pairs_copied{}, flags_copied{};
    for (std::int32_t index = 0; index < map->num_slots; ++index)
    {
        auto* pair = reinterpret_cast<std::uint8_t*>(map->data) +
                     static_cast<std::size_t>(index) * element_size;
        const auto block_index = static_cast<std::size_t>(index) % block_size;
        if (block_index == 0)
        {
            const auto count = std::min(block_size, static_cast<std::size_t>(map->num_slots - index));
            pairs_copied = safe_copy(pair, pairs.data(), count * element_size);
            flags_copied = map->secondary_flags &&
                safe_copy(map->secondary_flags + index / 32, flags.data(),
                          ((count + 31) / 32) * sizeof(std::uint32_t));
        }
        const auto allocated = flags_copied
            ? (flags[block_index / 32] & (1u << (index & 31))) != 0
            : slot_allocated(*map, index);
        if (!allocated) continue;
        std::optional<FName> name;
        if (pairs_copied)
        {
            FName copied{};
            std::memcpy(&copied, pairs.data() + block_index * element_size, sizeof(copied));
            name = copied;
        }
        else name = safe_read<FName>(pair); // Preserve field-read behavior on inaccessible padding.
        if (!name || (requested_key ? (name->comparison_index != requested_key->comparison_index || name->number != requested_key->number)
                                   : fname_to_string(runtime, *name) != requested_name))
            continue;
        // A snapshot accelerates search, but is not a durable row certificate.
        // Re-read the selected entry, allocation bit and container before use.
        const auto live_name = safe_read<FName>(pair);
        const auto live_row = safe_read_at<void*>(pair, 0x08);
        const auto live_map = safe_read_at<SparseContainerHeader>(table, 0x30);
        if (!live_name || !live_row || !live_map ||
            live_name->comparison_index != name->comparison_index || live_name->number != name->number ||
            std::memcmp(&*map, &*live_map, sizeof(*map)) != 0 || !slot_allocated(*live_map, index))
            return nullptr;
        if (pairs_copied)
        {
            void* copied_row{};
            std::memcpy(&copied_row, pairs.data() + block_index * element_size + 8, sizeof(copied_row));
            if (copied_row != *live_row) return nullptr;
        }
        return *live_row;
    }
    return nullptr;
}

std::string_view target_leaf_name(std::string_view path)
{
    const auto separator = path.find_last_of('.');
    return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

bool object_has_target_leaf_name(Runtime& runtime,
                                 const void* object,
                                 const std::unordered_set<std::string>& names)
{
    FName name{};
    return read_fname_direct(object, &name) &&
           names.contains(fname_to_string(runtime, name));
}

#ifdef INFINIFUSION_NATIVE_API
#include "unreal/object_discovery.inc"
// API179: matching native super-default getter loads UClass+118h, optionally
// calls CreateDefaultObject at vslot3A0, then reads the same field. We never
// invoke it: role queries may inspect an existing CDO, never create one.
bool certified_class_default_layout(Runtime& runtime) {
    static constexpr std::array<std::uint8_t,60> code{
        0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0x59,0x40,0x48,0x85,0xDB,0x74,0x25,
        0x48,0x8B,0x83,0x18,0x01,0x00,0x00,0x48,0x85,0xC0,0x75,0x1B,0x48,0x8B,0x03,
        0x48,0x8B,0xCB,0xFF,0x90,0xA0,0x03,0x00,0x00,0x48,0x8B,0x83,0x18,0x01,0x00,0x00,
        0x48,0x83,0xC4,0x20,0x5B,0xC3,0x33,0xC0,0x48,0x83,0xC4,0x20,0x5B,0xC3};
    std::array<std::uint8_t,code.size()> live{};
    return runtime.exe_timestamp==kKnownExeTimestamp && runtime.image_size==kKnownImageSize &&
        safe_copy(runtime.image_base+0xF4A550,live.data(),live.size()) && live==code;
}
#include "unreal/object_reflection.inc"
#include "unreal/object_targets.inc"
#include "unreal/object_serial.inc"
#include "unreal/object_inspection.inc"
#include "unreal/native_entry_hook.inc"
void emit_absolute_jump(std::uint8_t*, const void*);
#include "unreal/engine_allocator_probe.inc"
#include "unreal/managed_value_bridge.inc"
#include "unreal/object_property_provider.inc"
#include "unreal/game_services.inc"
#include "unreal/blueprint_template_boundary.inc"
#ifdef INFINIFUSION_DEVELOPMENT
#include "unreal/blueprint_creation_probe.inc"
#endif
#include "unreal/table_definition_provider.inc"
#include "unreal/message_definition_provider.inc"
#ifdef INFINIFUSION_DEVELOPMENT
#include "unreal/table_engine_probe.inc"
#endif
#include "unreal/asset_load_provider.inc"
#ifdef INFINIFUSION_DEVELOPMENT
#include "unreal/nkm_provider.inc"
#endif
#include "unreal/tbcr_trace.inc"
#include "runtime/api_runtime.inc"
#endif

void __fastcall game_engine_tick_hook(void* engine,
                                      float delta_seconds,
                                      std::uint8_t idle_mode)
{
    static thread_local bool inside_hook{};
    if (!inside_hook)
    {
        inside_hook = true;
#ifdef INFINIFUSION_NATIVE_API
        native_api_tick_guarded();
#endif
        inside_hook = false;
    }
    g_tick_hook.original(engine, delta_seconds, idle_mode);
}

void emit_absolute_jump(std::uint8_t* destination, const void* target)
{
    destination[0] = 0xFF;
    destination[1] = 0x25;
    destination[2] = 0x00;
    destination[3] = 0x00;
    destination[4] = 0x00;
    destination[5] = 0x00;
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    std::memcpy(destination + 6, &address, sizeof(address));
}

DWORD find_main_thread_id(DWORD fallback)
{
    DWORD result = fallback;
    ULONGLONG earliest = std::numeric_limits<ULONGLONG>::max();
    const DWORD process_id = GetCurrentProcessId();
    const DWORD current_thread_id = GetCurrentThreadId();
    const HANDLE snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;
    THREADENTRY32 entry{.dwSize = sizeof(entry)};
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID != process_id)
                continue;
            HANDLE thread = OpenThread(
                THREAD_QUERY_LIMITED_INFORMATION,
                FALSE,
                entry.th32ThreadID);
            if (!thread)
                continue;
            FILETIME creation{}, exit{}, kernel{}, user{};
            if (GetThreadTimes(
                    thread, &creation, &exit, &kernel, &user))
            {
                ULARGE_INTEGER value{};
                value.LowPart = creation.dwLowDateTime;
                value.HighPart = creation.dwHighDateTime;
                if (value.QuadPart < earliest)
                {
                    earliest = value.QuadPart;
                    result = entry.th32ThreadID;
                }
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result == current_thread_id && fallback != 0 ? fallback : result;
}

bool install_game_engine_tick_hook(Runtime& runtime)
{
    auto matches =
        scan_pattern(
            runtime.image_base,
            runtime.image_size,
            kGameEngineTickPattern,
            8);
    std::uint8_t* target =
        matches.size() == 1 ? matches.front() : nullptr;
    if (!target && runtime.exe_timestamp == kKnownExeTimestamp &&
        runtime.image_size == kKnownImageSize)
    {
        target = runtime.image_base + kKnownGameEngineTickRva;
        log_line(
            "WARN",
            "UGameEngine::Tick signature was not unique; using the "
            "fingerprint-gated build 17140961 RVA.");
    }
    if (!target ||
        !safe_copy(
            target,
            g_tick_hook.original_bytes.data(),
            g_tick_hook.original_bytes.size()) ||
        !std::equal(
            kKnownGameEngineTickPrologue.begin(),
            kKnownGameEngineTickPrologue.end(),
            g_tick_hook.original_bytes.begin()))
    {
        log_line(
            "ERROR",
            "Refusing to hook UGameEngine::Tick because its verified "
            "17-byte prologue does not match this build.");
        return false;
    }

    constexpr std::size_t jump_size = 14;
    constexpr std::size_t trampoline_size =
        kKnownGameEngineTickPrologue.size() + jump_size;
    auto* trampoline = static_cast<std::uint8_t*>(
        VirtualAlloc(
            nullptr,
            trampoline_size,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE));
    if (!trampoline)
    {
        log_line("ERROR", "Could not allocate the game-tick trampoline.");
        return false;
    }
    std::memcpy(
        trampoline,
        g_tick_hook.original_bytes.data(),
        g_tick_hook.original_bytes.size());
    emit_absolute_jump(
        trampoline + g_tick_hook.original_bytes.size(),
        target + g_tick_hook.original_bytes.size());

    DWORD trampoline_protection{};
    if (!VirtualProtect(trampoline, trampoline_size, PAGE_EXECUTE_READ, &trampoline_protection) ||
        !FlushInstructionCache(GetCurrentProcess(), trampoline, trampoline_size)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        log_line("ERROR", "Could not finalize the game-tick trampoline.");
        return false;
    }
    // Publish callable state before exposure, and retain it on uncertain rollback.
    auto retained_runtime = std::make_unique<Runtime>(runtime);
    const auto rva = static_cast<std::uintptr_t>(target - runtime.image_base);
    auto hook = std::make_unique<CertifiedEntryHook>(*retained_runtime, "UGameEngine::Tick",
        rva, kKnownGameEngineTickPrologue, rva, kKnownGameEngineTickPrologue,
        trampoline, reinterpret_cast<void*>(&game_engine_tick_hook));
    g_tick_hook.target = target;
    g_tick_hook.original = reinterpret_cast<GameEngineTickFn>(trampoline);
    if (!hook->install()) {
        g_tick_hook.original = nullptr;
        g_tick_hook.target = nullptr;
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    retained_runtime.release();
    hook.release(); // Every exposed hook and its trampoline live until process exit.

    std::ostringstream message;
    message << "Installed verified UGameEngine::Tick trampoline at RVA "
            << hex_value(
                   reinterpret_cast<std::uintptr_t>(target) -
                   reinterpret_cast<std::uintptr_t>(runtime.image_base))
            << "; signature_matches=" << matches.size()
            << ". This once-per-frame callback runs the shared API executor.";
    log_line("INFO", message.str());
    return true;
}

std::filesystem::path module_path(HMODULE module)
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

std::filesystem::path executable_path()
{
    return module_path(nullptr);
}

// Read-only inspection worker. Owns its copied caches/output state; never submits edits.
#ifdef INFINIFUSION_DEVELOPMENT
std::uint32_t fnv1a(std::string_view value)
{
    std::uint32_t hash = 2166136261u;
    for (const unsigned char c : value)
    {
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

std::string safe_table_file_name(std::string_view path)
{
    std::string leaf;
    if (const auto dot = path.find_last_of('.'); dot != std::string_view::npos)
        leaf.assign(path.substr(dot + 1));
    else if (const auto slash = path.find_last_of('/'); slash != std::string_view::npos)
        leaf.assign(path.substr(slash + 1));
    else
        leaf.assign(path);
    if (leaf.empty())
        leaf = "DataTable";
    for (char& c : leaf)
    {
        const auto uc = static_cast<unsigned char>(c);
        if (!(std::isalnum(uc) || c == '-' || c == '_'))
            c = '_';
    }
    if (leaf.size() > 96)
        leaf.resize(96);
    std::ostringstream out;
    out << leaf << '_' << std::hex << std::setw(8) << std::setfill('0') << fnv1a(path) << ".json";
    return out.str();
}

std::int32_t row_count(const SparseContainerHeader& map)
{
    if (map.num_slots < 0 || map.num_free_indices < 0 || map.num_free_indices > map.num_slots)
        return -1;
    return map.num_slots - map.num_free_indices;
}

bool atomic_write(const std::filesystem::path& path, const std::string& contents)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto temporary = path.wstring() + L".tmp";
    {
        std::ofstream stream(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
        if (!stream)
            return false;
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        stream.flush();
        if (!stream)
            return false;
    }
    if (!MoveFileExW(temporary.c_str(),
                     path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

TableSummary export_table(Runtime& runtime, void* table, std::uint32_t scan_index)
{
    TableSummary summary;
    summary.path = object_path(runtime, table);
    summary.class_name = class_name(runtime, table);
    const auto row_struct = safe_read_at<void*>(table, 0x28).value_or(nullptr);
    summary.row_struct_path = row_struct ? object_path(runtime, row_struct) : std::string{};
    const auto row_map = safe_read_at<SparseContainerHeader>(table, 0x30);
    if (!row_map || !sparse_header_valid(*row_map))
    {
        summary.error = "invalid RowMap";
        return summary;
    }
    summary.row_count = row_count(*row_map);
    if (summary.row_count < 0)
    {
        summary.error = "invalid RowMap live count";
        return summary;
    }
    if (summary.row_count == 0 && !runtime.config.include_empty_tables)
    {
        summary.error = "empty table skipped by configuration";
        return summary;
    }
    if (!row_struct)
    {
        summary.error = "null RowStruct";
        return summary;
    }

    const auto properties = properties_for_struct(row_struct);
    const auto struct_size = safe_read_at<std::int32_t>(row_struct, 0x58).value_or(0);
    const auto struct_alignment = safe_read_at<std::int16_t>(row_struct, 0x5C).value_or(0);
    summary.file_name = safe_table_file_name(summary.path);

    std::ostringstream out;
    out << "{\n  \"schema_version\":1,\n  \"captured_at_utc\":" << json_quote(utc_timestamp())
        << ",\n  \"scan_index\":" << scan_index << ",\n  \"game\":{\"name\":\"Shin Megami Tensei V: Vengeance\","
        << "\"engine\":\"UE4.27.2\",\"executable\":\"SMT5V-Win64-Shipping.exe\","
        << "\"pe_timestamp\":" << json_quote(hex_value(runtime.exe_timestamp))
        << ",\"image_size\":" << json_quote(hex_value(runtime.image_size)) << "},\n"
        << "  \"object_path\":" << json_quote(summary.path)
        << ",\n  \"class\":" << json_quote(summary.class_name)
        << ",\n  \"row_struct\":{\"name\":" << json_quote(object_name(runtime, row_struct))
        << ",\"path\":" << json_quote(summary.row_struct_path) << ",\"size\":" << struct_size
        << ",\"alignment\":" << struct_alignment << ",\"properties\":[";
    for (std::size_t i = 0; i < properties.size(); ++i)
    {
        if (i)
            out << ',';
        out << property_descriptor(runtime, properties[i], 0);
    }
    out << "]},\n  \"row_count\":" << summary.row_count << ",\n  \"rows\":[";

    const auto row_cap = runtime.config.max_rows_per_table == 0
                             ? std::numeric_limits<std::uint32_t>::max()
                             : runtime.config.max_rows_per_table;
    std::uint32_t emitted{};
    constexpr std::size_t kDataTableSetElementSize = 0x18;
    for (std::int32_t i = 0; i < row_map->num_slots && emitted < row_cap; ++i)
    {
        if (!slot_allocated(*row_map, i))
            continue;
        const auto* element = reinterpret_cast<const std::uint8_t*>(row_map->data) +
                              static_cast<std::size_t>(i) * kDataTableSetElementSize;
        const auto row_name = safe_read<FName>(element);
        const auto row_data = safe_read_at<void*>(element, 0x08).value_or(nullptr);
        if (!row_name || !row_data)
            continue;
        if (emitted++)
            out << ',';
        out << "\n    {\"name\":" << json_quote(fname_to_string(runtime, *row_name))
            << ",\"values\":" << serialize_struct(runtime, row_struct, row_data, 0) << '}';
    }
    if (emitted)
        out << '\n' << "  ";
    out << ']';
    if (emitted < static_cast<std::uint32_t>(summary.row_count))
    {
        out << ",\n  \"rows_truncated\":true,\n  \"rows_exported\":" << emitted;
    }
    out << ",\n  \"patch_template\":{\"schema_version\":1,\"asset_type\":\"data_table\",\n"
        << "    \"table\":" << json_quote(summary.path)
        << ",\"row_struct\":" << json_quote(summary.row_struct_path)
        << ",\"rows\":[]\n  }\n}\n";

    const auto final_row_struct = safe_read_at<void*>(table, 0x28).value_or(nullptr);
    const auto final_row_map = safe_read_at<SparseContainerHeader>(table, 0x30);
    if (final_row_struct != row_struct || !final_row_map ||
        std::memcmp(&*row_map, &*final_row_map, sizeof(SparseContainerHeader)) != 0)
    {
        summary.error = "RowMap changed during capture";
        return summary;
    }

    const auto path = runtime.scan_directory / L"tables" /
                      std::filesystem::path(summary.file_name);
    if (!atomic_write(path, out.str()))
    {
        summary.error = "could not write JSON";
        return summary;
    }
    summary.exported = true;
    return summary;
}

bool is_blueprint_object_export_candidate(Runtime& runtime, void* object)
{
    if (!object || is_data_table(runtime, object) ||
        is_script_message_asset(runtime, object))
        return false;
    const auto path = object_path(runtime, object);
    if (path.rfind("/Game/", 0) != 0)
        return false;
    const auto object_class =
        safe_read_at<void*>(object, 0x10).value_or(nullptr);
    if (!object_class)
        return false;
    const auto class_name_value = class_name(runtime, object);
    if (class_name_value == "Class" || class_name_value == "Function" ||
        class_name_value == "Package" || class_name_value == "Property")
        return false;
    if (properties_for_struct(object_class).empty())
        return false;
    const auto name = object_name(runtime, object);
    const bool generated_class =
        class_name_value.size() > 2 &&
        class_name_value.ends_with("_C");
    const bool component_template =
        class_name_value.find("Component") != std::string::npos ||
        name.find("GEN_VARIABLE") != std::string::npos;
    const bool class_default = name.rfind("Default__", 0) == 0;
    const bool blueprint_path = path.find("/Blueprints/") != std::string::npos;
    return generated_class || component_template || class_default || blueprint_path;
}

AssetExportSummary export_object_properties(Runtime& runtime,
                                             void* object,
                                             std::uint32_t scan_index)
{
    AssetExportSummary summary;
    summary.path = object_path(runtime, object);
    summary.class_name = class_name(runtime, object);
    const auto object_class =
        safe_read_at<void*>(object, 0x10).value_or(nullptr);
    if (!object_class)
    {
        summary.error = "object has no runtime class";
        return summary;
    }
    const auto properties = properties_for_struct(object_class);
    if (properties.empty())
    {
        summary.error = "runtime class exposes no reflected properties";
        return summary;
    }
    summary.file_name = safe_table_file_name(summary.path);

    std::ostringstream out;
    out << "{\n  \"schema_version\":1,\n  \"asset_type\":\"object\",\n"
        << "  \"captured_at_utc\":" << json_quote(utc_timestamp())
        << ",\n  \"scan_index\":" << scan_index
        << ",\n  \"object_path\":" << json_quote(summary.path)
        << ",\n  \"class\":" << json_quote(summary.class_name)
        << ",\n  \"class_path\":" << json_quote(object_path(runtime, object_class))
        << ",\n  \"property_source\":\"live_reflection\",\n  \"properties\":[";
    for (std::size_t index = 0; index < properties.size(); ++index)
    {
        if (index)
            out << ',';
        out << "\n    " << property_descriptor(runtime, properties[index], 0);
    }
    if (!properties.empty())
        out << '\n';
    out << "  ],\n  \"values\":"
        << serialize_struct(runtime, object_class, object, 0)
        << ",\n  \"patch_template\":{\"schema_version\":1,\"asset_type\":\"object\",\n"
        << "    \"object\":" << json_quote(summary.path)
        << ",\"class\":" << json_quote(object_path(runtime, object_class))
        << ",\"edits\":[]\n  }\n}\n";

    const auto path = runtime.scan_directory / L"objects" /
                      std::filesystem::path(summary.file_name);
    if (!atomic_write(path, out.str()))
    {
        summary.error = "could not write JSON";
        return summary;
    }
    summary.exported = true;
    return summary;
}

AssetExportSummary export_script_message_asset(Runtime& runtime,
                                                void* asset,
                                                std::uint32_t scan_index)
{
    AssetExportSummary summary;
    summary.path = object_path(runtime, asset);
    summary.class_name = class_name(runtime, asset);
    const auto asset_class = safe_read_at<void*>(asset, 0x10).value_or(nullptr);
    if (!asset_class)
    {
        summary.error = "asset has no runtime class";
        return summary;
    }
    const auto properties = properties_for_struct(asset_class);
    void* messages_property = nullptr;
    std::string property_error;
    messages_property = match_property(runtime, asset_class, "Messages", property_error);
    if (!messages_property)
    {
        summary.error = property_error.empty() ? "Messages property was not found" : property_error;
        return summary;
    }
    const auto messages_info = inspect_property(runtime, messages_property);
    const auto message_struct_property =
        safe_read_at<void*>(messages_property, 0x78).value_or(nullptr);
    const auto message_struct = message_struct_property
                                    ? safe_read_at<void*>(message_struct_property, 0x78).value_or(nullptr)
                                    : nullptr;
    const auto message_struct_info =
        message_struct_property ? inspect_property(runtime, message_struct_property) : PropertyInfo{};
    const auto messages = safe_read_at<FStringHeader>(asset, messages_info.offset);
    if (messages_info.type != "ArrayProperty" || !message_struct ||
        message_struct_info.type != "StructProperty" ||
        message_struct_info.element_size <= 0 || !messages || messages->num < 0 ||
        messages->max < messages->num || messages->num > 1'000'000 ||
        (messages->num > 0 && !messages->data))
    {
        summary.error = "Messages has an invalid runtime layout";
        return summary;
    }
    void* label_property = match_property(runtime, message_struct, "MessageLabel", property_error);
    void* pages_property = match_property(runtime, message_struct, "MessageTextPages", property_error);
    if (!label_property || !pages_property)
    {
        summary.error = property_error.empty() ? "message label/page properties were not found" : property_error;
        return summary;
    }
    const auto label_info = inspect_property(runtime, label_property);
    const auto pages_info = inspect_property(runtime, pages_property);
    const auto text_property = safe_read_at<void*>(pages_property, 0x78).value_or(nullptr);
    const auto text_info = text_property ? inspect_property(runtime, text_property) : PropertyInfo{};
    if (label_info.type != "NameProperty" || pages_info.type != "ArrayProperty" ||
        !text_property || text_info.type != "TextProperty" || text_info.element_size != 0x18)
    {
        summary.error = "MessageLabel or MessageTextPages has an unexpected reflected type";
        return summary;
    }
    summary.file_name = safe_table_file_name(summary.path);
    std::ostringstream out;
    out << "{\n  \"schema_version\":1,\n  \"asset_type\":\"script_message\",\n"
        << "  \"captured_at_utc\":" << json_quote(utc_timestamp())
        << ",\n  \"scan_index\":" << scan_index
        << ",\n  \"object_path\":" << json_quote(summary.path)
        << ",\n  \"class\":" << json_quote(summary.class_name)
        << ",\n  \"property_source\":\"live_reflection\",\n  \"properties\":[";
    for (std::size_t index = 0; index < properties.size(); ++index)
    {
        if (index)
            out << ',';
        out << "\n    " << property_descriptor(runtime, properties[index], 0);
    }
    if (!properties.empty())
        out << '\n';
    out << "  ],\n  \"messages\":[";
    bool first_message = true;
    for (std::int32_t index = 0; index < messages->num; ++index)
    {
        auto* message = reinterpret_cast<std::uint8_t*>(messages->data) +
                         static_cast<std::size_t>(index) *
                             static_cast<std::size_t>(message_struct_info.element_size);
        const auto label = safe_read_at<FName>(message, label_info.offset);
        const auto pages = safe_read_at<FStringHeader>(message, pages_info.offset);
        if (!label || !pages || pages->num < 0 || pages->max < pages->num ||
            pages->num > 1'000'000 || (pages->num > 0 && !pages->data))
            continue;
        if (!first_message)
            out << ',';
        first_message = false;
        out << "\n    {\"label\":" << json_quote(fname_to_string(runtime, *label))
            << ",\"pages\":[";
        for (std::int32_t page = 0; page < pages->num; ++page)
        {
            if (page)
                out << ',';
            auto* text = reinterpret_cast<std::uint8_t*>(pages->data) +
                         static_cast<std::size_t>(page) * 0x18;
            const auto text_data = safe_read<void*>(text).value_or(nullptr);
            const auto display_string = text_data
                                             ? safe_read_at<void*>(text_data, 0x8).value_or(nullptr)
                                             : nullptr;
            out << "{\"page\":" << page << ",\"text\":"
                << json_quote(display_string ? read_fstring(display_string) : std::string{})
                << '}';
        }
        out << "]}";
    }
    out << "\n  ],\n  \"patch_template\":{\"schema_version\":1,\"asset_type\":\"script_message\",\n"
        << "    \"asset\":" << json_quote(summary.path) << ",\"definitions\":[]\n  }\n}\n";

    const auto path = runtime.scan_directory / L"messages" /
                      std::filesystem::path(summary.file_name);
    if (!atomic_write(path, out.str()))
    {
        summary.error = "could not write JSON";
        return summary;
    }
    summary.exported = true;
    return summary;
}

void write_manifest(Runtime& runtime,
                    const std::vector<TableSummary>& tables,
                    const std::vector<AssetExportSummary>& messages,
                    const std::vector<AssetExportSummary>& objects,
                    std::uint32_t scan_index,
                    std::int32_t object_count)
{
    std::size_t exported{};
    for (const auto& table : tables)
        exported += table.exported ? 1 : 0;
    std::size_t exported_messages{};
    for (const auto& message : messages)
        exported_messages += message.exported ? 1 : 0;
    std::size_t exported_objects{};
    for (const auto& object : objects)
        exported_objects += object.exported ? 1 : 0;

    std::ostringstream out;
    out << "{\n  \"schema_version\":1,\n  \"captured_at_utc\":" << json_quote(utc_timestamp())
        << ",\n  \"scan_index\":" << scan_index << ",\n  \"game\":{"
        << "\"name\":\"Shin Megami Tensei V: Vengeance\",\"engine\":\"UE4.27.2\","
        << "\"steam_app_id\":1875830,\"executable\":\"SMT5V-Win64-Shipping.exe\","
        << "\"pe_timestamp\":" << json_quote(hex_value(runtime.exe_timestamp))
        << ",\"image_size\":" << json_quote(hex_value(runtime.image_size))
        << ",\"gobjects_rva\":"
        << json_quote(hex_value(reinterpret_cast<std::uintptr_t>(runtime.object_array) -
                                reinterpret_cast<std::uintptr_t>(runtime.image_base)))
        << ",\"fname_append_string_rva\":"
        << json_quote(hex_value(reinterpret_cast<std::uintptr_t>(runtime.append_string) -
                                reinterpret_cast<std::uintptr_t>(runtime.image_base)))
        << "},\n  \"uobject_count\":" << object_count << ",\n  \"discovered_table_count\":"
        << tables.size() << ",\n  \"exported_table_count\":" << exported
        << ",\n  \"discovered_script_message_count\":" << messages.size()
        << ",\n  \"exported_script_message_count\":" << exported_messages
        << ",\n  \"discovered_object_count\":" << objects.size()
        << ",\n  \"exported_object_count\":" << exported_objects
        << ",\n  \"tables\":[";
    for (std::size_t i = 0; i < tables.size(); ++i)
    {
        const auto& table = tables[i];
        if (i)
            out << ',';
        out << "\n    {\"object_path\":" << json_quote(table.path)
            << ",\"class\":" << json_quote(table.class_name)
            << ",\"row_struct_path\":" << json_quote(table.row_struct_path)
            << ",\"row_count\":" << table.row_count << ",\"exported\":"
            << (table.exported ? "true" : "false");
        if (!table.file_name.empty())
            out << ",\"file\":"
                << json_quote(runtime.scan_relative_directory + "/tables/" + table.file_name);
        if (!table.error.empty())
            out << ",\"note\":" << json_quote(table.error);
        out << '}';
    }
    if (!tables.empty())
        out << '\n' << "  ";
    out << "],\n  \"script_message_assets\":[";
    for (std::size_t i = 0; i < messages.size(); ++i)
    {
        const auto& message = messages[i];
        if (i)
            out << ',';
        out << "\n    {\"object_path\":" << json_quote(message.path)
            << ",\"class\":" << json_quote(message.class_name)
            << ",\"exported\":" << (message.exported ? "true" : "false");
        if (!message.file_name.empty())
            out << ",\"file\":"
                << json_quote(runtime.scan_relative_directory + "/messages/" + message.file_name);
        if (!message.error.empty())
            out << ",\"note\":" << json_quote(message.error);
        out << '}';
    }
    if (!messages.empty())
        out << '\n' << "  ";
    out << "],\n  \"objects\":[";
    for (std::size_t i = 0; i < objects.size(); ++i)
    {
        const auto& object = objects[i];
        if (i)
            out << ',';
        out << "\n    {\"object_path\":" << json_quote(object.path)
            << ",\"class\":" << json_quote(object.class_name)
            << ",\"exported\":" << (object.exported ? "true" : "false");
        if (!object.file_name.empty())
            out << ",\"file\":"
                << json_quote(runtime.scan_relative_directory + "/objects/" + object.file_name);
        if (!object.error.empty())
            out << ",\"note\":" << json_quote(object.error);
        out << '}';
    }
    if (!objects.empty())
        out << '\n' << "  ";
    out << "]\n}\n";
    if (!atomic_write(runtime.session_directory / L"manifest.json", out.str()))
        log_line("ERROR", "Failed to write export manifest.");
}

void export_scan(Runtime& runtime, std::uint32_t scan_index)
{
    const auto header = safe_read<UObjectArrayHeader>(runtime.object_array);
    if (!header || header->num_elements <= 0 || header->num_elements > header->max_elements)
    {
        log_line("ERROR", "GUObjectArray became invalid before scan.");
        return;
    }

    const auto scan_leaf = L"scan_" + std::to_wstring(scan_index);
    const auto scans_directory = runtime.session_directory / L"scans";
    const auto temporary_directory =
        scans_directory / std::filesystem::path(L"." + scan_leaf + L".tmp");
    const auto final_directory = scans_directory / std::filesystem::path(scan_leaf);
    std::error_code directory_error;
    if (std::filesystem::exists(temporary_directory, directory_error) ||
        std::filesystem::exists(final_directory, directory_error))
    {
        log_line("ERROR", "Refusing to reuse an existing scan directory.");
        return;
    }
    std::filesystem::create_directories(temporary_directory / L"tables", directory_error);
    std::filesystem::create_directories(temporary_directory / L"messages", directory_error);
    std::filesystem::create_directories(temporary_directory / L"objects", directory_error);
    if (directory_error)
    {
        log_line("ERROR", "Could not create temporary scan directory: " +
                              directory_error.message());
        return;
    }
    runtime.scan_directory = temporary_directory;
    runtime.scan_relative_directory = "scans/scan_" + std::to_string(scan_index);

    std::vector<TableSummary> tables;
    tables.reserve(1024);
    std::vector<AssetExportSummary> messages;
    messages.reserve(256);
    std::vector<AssetExportSummary> objects;
    objects.reserve(512);
    for (std::int32_t index = 0; index < header->num_elements; ++index)
    {
        void* object = object_at(runtime, index);
        if (!object)
            continue;
        if (is_data_table(runtime, object))
        {
            if (runtime.config.max_tables == 0 ||
                tables.size() < runtime.config.max_tables)
                tables.push_back(export_table(runtime, object, scan_index));
            continue;
        }
        if (runtime.config.export_script_message_assets &&
            is_script_message_asset(runtime, object) &&
            (runtime.config.max_script_message_assets == 0 ||
             messages.size() < runtime.config.max_script_message_assets))
        {
            messages.push_back(export_script_message_asset(runtime, object, scan_index));
            continue;
        }
        if (runtime.config.export_object_properties &&
            (runtime.config.max_object_exports == 0 ||
             objects.size() < runtime.config.max_object_exports) &&
            is_blueprint_object_export_candidate(runtime, object))
        {
            objects.push_back(export_object_properties(runtime, object, scan_index));
        }
    }

    std::filesystem::rename(temporary_directory, final_directory, directory_error);
    if (directory_error)
    {
        log_line("ERROR", "Could not publish completed scan directory: " +
                              directory_error.message());
        return;
    }
    runtime.scan_directory = final_directory;
    write_manifest(runtime, tables, messages, objects, scan_index, header->num_elements);

    std::size_t exported{};
    std::size_t rows{};
    for (const auto& table : tables)
    {
        if (table.exported)
        {
            ++exported;
            rows += static_cast<std::size_t>(std::max(0, table.row_count));
        }
    }
    std::size_t exported_messages{};
    for (const auto& message : messages)
        exported_messages += message.exported ? 1 : 0;
    std::size_t exported_objects{};
    for (const auto& object : objects)
        exported_objects += object.exported ? 1 : 0;
    std::ostringstream message;
    message << "Scan " << scan_index << " discovered " << tables.size() << " UDataTables; exported "
            << exported << " tables containing " << rows << " rows, "
            << exported_messages << " ScriptMessageAssets, and "
            << exported_objects << " reflected Blueprint/UObject exports.";
    log_line("INFO", message.str());
}

void capture_configured(Runtime runtime)
{
    if (runtime.config.output_directory.is_relative())
        runtime.config.output_directory = runtime.module_directory / runtime.config.output_directory;
    const auto stamp = utc_timestamp(true);
    runtime.session_directory = runtime.config.output_directory /
        std::filesystem::path(L"session_" + std::wstring(stamp.begin(), stamp.end()) + L"_pid" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::create_directories(runtime.session_directory / L"scans", error);
    if (error) { log_line("ERROR", "Could not create export directory: " + error.message()); return; }
    if (runtime.config.initial_delay_ms) Sleep(runtime.config.initial_delay_ms);
    for (std::uint32_t scan = 1; scan <= runtime.config.repeat_count; ++scan) {
        export_scan(runtime, scan);
        if (scan < runtime.config.repeat_count && runtime.config.repeat_interval_ms)
            Sleep(runtime.config.repeat_interval_ms);
    }
    log_line("INFO", "Configured read-only capture passes completed.");
}

void capture_configured_guarded(Runtime runtime) noexcept
{
    try { capture_configured(std::move(runtime)); }
    catch (const std::exception& error) { log_line("ERROR", "Read-only capture failed: " + std::string(error.what())); }
    catch (...) { log_line("ERROR", "Read-only capture failed with a native exception."); }
}

#endif // INFINIFUSION_DEVELOPMENT capture

void run_impl(HMODULE self_module, DWORD bootstrap_thread_id, StartupAdmission* admission)
{
    Runtime runtime;
    const auto self_path = module_path(self_module);
    runtime.module_directory = self_path.parent_path();
    const auto exe = executable_path();
    if (_wcsicmp(exe.filename().c_str(), L"SMT5V-Win64-Shipping.exe") != 0)
    {
        OutputDebugStringW(
            L"Infinifusion: host executable is not SMT5V-Win64-Shipping.exe; "
            L"runtime disabled.\n");
        return;
    }

    g_debug_logging = parse_bool(read_ini_string(find_config_file(runtime.module_directory),
        L"Logging", L"DebugMode", L"false"), false);
    auto logger = std::make_unique<RuntimeLog>();
    // Preserve the previous run for startup/crash troubleshooting. Worker IO only.
    std::error_code rotation_error;
    const auto previous_log = runtime.module_directory / L"infinifusion.previous.log";
    std::filesystem::remove(previous_log, rotation_error);
    rotation_error.clear();
    std::filesystem::rename(runtime.module_directory / L"infinifusion.log", previous_log, rotation_error);
    logger->file.open(runtime.module_directory / L"infinifusion.log",
                      std::ios::binary | std::ios::trunc);
    if (logger->file) {
        std::thread([sink=logger.get()] {
            RuntimeLog::Queue::Record record{};
            for (;;) {
                bool wrote = false;
                // Release the queue lock before every potentially blocking IO.
                for (unsigned i=0; i<128 && sink->queue.pop(record); ++i) {
                    sink->file << record.data();
                    wrote = true;
                }
                const auto dropped = sink->queue.take_dropped();
                if (dropped) {
                    sink->file << '[' << utc_timestamp() << "] [WARN] Runtime log records dropped=" << dropped << '\n';
                    wrote = true;
                }
                if (wrote) sink->file.flush();
                Sleep(10);
            }
        }).detach();
        g_runtime_log.store(logger.release(), std::memory_order_release);
    }
    {
        std::ostringstream message;
        message << "Infinifusion runtime loaded in PID "
                << GetCurrentProcessId()
                << ".";
        log_line("INFO", message.str());
    }

    log_line("INFO", "Profile=" + std::string(infinifusion::runtime::profile_name) +
        " version=0.9.0 API=0x0001000B debug=" + (g_debug_logging ? "true" : "false"));
    runtime.image_base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!runtime.image_base || !read_pe_metadata(runtime))
    {
        log_line("ERROR", "Could not read host PE metadata.");
        return;
    }
    {
        std::ostringstream message;
        message << "Host=" << narrow(exe.wstring()) << ", base="
                << hex_value(reinterpret_cast<std::uintptr_t>(runtime.image_base))
                << ", timestamp=" << hex_value(runtime.exe_timestamp)
                << ", image_size=" << hex_value(runtime.image_size);
        log_line("DEBUG", message.str());
    }

    runtime.config = read_config(runtime.module_directory);
    log_line(admission ? "INFO" : "WARN", admission ?
        "Startup mode=process-import; main entry awaits hook/definition admission outside loader lock." :
        "Startup mode=uncoordinated-load; early asset publication is not guaranteed.");
    const auto json_config =
        read_json_config(runtime.module_directory);
#ifdef INFINIFUSION_NATIVE_API
#ifdef INFINIFUSION_DEVELOPMENT
    const bool native_api_enabled = parse_bool(read_ini_string(
        runtime.module_directory / L"infinifusion.ini", L"API", L"Enabled", L"true"), true);
#else
    const bool native_api_enabled = true;
#endif
#else
    const bool native_api_enabled = false;
#endif
    if (!runtime.config.enabled && !native_api_enabled)
    {
        log_line(
            "INFO",
            "API host and read-only capture are disabled by configuration.");
        return;
    }

    if (!resolve_runtime(runtime))
        return;
    if (admission && admission->cancelled()) {
        log_line("ERROR", "Startup admission cancelled before hook installation; runtime stopped.");
        return;
    }
#ifdef INFINIFUSION_NATIVE_API
    if (native_api_enabled) {
        auto assets = std::make_unique<AssetLoadBoundary>(runtime, find_main_thread_id(bootstrap_thread_id));
        if (!assets->install()) {
            log_line("ERROR", "Shared asset boundary installation failed; API startup stopped.");
            return;
        }
        assets.release(); // Process lifetime, shared initial asset publication.
#ifdef INFINIFUSION_DEVELOPMENT
        const bool diagnostics = GetPrivateProfileIntW(L"Diagnostics", L"NkmStartupProbe", 0,
            (runtime.module_directory / L"infinifusion.ini").c_str()) != 0;
        if (diagnostics) {
            auto probe = std::make_unique<NkmBoundary>(runtime,true);
            if (probe->install()) probe.release(); // Opt-in read-only timing control.
        }
#endif
    }
#endif
#ifdef INFINIFUSION_NATIVE_API
    if (native_api_enabled)
    {
        // The API is the sole game-thread mutation executor.
        auto state = std::make_unique<NativeApiRuntime>();
        state->game_thread=find_main_thread_id(bootstrap_thread_id);
        state->runtime = std::make_unique<Runtime>(runtime);
        state->callback_budget_us = static_cast<std::uint32_t>(std::clamp(
            GetPrivateProfileIntW(L"API", L"CallbackBudgetUs", 1500,
                (runtime.module_directory / L"infinifusion.ini").c_str()), 250u, 8000u));
        state->discovery = std::make_unique<NativeObjectDiscovery>(*state->runtime);
        state->targets = std::make_unique<NativeObjectTargets>(*state->runtime,*state->discovery);
        state->object_property_timing=infinifusion::runtime::development_profile && GetPrivateProfileIntW(L"Diagnostics",L"ObjectPropertyTiming",0,
            (state->runtime->module_directory/L"infinifusion.ini").c_str())!=0;
        state->objects = std::make_shared<NativeObjectPropertyProvider>(*state->runtime,*state->targets);
        state->inspection = std::make_shared<NativeObjectInspectionProvider>(*state->runtime,*state->discovery);
        {
            state->tbcr_trace = std::make_unique<TbcrTrace>(*state->runtime);
            g_tbcr_trace.store(state->tbcr_trace.get(), std::memory_order_release);
        }
        state->host.add_provider(state->objects);
        state->host.add_provider(state->inspection);
        state->game_services=std::make_shared<NativeGameServices>(*state->runtime,*state->discovery,*state->targets,state->game_thread);
        state->host.set_game_services(state->game_services);
        if (!install_game_engine_tick_hook(*state->runtime)) {
            g_tbcr_trace.store(nullptr, std::memory_order_release);
            return;
        }
        auto* installed_api = state.release();
        g_native_api.store(installed_api, std::memory_order_release);
        {
            auto boundary=std::make_unique<BlueprintTemplateBoundary>(*installed_api->runtime);
            if (boundary->install()) boundary.release();
            else log_line("WARN","Blueprint template-copy boundary unavailable; early propagation is not covered");
        }
#ifdef INFINIFUSION_DEVELOPMENT
        if (GetPrivateProfileIntW(L"Diagnostics",L"BlueprintCreationProbe",0,
            (runtime.module_directory/L"infinifusion.ini").c_str())) {
            auto probe=std::make_unique<BlueprintCreationProbe>(*installed_api->runtime,find_main_thread_id(bootstrap_thread_id));
            if (probe->install()) probe.release(); // Hook context lives until process exit.
            else log_line("WARN","Blueprint creation observation unavailable; no coverage claimed");
        }
#endif
        installed_api->host.set_runtime_ready(true);
        log_line("INFO", "Infinifusion experimental API host ready; initial clients are being admitted.");
#ifdef INFINIFUSION_DEVELOPMENT
        native_api_load_plugins(*installed_api, runtime.module_directory);
#endif
        std::unique_ptr<infinifusion::json_mods::Client> json_client;
        if (json_config.enabled) {
            const auto root = json_config.mods_directory.is_relative() ? runtime.module_directory / json_config.mods_directory : json_config.mods_directory;
            json_client = std::make_unique<infinifusion::json_mods::Client>(installed_api->host.api(), root, IF_PERSISTENT_OVERLAY);
            if (!json_client->start()) json_client.reset();
            if (json_client) {
                // Initial stable reads run on the loader worker, before consumer
                // callbacks. Keep hot-reload debouncing; do not wait inside UE.
                json_client->poll(false,true);
                json_client->poll(false,true);
            }
        }
        if (admission && admission->cancelled()) {
            installed_api->host.fault("Startup admission timed out; early asset publication unavailable");
            log_line("ERROR", "Startup admission cancelled; API stopped before definitions became effective.");
            return;
        }
        const bool clients_admitted=!json_config.enabled || (json_client && json_client->has_submission());
        if (admission && clients_admitted) {
            // Only copied intent/format-provider work while startup is gated.
            // Pre-register requested paths so a pre-first-Tick class load can
            // reach the canonical property boundary without a registry scan.
            const bool frontier=installed_api->host.executor_definitions(
                std::chrono::steady_clock::now()+std::chrono::milliseconds(50));
            const auto watched=installed_api->host.object_targets();
            std::size_t count{};
            for (const auto& path:watched) if (installed_api->discovery->watch_target(path)) ++count;
            log_line(frontier && count==watched.size() ? "INFO" : "WARN",
                "Initial object intent frontier="+std::string(frontier ? "completed" : "pending")+
                " watched_targets="+std::to_string(count)+"/"+std::to_string(watched.size()));
        }
        if (admission && admission->cancelled()) {
            installed_api->host.fault("Startup cancelled during object-intent preparation; API writes stopped");
            log_line("ERROR","Startup object-intent handoff cancelled; callbacks remain disabled");
            return;
        }
        installed_api->initial_clients_admitted.store(clients_admitted,std::memory_order_release);
        installed_api->discovery_ready.store(true,std::memory_order_release);
        log_line(installed_api->initial_clients_admitted.load() ? "INFO" : "WARN",
            installed_api->initial_clients_admitted.load() ? "Initial API clients admitted; consumer reconciliation available." :
            "Initial JSON definitions not admitted; early consumer application unavailable.");
        // Hook and logical intent are ready without any UObject reads. Release
        // process startup NOW: waiting for engine objects while holding its
        // before-entry APC would deadlock engine initialization.
        const auto early_registry = safe_read<UObjectArrayHeader>(runtime.object_array);
        log_line("INFO", "Startup preparation complete; object_count="+
            std::to_string(early_registry ? early_registry->num_elements : -1));
        if (admission) admission->complete();
        if (!wait_for_objects(runtime, 60'000)) {
            installed_api->host.fault("UE object readiness timed out");
            return;
        }
#ifdef INFINIFUSION_DEVELOPMENT
        if (runtime.config.enabled)
            std::thread(capture_configured_guarded, runtime).detach();
#endif
        auto next_json = std::chrono::steady_clock::now();
        constexpr int json_hotkey_id = 0x4946;
        const bool json_hotkey_registered = json_client && json_config.reload_virtual_key &&
            RegisterHotKey(nullptr, json_hotkey_id, MOD_NOREPEAT, json_config.reload_virtual_key);
        if (json_client && !json_hotkey_registered)
            log_line("WARN", "JSON reload hotkey registration failed; using key polling fallback.");
        bool json_key_down{};
        bool initial_json = true;
        auto next_metrics = std::chrono::steady_clock::now();
        while (true) {
            const auto api = installed_api->host.api();
            for (auto& client : installed_api->clients) {
                const auto previous = client->state();
                client->poll();
                if (previous == infinifusion::host::ClientSession::State::Active &&
                    client->state() != previous) {
                    log_line("ERROR", "Native plugin callback failed; owner " +
                        std::to_string(client->owner()) + ": " + client->error());
                }
            }
#ifdef INFINIFUSION_LUA
            for (auto& client : installed_api->lua_clients) {
                const auto before = client->session().state();
                client->poll();
                if (before == infinifusion::host::ClientSession::State::Active &&
                    client->session().state() != before)
                    log_line("ERROR", "Lua mod stopped: " + client->metadata().id + ": " + client->error());
            }
#endif
            const bool key_down = json_config.reload_virtual_key && (GetAsyncKeyState(json_config.reload_virtual_key) & 0x8000);
            bool manual_reload = !json_hotkey_registered && key_down && !json_key_down;
            json_key_down = key_down;
            MSG json_message{};
            while (PeekMessageW(&json_message, nullptr, WM_HOTKEY, WM_HOTKEY, PM_REMOVE))
                if (json_message.wParam == json_hotkey_id) manual_reload = true;
            if (json_client && (manual_reload || std::chrono::steady_clock::now() >= next_json)) {

                json_client->poll(manual_reload, json_config.hot_reload || initial_json || manual_reload);
                initial_json = !json_client->has_active();
                next_json = std::chrono::steady_clock::now() + std::chrono::milliseconds(json_config.poll_interval_ms);
            }
            installed_api->host.drain_logs([](std::string_view message) { log_line("API", message); });
            const auto now = std::chrono::steady_clock::now();
            if (g_debug_logging && now >= next_metrics) {
                IF_Metrics metrics{}; metrics.size = sizeof(metrics);
                if (api.metrics(&metrics) == IF_OK) {
                    const auto times = installed_api->host.drain_timing();
                    std::ostringstream record;
                    record << "callbacks=" << metrics.callback_count << " samples=" << times.count
                           << " p50_us=" << times.p50_us << " p95_us=" << times.p95_us
                           << " p99_us=" << times.p99_us << " max_us=" << times.max_us
                           << " prefix_max_us=" << times.prefix_max_us
                           << " reconcile_max_us=" << times.reconcile_max_us
                           << " bind_max_us=" << times.bind_max_us
                           << " prepare_max_us=" << times.prepare_max_us
                           << " status_max_us=" << times.status_max_us
                           << " overruns=" << metrics.deadline_overruns << " queue=" << metrics.queue_depth
                           << " active=" << metrics.active_operations << " journal=" << metrics.journal_bytes
                           << " provider_calls=" << metrics.provider_calls << " sample_drops=" << times.dropped
                           << " log_drops=" << metrics.dropped_logs << " faulted=" << metrics.faulted;
                    log_line("API-PERF", record.str());
                }
                next_metrics = now + std::chrono::seconds(1);
            }
            Sleep(100);
        }
    }
#endif

    if (admission) admission->complete();
#ifdef INFINIFUSION_DEVELOPMENT
    if (runtime.config.enabled && wait_for_objects(runtime, 60'000))
        std::thread(capture_configured_guarded, runtime).detach();
#endif
}
} // namespace

void run(HMODULE self_module, DWORD bootstrap_thread_id, StartupAdmission* admission) noexcept
{
    struct Completion {
        StartupAdmission* admission;
        ~Completion() { if (admission) admission->complete(); }
    } completion{admission}; // Every disabled/error return releases forwarding.
    try
    {
        run_impl(self_module, bootstrap_thread_id, admission);
    }
    catch (const std::exception& exception)
    {
        log_line("FATAL",
            "Unhandled C++ exception in runtime worker: " +
                     std::string(exception.what()));
    }
    catch (...)
    {
        log_line("FATAL", "Unhandled non-standard C++ exception in runtime worker.");
    }
}
} // namespace smtvv
