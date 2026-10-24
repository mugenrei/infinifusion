#include "host/api_host.hpp"
#include "host/tbcr_service.hpp"
#include "formats/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>

namespace infinifusion::host {
namespace {
std::atomic<Host*> active_host{};
struct ExecutorContext {
    Host* host;
    bool logical_ready = false;
    ExecutorContext* previous;
    static thread_local ExecutorContext* current;
    explicit ExecutorContext(Host* owner) : host(owner), previous(current) { current = this; }
    ~ExecutorContext() { current = previous; }
};
thread_local ExecutorContext* ExecutorContext::current{};
ExecutorContext* executor_context(Host* host) {
    auto* context = ExecutorContext::current;
    return context && context->host == host ? context : nullptr;
}
constexpr std::size_t max_results = 1024;
constexpr std::size_t max_owner_results = 64;
constexpr std::size_t max_queue = 256;
constexpr std::size_t max_input_bytes = 4 * 1024 * 1024;
constexpr std::size_t max_queued_bytes = 16 * 1024 * 1024;
constexpr std::size_t max_properties = 256;
constexpr std::size_t max_owner_properties = 32;
constexpr std::string_view property_set_prefix = "__infinifusion_property/";

std::string copy_string(IF_String input) {
    if (input.length > 4096 || (input.length && !input.data))
        throw std::invalid_argument("invalid UTF-8 span");
    std::string value(input.length ? input.data : "", input.length);
    if (value.find('\0') != std::string::npos) throw std::invalid_argument("embedded NUL");
    return value;
}
core::Value copy_value(const IF_Value& value) {
    if (value.size != sizeof(value)) throw std::invalid_argument("value size");
    switch (value.kind) {
    case IF_BOOL:
        if (value.unsigned_value > 1) throw std::invalid_argument("boolean range");
        return core::Value::boolean(value.unsigned_value != 0);
    case IF_INT64: return core::Value::signed_integer(value.signed_value);
    case IF_UINT64: return core::Value::unsigned_integer(value.unsigned_value);
    case IF_FLOAT64: return core::Value::floating(value.float_value);
    case IF_STRING: return core::Value::string(copy_string(value.text));
    case IF_ENUM: return core::Value::enum_symbol(copy_string(value.enum_type), copy_string(value.text));
    case IF_BYTES:
        if (!value.bytes.data || !value.bytes.length || value.bytes.length > 4096) throw std::invalid_argument("invalid byte span");
        return core::Value::bytes({value.bytes.data, value.bytes.data + value.bytes.length});
    default: throw std::invalid_argument("unknown value tag");
    }
}
core::ResourceLocator copy_target(const IF_Target& target) {
    return {copy_string(target.provider), copy_string(target.object_path),
            copy_string(target.row), copy_string(target.class_guard)};
}
template<std::size_t N> void text_copy(char (&destination)[N], std::string_view source) {
    const auto count = std::min(source.size(), N - 1);
    std::memcpy(destination, source.data(), count);
    destination[count] = 0;
}
template<class Function> uint32_t guarded(Function&& function) noexcept {
    try { return function(); }
    catch (const std::invalid_argument&) { return IF_INVALID_ARGUMENT; }
    catch (...) { return static_cast<uint32_t>(core::ErrorCode::Internal); }
}
}

struct Host::Impl {
    struct Request {
        IF_Owner owner{};
        IF_Request id{};
        uint64_t core_id{};
        bool cancelled{};
        bool released{};
        bool removal{};
        bool disabling{};
        bool service_owned{};
        IF_Status summary{};
        std::vector<IF_Status> operations;
        std::optional<IF_ReadResult> read;
        std::optional<core::Value> read_value;
    };
    struct Work {
        enum Kind { Submit, Read, Remove, Disable, Resume, Catalog, ObjectSearch, Invoke } kind{};
        std::shared_ptr<Request> request;
        core::PatchSet set;
        std::vector<core::CatalogEntry> catalog;
        core::ResourceLocator target;
        core::PropertyPath path;
        std::string set_id;
        uint64_t revision{};
        uint32_t operation_index{};
        core::Value observed = core::Value::boolean(false);
        ObjectQuery query;
        FunctionCall call;
        std::size_t bytes{};
    };
    struct Property {
        IF_Owner owner{};
        core::ResourceLocator target;
        core::PropertyPath path;
        uint32_t kind{};
        std::string set_id;
        IF_Request read_request{}, edit_request{}, ack_request{};
        IF_PropertyState state{sizeof(IF_PropertyState)};
        std::optional<core::Value> desired;
        std::optional<core::Value> initial_value;
        uint32_t flags{};
        bool deferred{};
    };
    std::timed_mutex mutex;
    core::Engine engine{[] { core::EngineLimits limits; limits.max_journal_bytes = 65536;
        limits.max_whole_journal_bytes = 1024 * 1024; return limits; }()};
    std::shared_ptr<core::TbcrPatchProvider> tbcr = std::make_shared<core::TbcrPatchProvider>();
    std::shared_ptr<core::TablePatchProvider> tables = std::make_shared<core::TablePatchProvider>();
    std::shared_ptr<core::TablePatchProvider> messages = std::make_shared<core::TablePatchProvider>("unreal.message_definition");
    std::map<std::string, IF_TableState> table_observations;
    std::map<std::string, IF_TbcrState> tbcr_observations;
    std::deque<std::string> tbcr_observation_order;
    std::map<IF_Request, std::shared_ptr<Request>> requests;
    std::map<IF_Property, Property> properties;
    std::deque<Work> queue;
    std::deque<std::string> logs;
    std::vector<uint64_t> released_core_results;
    std::map<IF_Owner, bool> owners;
    std::set<IF_Owner> disabling_owners;
    std::set<std::string> providers;
    std::shared_ptr<GameServices> game_services;
    IF_Request next_request = 1;
    IF_Property next_property = 1;
    std::size_t queued_bytes{};
    bool ready{};
    bool faulted{};
    bool dirty{};
    bool definitions_dirty{true};
    std::chrono::steady_clock::time_point next_reconcile{};
    IF_Metrics telemetry{sizeof(IF_Metrics)};
    std::array<uint64_t, 4096> timing_samples{};
    std::size_t timing_count{};
    uint64_t timing_dropped{};
    uint64_t prefix_max_us{}, reconcile_max_us{}, status_max_us{};
    uint64_t bind_max_us{}, prepare_max_us{};

    void log(std::string value) {
        if (logs.size() < 1024) logs.push_back(std::move(value));
        else ++telemetry.dropped_logs;
    }
    static bool game_work(Work::Kind kind) { return kind == Work::ObjectSearch || kind == Work::Invoke; }
    static bool query_work(Work::Kind kind) { return game_work(kind) || kind == Work::Read || kind == Work::Resume; }
    void execute_game(Work& work, std::chrono::steady_clock::time_point deadline) {
        auto& request = *work.request;
        request.summary.observed = 1;
        request.summary.settled = 1;
        request.summary.pending_count = 0;
        if (request.cancelled || !owners.contains(request.owner) || !owners.at(request.owner) ||
            disabling_owners.contains(request.owner)) {
            request.summary.state = IF_CANCELLED;
            request.summary.error = IF_CANCELLED_ERROR;
            text_copy(request.summary.message, "cancelled before game-service execution");
            return;
        }
        core::Error error;
        std::string json;
        bool dispatched{}, uncertain{};
        uint64_t object_index{}, object_serial{};
        if (faulted) error = core::make_error(core::ErrorCode::Faulted, "host faulted before execution");
        else if (!ready) error = core::make_error(core::ErrorCode::NotReady, "runtime unavailable at execution");
        else if (!game_services) error = core::make_error(core::ErrorCode::CapabilityUnavailable, "game services unavailable at execution");
        else {
            try {
                if (work.kind == Work::ObjectSearch) {
                    auto result = game_services->search(work.query, deadline);
                    if (result) json = std::move(result.value());
                    else error = result.error();
                } else {
                    auto result = game_services->invoke(work.call, deadline);
                    json = std::move(result.json); error = std::move(result.error);
                    dispatched = result.dispatched; object_index = result.object_index; object_serial = result.object_serial;
                }
            } catch (...) {
                error = core::make_error(core::ErrorCode::Internal, "game-service exception; invocation dispatch may be uncertain");
                uncertain = work.kind == Work::Invoke;
            }
        }
        if (!json.empty()) {
            const bool bounded = json.size() <= IF_MAX_TEXT_BYTES && json.find('\0') == std::string::npos;
            auto parsed = bounded ? smtvv::json::parse(json) : smtvv::json::Result{};
            const auto* dispatch = parsed.value ? parsed.value->find("dispatched") : nullptr;
            const bool valid = parsed.value && parsed.value->object() &&
                (work.kind != Work::Invoke || (dispatch && dispatch->boolean() && *dispatch->boolean() == dispatched));
            if (!valid) {
                if (error.ok()) error = core::make_error(bounded ? core::ErrorCode::Internal : core::ErrorCode::LimitExceeded,
                    "game-service outcome cannot be copied as bounded JSON");
                json.clear();
            }
        }
        if (work.kind == Work::Invoke && json.empty()) {
            if (error.ok()) error = core::make_error(core::ErrorCode::Internal, "invocation returned no copied outcome");
            json = uncertain ? "{\"dispatched\":null,\"dispatch_uncertain\":true}" :
                dispatched ? "{\"dispatched\":true,\"outcome_available\":false}" :
                    "{\"dispatched\":false,\"outcome_available\":false}";
        } else if (work.kind == Work::ObjectSearch && json.empty() && error.ok())
            error = core::make_error(core::ErrorCode::Internal, "search returned no copied outcome");
        request.summary.state = error.ok() ? IF_COMPLETED : (dispatched || uncertain ? IF_OUTCOME_UNKNOWN : IF_REJECTED);
        request.summary.error = static_cast<uint32_t>(error.code);
        request.summary.rejected_count = error.ok() ? 0u : 1u;
        request.summary.object_index = object_index; request.summary.object_serial = object_serial;
        text_copy(request.summary.message, error.message);
        if (!json.empty()) {
            IF_ReadResult copied{}; copied.size = sizeof(copied); copied.kind = IF_STRING;
            copied.object_index = object_index; copied.object_serial = object_serial;
            copied.identity_tracking = IF_OBSERVED_ONLY;
            text_copy(copied.text, json); request.read = copied;
        }
    }
    uint32_t enqueue(Work work, IF_Request* output) {
        if (!output) return IF_INVALID_ARGUMENT;
        if (!owners.contains(work.request->owner) || !owners.at(work.request->owner))
            return static_cast<uint32_t>(core::ErrorCode::StaleToken);
        if (faulted) return static_cast<uint32_t>(core::ErrorCode::Faulted);
        if (disabling_owners.contains(work.request->owner)) return IF_STALE_TOKEN;
        std::size_t count{};
        for (const auto& [id, request] : requests) {
            (void)id;
            if (request->owner == work.request->owner && !request->service_owned) ++count;
        }
        const bool disabling = work.kind == Work::Disable;

        if (!disabling && ((!work.request->service_owned && count >= max_owner_results) || requests.size() >= max_results || queue.size() >= max_queue ||
            work.bytes > max_input_bytes || queued_bytes > max_queued_bytes || work.bytes > max_queued_bytes - queued_bytes))
            return IF_QUEUE_FULL;
        work.request->id = next_request++;
        work.request->summary.size = sizeof(IF_Status);
        work.request->summary.state = IF_ACCEPTED;
        work.request->removal = work.kind == Work::Remove || work.kind == Work::Disable;
        work.request->disabling = disabling;
        *output = work.request->id;
        requests.emplace(*output, work.request);
        queued_bytes += work.bytes;
        if (disabling) {
            disabling_owners.insert(work.request->owner);
            queue.push_front(std::move(work));
        } else queue.push_back(std::move(work));
        return IF_OK;
    }
    std::shared_ptr<Request> lookup(IF_Owner owner, IF_Request id) {
        auto found = requests.find(id);
        return found != requests.end() && found->second->owner == owner && !found->second->service_owned ? found->second : nullptr;
    }
    Property* property(IF_Owner owner, IF_Property id) {
        auto found = properties.find(id);
        return found != properties.end() && found->second.owner == owner ? &found->second : nullptr;
    }
    void release_internal(IF_Request& id, bool cancel_unobserved) {
        const auto found = requests.find(id);
        if (found != requests.end()) {
            auto& request = *found->second;
            if (cancel_unobserved && !request.summary.observed) request.cancelled = true;
            request.released = true;
            if (request.core_id) released_core_results.push_back(request.core_id);
            requests.erase(found);
        }
        id = 0;
    }
    uint32_t refresh(Property& property) {
        if (property.read_request) return IF_OK;
        Work work; work.kind = Work::Read;
        work.request = std::make_shared<Request>();
        work.request->owner = property.owner; work.request->service_owned = true;
        work.target = property.target; work.path = property.path;
        work.bytes = 64 + work.target.target.size() + work.target.provider.size() +
            work.target.row.size() + work.target.class_guard.size() + work.path.string().size();
        const auto error = enqueue(std::move(work), &property.read_request);
        if (error == IF_OK) { property.state.read_state = IF_ACCEPTED; property.state.read_error = IF_OK; }
        return error;
    }
    void cancel_ack(Property& property) {
        if (const auto found = requests.find(property.ack_request); found != requests.end()) {
            property.state.acknowledgement = found->second->summary;
            if (!found->second->summary.observed) {
                property.state.acknowledgement.state = IF_CANCELLED;
                property.state.acknowledgement.error = IF_CANCELLED_ERROR;
                property.state.acknowledgement.settled = 1;
                property.state.acknowledgement.pending_count = 0;
            }
        }
        release_internal(property.ack_request, true);
    }
    uint32_t queue_edit(Property& property, core::Value value, uint32_t flags, uint64_t revision) {
        if ((property.target.provider == "smtvv.tbcr" || property.target.provider == "unreal.table_definition" || property.target.provider == "unreal.message_definition") && (flags & IF_NO_RESTORE))
            return IF_INVALID_ARGUMENT;
        Work work; work.kind = Work::Submit;
        work.request = std::make_shared<Request>();
        work.request->owner = property.owner; work.request->service_owned = true;
        work.set.id = property.set_id; work.set.revision = revision;
        core::Assignment assignment;
        assignment.locator = property.target; assignment.path = property.path; assignment.value = value;
        assignment.drift_policy = flags & IF_PERSISTENT_OVERLAY ? core::DriftPolicy::PersistentOverlay : core::DriftPolicy::PreserveExternalChange;
        assignment.restore_policy = flags & IF_NO_RESTORE ? core::RestorePolicy::NoRestore : core::RestorePolicy::Required;
        work.set.assignments.push_back(std::move(assignment));
        work.bytes = sizeof(IF_Assignment) + property.set_id.size() + property.target.target.size() +
            property.target.provider.size() + property.target.row.size() + property.target.class_guard.size() + property.path.string().size();
        if (value.kind() == core::ValueKind::Bytes) work.bytes += value.as_bytes().size();
        if (value.kind() == core::ValueKind::EnumSymbol)
            work.bytes += value.as_enum().enum_type.size() + value.as_enum().symbol.size();
        IF_Request request{};
        const auto error = enqueue(std::move(work), &request);
        if (error != IF_OK) return error;
        release_internal(property.edit_request, true);
        cancel_ack(property);
        property.edit_request = request;
        property.state.has_intent = 1; property.state.revision = revision;
        property.desired = std::move(value); property.flags = flags; property.deferred = false;
        property.state.edit = requests.at(request)->summary;
        return IF_OK;
    }
    void retire_owner_properties(IF_Owner owner) {
        for (auto it = properties.begin(); it != properties.end();) {
            if (it->second.owner != owner) { ++it; continue; }
            release_internal(it->second.read_request, true);
            release_internal(it->second.edit_request, true);
            release_internal(it->second.ack_request, true);
            it = properties.erase(it);
        }
    }
    void update_properties(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
        std::size_t admitted{};
        for (auto& [id, property] : properties) {
            (void)id;
            if (const auto found = requests.find(property.read_request); found != requests.end()) {
                const auto request = found->second;
                property.state.read_state = request->summary.state;
                property.state.read_error = request->summary.error;
                if (request->read) {
                    if (request->read->kind != property.kind) {
                        property.state.read_state = IF_REJECTED;
                        property.state.read_error = IF_TYPE_MISMATCH;
                    } else {
                        property.state.current = *request->read;
                        property.state.has_current = 1;
                        ++property.state.read_generation;
                        if (!property.state.has_initial) {
                            property.state.initial = *request->read;
                            property.initial_value = request->read_value;
                            property.state.has_initial = 1;
                        }
                    }
                }
                if (request->summary.settled) release_internal(property.read_request, false);
            }
            if (const auto found = requests.find(property.edit_request); found != requests.end()) {
                const auto& request = *found->second;
                property.state.edit = request.operations.empty() ? request.summary : request.operations.front();
                if (property.state.removing && request.summary.settled && request.summary.state == IF_REMOVED) {
                    property.state.has_intent = 0; property.state.removing = 0;
                    release_internal(property.edit_request, false);

                    release_internal(property.read_request, true);
                    (void)refresh(property);
                } else if (property.state.removing && request.summary.settled && request.summary.state == IF_REJECTED) {
                    property.state.removing = 0;
                    release_internal(property.edit_request, false);
                }
            }
            if (property.deferred) {
                if (property.state.has_initial) {
                    if (admitted >= 8 || std::chrono::steady_clock::now() >= deadline) continue;
                    ++admitted;
                    const auto error = queue_edit(property, *property.desired, property.flags, property.state.revision);
                    if (error != IF_OK) property.state.edit.error = error;
                } else if (property.state.read_state == IF_REJECTED || property.state.read_state == IF_FAULTED) {
                    property.deferred = false; property.state.has_intent = 0;
                    property.state.edit.state = property.state.read_state;
                    property.state.edit.error = property.state.read_error;
                    property.state.edit.settled = 1; property.state.edit.pending_count = 0;
                } else {
                    property.state.edit.state = property.state.read_state;
                    property.state.edit.error = property.state.read_error;
                }
            }
            if (const auto found = requests.find(property.ack_request); found != requests.end()) {
                const auto request = found->second;
                property.state.acknowledgement = request->summary;
                if (request->summary.settled) release_internal(property.ack_request, false);
            }
        }
    }
};

namespace {
Host::Impl* get() { auto* host = active_host.load(); return host ? &host->impl() : nullptr; }
uint32_t IF_CALL register_owner(const IF_OwnerInfo* info, IF_Owner* output) {
    return guarded([&]() -> uint32_t {
        if (!info || !output || info->size != sizeof(*info)) return IF_INVALID_ARGUMENT;
        auto* host = get(); if (!host) return IF_NOT_READY;
        core::OwnerInfo owner{copy_string(info->id), copy_string(info->name), info->priority,
                              copy_string(info->ordering_key)};
        std::scoped_lock lock(host->mutex);
        if (host->faulted) return IF_FAULT_ERROR;
        auto result = host->engine.register_owner(std::move(owner));
        if (!result) return static_cast<uint32_t>(result.error().code);
        *output = result.value(); host->owners[*output] = true;
        host->log("API owner registered token=" + std::to_string(*output));
        return IF_OK;
    });
}
core::PatchSet copy_patch(const IF_PatchSet* input, std::size_t& bytes) {
    if (!input || input->size != sizeof(*input) || input->assignment_count > 128 ||
        (input->assignment_count && !input->assignments)) throw std::invalid_argument("invalid patch");
    core::PatchSet patch;
        patch.id = copy_string(input->id); patch.revision = input->revision; patch.order = input->order;
        if (patch.id.starts_with(property_set_prefix)) throw std::invalid_argument("invalid assignment/patch flags");
        bytes += sizeof(*input) + patch.id.size();
        for (uint32_t i = 0; i < input->assignment_count; ++i) {
            const auto& source = input->assignments[i];
            if (source.size != sizeof(source) || source.flags & ~3u || source.has_expect > 1 || source.reserved)
                throw std::invalid_argument("invalid assignment/patch flags");
            core::Assignment assignment;
            assignment.locator = copy_target(source.target);
            if ((assignment.locator.provider == "smtvv.tbcr" || assignment.locator.provider == "unreal.table_definition" || assignment.locator.provider == "unreal.message_definition") && (source.flags & IF_NO_RESTORE))
                throw std::invalid_argument("TBCR definitions require logical restoration");
            auto path = core::PropertyPath::parse(copy_string(source.property));
            if (!path) throw std::invalid_argument(path.error().message);
            assignment.path = std::move(path.value()); assignment.value = copy_value(source.value);
            if (source.has_expect) assignment.expected_original = copy_value(source.expect);
            assignment.drift_policy = source.flags & IF_PERSISTENT_OVERLAY ?
                core::DriftPolicy::PersistentOverlay : core::DriftPolicy::PreserveExternalChange;
            assignment.restore_policy = source.flags & IF_NO_RESTORE ?
                core::RestorePolicy::NoRestore : core::RestorePolicy::Required;
            bytes += sizeof(source) + assignment.locator.target.size() + assignment.locator.row.size() +
                assignment.locator.class_guard.size() + assignment.locator.provider.size() +
                source.property.length + source.value.text.length + source.value.enum_type.length + source.value.bytes.length;
            if (source.has_expect) bytes += source.expect.text.length + source.expect.enum_type.length + source.expect.bytes.length;
            patch.assignments.push_back(std::move(assignment));
        }
    return patch;
}
uint32_t IF_CALL submit(IF_Owner owner, const IF_PatchSet* input, IF_Request* output) {
    return guarded([&]() -> uint32_t {
        if (!input || input->size != sizeof(*input) || input->assignment_count > 128 ||
            (input->assignment_count && !input->assignments)) return IF_INVALID_ARGUMENT;
        auto* host = get(); if (!host) return IF_NOT_READY;
        Host::Impl::Work work; work.kind = Host::Impl::Work::Submit;
        work.request = std::make_shared<Host::Impl::Request>(); work.request->owner = owner;
        work.set = copy_patch(input, work.bytes);
        std::scoped_lock lock(host->mutex);
        return host->enqueue(std::move(work), output);
    });
}
uint32_t IF_CALL catalog_publish(IF_Owner controller, const IF_Catalog* input, IF_Request* output) {
    return guarded([&]() -> uint32_t {
        if (!input || input->size != sizeof(*input) || !input->revision ||
            input->entry_count > 127 || (input->entry_count && !input->entries)) return IF_INVALID_ARGUMENT;
        auto* host = get(); if (!host) return IF_NOT_READY;
        Host::Impl::Work work; work.kind = Host::Impl::Work::Catalog;
        work.request = std::make_shared<Host::Impl::Request>(); work.request->owner = controller;
        work.revision = input->revision; work.bytes = sizeof(*input);
        uint32_t assignments{};
        for (uint32_t i = 0; i < input->entry_count; ++i) {
            const auto& source = input->entries[i];
            if (source.size != sizeof(source) || source.reserved || source.owner.size != sizeof(IF_OwnerInfo)) return IF_INVALID_ARGUMENT;
            core::CatalogEntry entry;
            entry.owner = {copy_string(source.owner.id), copy_string(source.owner.name), source.owner.priority,
                           copy_string(source.owner.ordering_key)};
            entry.patch = copy_patch(&source.patch, work.bytes);
            entry.patch.revision = input->revision;
            assignments += static_cast<uint32_t>(entry.patch.assignments.size());
            if (assignments > 128) return IF_LIMIT_EXCEEDED;
            work.bytes += sizeof(source) + entry.owner.id.size() + entry.owner.display_name.size() + entry.owner.ordering_key.size();
            if (work.bytes > max_input_bytes) return IF_LIMIT_EXCEEDED;
            work.catalog.push_back(std::move(entry));
        }
        std::scoped_lock lock(host->mutex);
        return host->enqueue(std::move(work), output);
    });
}
uint32_t IF_CALL read(IF_Owner owner, const IF_Target* target, IF_String property, IF_Request* output) {
    return guarded([&]() -> uint32_t {
        if (!target) return IF_INVALID_ARGUMENT;
        auto* host = get(); if (!host) return IF_NOT_READY;
        Host::Impl::Work work; work.kind = Host::Impl::Work::Read;
        work.request = std::make_shared<Host::Impl::Request>(); work.request->owner = owner;
        work.target = copy_target(*target);
        auto path = core::PropertyPath::parse(copy_string(property));
        if (!path) return static_cast<uint32_t>(path.error().code);
        work.path = std::move(path.value());
        work.bytes = 64 + work.target.target.size() + work.target.row.size() + work.target.class_guard.size() +
            work.target.provider.size() + property.length;
        std::scoped_lock lock(host->mutex); return host->enqueue(std::move(work), output);
    });
}
uint32_t IF_CALL object_search(IF_Owner owner, const IF_ObjectQuery* input, IF_Request* output) {
    return guarded([&]() -> uint32_t {
        if (!input || input->size != sizeof(*input) || (input->flags & ~IF_OBJECT_QUERY_INCLUDE_DEFAULTS) ||
            !input->scan_limit || input->scan_limit > 4096 || !input->result_limit || input->result_limit > 16)
            return IF_INVALID_ARGUMENT;
        auto* host = get(); if (!host) return IF_NOT_READY;
        Host::Impl::Work work; work.kind = Host::Impl::Work::ObjectSearch;
        work.request = std::make_shared<Host::Impl::Request>(); work.request->owner = owner;
        work.query = {input->flags, input->start_index, input->scan_limit, input->result_limit,
            copy_string(input->path_contains), copy_string(input->class_path)};
        work.bytes = sizeof(*input) + work.query.path_contains.size() + work.query.class_path.size();
        std::scoped_lock lock(host->mutex); return host->enqueue(std::move(work), output);
    });
}
bool valid_import_object(std::string_view text, std::size_t limit, bool property_paths) {
    if (text.empty()) return property_paths;

    bool quoted{}, escaped{}; unsigned depth{};
    for (const char ch : text) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '[' || (ch == '{' && ++depth > 1)) return false;
        else if (ch == '}' && depth) --depth;
    }
    auto parsed = smtvv::json::parse(text);
    if (!parsed.value || !parsed.value->object() || parsed.value->object()->size() > limit) return false;
    for (const auto& [name, value] : *parsed.value->object()) {
        if (name.empty() || name.find('\0') != std::string::npos || !value.string() ||
            value.string()->find('\0') != std::string::npos) return false;
        if (property_paths && !core::PropertyPath::parse(name)) return false;
    }
    return true;
}
uint32_t IF_CALL invoke(IF_Owner owner, const IF_InvokeSpec* input, IF_Request* output) {
    return guarded([&]() -> uint32_t {
        if (!input || input->size != sizeof(*input) || (input->flags & ~IF_INVOKE_EXPECT_IDENTITY))
            return IF_INVALID_ARGUMENT;
        auto* host = get(); if (!host) return IF_NOT_READY;
        Host::Impl::Work work; work.kind = Host::Impl::Work::Invoke;
        work.request = std::make_shared<Host::Impl::Request>(); work.request->owner = owner;
        work.call = {input->flags, copy_target(input->target), copy_string(input->function),
            copy_string(input->arguments), input->expected_object_index, input->expected_object_serial,
            copy_string(input->expected_properties)};
        const auto& call = work.call;
        if (call.target.provider != "unreal.object" || call.target.target.empty() || call.function.empty())
            return IF_INVALID_ARGUMENT;
        if (!valid_import_object(call.arguments,512,false) ||
            !valid_import_object(call.expected_properties,8,true)) return IF_INVALID_ARGUMENT;
        work.bytes = sizeof(*input) + call.target.provider.size() + call.target.target.size() +
            call.target.row.size() + call.target.class_guard.size() + call.function.size() + call.arguments.size() + call.expected_properties.size();
        std::scoped_lock lock(host->mutex); return host->enqueue(std::move(work), output);
    });
}
uint32_t remove_or_disable(IF_Owner owner, IF_String id, IF_Request* output, bool disable) {
    return guarded([&]() -> uint32_t {
        auto* host = get(); if (!host) return IF_NOT_READY;
        Host::Impl::Work work; work.kind = disable ? Host::Impl::Work::Disable : Host::Impl::Work::Remove;
        work.request = std::make_shared<Host::Impl::Request>(); work.request->owner = owner;
        work.set_id = copy_string(id); work.bytes = 64 + work.set_id.size();
        if (!disable && work.set_id.starts_with(property_set_prefix)) return IF_INVALID_ARGUMENT;
        std::scoped_lock lock(host->mutex); return host->enqueue(std::move(work), output);
    });
}
uint32_t IF_CALL remove_set(IF_Owner owner, IF_String id, IF_Request* out) { return remove_or_disable(owner, id, out, false); }
uint32_t IF_CALL disable_owner(IF_Owner owner, IF_Request* out) { return remove_or_disable(owner, {}, out, true); }
uint32_t IF_CALL status(IF_Owner owner, IF_Request id, uint32_t index, IF_Status* output) {
    return guarded([&]() -> uint32_t {
        if (!output || output->size != sizeof(*output)) return IF_INVALID_ARGUMENT;
        auto* host = get(); if (!host) return IF_NOT_READY;
        std::scoped_lock lock(host->mutex); auto request = host->lookup(owner, id);
        if (!request) return IF_INVALID_HANDLE;
        if (index == UINT32_MAX) *output = request->summary;
        else {
            if (index >= request->operations.size()) return IF_NOT_READY;
            *output = request->operations[index];
        }
        return IF_OK;
    });
}
uint32_t IF_CALL copy_read(IF_Owner owner, IF_Request id, IF_ReadResult* output) {
    return guarded([&]() -> uint32_t {
        if (!output || output->size != sizeof(*output)) return IF_INVALID_ARGUMENT;
        auto* host = get(); if (!host) return IF_NOT_READY;
        std::scoped_lock lock(host->mutex); auto request = host->lookup(owner, id);
        if (!request) return IF_INVALID_HANDLE;
        if (!request->read) return request->summary.error ? request->summary.error : IF_NOT_READY;
        *output = *request->read; return IF_OK;
    });
}
uint32_t IF_CALL release(IF_Owner owner, IF_Request id) {
    return guarded([&]() -> uint32_t {
        auto* host = get(); if (!host) return IF_NOT_READY;
        std::scoped_lock lock(host->mutex);
        auto request = host->lookup(owner, id);
        if (!request) return IF_INVALID_HANDLE;
        request->released = true;
        if (request->core_id) host->released_core_results.push_back(request->core_id);
        host->requests.erase(id); return IF_OK;
    });
}
uint32_t IF_CALL property_open(IF_Owner owner, const IF_PropertySpec* spec, IF_Property* output) {
    return guarded([&]() -> uint32_t {
        if (!spec || !output || spec->size != sizeof(*spec) || (spec->kind > IF_ENUM && spec->kind != IF_BYTES))
            return IF_INVALID_ARGUMENT;
        auto* host = get(); if (!host) return IF_NOT_READY;
        Host::Impl::Property property;
        property.owner = owner; property.kind = spec->kind; property.target = copy_target(spec->target);
        if (property.target.target.empty() || property.target.provider.empty()) return IF_INVALID_ARGUMENT;
        auto path = core::PropertyPath::parse(copy_string(spec->property));
        if (!path) return static_cast<uint32_t>(path.error().code);
        property.path = std::move(path.value());
        property.state.initial.size = property.state.current.size = sizeof(IF_ReadResult);
        property.state.edit.size = sizeof(IF_Status); property.state.edit.state = IF_REMOVED;
        property.state.edit.settled = 1;
        property.state.acknowledgement.size = sizeof(IF_Status);
        property.state.acknowledgement.state = IF_COMPLETED; property.state.acknowledgement.settled = 1;
        std::scoped_lock lock(host->mutex);
        if (host->faulted) return IF_FAULT_ERROR;
        if (!host->owners.contains(owner) || !host->owners.at(owner)) return IF_STALE_TOKEN;
        const auto count = std::count_if(host->properties.begin(), host->properties.end(),
            [&](const auto& item) { return item.second.owner == owner; });
        if (count >= max_owner_properties || host->properties.size() >= max_properties) return IF_LIMIT_EXCEEDED;
        const auto id = host->next_property++;
        property.set_id = std::string(property_set_prefix) + std::to_string(id);
        const auto error = host->refresh(property);
        if (error != IF_OK) return error;
        host->properties.emplace(id, std::move(property)); *output = id;
        return IF_OK;
    });
}
uint32_t IF_CALL property_get(IF_Owner owner, IF_Property id, IF_PropertyState* output) {
    return guarded([&]() -> uint32_t {
        if (!output || output->size != sizeof(*output)) return IF_INVALID_ARGUMENT;
        auto* host = get(); if (!host) return IF_NOT_READY;
        std::scoped_lock lock(host->mutex);
        const auto* property = host->property(owner, id); if (!property) return IF_INVALID_HANDLE;
        *output = property->state;
        return IF_OK;
    });
}
uint32_t IF_CALL property_refresh(IF_Owner owner, IF_Property id) {
    return guarded([&]() -> uint32_t {
        auto* host = get(); if (!host) return IF_NOT_READY;
        std::scoped_lock lock(host->mutex);
        auto* property = host->property(owner, id); if (!property) return IF_INVALID_HANDLE;
        return host->refresh(*property);
    });
}
uint32_t set_property_locked(Host::Impl& host, Host::Impl::Property& property,
                             core::Value value, uint32_t flags, uint64_t revision) {
    if (host.disabling_owners.contains(property.owner)) return IF_STALE_TOKEN;
    if (host.faulted) return IF_FAULT_ERROR;
    if (flags & ~3u) return IF_INVALID_ARGUMENT;
    if (static_cast<uint32_t>(value.kind()) != property.kind) return IF_TYPE_MISMATCH;
    if (value.kind() == core::ValueKind::Float && !std::isfinite(value.as_float())) return IF_NOT_FINITE;
    if (property.state.removing) return IF_NOT_READY;
    const bool definition = property.target.provider == "smtvv.tbcr" ||
        property.target.provider == "unreal.table_definition" || property.target.provider == "unreal.message_definition";

    if (!definition && !property.state.has_initial && property.state.read_state == IF_REJECTED) return property.state.read_error;
    if (!revision) {
        if (property.state.revision == std::numeric_limits<uint64_t>::max()) return IF_OUT_OF_RANGE;
        revision = property.state.revision + 1;
    }
    if (revision < property.state.revision) return IF_STALE_REVISION;
    if (revision == property.state.revision) {
        return property.state.has_intent && property.desired && *property.desired == value && property.flags == flags ?
            IF_OK : IF_REVISION_CONFLICT;
    }
    if (definition || property.state.has_initial) return host.queue_edit(property, std::move(value), flags, revision);
    property.desired = std::move(value); property.flags = flags; property.deferred = true;
    property.state.has_intent = 1; property.state.revision = revision;
    property.state.edit = {}; property.state.edit.size = sizeof(IF_Status);
    property.state.edit.state = property.state.read_state; property.state.edit.pending_count = 1;
    property.state.edit.revision = revision;
    return IF_OK;
}
uint32_t IF_CALL property_set(IF_Owner owner, IF_Property id, const IF_Value* input, uint32_t flags, uint64_t revision) {
    return guarded([&]() -> uint32_t {
        if (!input) return IF_INVALID_ARGUMENT;
        auto value = copy_value(*input);
        auto* host = get(); if (!host) return IF_NOT_READY;
        std::scoped_lock lock(host->mutex);
        auto* property = host->property(owner, id); if (!property) return IF_INVALID_HANDLE;
        return set_property_locked(*host, *property, std::move(value), flags, revision);
    });
}
uint32_t IF_CALL property_reset(IF_Owner owner, IF_Property id, uint32_t flags, uint64_t revision) {
    return guarded([&]() -> uint32_t {
        auto* host = get(); if (!host) return IF_NOT_READY;
        std::scoped_lock lock(host->mutex);
        auto* property = host->property(owner, id); if (!property) return IF_INVALID_HANDLE;
        if (!property->state.has_initial || !property->initial_value) return IF_NOT_READY;
        return set_property_locked(*host, *property, *property->initial_value, flags, revision);
    });
}
uint32_t IF_CALL property_remove(IF_Owner owner, IF_Property id) {
    return guarded([&]() -> uint32_t {
        auto* host = get(); if (!host) return IF_NOT_READY;
        std::scoped_lock lock(host->mutex);
        auto* property = host->property(owner, id); if (!property) return IF_INVALID_HANDLE;
        if (host->faulted) return IF_FAULT_ERROR;
        if (!property->state.has_intent || property->state.removing) return IF_OK;
        if (property->deferred) {
            property->deferred = false; property->state.has_intent = 0;
            property->state.edit = {}; property->state.edit.size = sizeof(IF_Status);
            property->state.edit.state = IF_REMOVED; property->state.edit.settled = 1;
            return IF_OK;
        }
        Host::Impl::Work work; work.kind = Host::Impl::Work::Remove;
        work.request = std::make_shared<Host::Impl::Request>();
        work.request->owner = owner; work.request->service_owned = true;
        work.set_id = property->set_id; work.bytes = 64 + work.set_id.size();
        IF_Request request{}; const auto error = host->enqueue(std::move(work), &request);
        if (error != IF_OK) return error;

        host->release_internal(property->edit_request, true);
        host->cancel_ack(*property);
        property->edit_request = request; property->state.removing = 1;
        property->state.edit = host->requests.at(request)->summary;
        return IF_OK;
    });
}
uint32_t IF_CALL property_close(IF_Owner owner, IF_Property id) {
    return guarded([&]() -> uint32_t {
        auto* host = get(); if (!host) return IF_NOT_READY;
        std::scoped_lock lock(host->mutex);
        auto* property = host->property(owner, id); if (!property) return IF_INVALID_HANDLE;
        if (property->state.has_intent || property->state.removing) return IF_REVISION_CONFLICT;
        host->release_internal(property->read_request, true);
        host->release_internal(property->edit_request, true);
        host->release_internal(property->ack_request, true);
        host->properties.erase(id); return IF_OK;
    });
}
uint32_t IF_CALL property_resume(IF_Owner owner, IF_Property id, const IF_Value* observed) {
    return guarded([&]() -> uint32_t {
        if (!observed) return IF_INVALID_ARGUMENT;
        auto value = copy_value(*observed);
        auto* host = get(); if (!host) return IF_NOT_READY;
        std::scoped_lock lock(host->mutex);
        auto* property = host->property(owner, id); if (!property) return IF_INVALID_HANDLE;
        if (!property->state.has_intent || property->state.removing || property->deferred) return IF_NOT_READY;
        if (observed->kind != property->kind) return IF_TYPE_MISMATCH;
        Host::Impl::Work work; work.kind = Host::Impl::Work::Resume;
        work.request = std::make_shared<Host::Impl::Request>();
        work.request->owner = owner; work.request->service_owned = true;
        work.set_id = property->set_id; work.revision = property->state.revision;
        work.observed = std::move(value); work.bytes = sizeof(IF_Value) + work.set_id.size() + observed->text.length + observed->enum_type.length + observed->bytes.length;
        IF_Request request{}; const auto error = host->enqueue(std::move(work), &request);
        if (error != IF_OK) return error;
        host->release_internal(property->ack_request, true);
        property->ack_request = request;
        property->state.acknowledgement = host->requests.at(request)->summary;
        return IF_OK;
    });
}
uint32_t IF_CALL resume(IF_Owner owner, IF_String id, uint64_t revision, uint32_t index,
                        const IF_Value* observed, IF_Request* output) {
    return guarded([&]() -> uint32_t {
        if (!observed || !revision) return IF_INVALID_ARGUMENT;
        auto* host = get(); if (!host) return IF_NOT_READY;
        Host::Impl::Work work; work.kind = Host::Impl::Work::Resume;
        work.request = std::make_shared<Host::Impl::Request>(); work.request->owner = owner;
        work.set_id = copy_string(id); work.revision = revision; work.operation_index = index;
        if (work.set_id.starts_with(property_set_prefix)) return IF_INVALID_ARGUMENT;
        work.observed = copy_value(*observed); work.bytes = sizeof(IF_Value) + work.set_id.size() +
            observed->text.length + observed->enum_type.length + observed->bytes.length;
        std::scoped_lock lock(host->mutex); return host->enqueue(std::move(work), output);
    });
}
uint32_t IF_CALL metrics(IF_Metrics* output) {
    return guarded([&]() -> uint32_t {
        if (!output || output->size != sizeof(*output)) return IF_INVALID_ARGUMENT;
        auto* host = get(); if (!host) return IF_NOT_READY;
        std::scoped_lock lock(host->mutex); *output = host->telemetry;
        output->queue_depth = host->queue.size(); output->retained_results = host->requests.size();
        output->property_bindings = host->properties.size();
        output->faulted = host->faulted;
        return IF_OK;
    });
}
uint32_t IF_CALL cancel(IF_Owner owner, IF_Request id) {
    return guarded([&]() -> uint32_t {
        auto* host = get(); if (!host) return IF_NOT_READY;
        std::scoped_lock lock(host->mutex); auto request = host->lookup(owner, id);
        if (!request) return IF_INVALID_HANDLE;
        if (request->disabling) return IF_INVALID_ARGUMENT;
        if (request->summary.observed) return static_cast<uint32_t>(core::ErrorCode::RevisionConflict);
        request->cancelled = true; return IF_OK;
    });
}
uint32_t IF_CALL capabilities(IF_Capability* out, uint32_t capacity, uint32_t* count) {
    return guarded([&]() -> uint32_t {
        if (!count || (capacity && !out)) return IF_INVALID_ARGUMENT;
        auto* host = get(); if (!host) return IF_NOT_READY;
        std::scoped_lock lock(host->mutex);
        const char* names[] = {"infinifusion.core.patch_sets", "unreal.reflected.read", "unreal.scalar.assign", "infinifusion.properties", "infinifusion.catalogs", "smtvv.tbcr.definition", "tbcr.record.append", "unreal.table.definition", "unreal.message.definition", "unreal.reflection.inspect", "unreal.object.search", "unreal.function.invoke"};
        *count = static_cast<uint32_t>(std::size(names));
        if (capacity < *count) return static_cast<uint32_t>(core::ErrorCode::LimitExceeded);
        for (uint32_t i = 0; i < *count; ++i) {
            out[i] = {}; text_copy(out[i].id, names[i]); out[i].major = 1;
            if(i==11)out[i].minor=1;
            const bool scalar = host->providers.contains("unreal.object");
            out[i].available = !host->faulted && (i >= 10 ? (host->ready && !!host->game_services) : i == 9 ?
                (host->ready && host->providers.contains("unreal.inspection")) : (i == 0 || i >= 3 ||
                (host->ready && scalar && (i == 1 || i == 2))));
        }
        return IF_OK;
    });
}
uint32_t IF_CALL tbcr_state(IF_String asset, IF_TbcrState* output) {
    return guarded([&]() -> uint32_t {
        if (!output || output->size != sizeof(*output)) return IF_INVALID_ARGUMENT;
        const auto copied_asset = copy_string(asset);
        if (copied_asset.empty()) return IF_INVALID_ARGUMENT;
        auto* host = get(); if (!host) return IF_NOT_READY;
        std::scoped_lock lock(host->mutex);
        const auto observed = host->tbcr_observations.find(copied_asset);
        if (observed == host->tbcr_observations.end()) return IF_NOT_FOUND;
        *output = observed->second;
        return IF_OK;
    });
}
uint32_t IF_CALL table_state(IF_String asset, IF_TableState* output) {
    return guarded([&]() -> uint32_t {
        if (!output || output->size != sizeof(*output)) return IF_INVALID_ARGUMENT;
        const auto copied_asset = copy_string(asset);
        if (copied_asset.empty()) return IF_INVALID_ARGUMENT;
        auto* host = get(); if (!host) return IF_NOT_READY;
        std::scoped_lock lock(host->mutex);
        const auto found = host->table_observations.find(copied_asset);
        if (found == host->table_observations.end()) return IF_NOT_FOUND;
        *output = found->second; return IF_OK;
    });
}
void IF_CALL log(uint32_t level, IF_String message) {
    (void)guarded([&]() -> uint32_t {
        auto* host = get(); if (!host) return IF_NOT_READY;
        auto copied = copy_string(message);
        std::scoped_lock lock(host->mutex);
        host->log("Plugin[" + std::to_string(level) + "] " + copied); return IF_OK;
    });
}
}

Host::Host() : impl_(std::make_unique<Impl>()) {
    const auto tbcr_error = impl_->engine.add_provider(impl_->tbcr);
    if (!tbcr_error.ok()) throw std::logic_error(tbcr_error.message);
    impl_->providers.insert(std::string(impl_->tbcr->id()));
    const auto table_error = impl_->engine.add_provider(impl_->tables);
    if (!table_error.ok()) throw std::logic_error(table_error.message);
    impl_->providers.insert(std::string(impl_->tables->id()));
    const auto message_error = impl_->engine.add_provider(impl_->messages);
    if (!message_error.ok()) throw std::logic_error(message_error.message);
    impl_->providers.insert(std::string(impl_->messages->id()));
    Host* expected = nullptr;
    if (!active_host.compare_exchange_strong(expected, this)) throw std::logic_error("one API host per process");
}
Host::~Host() { active_host.store(nullptr); }
Host::Impl& Host::impl() { return *impl_; }
IF_Api Host::api() const {
    return {sizeof(IF_Api), IF_API_VERSION, register_owner, submit, read, remove_set,
            disable_owner, status, copy_read, release, cancel, capabilities, log, resume, metrics,
            property_open, property_get, property_refresh, property_set, property_reset, property_remove, property_close, property_resume, catalog_publish, tbcr_state, tbcr_compile_append, tbcr_compile_update, table_state, object_search, invoke};
}
void Host::set_game_services(std::shared_ptr<GameServices> services) {
    std::scoped_lock lock(impl_->mutex);
    impl_->game_services = std::move(services);
}
core::Error Host::add_provider(std::shared_ptr<core::Provider> provider) {
    std::scoped_lock lock(impl_->mutex);
    const std::string id = provider ? std::string(provider->id()) : "";
    auto error = impl_->engine.add_provider(std::move(provider));
    if (error.ok()) impl_->providers.insert(id);
    return error;
}
std::vector<core::TbcrPatchProvider::Definition> Host::tbcr_definitions() {
    if (const auto* context = executor_context(this))
        return context->logical_ready ? impl_->tbcr->definitions() : std::vector<core::TbcrPatchProvider::Definition>{};
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->ready || impl_->faulted) return {};
    return impl_->tbcr->definitions();
}
std::vector<core::TablePatchProvider::Definition> Host::table_definitions() {
    if (const auto* context = executor_context(this))
        return context->logical_ready ? impl_->tables->definitions() : std::vector<core::TablePatchProvider::Definition>{};
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->ready || impl_->faulted) return {};
    return impl_->tables->definitions();
}
std::vector<core::TablePatchProvider::Definition> Host::message_definitions() {
    if (const auto* context = executor_context(this))
        return context->logical_ready ? impl_->messages->definitions() : std::vector<core::TablePatchProvider::Definition>{};
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->ready || impl_->faulted) return {};
    return impl_->messages->definitions();
}
void Host::table_observed(std::string_view asset, const IF_TableState& state) {
    if (asset.empty() || asset.size() > 2048 || asset.find('\0') != asset.npos || state.size != sizeof(state))
        throw std::invalid_argument("invalid copied table observation");
    std::unique_lock lock(impl_->mutex, std::defer_lock);
    if (const auto* context = executor_context(this)) {
        if (!context->logical_ready) return;
    } else lock.lock();
    if (!impl_->table_observations.contains(std::string(asset)) && impl_->table_observations.size() >= 128)
        impl_->table_observations.erase(impl_->table_observations.begin());
    auto copied = state; copied.message[sizeof(copied.message)-1] = 0;
    impl_->table_observations[std::string(asset)] = copied;
}
void Host::tbcr_observed(std::string_view asset, const IF_TbcrState& state) {
    if (asset.empty() || asset.size() > 2048 || asset.find('\0') != asset.npos || state.size != sizeof(state))
        throw std::invalid_argument("invalid copied TBCR observation");
    std::unique_lock lock(impl_->mutex, std::defer_lock);
    if (const auto* context = executor_context(this)) {
        if (!context->logical_ready) return;
    } else lock.lock();
    auto& observations = impl_->tbcr_observations;
    const std::string key(asset);
    auto& order = impl_->tbcr_observation_order;
    if (auto existing = std::find(order.begin(), order.end(), key); existing != order.end()) order.erase(existing);
    order.push_back(key);
    if (order.size() > 128) { observations.erase(order.front()); order.pop_front(); }
    auto copied = state; copied.message[sizeof(copied.message) - 1] = 0;
    observations[key] = copied;
}
void Host::set_runtime_ready(bool ready) { std::scoped_lock lock(impl_->mutex); impl_->ready = ready; }
std::vector<std::string> Host::object_targets() {
    std::scoped_lock lock(impl_->mutex);
    return impl_->engine.targets("unreal.object");
}
bool Host::notify_target(std::string_view provider, std::string_view target) {
    std::unique_lock lock(impl_->mutex,std::try_to_lock);
    if (!lock.owns_lock()) return false;
    impl_->engine.notify_target(provider,target);
    impl_->dirty=true;
    return true;
}
void Host::fault(std::string message) {
    std::scoped_lock lock(impl_->mutex); impl_->faulted = true; impl_->log("API FAULT: " + message);
    for (auto& [id, request] : impl_->requests) {
        (void)id;
        if (request->read || request->cancelled) continue;
        request->summary.state = IF_FAULTED; request->summary.error = IF_FAULT_ERROR;
        request->summary.settled = 1; request->summary.pending_count = 0;
        text_copy(request->summary.message, message);
        for (auto& operation : request->operations) {
            operation.state = IF_FAULTED; operation.error = IF_FAULT_ERROR;
            operation.settled = 1; operation.pending_count = 0; text_copy(operation.message, message);
        }
    }
    impl_->queue.clear(); impl_->queued_bytes = 0;
    impl_->update_properties();
    for (auto& [id, property] : impl_->properties) {
        (void)id;
        property.state.read_state = IF_FAULTED; property.state.read_error = IF_FAULT_ERROR;
        property.state.edit.state = IF_FAULTED; property.state.edit.error = IF_FAULT_ERROR;
        property.state.edit.settled = 1; text_copy(property.state.edit.message, message);
    }
}
void Host::drain_logs(const std::function<void(std::string_view)>& sink) {
    std::deque<std::string> logs;
    { std::scoped_lock lock(impl_->mutex); logs.swap(impl_->logs); }
    for (const auto& message : logs) sink(message);
}
TimingWindow Host::drain_timing() {
    std::vector<uint64_t> samples;
    TimingWindow result;
    {
        std::scoped_lock lock(impl_->mutex);
        samples.assign(impl_->timing_samples.begin(), impl_->timing_samples.begin() + impl_->timing_count);
        result.dropped = impl_->timing_dropped;
        result.prefix_max_us = impl_->prefix_max_us;
        result.reconcile_max_us = impl_->reconcile_max_us;
        result.status_max_us = impl_->status_max_us;
        result.bind_max_us = impl_->bind_max_us;
        result.prepare_max_us = impl_->prepare_max_us;
        impl_->prefix_max_us = impl_->reconcile_max_us = impl_->status_max_us = 0;
        impl_->bind_max_us = impl_->prepare_max_us = 0;
        impl_->timing_count = 0; impl_->timing_dropped = 0;
    }
    if (samples.empty()) return result;
    std::sort(samples.begin(), samples.end()); result.count = samples.size();
    const auto percentile = [&](std::size_t percent) { return samples[(samples.size() * percent + 99) / 100 - 1]; };
    result.p50_us = percentile(50); result.p95_us = percentile(95); result.p99_us = percentile(99);
    result.max_us = samples.back(); return result;
}
void Host::executor_tick(std::chrono::steady_clock::time_point deadline,
                          std::chrono::steady_clock::time_point callback_start) {
    (void)executor_pass(deadline, callback_start, false);
}
bool Host::executor_definitions(std::chrono::steady_clock::time_point deadline) {
    return executor_pass(deadline, {}, true);
}
bool Host::executor_object_definitions(std::string_view target,
                                       std::chrono::steady_clock::time_point deadline,bool refresh) {
    if (target.empty()) return false;
    return executor_pass(deadline, {}, true, target,refresh);
}
bool Host::executor_pass(std::chrono::steady_clock::time_point deadline,
                         std::chrono::steady_clock::time_point callback_start, bool definitions,
                         std::string_view object_target,bool refresh_object) {
    const auto started = callback_start == std::chrono::steady_clock::time_point{} ?
        std::chrono::steady_clock::now() : callback_start;
    if (deadline == std::chrono::steady_clock::time_point{}) deadline = started + std::chrono::microseconds(1500);
    if (const auto* context = executor_context(this))
        return definitions && object_target.empty() && context->logical_ready &&
            std::chrono::steady_clock::now() < deadline;
    std::unique_lock lock(impl_->mutex, std::defer_lock);

    if (definitions) {
        if (std::chrono::steady_clock::now() >= deadline || !lock.try_lock_until(deadline)) return false;
        if (std::chrono::steady_clock::now() >= deadline) return false;
    } else if (!lock.try_lock()) return false;
    if (!impl_->ready || impl_->faulted) {

        if (!definitions) {
            std::size_t processed{};
            for (auto it = impl_->queue.begin(); it != impl_->queue.end() && processed < 8 &&
                std::chrono::steady_clock::now() < deadline;) {
                if (!Impl::game_work(it->kind)) { ++it; continue; }
                auto work = std::move(*it); it = impl_->queue.erase(it);
                impl_->queued_bytes -= work.bytes;
                impl_->execute_game(work, deadline); ++processed;
            }
        }
        return false;
    }
    if (!object_target.empty() && std::find(impl_->providers.begin(), impl_->providers.end(),
        "unreal.object") == impl_->providers.end()) return false;
    ExecutorContext context(this);
    if (!definitions) impl_->prefix_max_us = std::max(impl_->prefix_max_us,
        static_cast<uint64_t>(std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count())));

    for (const auto core_id : impl_->released_core_results) {
        const bool retained = std::any_of(impl_->requests.begin(), impl_->requests.end(), [&](const auto& item) {
            return item.second->core_id == core_id;
        });
        if (!retained) (void)impl_->engine.release_request(core_id);
    }
    impl_->released_core_results.clear();
    std::size_t processed{};
    const auto queue_at_start = impl_->queue.size();
    const bool periodic_due = started >= impl_->next_reconcile;
    const bool reconcile_first = impl_->dirty || (periodic_due &&
        (impl_->telemetry.active_operations || impl_->telemetry.journal_bytes));
    std::size_t cursor{};
    const auto frontier = definitions ? queue_at_start : std::min<std::size_t>(8, queue_at_start);
    while ((definitions || !reconcile_first) && !impl_->queue.empty() && processed < frontier && std::chrono::steady_clock::now() < deadline) {
        ++processed;

        const auto kind = impl_->queue[cursor].kind;
        if (definitions && Impl::query_work(kind)) {
            ++cursor;
            continue;
        }
        auto work = std::move(impl_->queue[cursor]);
        if (definitions) impl_->queue.erase(impl_->queue.begin() + cursor);
        else impl_->queue.pop_front();
        impl_->queued_bytes -= work.bytes;
        auto& request = *work.request;
        if (Impl::game_work(work.kind)) { impl_->execute_game(work, deadline); continue; }
        if (request.cancelled) { request.summary.state = IF_CANCELLED; request.summary.error = IF_CANCELLED_ERROR;
            request.summary.observed = 1; request.summary.settled = 1; continue; }
        request.summary.observed = 1;
        if (work.kind == Impl::Work::Read) {
            auto result = impl_->engine.read(request.owner, work.target, work.path);
            if (!result) {
                request.summary.error = static_cast<uint32_t>(result.error().code);
                request.summary.state = result.error().code == core::ErrorCode::NotReady ? IF_WAITING_RUNTIME :
                    result.error().code == core::ErrorCode::NotFound ? IF_WAITING_TARGET : IF_REJECTED;
                text_copy(request.summary.message, result.error().message);
                const bool retry = request.summary.state == IF_WAITING_RUNTIME || request.summary.state == IF_WAITING_TARGET;
                request.summary.settled = !retry; request.summary.pending_count = retry ? 1u : 0u;
                if (retry && !request.released) { impl_->queued_bytes += work.bytes; impl_->queue.push_back(std::move(work)); }
            } else {
                const auto& value = result->value;
                const std::string* text = value.kind() == core::ValueKind::String ? &value.as_string() :
                    value.kind() == core::ValueKind::EnumSymbol ? &value.as_enum().symbol : nullptr;
                if (text && (text->size() > IF_MAX_TEXT_BYTES || text->find('\0') != std::string::npos)) {
                    request.summary.state = IF_REJECTED; request.summary.settled = 1;
                    request.summary.error = IF_LIMIT_EXCEEDED; request.summary.pending_count = 0;
                    text_copy(request.summary.message, "read text exceeds the public copied-value bounds");
                    continue;
                }
                IF_ReadResult copied{}; copied.size = sizeof(copied); copied.kind = static_cast<uint32_t>(result->value.kind());
                copied.object_index = result->region.identity.object_index; copied.object_serial = result->region.identity.object_serial;
                copied.identity_tracking = static_cast<uint32_t>(result->region.identity_tracking); copied.restorable = result->region.restorable;
                text_copy(copied.canonical_path, result->region.canonical_path);
                switch (result->value.kind()) {
                case core::ValueKind::Boolean: copied.unsigned_value = result->value.as_bool(); break;
                case core::ValueKind::SignedInteger: copied.signed_value = result->value.as_signed(); break;
                case core::ValueKind::UnsignedInteger: copied.unsigned_value = result->value.as_unsigned(); break;
                case core::ValueKind::Float: copied.float_value = result->value.as_float(); break;
                case core::ValueKind::String: text_copy(copied.text, result->value.as_string()); break;
                case core::ValueKind::EnumSymbol: text_copy(copied.text, result->value.as_enum().symbol); break;
                case core::ValueKind::Bytes: {
                    const auto& bytes = result->value.as_bytes();
                    if (bytes.size() > sizeof(copied.bytes)) throw std::logic_error("provider byte result exceeds ABI limit");
                    copied.byte_count = static_cast<uint32_t>(bytes.size());
                    std::copy(bytes.begin(), bytes.end(), copied.bytes); break;
                }
                }
                request.read = copied; request.read_value = result->value;
                request.summary.state = IF_COMPLETED; request.summary.settled = 1;
                request.summary.error = IF_OK; request.summary.pending_count = 0; request.summary.message[0] = 0;
            }
            continue;
        }
        if (work.kind == Impl::Work::Resume) {
            const auto error = impl_->engine.resume(request.owner, work.set_id, work.revision,
                work.operation_index, work.observed);
            request.summary.state = error.ok() ? IF_COMPLETED : IF_REJECTED;
            request.summary.error = static_cast<uint32_t>(error.code); request.summary.settled = 1;
            request.summary.revision = work.revision; text_copy(request.summary.message, error.message);
            if (error.ok()) { impl_->dirty = true; impl_->definitions_dirty = true; }
            continue;
        }
        auto receipt = work.kind == Impl::Work::Catalog ? impl_->engine.publish_catalog(request.owner, work.revision, std::move(work.catalog)) :
            work.kind == Impl::Work::Submit ? impl_->engine.replace_set(request.owner, std::move(work.set)) :
            work.kind == Impl::Work::Remove ? impl_->engine.remove_set(request.owner, work.set_id) :
            impl_->engine.disable_owner(request.owner);
        if (!receipt) {
            request.summary.state = IF_REJECTED; request.summary.error = static_cast<uint32_t>(receipt.error().code);
            text_copy(request.summary.message, receipt.error().message); request.summary.settled = 1;
        } else {
            request.core_id = receipt->request_id; request.summary.revision = receipt->revision;
            if (request.released) {
                const bool retained = std::any_of(impl_->requests.begin(), impl_->requests.end(), [&](const auto& item) {
                    return item.second->core_id == request.core_id;
                });
                if (!retained) (void)impl_->engine.release_request(request.core_id);
            }
            impl_->dirty = true;
            impl_->definitions_dirty = true;
            if (work.kind == Impl::Work::Disable) {
                impl_->owners[request.owner] = false;
                impl_->retire_owner_properties(request.owner);
            }
        }
    }
    bool definitions_complete = false;
    const auto now = std::chrono::steady_clock::now();
    if ((definitions || impl_->dirty || periodic_due) && now < deadline &&
        (!definitions || processed == frontier)) {
        constexpr std::array<std::string_view, 3> definition_providers{"smtvv.tbcr", "unreal.table_definition", "unreal.message_definition"};
        core::ReconcileReport report;
        report.active_operations = impl_->telemetry.active_operations;
        report.journal_bytes = impl_->telemetry.journal_bytes;
        const auto merge = [&](const core::ReconcileReport& next) {
            report.active_operations = next.active_operations;
            report.journal_bytes = next.journal_bytes;
            report.processed_operations += next.processed_operations;
            report.provider_calls += next.provider_calls;
            report.expired_journal_bytes += next.expired_journal_bytes;
            report.elapsed_us += next.elapsed_us;
            report.bind_us += next.bind_us;
            report.prepare_us += next.prepare_us;
            if (report.error.ok()) report.error = next.error;
        };

        if (impl_->definitions_dirty) {
            merge(impl_->engine.reconcile(definition_providers));
            if (report.error.ok()) impl_->definitions_dirty = false;
        }
        const bool structural_pending = std::any_of(impl_->queue.begin(), impl_->queue.end(), [](const auto& work) {
            return !Impl::query_work(work.kind);
        });
        context.logical_ready = !impl_->definitions_dirty && !structural_pending;
        bool physical_complete = definitions;
        if (!definitions && report.error.ok() && std::chrono::steady_clock::now() < deadline) {
            std::vector<std::string_view> periodic_providers;
            for (const auto& provider : impl_->providers)
                if (provider != "smtvv.tbcr" && provider != "unreal.table_definition" && provider != "unreal.message_definition")
                    periodic_providers.push_back(provider);

            if (!periodic_providers.empty()) merge(impl_->engine.reconcile(periodic_providers));
            physical_complete = true;
        }
        if (!object_target.empty() && report.error.ok() && std::chrono::steady_clock::now() < deadline) {
            if (refresh_object) impl_->engine.notify_target("unreal.object", object_target);
            constexpr std::array<std::string_view, 1> object_providers{"unreal.object"};
            merge(impl_->engine.reconcile(object_providers));
        }
        if (!definitions && physical_complete && report.error.ok()) impl_->dirty = false;
        definitions_complete = definitions && report.error.ok() && std::chrono::steady_clock::now() < deadline;

        if (!definitions) impl_->next_reconcile = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        impl_->telemetry.active_operations = report.active_operations;
        impl_->telemetry.journal_bytes = report.journal_bytes;
        impl_->telemetry.provider_calls = report.provider_calls;
        if (!definitions) impl_->reconcile_max_us = std::max(impl_->reconcile_max_us, report.elapsed_us);
        if (!definitions) {
            impl_->bind_max_us = std::max(impl_->bind_max_us, report.bind_us);
            impl_->prepare_max_us = std::max(impl_->prepare_max_us, report.prepare_us);
        }
        if (report.elapsed_us > 8000) impl_->log("API PERF-WARN reconcile_us=" + std::to_string(report.elapsed_us));
        const auto status_started = std::chrono::steady_clock::now();
        for (const auto& [id, request] : impl_->requests) {
            if (!request->core_id) continue;
            auto result = impl_->engine.status(request->core_id); if (!result) continue;
            const auto previous = request->summary.state;
            request->summary.observed = result->observed; request->summary.settled = result->settled;
            request->summary.operation_count = static_cast<uint32_t>(result->operations.size());
            request->summary.pending_count = static_cast<uint32_t>(result->pending_count);
            request->summary.rejected_count = static_cast<uint32_t>(result->rejected_count);
            request->operations.clear();
            for (const auto& operation : result->operations) {
                IF_Status copied = request->summary; copied.state = static_cast<uint32_t>(operation.state);
                copied.error = static_cast<uint32_t>(operation.error.code); copied.effective_owner = operation.effective_owner;
                copied.object_index = operation.identity.object_index; copied.object_serial = operation.identity.object_serial;
                copied.baseline_restore_skipped = operation.baseline_restore_skipped;
                copied.drift_detected = operation.drift_detected;
                text_copy(copied.message, operation.error.message); text_copy(copied.canonical_path, operation.canonical_path);
                request->operations.push_back(copied);
            }
            request->summary.error = static_cast<uint32_t>(result->error.code);
            text_copy(request->summary.message, result->error.message);
            if (request->operations.empty()) request->summary.state = result->settled ?
                (request->removal ? IF_REMOVED : IF_COMPLETED) : IF_REMOVING;
            else {
                request->summary.state = request->operations.front().state;
                for (const auto& operation : request->operations) if (operation.state == IF_REJECTED ||
                    operation.state == IF_FAULTED || operation.state == IF_RESTORE_FAILED || operation.state == IF_DRIFTED) {
                    request->summary.state = operation.state; request->summary.error = operation.error;
                    text_copy(request->summary.message, operation.message); break;
                }
                if (request->summary.error == IF_OK && !result->settled) {
                    for (const auto& operation : request->operations) if (operation.state == IF_WAITING_TARGET ||
                        operation.state == IF_WAITING_RUNTIME || operation.state == IF_REMOVING || operation.state == IF_PREPARED ||
                        operation.state == IF_ACCEPTED) {
                        request->summary.state = operation.state;
                        request->summary.error = operation.error;
                        text_copy(request->summary.message, operation.message); break;
                    }
                }
            }
            if (previous != request->summary.state) impl_->log("API request=" + std::to_string(id) +
                " state=" + std::to_string(request->summary.state) + " pending=" + std::to_string(result->pending_count));
        }
        if (!definitions) impl_->status_max_us = std::max(impl_->status_max_us,
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - status_started).count()));
    }
    if (definitions) return definitions_complete;
    impl_->update_properties(deadline);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    ++impl_->telemetry.callback_count; impl_->telemetry.last_callback_us = static_cast<uint64_t>(elapsed);
    impl_->telemetry.max_callback_us = std::max(impl_->telemetry.max_callback_us, static_cast<uint64_t>(elapsed));
    if (impl_->timing_count < impl_->timing_samples.size()) impl_->timing_samples[impl_->timing_count++] = static_cast<uint64_t>(elapsed);
    else ++impl_->timing_dropped;
    if (std::chrono::steady_clock::now() > deadline) ++impl_->telemetry.deadline_overruns;
    if (elapsed > 8000) impl_->log("API PERF-WARN callback_us=" + std::to_string(elapsed));
    return true;
}
}

extern "C" uint32_t IF_CALL Infinifusion_QueryApi(uint32_t version, uint32_t size, IF_Api* output) {
    if (!output || size != sizeof(IF_Api) || version != IF_API_VERSION) return IF_ABI_MISMATCH;
    auto* host = infinifusion::host::active_host.load();
    if (!host) return IF_NOT_READY;
    *output = host->api(); return IF_OK;
}
