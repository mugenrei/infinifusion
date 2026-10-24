#pragma once

#include "core/provider.hpp"
#include <map>
#include <optional>

namespace infinifusion::core {

using OwnerToken = std::uint64_t;

struct OwnerInfo {
    std::string id;
    std::string display_name;
    std::int32_t priority = 0;
    std::string ordering_key;
};

enum class DriftPolicy { PreserveExternalChange, PersistentOverlay };
enum class RestorePolicy { Required, NoRestore };
enum class OperationState {
    Accepted, WaitingForRuntime, WaitingForTarget, Prepared, Applied,
    AlreadyEffective, Shadowed, Rejected, Drifted, Removing, Removed,
    RestoreFailed, TargetExpired, Cancelled, Faulted, OutcomeUnknown
};
std::string_view to_string(OperationState) noexcept;

struct Assignment {
    ResourceLocator locator;
    PropertyPath path;
    Value value;
    std::optional<Value> expected_original;
    DriftPolicy drift_policy = DriftPolicy::PreserveExternalChange;
    RestorePolicy restore_policy = RestorePolicy::Required;
    std::uint64_t group = 0;
};

struct PatchSet {
    std::string id;
    std::uint64_t revision = 0;
    std::int64_t order = 0;
    std::vector<Assignment> assignments;
};

struct CatalogEntry {
    OwnerInfo owner;
    PatchSet patch;
};

struct RequestReceipt {
    std::uint64_t request_id = 0;
    std::uint64_t revision = 0;
};

struct OperationResult {
    OwnerToken owner = 0;
    std::string owner_id;
    std::string set_id;
    std::uint64_t operation_index = 0;
    std::uint64_t revision = 0;
    std::uint64_t request_id = 0;
    OperationState state = OperationState::Accepted;
    Error error;
    TargetIdentity identity;
    std::string canonical_path;
    std::string capability = "unreal.scalar.assign@1";
    OwnerToken effective_owner = 0;
    std::string effective_set;
    std::uint64_t effective_revision = 0;
    std::uint64_t updated_at_us = 0;
    bool baseline_restore_skipped = false;
    bool drift_detected = false;
};

struct RequestStatus {
    RequestReceipt receipt;
    std::vector<OperationResult> operations;
    bool observed = false;
    bool settled = false;
    std::size_t pending_count = 0;
    std::size_t rejected_count = 0;
    Error error;
};

struct ReadResult {
    Region region;
    Value value;
};

struct ReconcileReport {
    std::size_t active_operations = 0;
    std::size_t processed_operations = 0;
    std::size_t provider_calls = 0;
    std::size_t journal_bytes = 0;
    std::size_t expired_journal_bytes = 0;
    std::uint64_t elapsed_us = 0;
    std::uint64_t bind_us = 0;
    std::uint64_t prepare_us = 0;
    Error error;
};

struct EngineLimits {
    std::size_t max_active_operations = 128;
    std::size_t max_owners = 128;
    std::size_t max_journal_bytes = 1024;
    std::size_t max_retained_requests = 1024;
    std::size_t max_providers = 16;
    std::size_t max_whole_journal_bytes = 65536;
};

class Engine {
public:
    explicit Engine(EngineLimits limits = {});
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Error add_provider(std::shared_ptr<Provider>);
    Result<OwnerToken> register_owner(OwnerInfo);
    Result<RequestReceipt> replace_set(OwnerToken, PatchSet);

    Result<RequestReceipt> publish_catalog(OwnerToken controller, std::uint64_t revision,
                                          std::vector<CatalogEntry> entries);
    Result<RequestReceipt> remove_set(OwnerToken, std::string_view set_id);
    Result<RequestReceipt> disable_owner(OwnerToken);
    Result<ReadResult> read(OwnerToken, const ResourceLocator&, const PropertyPath&);
    Result<RequestStatus> status(std::uint64_t request_id) const;

    Error release_request(std::uint64_t request_id);

    Error resume(OwnerToken, std::string_view set_id, std::uint64_t revision,
                 std::size_t operation_index, const Value& observed_external);

    void notify_target(std::string_view provider, std::string_view target);

    std::vector<std::string> targets(std::string_view provider) const;

    ReconcileReport reconcile(std::span<const std::string_view> providers = {});
    const EngineLimits& limits() const noexcept { return limits_; }
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    EngineLimits limits_;
};

}
