#pragma once

#include "core/provider.hpp"
#include <chrono>
#include <string>

namespace infinifusion::host {

struct ObjectQuery {
    uint32_t flags{}, start_index{}, scan_limit{}, result_limit{};
    std::string path_contains, class_path;
};
struct FunctionCall {
    uint32_t flags{};
    core::ResourceLocator target;
    std::string function, arguments;
    uint64_t expected_object_index{}, expected_object_serial{};
    std::string expected_properties;
};
struct InvokeOutcome {
    std::string json;
    core::Error error;
    bool dispatched{};
    uint64_t object_index{}, object_serial{};
};

class GameServices {
public:
    virtual ~GameServices() = default;
    virtual core::Result<std::string> search(const ObjectQuery& query,
        std::chrono::steady_clock::time_point deadline) = 0;
    virtual InvokeOutcome invoke(const FunctionCall& call,
        std::chrono::steady_clock::time_point deadline) = 0;
};

}
