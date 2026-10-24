#include "core/engine.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <new>
#include <set>
#include <tuple>

namespace infinifusion::core {
namespace {
std::uint64_t now_us() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool pending(OperationState state) {
    return state == OperationState::Accepted || state == OperationState::Prepared ||
        state == OperationState::WaitingForRuntime || state == OperationState::WaitingForTarget ||
        state == OperationState::Removing || state == OperationState::RestoreFailed;
}
bool mask_equal(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b,
                std::span<const std::uint8_t> mask) {
    if (a.size() != b.size() || a.size() != mask.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) if ((a[i] ^ b[i]) & mask[i]) return false;
    return true;
}
Error valid_region(const Region& region) {
    const auto width = region.byte_mask.size();
    if (region.storage_key.empty() || !width ||
        region.offset > std::numeric_limits<std::uint64_t>::max() - width)
        return make_error(ErrorCode::InvalidArgument, "Provider returned invalid region");
    if (region.whole_value) {
        if (region.byte_sequence || region.offset || width > 16384 ||
            !std::all_of(region.byte_mask.begin(), region.byte_mask.end(), [](auto mask) { return mask == 0xFF; }))
            return make_error(ErrorCode::InvalidArgument, "Provider returned invalid whole-value region");
    } else if (region.byte_sequence) {
        if (width > 8192)
            return make_error(ErrorCode::InvalidArgument, "Provider returned oversized byte-sequence region");
    } else if (width != region.type.size || width > 8)
        return make_error(ErrorCode::InvalidArgument, "Provider returned invalid scalar region");
    return {};
}
Error valid_encoding(const Region& region, const EncodedValue& value) {
    if (value.bytes.size() != region.byte_mask.size() || value.mask != region.byte_mask)
        return make_error(ErrorCode::InvalidArgument, "Codec/storage width or mask mismatch");
    return {};
}
Error valid_path(const PropertyPath& path) {
    auto parsed = PropertyPath::parse(path.string());
    if (!parsed || parsed->segments != path.segments)
        return make_error(ErrorCode::InvalidArgument, "PropertyPath has invalid segments");
    return {};
}
bool same_locator(const ResourceLocator& a, const ResourceLocator& b) {
    return a.provider == b.provider && a.target == b.target && a.row == b.row &&
        a.class_guard == b.class_guard;
}
bool same_assignment(const Assignment& a, const Assignment& b) {
    return same_locator(a.locator, b.locator) && a.path.segments == b.path.segments &&
        a.value == b.value && a.expected_original == b.expected_original &&
        a.drift_policy == b.drift_policy && a.restore_policy == b.restore_policy && a.group == b.group;
}

bool same_content(const PatchSet& a, const PatchSet& b) {
    if (a.id != b.id || a.order != b.order || a.assignments.size() != b.assignments.size())
        return false;
    for (std::size_t i = 0; i < a.assignments.size(); ++i)
        if (!same_assignment(a.assignments[i], b.assignments[i])) return false;
    return true;
}
bool same_owner(const OwnerInfo& a, const OwnerInfo& b) {
    return a.id == b.id && a.display_name == b.display_name &&
        a.priority == b.priority && a.ordering_key == b.ordering_key;
}
Error valid_owner(const OwnerInfo& info) {
    if (info.id.empty() || info.id.size() > 256 || info.ordering_key.size() > 256 || info.display_name.size() > 1024)
        return make_error(ErrorCode::InvalidArgument, "Invalid owner metadata size");
    return {};
}
}

std::string_view to_string(OperationState state) noexcept {
#define IF_STATE_NAME(name) case OperationState::name: return #name
    switch (state) {
    IF_STATE_NAME(Accepted); IF_STATE_NAME(WaitingForRuntime); IF_STATE_NAME(WaitingForTarget);
    IF_STATE_NAME(Prepared); IF_STATE_NAME(Applied); IF_STATE_NAME(AlreadyEffective);
    IF_STATE_NAME(Shadowed); IF_STATE_NAME(Rejected); IF_STATE_NAME(Drifted);
    IF_STATE_NAME(Removing); IF_STATE_NAME(Removed); IF_STATE_NAME(RestoreFailed);
    IF_STATE_NAME(TargetExpired); IF_STATE_NAME(Cancelled); IF_STATE_NAME(Faulted);
    IF_STATE_NAME(OutcomeUnknown);
    }
#undef IF_STATE_NAME
    return "Unknown";
}

struct Engine::Impl {
    struct Set {
        PatchSet patch;
        std::uint64_t request = 0;
        std::vector<std::optional<Region>> bindings;
        std::set<std::size_t> dirty;
    };
    struct Owner {
        OwnerInfo info;
        bool enabled = true;
        std::map<std::string, Set> sets;
        std::map<std::string, std::uint64_t> revisions;
        OwnerToken controller = 0;
    };
    struct Catalog {
        std::uint64_t revision = 0;
        std::uint64_t request = 0;
        std::vector<CatalogEntry> entries;
        std::map<std::string, OwnerToken> members;
    };
    struct ResultReference { std::uint64_t request; std::size_t index; };
    struct ByteKey {
        std::string provider;
        TargetIdentity identity;
        std::string storage;
        std::uint64_t offset;
        bool operator<(const ByteKey& other) const {
            return std::tie(provider, identity.epoch, identity.object_index, identity.object_serial,
                identity.structural_epoch, storage, offset) <
                std::tie(other.provider, other.identity.epoch, other.identity.object_index,
                    other.identity.object_serial, other.identity.structural_epoch, other.storage, other.offset);
        }
    };
    struct Journal {
        Region region;
        std::uint8_t baseline = 0;
        std::uint8_t last = 0;
        std::uint8_t mask = 0;
        std::uint8_t suspended = 0;
        std::uint8_t preserve = 0;
        std::uint8_t no_restore = 0;
        std::array<std::optional<Region>, 8> preserve_scopes;
        Error quarantine;
    };
    struct Removal {
        std::uint64_t request;
        std::vector<std::optional<Region>> regions;
        std::vector<std::string> providers;
    };
    struct WholeJournal {
        Region region;
        std::vector<std::uint8_t> baseline;
        std::vector<std::uint8_t> last;
        bool suspended = false;
        bool preserve = false;
        bool no_restore = false;

        bool uncertain = false;
        Error quarantine;
    };
    struct Candidate {
        OwnerToken token;
        Owner* owner;
        Set* set;
        std::size_t index;
        Region region;
        EncodedValue desired;
        std::vector<std::uint8_t> current;
        OperationResult* result;
        Candidate* winner = nullptr;
        bool selected = false;
        bool eligible = true;
    };
    std::map<std::string, std::shared_ptr<Provider>> providers;
    std::map<OwnerToken, Owner> owners;
    std::map<std::string, OwnerToken> live_ids;
    std::map<std::uint64_t, RequestStatus> requests;
    std::map<ByteKey, Journal> journals;
    std::map<ByteKey, WholeJournal> whole_journals;
    std::vector<Removal> removals;
    std::map<OwnerToken, Catalog> catalogs;
    std::map<std::uint64_t, std::vector<ResultReference>> aggregates;
    OwnerToken next_owner = 1;
    std::uint64_t next_request = 1;

    std::set<std::uint64_t> released;

    static ByteKey key(std::string_view provider, const Region& region, std::size_t index) {
        return {std::string(provider), region.identity, region.storage_key, region.offset + index};
    }
    Result<Owner*> owner(OwnerToken token) {
        auto found = owners.find(token);
        if (found == owners.end()) return make_error(ErrorCode::UnknownOwner, "Unknown owner token");
        if (!found->second.enabled) return make_error(ErrorCode::StaleToken, "Owner session was revoked");
        return &found->second;
    }
    std::size_t active() const {
        std::size_t count = 0;
        for (const auto& [token, owner] : owners) if (owner.enabled)
            for (const auto& [id, set] : owner.sets) count += set.patch.assignments.size();
        return count;
    }

    bool referenced(std::uint64_t request) const {
        for (const auto& [token, owner] : owners)
            for (const auto& [id, set] : owner.sets)
                if (set.request == request) return true;
        for (const auto& removal : removals)
            if (removal.request == request) return true;
        for (const auto& [controller, catalog] : catalogs)
            if (catalog.request == request) return true;
        for (const auto& [aggregate, references] : aggregates)
            for (const auto& reference : references)
                if (reference.request == request) return true;
        return false;
    }
    static bool same_storage(const ByteKey& a, const ByteKey& b) {
        return a.provider == b.provider && a.identity == b.identity && a.storage == b.storage;
    }
    std::size_t whole_bytes() const {
        std::size_t count = 0;
        for (const auto& [key, journal] : whole_journals) count += journal.baseline.size();
        return count;
    }
    void prune_released() {

        bool changed;
        do {
            changed = false;
            for (auto iterator = released.begin(); iterator != released.end();) {
                if (!referenced(*iterator)) {
                    aggregates.erase(*iterator);
                    requests.erase(*iterator);
                    iterator = released.erase(iterator);
                    changed = true;
                } else ++iterator;
            }
        } while (changed);
    }
    std::size_t owner_budget() const {

        std::size_t count = 0;
        for (const auto& [token, owner] : owners)
            if (owner.enabled || !owner.controller) ++count;
        return count;
    }
    void prune_managed_owners() {
        for (auto iterator = owners.begin(); iterator != owners.end();) {
            const auto token = iterator->first;
            const auto& owner = iterator->second;
            bool obligation = false;
            if (owner.controller && !owner.enabled && owner.sets.empty()) {
                for (const auto& removal : removals) {
                    const auto& status = requests.at(removal.request);
                    for (const auto& operation : status.operations)
                        if (operation.owner == token) obligation = true;
                }
                if (!obligation) { iterator = owners.erase(iterator); continue; }
            }
            ++iterator;
        }
    }
    void refresh_aggregates(bool observed) {
        for (const auto& [request, references] : aggregates) {
            auto& status = requests.at(request);
            status.operations.clear();
            status.observed = status.observed || observed;
            for (const auto& reference : references) {
                const auto& child = requests.at(reference.request);
                status.observed = status.observed && child.observed;
                auto operation = child.operations.at(reference.index);
                operation.request_id = request;
                status.operations.push_back(std::move(operation));
            }
            summarize(status);
        }
    }
    static bool overlap(const Candidate& a, const Candidate& b) {
        if (a.region.identity != b.region.identity || a.region.storage_key != b.region.storage_key ||
            a.set->patch.assignments[a.index].locator.provider != b.set->patch.assignments[b.index].locator.provider)
            return false;
        if (a.region.whole_value || b.region.whole_value) return true;
        for (std::size_t i = 0; i < a.region.byte_mask.size(); ++i) {
            const auto offset = a.region.offset + i;
            if (offset >= b.region.offset && offset - b.region.offset < b.region.byte_mask.size() &&
                (a.region.byte_mask[i] & b.region.byte_mask[static_cast<std::size_t>(offset - b.region.offset)])) return true;
        }
        return false;
    }
    static bool overlap(std::string_view a_provider, const Region& a,
                        std::string_view b_provider, const Region& b) {
        if (a_provider != b_provider || a.identity != b.identity || a.storage_key != b.storage_key) return false;
        if (a.whole_value || b.whole_value) return true;
        for (std::size_t i = 0; i < a.byte_mask.size(); ++i) {
            const auto offset = a.offset + i;
            if (offset >= b.offset && offset - b.offset < b.byte_mask.size() &&
                (a.byte_mask[i] & b.byte_mask[static_cast<std::size_t>(offset - b.offset)])) return true;
        }
        return false;
    }
    static void summarize(RequestStatus& status) {
        status.pending_count = 0;
        status.rejected_count = 0;
        for (const auto& operation : status.operations) {
            if (pending(operation.state)) ++status.pending_count;
            if (operation.state == OperationState::Rejected || operation.state == OperationState::Faulted ||
                operation.state == OperationState::OutcomeUnknown) ++status.rejected_count;
        }
        status.settled = status.observed && !status.pending_count;
    }
    void retire_set(Set& set) {
        auto& status = requests.at(set.request);
        for (auto& operation : status.operations) {
            operation.state = operation.state == OperationState::Accepted ? OperationState::Cancelled : OperationState::Removing;
            operation.updated_at_us = now_us();
        }
        Removal removal{set.request, set.bindings, {}};
        for (const auto& assignment : set.patch.assignments) removal.providers.push_back(assignment.locator.provider);
        removals.push_back(std::move(removal));
        summarize(status);
    }
};

Engine::Engine(EngineLimits limits) : impl_(std::make_unique<Impl>()), limits_(limits) {}
Engine::~Engine() = default;

Error Engine::add_provider(std::shared_ptr<Provider> provider) {
    if (!provider || provider->id().empty()) return make_error(ErrorCode::InvalidArgument, "Provider needs an ID");
    const std::string id(provider->id());
    if (impl_->providers.contains(id)) return make_error(ErrorCode::InvalidArgument, "Duplicate provider ID");
    if (impl_->providers.size() >= limits_.max_providers) return make_error(ErrorCode::LimitExceeded, "Provider limit");
    impl_->providers.emplace(id, std::move(provider));
    return {};
}

Result<OwnerToken> Engine::register_owner(OwnerInfo info) {
    if (auto error = valid_owner(info); !error.ok()) return error;
    if (impl_->live_ids.contains(info.id)) return make_error(ErrorCode::DuplicateOwner, "Duplicate live owner ID");
    if (impl_->owner_budget() >= limits_.max_owners) return make_error(ErrorCode::LimitExceeded, "Owner/session limit");
    if (info.ordering_key.empty()) info.ordering_key = info.id;
    const auto token = impl_->next_owner++;
    impl_->live_ids.emplace(info.id, token);
    impl_->owners.emplace(token, Impl::Owner{std::move(info), true, {}, {}});
    return token;
}

Result<RequestReceipt> Engine::replace_set(OwnerToken token, PatchSet patch) {
    auto owner_result = impl_->owner(token);
    if (!owner_result) return owner_result.error();
    auto& owner = *owner_result.value();
    if (patch.id.empty() || patch.id.size() > 256 || !patch.revision)
        return make_error(ErrorCode::InvalidArgument, "Patch set needs an ID and nonzero revision");
    const auto existing = owner.sets.find(patch.id);
    const auto revision = owner.revisions.find(patch.id);
    if (existing != owner.sets.end() && patch.revision == existing->second.patch.revision) {

        if (same_content(existing->second.patch, patch)) {

            impl_->released.erase(existing->second.request);
            return RequestReceipt{existing->second.request, existing->second.patch.revision};
        }
        return make_error(ErrorCode::RevisionConflict, "Revision already exists with different content");
    }
    if (revision != owner.revisions.end()) {
        if (patch.revision < revision->second)
            return make_error(ErrorCode::StaleRevision, "Revision must increase monotonically");
        if (patch.revision == revision->second)
            return make_error(ErrorCode::RevisionConflict, "A removed revision cannot be resubmitted");
    }
    impl_->prune_released();
    const auto old_size = existing == owner.sets.end() ? 0 : existing->second.patch.assignments.size();
    if (patch.assignments.size() > limits_.max_active_operations ||
        impl_->active() - old_size > limits_.max_active_operations - patch.assignments.size())
        return make_error(ErrorCode::LimitExceeded, "Synchronous active operation limit exceeded");
    if (impl_->requests.size() >= limits_.max_retained_requests)
        return make_error(ErrorCode::LimitExceeded, "Retained request limit exceeded");
    for (const auto& assignment : patch.assignments) {
        if (assignment.group) return make_error(ErrorCode::CapabilityUnavailable, "Atomic groups are unsupported");
        if (!impl_->providers.contains(assignment.locator.provider))
            return make_error(ErrorCode::ProviderUnknown, "Provider is not registered");
        if (assignment.locator.target.empty() || assignment.locator.target.size() > 4096 ||
            assignment.locator.row.size() > 1024 || assignment.locator.class_guard.size() > 4096)
            return make_error(ErrorCode::InvalidArgument, "Invalid resource selector size");
        if (auto error = valid_path(assignment.path); !error.ok()) return error;
        if ((assignment.drift_policy != DriftPolicy::PreserveExternalChange && assignment.drift_policy != DriftPolicy::PersistentOverlay) ||
            (assignment.restore_policy != RestorePolicy::Required && assignment.restore_policy != RestorePolicy::NoRestore))
            return make_error(ErrorCode::InvalidArgument, "Unknown assignment policy");
    }
    RequestReceipt receipt{impl_->next_request++, patch.revision};
    RequestStatus status;
    status.receipt = receipt;
    for (std::size_t i = 0; i < patch.assignments.size(); ++i) {
        OperationResult operation;
        operation.owner = token;
        operation.owner_id = owner.info.id;
        operation.set_id = patch.id;
        operation.operation_index = i;
        operation.revision = patch.revision;
        operation.request_id = receipt.request_id;
        operation.updated_at_us = now_us();
        status.operations.push_back(std::move(operation));
    }
    if (existing != owner.sets.end()) impl_->retire_set(existing->second);
    owner.revisions[patch.id] = patch.revision;
    const std::string id = patch.id;
    Impl::Set replacement{std::move(patch), receipt.request_id, {}};
    replacement.bindings.resize(replacement.patch.assignments.size());
    owner.sets.insert_or_assign(id, std::move(replacement));
    impl_->requests.emplace(receipt.request_id, std::move(status));
    return receipt;
}

Result<RequestReceipt> Engine::publish_catalog(OwnerToken controller, std::uint64_t revision,
                                             std::vector<CatalogEntry> entries) {
    auto controller_result = impl_->owner(controller);
    if (!controller_result) return controller_result.error();
    if (controller_result.value()->controller)
        return make_error(ErrorCode::InvalidArgument, "A managed member cannot control a catalog");
    if (!revision) return make_error(ErrorCode::InvalidArgument, "Catalog revision must be nonzero");
    if (entries.size() > limits_.max_owners)
        return make_error(ErrorCode::LimitExceeded, "Catalog member limit exceeded");
    try {
        std::set<std::string> ids;
        std::size_t operations = 0;
        for (auto& entry : entries) {
            if (auto error = valid_owner(entry.owner); !error.ok()) return error;
            if (entry.owner.ordering_key.empty()) entry.owner.ordering_key = entry.owner.id;
            if (!ids.insert(entry.owner.id).second)
                return make_error(ErrorCode::DuplicateOwner, "Duplicate catalog member ID");
            const auto live = impl_->live_ids.find(entry.owner.id);
            if (live != impl_->live_ids.end() && impl_->owners.at(live->second).controller != controller)
                return make_error(ErrorCode::DuplicateOwner, "Catalog ID collides with an unrelated owner");
            auto& patch = entry.patch;
            patch.revision = revision;
            if (patch.id.empty() || patch.id.size() > 256)
                return make_error(ErrorCode::InvalidArgument, "Catalog patch needs a valid ID");
            if (patch.assignments.size() > limits_.max_active_operations - operations)
                return make_error(ErrorCode::LimitExceeded, "Catalog operation limit exceeded");
            operations += patch.assignments.size();
            for (const auto& assignment : patch.assignments) {
                if (assignment.group) return make_error(ErrorCode::CapabilityUnavailable, "Atomic groups are unsupported");
                if (!impl_->providers.contains(assignment.locator.provider))
                    return make_error(ErrorCode::ProviderUnknown, "Provider is not registered");
                if (assignment.locator.target.empty() || assignment.locator.target.size() > 4096 ||
                    assignment.locator.row.size() > 1024 || assignment.locator.class_guard.size() > 4096)
                    return make_error(ErrorCode::InvalidArgument, "Invalid resource selector size");
                if (auto error = valid_path(assignment.path); !error.ok()) return error;
                if ((assignment.drift_policy != DriftPolicy::PreserveExternalChange && assignment.drift_policy != DriftPolicy::PersistentOverlay) ||
                    (assignment.restore_policy != RestorePolicy::Required && assignment.restore_policy != RestorePolicy::NoRestore))
                    return make_error(ErrorCode::InvalidArgument, "Unknown assignment policy");
            }
        }
        const auto prior_catalog = impl_->catalogs.find(controller);
        if (prior_catalog != impl_->catalogs.end()) {
            const auto& prior = prior_catalog->second;
            if (revision < prior.revision)
                return make_error(ErrorCode::StaleRevision, "Catalog revision must increase monotonically");
            if (revision == prior.revision) {
                bool same = entries.size() == prior.entries.size();
                for (std::size_t i = 0; same && i < entries.size(); ++i)
                    same = same_owner(entries[i].owner, prior.entries[i].owner) &&
                        same_content(entries[i].patch, prior.entries[i].patch);
                if (!same) return make_error(ErrorCode::RevisionConflict, "Catalog revision has different content");
                impl_->released.erase(prior.request);
                return RequestReceipt{prior.request, revision};
            }
        }

        auto staged = std::make_unique<Impl>(*impl_);
        auto& catalog = staged->catalogs[controller];
        const auto old_members = catalog.members;
        catalog.request = 0;
        for (const auto& [id, token] : old_members) {
            auto& owner = staged->owners.at(token);
            for (auto& [set_id, set] : owner.sets) staged->retire_set(set);
            owner.sets.clear();
            owner.revisions.clear();
            if (!ids.contains(id)) {
                owner.enabled = false;
                staged->live_ids.erase(id);
            }
        }
        staged->prune_released();
        staged->prune_managed_owners();
        if (staged->active() > limits_.max_active_operations - operations)
            return make_error(ErrorCode::LimitExceeded, "Synchronous active operation limit exceeded");

        catalog.members.clear();
        std::vector<Impl::ResultReference> references;
        for (const auto& entry : entries) {
            OwnerToken token;
            const auto existing = old_members.find(entry.owner.id);
            if (existing != old_members.end()) token = existing->second;
            else {
                if (staged->next_owner == std::numeric_limits<OwnerToken>::max())
                    return make_error(ErrorCode::LimitExceeded, "Owner token space exhausted");
                token = staged->next_owner++;
                staged->owners.emplace(token, Impl::Owner{entry.owner, true, {}, {}, controller});
                staged->live_ids.emplace(entry.owner.id, token);
            }
            auto& owner = staged->owners.at(token);
            owner.info = entry.owner;
            catalog.members.emplace(entry.owner.id, token);
            if (staged->next_request == std::numeric_limits<std::uint64_t>::max())
                return make_error(ErrorCode::LimitExceeded, "Request ID space exhausted");
            const auto child_request = staged->next_request++;
            RequestStatus child;
            child.receipt = {child_request, revision};
            Impl::Set set{entry.patch, child_request, {}};
            set.bindings.resize(entry.patch.assignments.size());
            for (std::size_t index = 0; index < entry.patch.assignments.size(); ++index) {
                OperationResult result;
                result.owner = token;
                result.owner_id = owner.info.id;
                result.set_id = entry.patch.id;
                result.operation_index = index;
                result.revision = revision;
                result.request_id = child_request;
                result.updated_at_us = now_us();
                child.operations.push_back(std::move(result));
                references.push_back({child_request, index});
            }
            owner.revisions.emplace(entry.patch.id, revision);
            owner.sets.emplace(entry.patch.id, std::move(set));
            staged->requests.emplace(child_request, std::move(child));
            staged->released.insert(child_request);
        }
        if (staged->owner_budget() > limits_.max_owners)
            return make_error(ErrorCode::LimitExceeded, "Owner/session limit");

        std::set<std::tuple<OwnerToken, std::string, std::uint64_t, std::uint64_t>> included;
        for (const auto& removal : staged->removals) {
            const auto& child = staged->requests.at(removal.request);
            for (std::size_t index = 0; index < child.operations.size(); ++index) {
                const auto& operation = child.operations[index];
                const auto member = staged->owners.find(operation.owner);
                if (member == staged->owners.end() || member->second.controller != controller) continue;
                if (included.emplace(operation.owner, operation.set_id, operation.revision,
                                     operation.operation_index).second)
                    references.push_back({removal.request, index});
            }
        }
        if (staged->requests.size() >= limits_.max_retained_requests)
            return make_error(ErrorCode::LimitExceeded, "Retained request limit exceeded");
        if (staged->next_request == std::numeric_limits<std::uint64_t>::max())
            return make_error(ErrorCode::LimitExceeded, "Request ID space exhausted");
        RequestReceipt receipt{staged->next_request++, revision};
        RequestStatus aggregate;
        aggregate.receipt = receipt;
        staged->requests.emplace(receipt.request_id, std::move(aggregate));
        staged->aggregates.emplace(receipt.request_id, std::move(references));
        catalog.revision = revision;
        catalog.request = receipt.request_id;
        catalog.entries = std::move(entries);
        staged->refresh_aggregates(false);
        impl_.swap(staged);
        return receipt;
    } catch (const std::bad_alloc&) {
        return make_error(ErrorCode::LimitExceeded, "Catalog staging allocation failed");
    }
}

Result<RequestReceipt> Engine::remove_set(OwnerToken token, std::string_view set_id) {
    auto owner_result = impl_->owner(token);
    if (!owner_result) return owner_result.error();
    auto& owner = *owner_result.value();
    auto found = owner.sets.find(std::string(set_id));
    if (found == owner.sets.end()) return make_error(ErrorCode::NotFound, "Patch set not found");
    impl_->prune_released();
    if (impl_->requests.size() >= limits_.max_retained_requests)
        return make_error(ErrorCode::LimitExceeded, "Retained request limit exceeded");
    RequestReceipt receipt{impl_->next_request++, found->second.patch.revision};
    auto status = impl_->requests.at(found->second.request);
    status.receipt = receipt;
    for (auto& operation : status.operations) {
        operation.request_id = receipt.request_id;
        operation.state = OperationState::Removing;
        operation.error = {};
    }
    impl_->retire_set(found->second);
    Impl::Removal removal{receipt.request_id, found->second.bindings, {}};
    for (const auto& assignment : found->second.patch.assignments) removal.providers.push_back(assignment.locator.provider);
    impl_->removals.push_back(std::move(removal));
    owner.sets.erase(found);
    impl_->requests.emplace(receipt.request_id, std::move(status));
    return receipt;
}

Result<RequestReceipt> Engine::disable_owner(OwnerToken token) {
    auto owner_result = impl_->owner(token);
    if (!owner_result) return owner_result.error();
    impl_->prune_released();
    if (impl_->requests.size() >= limits_.max_retained_requests + limits_.max_owners)
        return make_error(ErrorCode::LimitExceeded, "Owner disable reserve exhausted");
    RequestReceipt receipt{impl_->next_request++, 0};
    RequestStatus status;
    status.receipt = receipt;
    Impl::Removal removal{receipt.request_id, {}, {}};
    std::set<OwnerToken> disabled{token};
    if (impl_->catalogs.contains(token))
        for (const auto& [member, owner] : impl_->owners)
            if (owner.controller == token) disabled.insert(member);
    for (const auto member : disabled) {
        auto& owner = impl_->owners.at(member);
        for (auto& [id, set] : owner.sets) impl_->retire_set(set);
    }

    std::set<std::tuple<OwnerToken, std::string, std::uint64_t, std::uint64_t>> included;
    for (const auto& prior : impl_->removals) {
        const auto& operations = impl_->requests.at(prior.request).operations;
        for (std::size_t i = 0; i < operations.size(); ++i) {
            const auto& operation = operations[i];
            if (!disabled.contains(operation.owner) || !included.emplace(operation.owner, operation.set_id,
                    operation.revision, operation.operation_index).second) continue;
            status.operations.push_back(operation);
            removal.regions.push_back(prior.regions[i]);
            removal.providers.push_back(prior.providers[i]);
        }
    }
    for (auto& operation : status.operations) {
        operation.request_id = receipt.request_id;
        operation.state = OperationState::Removing;
        operation.error = {};
    }
    for (const auto member : disabled) {
        auto& owner = impl_->owners.at(member);
        owner.sets.clear();
        owner.enabled = false;
        impl_->live_ids.erase(owner.info.id);
    }
    impl_->catalogs.erase(token);
    impl_->requests.emplace(receipt.request_id, std::move(status));
    impl_->removals.push_back(std::move(removal));
    impl_->refresh_aggregates(false);
    return receipt;
}

Result<ReadResult> Engine::read(OwnerToken token, const ResourceLocator& locator, const PropertyPath& path) {
    if (auto owner = impl_->owner(token); !owner) return owner.error();
    if (auto error = valid_path(path); !error.ok()) return error;
    auto found = impl_->providers.find(locator.provider);
    if (found == impl_->providers.end()) return make_error(ErrorCode::ProviderUnknown, "Provider is not registered");
    auto region = found->second->bind_owned(locator, path, impl_->owner(token).value()->info.id);
    if (!region) return region.error();
    if (auto error = valid_region(region.value()); !error.ok()) return error;
    if (auto error = found->second->validate(region.value()); !error.ok()) return error;
    auto bytes = found->second->read(region.value());
    if (!bytes) return bytes.error();
    if (bytes->size() != region->byte_mask.size()) return make_error(ErrorCode::ReadFailed, "Provider read width mismatch");
    auto value = found->second->decode(region.value(), bytes.value());
    if (!value) return value.error();
    return ReadResult{std::move(region).value(), std::move(value).value()};
}

Result<RequestStatus> Engine::status(std::uint64_t request_id) const {
    const auto found = impl_->requests.find(request_id);
    if (found == impl_->requests.end()) return make_error(ErrorCode::NotFound, "Request status not found");
    return found->second;
}

Error Engine::release_request(std::uint64_t request_id) {
    if (impl_->requests.find(request_id) == impl_->requests.end())
        return make_error(ErrorCode::NotFound, "Request status not found");

    impl_->released.insert(request_id);
    impl_->refresh_aggregates(false);
    impl_->prune_released();
    impl_->prune_managed_owners();
    return {};
}

Error Engine::resume(OwnerToken token, std::string_view set_id, std::uint64_t revision,
                     std::size_t index, const Value& observed) {
    auto owner = impl_->owner(token);
    if (!owner) return owner.error();
    auto set = owner.value()->sets.find(std::string(set_id));
    if (set == owner.value()->sets.end()) return make_error(ErrorCode::NotFound, "Patch set not found");
    if (set->second.patch.revision != revision) return make_error(ErrorCode::StaleRevision, "Resume revision mismatch");
    if (index >= set->second.patch.assignments.size()) return make_error(ErrorCode::InvalidArgument, "Operation index out of range");
    const auto& assignment = set->second.patch.assignments[index];
    auto& provider = *impl_->providers.at(assignment.locator.provider);
    auto region = provider.bind_owned(assignment.locator, assignment.path, owner.value()->info.id);
    if (!region) return region.error();
    if (auto error = valid_region(region.value()); !error.ok()) return error;
    if (!set->second.bindings[index] || set->second.bindings[index]->identity != region->identity ||
        set->second.bindings[index]->storage_key != region->storage_key ||
        set->second.bindings[index]->offset != region->offset || set->second.bindings[index]->byte_mask != region->byte_mask ||
        set->second.bindings[index]->whole_value != region->whole_value ||
        set->second.bindings[index]->byte_sequence != region->byte_sequence)
        return make_error(ErrorCode::TargetExpired, "Resume cannot acknowledge a replacement binding");
    if (auto error = provider.validate(region.value()); !error.ok()) return error;
    auto expected = provider.prepare(region.value(), observed);
    if (!expected) return expected.error();
    if (auto error = valid_encoding(region.value(), expected.value()); !error.ok()) return error;
    auto current = provider.read(region.value());
    if (!current) return current.error();
    if (current->size() != region->byte_mask.size())
        return make_error(ErrorCode::ReadFailed, "Provider read width mismatch");
    if (!mask_equal(current.value(), expected->bytes, region->byte_mask))
        return make_error(ErrorCode::ExpectationFailed, "External value changed before resume");
    if (region->whole_value) {
        auto journal = impl_->whole_journals.find(Impl::key(assignment.locator.provider, region.value(), 0));
        if (journal == impl_->whole_journals.end() || !journal->second.suspended)
            return make_error(ErrorCode::InvalidArgument, "Region is not drift suspended");
        journal->second.suspended = false;
        journal->second.last = std::move(current).value();
        set->second.dirty.insert(index);
        return {};
    }
    bool suspended = false;
    for (std::size_t i = 0; i < region->byte_mask.size(); ++i) {
        const auto journal = impl_->journals.find(Impl::key(assignment.locator.provider, region.value(), i));
        if (journal != impl_->journals.end() && (journal->second.suspended & region->byte_mask[i])) suspended = true;
    }
    if (!suspended) return make_error(ErrorCode::InvalidArgument, "Region is not drift suspended");
    for (std::size_t i = 0; i < region->byte_mask.size(); ++i) {
        auto journal = impl_->journals.find(Impl::key(assignment.locator.provider, region.value(), i));
        if (journal == impl_->journals.end()) continue;
        journal->second.suspended &= static_cast<std::uint8_t>(~region->byte_mask[i]);

        journal->second.last = static_cast<std::uint8_t>((journal->second.last & ~region->byte_mask[i]) |
            (current.value()[i] & region->byte_mask[i]));
    }
    set->second.dirty.insert(index);
    return {};
}

std::vector<std::string> Engine::targets(std::string_view provider) const {
    std::set<std::string> targets;
    for (const auto& [token, owner] : impl_->owners) if (owner.enabled)
        for (const auto& [id, set] : owner.sets)
            for (const auto& assignment : set.patch.assignments)
                if (assignment.locator.provider == provider && !assignment.locator.target.empty())
                    targets.insert(assignment.locator.target);
    return {targets.begin(), targets.end()};
}

void Engine::notify_target(std::string_view provider, std::string_view target) {
    for (auto& [token, owner] : impl_->owners) if (owner.enabled)
        for (auto& [id, set] : owner.sets)
            for (std::size_t i = 0; i < set.patch.assignments.size(); ++i) {
                const auto& locator = set.patch.assignments[i].locator;
                if (locator.provider == provider && locator.target == target) set.dirty.insert(i);
            }
}

ReconcileReport Engine::reconcile(std::span<const std::string_view> providers) {
    const auto allowed = [&](std::string_view provider) {
        return providers.empty() || std::find(providers.begin(), providers.end(), provider) != providers.end();
    };
    const auto skip_provider = [&](auto& journals, auto iterator) {

        std::string_view next;
        for (const auto provider : providers)
            if (provider > iterator->first.provider && (next.empty() || provider < next)) next = provider;
        return next.empty() ? journals.end() :
            journals.lower_bound(Impl::ByteKey{std::string(next), {0, 0, 0, 0}, "", 0});
    };
    ReconcileReport report;
    const auto started = now_us();
    report.active_operations = impl_->active();

    using OperationKey = std::pair<Impl::Set*, std::size_t>;
    std::set<OperationKey> scheduled;
    std::map<OperationKey, Result<Region>> bound;
    std::vector<std::pair<std::string, Region>> touched;
    const auto touch = [&](std::string_view provider, const Region& region) {
        touched.emplace_back(std::string(provider), region);
    };
    const auto affected = [&](std::string_view provider, const Region& region) {
        if (!impl_->providers.at(std::string(provider))->change_driven()) return true;
        return std::any_of(touched.begin(), touched.end(), [&](const auto& entry) {
            return Impl::overlap(provider, region, entry.first, entry.second);
        });
    };
    for (const auto& removal : impl_->removals)
        for (std::size_t i = 0; i < removal.regions.size(); ++i)
            if (allowed(removal.providers[i]) && removal.regions[i]) touch(removal.providers[i], *removal.regions[i]);
    for (const auto& [key, journal] : impl_->journals)
        if (allowed(key.provider) && !journal.quarantine.ok()) touch(key.provider, journal.region);
    for (const auto& [key, journal] : impl_->whole_journals)
        if (allowed(key.provider) && (!journal.quarantine.ok() || journal.uncertain)) touch(key.provider, journal.region);
    const auto schedule = [&](Impl::Owner& owner, Impl::Set& set, std::size_t index) {
        const OperationKey key{&set, index};
        if (!scheduled.insert(key).second) return;
        const auto& assignment = set.patch.assignments[index];
        if (set.bindings[index]) touch(assignment.locator.provider, *set.bindings[index]);
        auto& provider = *impl_->providers.at(assignment.locator.provider);
        ++report.provider_calls;
        const auto bind_started = now_us();
        auto region = provider.bind_owned(assignment.locator, assignment.path, owner.info.id);
        report.bind_us += now_us() - bind_started;
        if (region && valid_region(region.value()).ok()) touch(assignment.locator.provider, region.value());
        bound.emplace(key, std::move(region));
    };
    for (auto& [token, owner] : impl_->owners) if (owner.enabled)
        for (auto& [id, set] : owner.sets)
            for (std::size_t index = 0; index < set.patch.assignments.size(); ++index) {
                const auto& assignment = set.patch.assignments[index];
                if (!allowed(assignment.locator.provider)) continue;
                const auto state = impl_->requests.at(set.request).operations[index].state;
                if (!impl_->providers.at(assignment.locator.provider)->change_driven() ||
                    assignment.drift_policy == DriftPolicy::PersistentOverlay || set.dirty.contains(index) ||
                    pending(state) || state == OperationState::Faulted || state == OperationState::TargetExpired)
                    schedule(owner, set, index);
            }
    bool expanded;
    do {
        expanded = false;
        for (auto& [token, owner] : impl_->owners) if (owner.enabled)
            for (auto& [id, set] : owner.sets)
                for (std::size_t index = 0; index < set.patch.assignments.size(); ++index) {
                    const auto& assignment = set.patch.assignments[index];
                    if (!allowed(assignment.locator.provider) || scheduled.contains({&set, index}) || !set.bindings[index]) continue;
                    if (affected(assignment.locator.provider, *set.bindings[index])) {
                        schedule(owner, set, index);
                        expanded = true;
                    }
                }
    } while (expanded);
    std::map<Impl::ByteKey, Error> failures;
    std::set<Impl::ByteKey> expired;
    std::set<Impl::ByteKey> skipped;
    std::map<Impl::ByteKey, std::uint8_t> live_bytes;
    std::map<Impl::ByteKey, std::uint8_t> drifted_bits;
    std::set<Impl::ByteKey> whole_drifted;
    for (auto iterator = impl_->whole_journals.begin(); iterator != impl_->whole_journals.end();) {
        auto& [key, journal] = *iterator;
        if (!allowed(key.provider)) { iterator = skip_provider(impl_->whole_journals, iterator); continue; }
        if (!affected(key.provider, journal.region)) { ++iterator; continue; }
        auto& provider = *impl_->providers.at(key.provider);
        ++report.provider_calls;

        auto current = provider.read(journal.region);
        auto error = current ? Error{} : current.error();
        if (error.code == ErrorCode::TargetExpired) {
            expired.insert(key);
            report.expired_journal_bytes += journal.baseline.size();
            iterator = impl_->whole_journals.erase(iterator);
            continue;
        }
        if (error.ok()) {
            if (current->size() != journal.region.byte_mask.size())
                error = make_error(ErrorCode::ReadFailed, "Whole journal read width mismatch");
            else if (!journal.uncertain && current.value() != journal.last) {
                whole_drifted.insert(key);
                if (journal.preserve) journal.suspended = true;
            }
        }
        journal.quarantine = error;
        if (!error.ok()) failures[key] = error;
        ++iterator;
    }

    for (auto iterator = impl_->journals.begin(); iterator != impl_->journals.end();) {
        auto& [key, journal] = *iterator;
        if (!allowed(key.provider)) { iterator = skip_provider(impl_->journals, iterator); continue; }
        if (!affected(key.provider, journal.region)) { ++iterator; continue; }
        auto& provider = *impl_->providers.at(key.provider);
        ++report.provider_calls;
        auto current = provider.read(journal.region);
        auto error = current ? Error{} : current.error();
        if (error.code == ErrorCode::TargetExpired) {
            expired.insert(key);
            ++report.expired_journal_bytes;
            iterator = impl_->journals.erase(iterator);
            continue;
        }
        if (!error.ok()) {
            journal.quarantine = error;
            failures[key] = error;
            ++iterator;
            continue;
        }
        if (!current || current->size() != 1) {
            journal.quarantine = current ? make_error(ErrorCode::ReadFailed, "Journal read width mismatch") : current.error();
            failures[key] = journal.quarantine;
            ++iterator;
            continue;
        }
        journal.quarantine = {};
        live_bytes[key] = current.value()[0];
        const auto changed = static_cast<std::uint8_t>((current.value()[0] ^ journal.last) & journal.mask);
        drifted_bits[key] = changed;

        for (unsigned bit = 0; bit < 8; ++bit) if ((changed & (1u << bit)) && journal.preserve_scopes[bit]) {
            const auto& scope = *journal.preserve_scopes[bit];
            for (std::size_t byte = 0; byte < scope.byte_mask.size(); ++byte) {
                auto scoped_journal = impl_->journals.find(Impl::key(key.provider, scope, byte));
                if (scoped_journal != impl_->journals.end())
                    scoped_journal->second.suspended |= static_cast<std::uint8_t>(scope.byte_mask[byte] & scoped_journal->second.preserve);
            }
        }
        ++iterator;
    }

    std::vector<Impl::Candidate> candidates;
    candidates.reserve(report.active_operations);
    for (auto& [token, owner] : impl_->owners) if (owner.enabled) {
        for (auto& [id, set] : owner.sets) {
            auto& status = impl_->requests.at(set.request);
            if (providers.empty() || set.patch.assignments.empty()) status.observed = true;
            for (std::size_t index = 0; index < set.patch.assignments.size(); ++index) {
                const auto& assignment = set.patch.assignments[index];
                if (!allowed(assignment.locator.provider)) continue;
                if (!scheduled.contains({&set, index})) continue;
                set.dirty.erase(index);
                status.observed = true;
                ++report.processed_operations;
                auto& result = status.operations[index];
                result.state = OperationState::Prepared;
                result.error = {};
                result.effective_owner = 0;
                result.effective_set.clear();
                result.effective_revision = 0;
                result.baseline_restore_skipped = false;
                result.drift_detected = false;
                result.updated_at_us = now_us();
                auto reject = [&](Error error) {
                    result.state = error.code == ErrorCode::NotReady ? OperationState::WaitingForRuntime :
                        error.code == ErrorCode::NotFound ? OperationState::WaitingForTarget :
                        error.code == ErrorCode::TargetExpired ? OperationState::TargetExpired : OperationState::Rejected;
                    result.error = std::move(error);
                };
                auto& provider = *impl_->providers.at(assignment.locator.provider);
                auto region = std::move(bound.at({&set, index}));
                if (!region) { reject(region.error()); continue; }
                result.identity = region->identity;
                result.canonical_path = region->canonical_path;
                if (auto error = valid_region(region.value()); !error.ok()) { reject(error); continue; }
                set.bindings[index] = region.value();
                const auto prepare_started = now_us();
                auto desired = provider.prepare(region.value(), assignment.value);
                report.prepare_us += now_us() - prepare_started;
                if (!desired) { reject(desired.error()); continue; }

                if (auto error = valid_encoding(region.value(), desired.value()); !error.ok()) { reject(error); continue; }
                const auto storage = Impl::key(assignment.locator.provider, region.value(), 0);
                auto storage_begin = storage;
                storage_begin.offset = 0;
                bool incompatible = false;
                if (region->whole_value) {

                    const auto scalar = impl_->journals.lower_bound(storage_begin);
                    incompatible = scalar != impl_->journals.end() && Impl::same_storage(storage, scalar->first);
                    const auto journal = impl_->whole_journals.find(storage);
                    if (journal != impl_->whole_journals.end() && journal->second.region.byte_mask != region->byte_mask)
                        incompatible = true;
                } else {

                    incompatible = impl_->whole_journals.contains(storage_begin);
                }
                if (incompatible) { reject(make_error(ErrorCode::InvalidArgument, "Incompatible physical storage certificate")); continue; }
                if (assignment.restore_policy == RestorePolicy::Required &&
                    (!region->restorable || region->identity_tracking == IdentityTracking::ObservedOnly)) {
                    reject(make_error(ErrorCode::LifetimeUnverifiable, "Provider cannot certify durable restoration")); continue;
                }
                if (region->identity_tracking == IdentityTracking::ObservedOnly &&
                    assignment.drift_policy == DriftPolicy::PreserveExternalChange) {
                    reject(make_error(ErrorCode::CapabilityUnavailable,
                        "Observed-only leases cannot preserve external changes; require explicit PersistentOverlay")); continue;
                }
                ++report.provider_calls;
                auto current = provider.read(region.value());
                if (!current || current->size() != region->byte_mask.size()) {
                    reject(current ? make_error(ErrorCode::ReadFailed, "Provider read width mismatch") : current.error()); continue;
                }
                std::vector<std::uint8_t> baseline = current.value();
                bool quarantine = false;
                if (region->whole_value) {
                    auto journal = impl_->whole_journals.find(storage);
                    if (journal != impl_->whole_journals.end()) {
                        baseline = journal->second.baseline;
                        if (!journal->second.quarantine.ok()) { result.error = journal->second.quarantine; quarantine = true; }
                    }
                } else for (std::size_t i = 0; i < region->byte_mask.size(); ++i) {
                    auto key = Impl::key(assignment.locator.provider, region.value(), i);
                    live_bytes[key] = current.value()[i];
                    auto journal = impl_->journals.find(key);
                    if (journal != impl_->journals.end()) {
                        baseline[i] = journal->second.baseline;
                        if (!journal->second.quarantine.ok()) { result.error = journal->second.quarantine; quarantine = true; }
                    }
                }
                if (quarantine) { result.state = OperationState::RestoreFailed; continue; }
                if (assignment.expected_original) {
                    auto expectation = provider.prepare(region.value(), *assignment.expected_original);
                    if (!expectation) { reject(expectation.error()); continue; }
                    if (auto error = valid_encoding(region.value(), expectation.value()); !error.ok()) { reject(error); continue; }
                    if (!mask_equal(baseline, expectation->bytes, region->byte_mask)) {
                        reject(make_error(ErrorCode::ExpectationFailed, "Original-value expectation failed")); continue;
                    }
                }
                set.bindings[index] = region.value();
                candidates.push_back({token, &owner, &set, index, std::move(region).value(),
                    std::move(desired).value(), std::move(current).value(), &result, nullptr, false});
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return std::tie(a.owner->info.priority, a.owner->info.ordering_key, a.owner->info.id,
                    a.set->patch.order, a.set->patch.id, a.index) >
            std::tie(b.owner->info.priority, b.owner->info.ordering_key, b.owner->info.id,
                    b.set->patch.order, b.set->patch.id, b.index);
    });
    std::map<Impl::ByteKey, std::vector<Impl::Candidate*>> selected_index;
    std::vector<Impl::Candidate*> selected_candidates;
    std::set<Impl::ByteKey> reserved_whole;
    std::size_t reserved_whole_bytes = impl_->whole_bytes();

    std::set<Impl::ByteKey> reserved_journals;
    std::size_t reserved_journal_bytes = impl_->journals.size();
    for (auto& candidate : candidates) {
        const auto storage = Impl::key(candidate.set->patch.assignments[candidate.index].locator.provider, candidate.region, 0);
        bool incompatible = false;
        for (auto* selected : selected_candidates) if (Impl::overlap(candidate, *selected) &&
            (candidate.region.whole_value || selected->region.whole_value)) {
            if (candidate.region.whole_value != selected->region.whole_value ||
                candidate.region.byte_mask != selected->region.byte_mask) incompatible = true;
            else candidate.winner = selected;
            break;
        }
        if (incompatible) {
            candidate.eligible = false;
            candidate.result->state = OperationState::Rejected;
            candidate.result->error = make_error(ErrorCode::InvalidArgument, "Incompatible physical storage certificate");
            continue;
        }
        if (candidate.region.whole_value) {
            const auto needed = (impl_->whole_journals.contains(storage) || reserved_whole.contains(storage)) ? 0 : candidate.region.byte_mask.size();
            if (!candidate.winner && (needed > limits_.max_whole_journal_bytes ||
                reserved_whole_bytes > limits_.max_whole_journal_bytes - needed)) {
                candidate.eligible = false;
                candidate.result->state = OperationState::Rejected;
                candidate.result->error = make_error(ErrorCode::LimitExceeded, "Whole-value journal byte limit exceeded");
                continue;
            }
            candidate.selected = candidate.winner == nullptr;
            if (candidate.selected) {
                reserved_whole.insert(storage);
                reserved_whole_bytes += needed;
                selected_candidates.push_back(&candidate);
            }
            continue;
        }
        std::vector<Impl::ByteKey> needed;
        for (std::size_t i = 0; i < candidate.region.byte_mask.size(); ++i) {
            auto key = Impl::key(candidate.set->patch.assignments[candidate.index].locator.provider, candidate.region, i);
            if (!impl_->journals.contains(key) && !reserved_journals.contains(key)) needed.push_back(std::move(key));
        }
        if (needed.size() > limits_.max_journal_bytes || reserved_journal_bytes > limits_.max_journal_bytes - needed.size()) {
            candidate.eligible = false;
            candidate.result->state = OperationState::Rejected;
            candidate.result->error = make_error(ErrorCode::LimitExceeded, "Baseline journal byte limit exceeded");
            continue;
        }
        for (std::size_t i = 0; i < candidate.region.byte_mask.size() && !candidate.winner; ++i) {
            const auto key = Impl::key(candidate.set->patch.assignments[candidate.index].locator.provider, candidate.region, i);
            const auto found = selected_index.find(key);
            if (found == selected_index.end()) continue;
            for (auto* selected : found->second) if (Impl::overlap(candidate, *selected)) { candidate.winner = selected; break; }
        }
        candidate.selected = candidate.winner == nullptr;
        if (candidate.selected) {
            selected_candidates.push_back(&candidate);
            reserved_journals.insert(needed.begin(), needed.end());
            reserved_journal_bytes += needed.size();
            for (std::size_t i = 0; i < candidate.region.byte_mask.size(); ++i)
                selected_index[Impl::key(candidate.set->patch.assignments[candidate.index].locator.provider, candidate.region, i)].push_back(&candidate);
        }
    }

    std::map<Impl::ByteKey, std::uint8_t> selected_masks;
    std::set<Impl::ByteKey> selected_whole;

    for (const auto& [token, owner] : impl_->owners) if (owner.enabled)
        for (const auto& [id, set] : owner.sets)
            for (std::size_t i = 0; i < set.bindings.size(); ++i) {
                if (!set.bindings[i] || scheduled.contains({const_cast<Impl::Set*>(&set), i})) continue;
                const auto state = impl_->requests.at(set.request).operations[i].state;

                if (state != OperationState::Applied && state != OperationState::AlreadyEffective &&
                    state != OperationState::Shadowed && state != OperationState::Drifted) continue;
                const auto& region = *set.bindings[i];
                const auto& provider = set.patch.assignments[i].locator.provider;
                if (region.whole_value) selected_whole.insert(Impl::key(provider, region, 0));
                else for (std::size_t byte = 0; byte < region.byte_mask.size(); ++byte)
                    selected_masks[Impl::key(provider, region, byte)] |= region.byte_mask[byte];
            }
    for (auto& candidate : candidates) {
        if (!candidate.eligible) continue;
        auto& result = *candidate.result;
        const auto& assignment = candidate.set->patch.assignments[candidate.index];
        auto* winner = candidate.winner ? candidate.winner : &candidate;
        result.effective_owner = winner->token;
        result.effective_set = winner->set->patch.id;
        result.effective_revision = winner->set->patch.revision;
        if (candidate.region.whole_value) {
            const auto key = Impl::key(assignment.locator.provider, candidate.region, 0);
            result.drift_detected = whole_drifted.contains(key);
            if (candidate.selected) selected_whole.insert(key);
            auto journal = impl_->whole_journals.find(key);
            if (journal != impl_->whole_journals.end() && journal->second.suspended) {
                result.state = OperationState::Drifted;
                result.error = make_error(ErrorCode::ExternalChange, "Region suspended after external change");
                continue;
            }
            if (!candidate.selected) { result.state = OperationState::Shadowed; continue; }
            auto& provider = *impl_->providers.at(assignment.locator.provider);
            ++report.provider_calls;
            auto error = provider.validate(candidate.region);
            if (!error.ok()) {
                result.state = error.code == ErrorCode::TargetExpired ? OperationState::TargetExpired : OperationState::Rejected;
                result.error = error;
                continue;
            }
            auto [entry, inserted] = impl_->whole_journals.try_emplace(key);
            auto& record = entry->second;
            if (inserted) {
                record.region = candidate.region;
                record.baseline = candidate.current;
                record.last = candidate.current;
            }
            record.preserve = assignment.drift_policy == DriftPolicy::PreserveExternalChange;
            record.no_restore = assignment.restore_policy == RestorePolicy::NoRestore;
            const bool equal = candidate.current == candidate.desired.bytes;

            if (!equal || record.uncertain) {
                ++report.provider_calls;
                error = provider.write(candidate.region, candidate.desired.bytes);
                if (!error.ok()) {
                    record.uncertain = true;
                    failures[key] = error;
                    result.state = error.code == ErrorCode::TargetExpired ? OperationState::TargetExpired : OperationState::Faulted;
                    result.error = error;
                    continue;
                }
            }
            record.uncertain = false;
            record.last = candidate.desired.bytes;
            result.state = equal ? OperationState::AlreadyEffective : OperationState::Applied;
            continue;
        }
        bool suspended = false;
        for (std::size_t i = 0; i < candidate.region.byte_mask.size(); ++i) {
            const auto key = Impl::key(assignment.locator.provider, candidate.region, i);
            if (drifted_bits[key] & candidate.region.byte_mask[i]) result.drift_detected = true;
            auto journal = impl_->journals.find(key);
            if (journal != impl_->journals.end() && (journal->second.suspended & candidate.region.byte_mask[i])) suspended = true;
            if (candidate.selected) selected_masks[key] |= candidate.region.byte_mask[i];
        }
        if (suspended) {
            result.state = OperationState::Drifted;
            result.error = make_error(ErrorCode::ExternalChange, "Region suspended after external change");
            continue;
        }
        if (!candidate.selected) { result.state = OperationState::Shadowed; continue; }
        std::size_t required = 0;
        for (std::size_t i = 0; i < candidate.region.byte_mask.size(); ++i)
            if (!impl_->journals.contains(Impl::key(assignment.locator.provider, candidate.region, i))) ++required;
        if (required > limits_.max_journal_bytes || impl_->journals.size() > limits_.max_journal_bytes - required) {
            result.state = OperationState::Rejected;
            result.error = make_error(ErrorCode::LimitExceeded, "Baseline journal byte limit exceeded");
            continue;
        }
        auto& provider = *impl_->providers.at(assignment.locator.provider);

        ++report.provider_calls;
        auto error = provider.validate(candidate.region);
        if (!error.ok()) {
            result.state = error.code == ErrorCode::TargetExpired ? OperationState::TargetExpired : OperationState::Rejected;
            result.error = error;
            continue;
        }
        const bool equal = mask_equal(candidate.current, candidate.desired.bytes, candidate.region.byte_mask);

        for (std::size_t i = 0; i < candidate.region.byte_mask.size(); ++i) {
            const auto key = Impl::key(assignment.locator.provider, candidate.region, i);
            auto [journal, inserted] = impl_->journals.try_emplace(key);
            if (inserted) {
                journal->second.region = candidate.region;
                journal->second.region.offset += i;
                journal->second.region.byte_mask = {candidate.region.byte_mask[i]};
                journal->second.baseline = candidate.current[i];
                journal->second.last = candidate.current[i];
            }
            journal->second.mask |= candidate.region.byte_mask[i];
            journal->second.region.byte_mask[0] = journal->second.mask;
        }
        if (!equal) {
            ++report.provider_calls;
            error = provider.write(candidate.region, candidate.desired.bytes);
            if (!error.ok()) {
                result.state = error.code == ErrorCode::TargetExpired ? OperationState::TargetExpired : OperationState::Faulted;
                result.error = error;
                for (std::size_t i = 0; i < candidate.region.byte_mask.size(); ++i)
                    failures[Impl::key(assignment.locator.provider, candidate.region, i)] = error;
                continue;
            }
        }
        for (std::size_t i = 0; i < candidate.region.byte_mask.size(); ++i) {
            const auto key = Impl::key(assignment.locator.provider, candidate.region, i);
            auto& journal = impl_->journals.at(key);
            const auto mask = candidate.region.byte_mask[i];
            journal.last = static_cast<std::uint8_t>((journal.last & ~mask) | (candidate.desired.bytes[i] & mask));
            journal.preserve = static_cast<std::uint8_t>((journal.preserve & ~mask) |
                (assignment.drift_policy == DriftPolicy::PreserveExternalChange ? mask : 0));
            for (unsigned bit = 0; bit < 8; ++bit) if (mask & (1u << bit)) {
                if (assignment.drift_policy == DriftPolicy::PreserveExternalChange) journal.preserve_scopes[bit] = candidate.region;
                else journal.preserve_scopes[bit].reset();
            }
            journal.no_restore = static_cast<std::uint8_t>((journal.no_restore & ~mask) |
                (assignment.restore_policy == RestorePolicy::NoRestore ? mask : 0));
            live_bytes[key] = static_cast<std::uint8_t>((live_bytes[key] & ~mask) | (candidate.desired.bytes[i] & mask));
        }
        result.state = equal ? OperationState::AlreadyEffective : OperationState::Applied;
    }

    for (auto iterator = impl_->whole_journals.begin(); iterator != impl_->whole_journals.end();) {
        auto& [key, journal] = *iterator;
        if (!allowed(key.provider)) { iterator = skip_provider(impl_->whole_journals, iterator); continue; }
        if (!affected(key.provider, journal.region)) { ++iterator; continue; }
        if (selected_whole.contains(key) || !journal.quarantine.ok()) { ++iterator; continue; }
        if (journal.suspended || journal.no_restore) {
            skipped.insert(key);
            iterator = impl_->whole_journals.erase(iterator);
            continue;
        }
        auto& provider = *impl_->providers.at(key.provider);
        ++report.provider_calls;
        auto error = provider.validate(journal.region);
        if (error.code == ErrorCode::TargetExpired) {
            expired.insert(key);
            report.expired_journal_bytes += journal.baseline.size();
            iterator = impl_->whole_journals.erase(iterator);
            continue;
        }
        if (error.ok() && (!journal.region.restorable || journal.region.identity_tracking == IdentityTracking::ObservedOnly))
            error = make_error(ErrorCode::LifetimeUnverifiable, "Restoration lifetime no longer certified");
        if (error.ok()) {
            ++report.provider_calls;
            error = provider.write(journal.region, journal.baseline);
            if (!error.ok()) journal.uncertain = true;
        }
        if (!error.ok()) {
            journal.quarantine = error;
            failures[key] = error;
            ++iterator;
        } else iterator = impl_->whole_journals.erase(iterator);
    }

    for (auto iterator = impl_->journals.begin(); iterator != impl_->journals.end();) {
        auto& [key, journal] = *iterator;
        if (!allowed(key.provider)) { iterator = skip_provider(impl_->journals, iterator); continue; }
        if (!affected(key.provider, journal.region)) { ++iterator; continue; }
        const auto released = static_cast<std::uint8_t>(journal.mask & ~selected_masks[key]);
        if (!released || !journal.quarantine.ok()) { ++iterator; continue; }

        journal.suspended |= static_cast<std::uint8_t>(released & journal.preserve & drifted_bits[key]);
        const auto abandoned = static_cast<std::uint8_t>(released & (journal.suspended | journal.no_restore));
        if (abandoned) skipped.insert(key);
        const auto restore = static_cast<std::uint8_t>(released & ~abandoned);
        if (restore) {
            auto& provider = *impl_->providers.at(key.provider);
            ++report.provider_calls;
            auto error = provider.validate(journal.region);
            if (error.code == ErrorCode::TargetExpired) {
                expired.insert(key);
                ++report.expired_journal_bytes;
                iterator = impl_->journals.erase(iterator);
                continue;
            }
            if (error.ok() && (!journal.region.restorable || journal.region.identity_tracking == IdentityTracking::ObservedOnly))
                error = make_error(ErrorCode::LifetimeUnverifiable, "Restoration lifetime no longer certified");
            if (error.ok()) {
                Region region = journal.region;
                region.byte_mask = {restore};
                const std::uint8_t bytes[] = {journal.baseline};
                ++report.provider_calls;
                error = provider.write(region, bytes);
            }
            if (!error.ok()) {
                journal.quarantine = error;
                failures[key] = error;
                ++iterator;
                continue;
            }
        }
        journal.mask &= static_cast<std::uint8_t>(~released);
        journal.suspended &= journal.mask;
        journal.preserve &= journal.mask;
        journal.no_restore &= journal.mask;
        for (unsigned bit = 0; bit < 8; ++bit) if (!(journal.mask & (1u << bit))) journal.preserve_scopes[bit].reset();
        journal.region.byte_mask[0] = journal.mask;
        if (!journal.mask) iterator = impl_->journals.erase(iterator); else ++iterator;
    }

    for (auto iterator = impl_->removals.begin(); iterator != impl_->removals.end();) {
        auto& status = impl_->requests.at(iterator->request);
        if (providers.empty() || status.operations.empty()) status.observed = true;
        bool retry = false;
        for (std::size_t i = 0; i < status.operations.size(); ++i) {
            if (!allowed(iterator->providers[i])) { retry = true; continue; }
            status.observed = true;
            auto& operation = status.operations[i];
            operation.state = OperationState::Removed;
            operation.error = {};
            operation.updated_at_us = now_us();
            if (!iterator->regions[i]) continue;
            const auto& region = *iterator->regions[i];
            const auto count = region.whole_value ? 1 : region.byte_mask.size();
            for (std::size_t byte = 0; byte < count; ++byte) {
                const auto key = Impl::key(iterator->providers[i], region, byte);
                if (skipped.contains(key)) operation.baseline_restore_skipped = true;
                if (expired.contains(key)) {
                    if (region.identity_tracking == IdentityTracking::ObservedOnly) {

                        operation.baseline_restore_skipped = true;
                    } else operation.state = OperationState::TargetExpired;
                }
                auto failure = failures.find(key);
                if (failure != failures.end()) {
                    operation.state = OperationState::RestoreFailed;
                    operation.error = failure->second;
                    retry = true;
                }
            }
        }
        Impl::summarize(status);
        if (retry) ++iterator; else iterator = impl_->removals.erase(iterator);
    }

    impl_->refresh_aggregates(true);
    impl_->prune_released();
    impl_->prune_managed_owners();
    for (auto& [request, status] : impl_->requests) Impl::summarize(status);
    report.journal_bytes = impl_->journals.size() + impl_->whole_bytes();
    report.elapsed_us = now_us() - started;
    return report;
}

}
