class NativeObjectDiscovery {
public:
    struct Resolved { void* object{}; std::int32_t index{}, serial{}; void* klass{}; std::uint32_t flags{}; FName name{}; void* outer{}; };
    explicit NativeObjectDiscovery(Runtime& runtime) : runtime_(runtime) {}

    bool watch_target(std::string_view target) {
        if (target.empty() || (!targets_.contains(std::string(target)) && targets_.size()>=256)) return false;
        targets_.emplace(target);
        leaf_names_.emplace(target_leaf_name(std::string(target)));
        return true;
    }

    std::string requested_path(void* object) {
        if (targets_.empty() || !object || !object_has_target_leaf_name(runtime_,object,leaf_names_)) return {};
        const auto path=object_path(runtime_,object);
        if (!targets_.contains(path)) return {};
        return path;
    }

    std::string loaded(void* object,bool* changed=nullptr) {
        if (changed) *changed=false;
        const auto path=requested_path(object);
        if (path.empty()) return {};
        const auto identity=certify(object);
        if (!identity) return {};
        const auto old=known_.find(path);
        if (changed) *changed=old==known_.end() || old->second.object!=identity->object ||
            old->second.index!=identity->index || old->second.serial!=identity->serial || old->second.klass!=identity->klass;
        known_[path]=identity.value();
        return path;
    }
    void discover(std::chrono::steady_clock::time_point deadline) {
        if (targets_.empty() || known_.size() == targets_.size()) return;
        const auto registry = capture_object_registry(runtime_);
        if (!registry) return;
        if (cursor_ < 0 || cursor_ >= registry->header.num_elements) cursor_ = registry->header.num_elements - 1;
        while (cursor_ >= 0 && std::chrono::steady_clock::now() < deadline) {
            const auto index = cursor_--;
            auto* object = object_at(*registry, index);
            if (!object || !object_has_target_leaf_name(runtime_, object, leaf_names_)) continue;
            const auto path = object_path(runtime_, object);
            if (targets_.contains(path)) {
                const auto slot = item(index);
                if (slot && safe_read<void*>(slot).value_or(nullptr) == object)
                    known_[path] = {object, index, safe_read_at<std::int32_t>(slot, 0x10).value_or(0), nullptr};
            }
            if (known_.size() == targets_.size()) break;
        }
    }
    infinifusion::core::Result<Resolved> resolve(const infinifusion::core::ResourceLocator& locator) {
        using namespace infinifusion::core;
        if (!watch_target(locator.target))
            return make_error(ErrorCode::LimitExceeded, "provider target discovery limit (256)");
        const auto found = known_.find(locator.target);
        if (found == known_.end()) return make_error(ErrorCode::NotFound, "target not loaded/discovered");
        auto resolved = found->second;
        const auto slot = item(resolved.index);
        if (!slot || safe_read<void*>(slot).value_or(nullptr) != resolved.object) {
            known_.erase(found); return make_error(ErrorCode::TargetExpired, "object registry slot changed");
        }
        if (safe_read_at<std::uint32_t>(slot, 8).value_or(0xFFFFFFFF) & ((1u << 28) | (1u << 29)))
            return make_error(ErrorCode::NotReady, "object unreachable or pending kill");
        if (safe_read_at<std::int32_t>(slot, 0x10).value_or(-1) != resolved.serial ||
            object_path(runtime_, resolved.object) != locator.target) {
            known_.erase(found); return make_error(ErrorCode::NotFound, "discovered object identity changed");
        }
        resolved.klass = safe_read_at<void*>(resolved.object, 0x10).value_or(nullptr);
        if (!resolved.klass) return make_error(ErrorCode::NotReady, "object class unavailable");
        if (!locator.class_guard.empty() && object_path(runtime_, resolved.klass) != locator.class_guard)
            return make_error(ErrorCode::GuardMismatch, "class guard mismatch");
        return resolved;
    }

    void owned_serial_assigned(void* object) {
        const auto live=certify(object,true);
        if(!live) return;
        for(auto& [path,known]:known_) if(known.object==object && known.index==live->index &&
            known.serial==0 && object_path(runtime_,object)==path) known.serial=live->serial;
    }

    infinifusion::core::Result<Resolved> certify(void* object, bool require_serial = false) {
        const auto registry = capture_object_registry(runtime_);
        if (!registry) return infinifusion::core::make_error(infinifusion::core::ErrorCode::NotReady, "object registry unavailable");
        return certify(object, *registry, require_serial);
    }
    infinifusion::core::Result<Resolved> certify(void* object, const ObjectRegistryView& registry, bool require_serial = false) {
        using namespace infinifusion::core;
        if (registry.header.num_elements < 0 || registry.header.num_elements > registry.header.max_elements ||
            registry.header.num_chunks <= 0 || registry.header.num_chunks > registry.header.max_chunks ||
            registry.header.num_chunks >= 4096 || registry.chunks.size() != static_cast<std::size_t>(registry.header.num_chunks))
            return make_error(ErrorCode::NotReady, "object registry view invalid");
        const auto header = safe_read<ObjectHeader>(object);
        if (!header || header->index < 0 || header->index >= registry.header.num_elements)
            return make_error(ErrorCode::TargetExpired, "referenced object registry slot unavailable");
        const auto chunk_index = static_cast<std::size_t>(header->index / kElementsPerChunk);
        if (chunk_index >= registry.chunks.size() || !registry.chunks[chunk_index])
            return make_error(ErrorCode::TargetExpired, "referenced object registry chunk unavailable");
        const auto* address = static_cast<const std::uint8_t*>(registry.chunks[chunk_index]) + (header->index % kElementsPerChunk) * kObjectItemSize;
        const auto slot = safe_read<ObjectSlot>(address);
        if (!slot || slot->object != object)
            return make_error(ErrorCode::TargetExpired, "referenced object registry slot unavailable");
        if ((slot->flags & ((1u << 28) | (1u << 29) | (1u << 31) | (1u << 26))) ||
            (header->flags & (0x200u | 0x400u | 0x1000u | 0x2000u | 0x4000u | 0x8000u | 0x10000u | 0x20000u)))
            return make_error(ErrorCode::NotReady, "referenced object loading or pending GC");
        if (slot->serial < 0 || (require_serial && slot->serial == 0) || !header->klass)
            return make_error(ErrorCode::NotReady, "referenced object serial/class unavailable");
        return Resolved{object, header->index, slot->serial, header->klass, header->flags, header->name, header->outer};
    }
private:
    struct ObjectHeader { void* vtable{}; std::uint32_t flags{}; std::int32_t index{}; void* klass{}; FName name{}; void* outer{}; };
    struct ObjectSlot { void* object{}; std::uint32_t flags{}; std::int32_t cluster{}; std::int32_t serial{}; std::uint32_t padding{}; };
    static_assert(sizeof(ObjectHeader)==0x28 && offsetof(ObjectHeader,flags)==8 && offsetof(ObjectHeader,index)==0x0C &&
        offsetof(ObjectHeader,klass)==0x10 && offsetof(ObjectHeader,name)==0x18 && offsetof(ObjectHeader,outer)==0x20);
    static_assert(sizeof(ObjectSlot)==0x18 && offsetof(ObjectSlot,object)==0 && offsetof(ObjectSlot,flags)==8 && offsetof(ObjectSlot,serial)==0x10);
    const std::uint8_t* item(std::int32_t index) {
        const auto registry = capture_object_registry(runtime_);
        if (!registry || index < 0 || index >= registry->header.num_elements) return nullptr;
        const auto chunk = safe_read<void*>(registry->header.objects + index / kElementsPerChunk).value_or(nullptr);
        return chunk ? static_cast<const std::uint8_t*>(chunk) + (index % kElementsPerChunk) * kObjectItemSize : nullptr;
    }
    Runtime& runtime_;
    std::int32_t cursor_{-1};
    std::unordered_set<std::string> targets_, leaf_names_;
    std::unordered_map<std::string, Resolved> known_;
};
