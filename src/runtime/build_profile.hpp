#pragma once
#include <initializer_list>
#include <string_view>

namespace infinifusion::runtime {
#if defined(INFINIFUSION_DEVELOPMENT)
inline constexpr bool development_profile = true;
inline constexpr std::string_view profile_name = "Development";
#else
inline constexpr bool development_profile = false;
inline constexpr std::string_view profile_name = "Release";
#endif

constexpr bool log_enabled(std::string_view level, bool debug,
                           std::string_view message = {}) noexcept {
    if (debug) return true;
    if (level == "WARN" || level == "ERROR" || level == "FATAL") return true;
    if (level == "INFO") {
        for (const auto prefix : {"TBCR source", "TBCR outcome", "TBCR initial",
             "TABLE outcome", "TBCR trace armed", "Fusion initializer discovery"})
            if (message.starts_with(prefix)) return false;
        return true;
    }
    if (level == "API") {

        const bool field_progress = message.starts_with("Plugin[") &&
            message.find(" JSON ") != std::string_view::npos &&
            message.find(" JSON revision=") == std::string_view::npos;
        return !field_progress || (message.find(" error=0 ") == std::string_view::npos &&
                                  message.find(" error=27 ") == std::string_view::npos);
    }
    return false;
}
}
