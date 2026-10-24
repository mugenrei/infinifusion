#pragma once

#include "core/engine.hpp"
#include "core/tbcr_patch.hpp"
#include "core/table_patch.hpp"
#include "infinifusion/api.h"
#include "host/game_services.hpp"
#include <functional>
#include <chrono>
#include <memory>
#include <string>

namespace infinifusion::host {

struct TimingWindow {
    uint64_t count{}, dropped{}, p50_us{}, p95_us{}, p99_us{}, max_us{};
    uint64_t prefix_max_us{}, reconcile_max_us{}, status_max_us{};
    uint64_t bind_max_us{}, prepare_max_us{};
};

class Host {
public:
    Host();
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    core::Error add_provider(std::shared_ptr<core::Provider> provider);
    void set_game_services(std::shared_ptr<GameServices> services);
    void executor_tick(std::chrono::steady_clock::time_point deadline = {},
                       std::chrono::steady_clock::time_point callback_start = {});

    bool executor_definitions(std::chrono::steady_clock::time_point deadline);

    bool executor_object_definitions(std::string_view target,
                                     std::chrono::steady_clock::time_point deadline,
                                     bool refresh = true);

    std::vector<std::string> object_targets();
    void drain_logs(const std::function<void(std::string_view)>& sink);
    TimingWindow drain_timing();
    void set_runtime_ready(bool ready);

    bool notify_target(std::string_view provider, std::string_view target);
    void fault(std::string message);
    IF_Api api() const;
    std::vector<core::TbcrPatchProvider::Definition> tbcr_definitions();
    std::vector<core::TablePatchProvider::Definition> table_definitions();
    std::vector<core::TablePatchProvider::Definition> message_definitions();
    void table_observed(std::string_view asset, const IF_TableState& state);
    void tbcr_observed(std::string_view asset, const IF_TbcrState& state);
    struct Impl;
    Impl& impl();
private:
    bool executor_pass(std::chrono::steady_clock::time_point deadline,
                       std::chrono::steady_clock::time_point callback_start, bool definitions,
                       std::string_view object_target = {}, bool refresh_object = true);
    std::unique_ptr<Impl> impl_;
};

}
