class NativeObjectInspectionProvider final : public infinifusion::core::Provider, private NativeObjectReflection {
    using Error = infinifusion::core::Error;
    using Code = infinifusion::core::ErrorCode;
    using Region = infinifusion::core::Region;
    using Locator = infinifusion::core::ResourceLocator;
    struct Binding { Locator locator; infinifusion::core::PropertyPath query; Snapshot snapshot; infinifusion::core::TargetIdentity identity; };
public:
    NativeObjectInspectionProvider(Runtime& runtime, NativeObjectDiscovery& discovery) : NativeObjectReflection(runtime,discovery) {}
    std::string_view id() const noexcept override { return "unreal.inspection"; }
    void frame(std::uint64_t epoch) { frame_ = epoch; bindings_.clear(); }
    infinifusion::core::Result<Region> bind(const Locator& locator, const infinifusion::core::PropertyPath& query) override {
        using namespace infinifusion::core;
        if (locator.provider != id() || !locator.row.empty()) return make_error(Code::InvalidArgument, "inspection requires an object target");
        const auto resolved = discovery_.resolve(locator);
        if (!resolved) return resolved.error();
        if (bindings_.size() >= 2048) return make_error(Code::LimitExceeded, "inspection callback binding limit (2048)");
        try {
            const auto root = discovery_.certify(resolved->object);
            if (!root) return root.error();
            if (root->index != resolved->index || root->serial != resolved->serial || root->klass != resolved->klass)
                return error(Code::LifetimeUnverifiable, "inspection root registry/index mismatch");
            auto snapshot = observe(resolved->object, query);
            Region region;
            region.identity = {frame_, static_cast<std::uint64_t>(resolved->index), static_cast<std::uint64_t>(std::max(0, resolved->serial)), 0};
            region.identity_tracking = IdentityTracking::ObservedOnly;
            region.storage_key = std::to_string(++sequence_);
            region.canonical_path = query.string();
            region.whole_value = true; region.restorable = false;
            region.byte_mask.assign(snapshot.json.size(), 0xFF);
            bindings_.emplace(region.storage_key, Binding{locator, query, std::move(snapshot), region.identity});
            return region;
        } catch (const Error& error) { return error; }
    }
    Error validate(const Region& region) override {
        const auto found = bindings_.find(region.storage_key);
        if (region.identity.epoch != frame_ || found == bindings_.end()) return error(Code::TargetExpired, "inspection callback lease ended");
        const auto& binding = found->second;
        if (region.identity != binding.identity || !region.whole_value || region.restorable || region.offset ||
            region.canonical_path != binding.query.string() || region.byte_mask != std::vector<std::uint8_t>(binding.snapshot.json.size(), 0xFF))
            return error(Code::LifetimeUnverifiable, "inspection binding certificate changed");
        const auto resolved = discovery_.resolve(binding.locator);
        if (!resolved) return resolved.error();
        if (resolved->index != static_cast<std::int32_t>(region.identity.object_index) ||
            resolved->serial != static_cast<std::int32_t>(region.identity.object_serial)) return error(Code::TargetExpired, "inspection root identity changed");
        try {
            const auto current = observe(resolved->object, binding.query);
            if (current.trace != binding.snapshot.trace || current.paths != binding.snapshot.paths || current.json != binding.snapshot.json)
                return error(Code::LifetimeUnverifiable, "inspection reference/layout changed within callback");
            return {};
        } catch (const Error& failure) { return failure; }
    }
    infinifusion::core::Result<std::vector<std::uint8_t>> read(const Region& region) override {
        const auto result = validate(region); if (!result.ok()) return result;
        const auto& json = bindings_.at(region.storage_key).snapshot.json;
        return std::vector<std::uint8_t>(json.begin(), json.end());
    }
    infinifusion::core::Result<infinifusion::core::Value> decode(const Region& region, std::span<const std::uint8_t> bytes) override {
        const auto found = bindings_.find(region.storage_key);
        if (region.identity.epoch != frame_ || found == bindings_.end()) return error(Code::TargetExpired, "inspection callback lease ended");
        if (bytes.size() != found->second.snapshot.json.size() || bytes.size() > 4096 ||
            !std::equal(bytes.begin(), bytes.end(), found->second.snapshot.json.begin(), [](std::uint8_t a, char b) { return a == static_cast<std::uint8_t>(b); })) return error(Code::ReadFailed, "inspection copied descriptor mismatch");
        return infinifusion::core::Value::string(std::string(bytes.begin(), bytes.end()));
    }
    infinifusion::core::Result<infinifusion::core::EncodedValue> prepare(const Region&, const infinifusion::core::Value&) override {
        return error(Code::CapabilityUnavailable, "inspection is read-only");
    }
    Error write(const Region&, std::span<const std::uint8_t>) override { return error(Code::CapabilityUnavailable, "inspection is read-only"); }
private:
    Snapshot observe(void* object, const infinifusion::core::PropertyPath& query) {
        using Kind = infinifusion::core::PathSegment::Kind;
        if (query.segments.empty() || query.segments[0].kind != Kind::Field) fail(Code::InvalidArgument, "inspection query missing");
        const auto& name = query.segments[0].field;
        const bool describe = name == "Describe";
        const bool singleton = describe || name == "DefaultObject";
        const bool named = name == "Property" || name == "Function" || name == "EffectiveComponent";
        if ((singleton && query.segments.size() != 1) || (!singleton && (query.segments.size() != 2 || query.segments[1].kind != (named ? Kind::Field : Kind::Index))))
            fail(Code::InvalidArgument, "inspection query requires one explicit index");
        auto index = singleton || named ? 0u : query.segments[1].index;
        const auto selection = named ? query.segments[1].field : std::string{};
        Observation out;
        const auto registry = capture_object_registry(runtime_);
        if (!registry) fail(Code::NotReady, "inspection object registry unavailable");
        out.registry = *registry;
        reference(out, object);
        const bool class_target = is_a(out, object, "/Script/CoreUObject.Class");
        auto* effective = class_target ? object : required<void*>(object, 0x10);
        auto classes = hierarchy(out, effective);
        out.json = "{\"query\":" + quote(name) + ",\"index\":" + std::to_string(index);
        if (named) out.json += ",\"selector\":" + quote(selection);
        if (describe) {
            const auto flags = required<std::uint32_t>(object, 8);
            out.json += ",\"object_path\":" + path(out, object) + ",\"class\":" + path(out, required<void*>(object, 0x10)) +
                ",\"super\":" + path(out, classes.size() > 1 ? classes[1] : nullptr) + ",\"flags\":" + std::to_string(flags) +
                ",\"class_default_object\":" + (flags & 0x10u ? "true" : "false") + ",\"archetype\":" + (flags & 0x20u ? "true" : "false") +
                ",\"default_subobject\":" + (flags & 0x40000u ? "true" : "false") + ",\"is_class\":" + (class_target ? "true" : "false") +
                ",\"is_blueprint_generated_class\":" + (is_a(out, object, "/Script/Engine.BlueprintGeneratedClass") ? "true" : "false") +
                ",\"cdo_resolution\":\"existing_only\",\"native_default_subobjects\":\"pending\"";
        } else if(name=="DefaultObject") {
            auto* defaults=class_default(out,effective);
            out.json+=",\"object_path\":"+path(out,defaults)+",\"class\":"+path(out,effective)+",\"class_default_object\":true";
        } else if(name=="EffectiveComponent") {
            const auto selected=component(out,effective,selection);
            out.json+=",\"template\":"+path(out,selected.object)+",\"component_class\":"+path(out,required<void*>(selected.object,0x10))+
                ",\"defining_class\":"+path(out,selected.defining_class)+",\"node\":"+path(out,selected.node)+
                ",\"override_class\":"+path(out,selected.override_class)+",\"effective_resolution\":\"resolved\"";
            if(selected.cooked_data && selected.cooked_type) {
                reference(out,selected.cooked_type);
                auto* property=field(out,selected.cooked_type,"bHasValidCookedData");
                const auto info=metadata(out,property);
                const auto size=required<std::int32_t>(selected.cooked_type,0x58);
                if(info.type!="BoolProperty" || info.array_dim!=1 || info.offset<0 || info.offset>=size)
                    fail(Code::TypeMismatch,"cooked component validity field incompatible");
                const auto byte=required<std::uint8_t>(property,0x79),mask=required<std::uint8_t>(property,0x7B);
                if(!mask || info.offset+byte>=size) fail(Code::TypeMismatch,"cooked validity bit field incompatible");
                const auto bits=required<std::uint8_t>(selected.cooked_data,info.offset+byte);
                out.trace.insert(out.trace.end(),{reinterpret_cast<std::uintptr_t>(selected.cooked_data),byte,mask,bits});
                out.json+=std::string(",\"has_valid_cooked_data\":")+((bits&mask)?"true":"false");
            } else out.json+=",\"has_valid_cooked_data\":null";
        } else if (name == "Parents") {
            if (index >= classes.size()) fail(Code::OutOfRange, "inspection parent index out of range");
            out.json += ",\"class\":" + path(out, classes[index]);
        } else if (name == "Properties" || name == "Property") {
            auto* structure = is_a(out, object, "/Script/CoreUObject.Struct") ? object : effective;
            const auto all = fields(out, structure);
            if (named) {
                const auto found = std::find_if(all.begin(), all.end(), [&](void* value) { return infinifusion::unreal::reflected_name_equal(metadata(out, value).name, selection); });
                if (found == all.end()) fail(Code::PathNotFound, "inspection named property unavailable");
                index = static_cast<std::uint32_t>(found - all.begin());
                out.json += ",\"resolved_index\":" + std::to_string(index);
            }
            if (index >= all.size()) fail(Code::OutOfRange, "inspection property index out of range");
            const auto info = metadata(out, all[index]);
            out.json += ",\"name\":" + quote(info.name) + ",\"type\":" + quote(info.type) + ",\"width\":" + std::to_string(info.element_size) +
                ",\"array_dim\":" + std::to_string(info.array_dim) + ",\"flags\":" + std::to_string(info.flags) +
                ",\"parameter\":" + (info.flags & 0x80u ? "true" : "false") + ",\"out_parameter\":" + (info.flags & 0x100u ? "true" : "false") +
                ",\"return_parameter\":" + (info.flags & 0x400u ? "true" : "false");
        } else if (name == "Functions" || name == "Function") {
            const auto functions=this->functions(out,effective);
            if (named) {
                const auto found = std::find_if(functions.begin(), functions.end(), [&](void* value) { return infinifusion::unreal::reflected_name_equal(object_name_cached(out, value), selection); });
                if (found == functions.end()) fail(Code::PathNotFound, "inspection named function unavailable");
                index = static_cast<std::uint32_t>(found - functions.begin());
                out.json += ",\"resolved_index\":" + std::to_string(index);
            }
            if (index >= functions.size()) fail(Code::OutOfRange, "inspection function index out of range");
            auto* function = functions[index];
            out.json += ",\"name\":" + quote(object_name_cached(out, function)) + ",\"path\":" + path(out, function) + ",\"owner\":" + path(out, required<void*>(function, 0x20));
            out.json += ",\"flags\":"+std::to_string(required<std::uint32_t>(function,0xB0))+
                ",\"parameter_bytes\":"+std::to_string(required<std::uint16_t>(function,0xB6))+
                ",\"parameters\":[";
            bool first=true;
            for(auto* property:fields(out,function)) {
                const auto info=metadata(out,property);
                if(!(info.flags&0x80))continue;
                if(!first)out.json+=',';first=false;
                out.json+="{\"name\":"+quote(info.name)+",\"type\":"+quote(info.type)+
                    ",\"flags\":"+std::to_string(info.flags)+",\"offset\":"+std::to_string(info.offset)+
                    ",\"width\":"+std::to_string(info.element_size)+",\"array_dim\":"+std::to_string(info.array_dim)+"}";
                if(out.json.size()>3900)fail(Code::LimitExceeded,"function descriptor exceeds copied result bound");
            }
            out.json+=']';
        } else if (name == "Components" || name == "Overrides") {
            std::vector<std::string> items;
            std::uint32_t ordinal{};
            for (auto* klass : classes) {
                if (!is_a(out, klass, "/Script/Engine.BlueprintGeneratedClass")) continue;
                if (name == "Components") {
                    auto* scs = object_field(out, klass, "SimpleConstructionScript");
                    if (scs) {
                        const auto nodes = array(out, scs, "AllNodes");
                        if (metadata(out, nodes.inner).type != "ObjectProperty" || nodes.stride != sizeof(void*)) fail(Code::TypeMismatch, "inspection SCS nodes inner incompatible");
                        for (int i = 0; i < nodes.count; ++i) {
                            if (ordinal >= 4096) fail(Code::LimitExceeded, "inspection component inventory limit");
                            if (ordinal++ != index) continue;
                            auto* node = required<void*>(element(nodes, i), 0); reference(out, node);
                            items.push_back("\"source\":\"scs_node\",\"origin_class\":" + path(out, klass) + ",\"node\":" + path(out, node) +
                                ",\"component_class\":" + path(out, object_field(out, node, "ComponentClass")) + ",\"template\":" + path(out, object_field(out, node, "ComponentTemplate")) +
                                ",\"variable_name\":" + optional_scalar(out, node, "InternalVariableName") + ",\"parent_name\":" + optional_scalar(out, node, "ParentComponentOrVariableName") +
                                ",\"parent_owner_class_name\":" + optional_scalar(out, node, "ParentComponentOwnerClassName") + ",\"variable_guid\":" + optional_scalar(out, node, "VariableGuid"));
                            break;
                        }
                    }
                    if (!items.empty()) break;
                    const auto templates = array(out, klass, "ComponentTemplates");
                    if (metadata(out, templates.inner).type != "ObjectProperty" || templates.stride != sizeof(void*)) fail(Code::TypeMismatch, "inspection component templates inner incompatible");
                    for (int i = 0; i < templates.count; ++i) {
                        if (ordinal >= 4096) fail(Code::LimitExceeded, "inspection component inventory limit");
                        if (ordinal++ != index) continue;
                        auto* component = required<void*>(element(templates, i), 0); reference(out, component);
                        items.push_back("\"source\":\"add_component_template\",\"origin_class\":" + path(out, klass) + ",\"template\":" + path(out, component) + ",\"component_class\":" + path(out, required<void*>(component, 0x10)));
                        break;
                    }
                } else {
                    auto* handler = object_field(out, klass, "InheritableComponentHandler");
                    if (!handler) continue;
                    const auto records = array(out, handler, "Records");
                    if (metadata(out, records.inner).type != "StructProperty") fail(Code::TypeMismatch, "inspection override records inner incompatible");
                    auto* record_struct = required<void*>(records.inner, 0x78); reference(out, record_struct);
                    auto* template_field = field(out, record_struct, "ComponentTemplate");
                    auto* class_field = field(out, record_struct, "ComponentClass");
                    auto* key_field = field(out, record_struct, "ComponentKey");
                    const auto key_info = metadata(out, key_field);
                    if (key_info.type != "StructProperty" || key_info.array_dim != 1) fail(Code::TypeMismatch, "inspection override key incompatible");
                    auto* key_struct = required<void*>(key_field, 0x78); reference(out, key_struct);
                    auto* owner_field = field(out, key_struct, "OwnerClass");
                    auto* variable_field = field(out, key_struct, "SCSVariableName");
                    auto* guid_field = field(out, key_struct, "AssociatedGuid");
                    for (int i = 0; i < records.count; ++i) {
                        if (ordinal >= 4096) fail(Code::LimitExceeded, "inspection override inventory limit");
                        if (ordinal++ != index) continue;
                        auto* data = element(records, i);
                        auto* key = member(out, data, key_field, records.stride, "StructProperty");
                        const auto key_size = metadata(out, key_field).element_size;
                        auto* component = required<void*>(member(out, data, template_field, records.stride, "ObjectProperty"), 0);
                        auto* component_class = required<void*>(member(out, data, class_field, records.stride, "ClassProperty"), 0);
                        auto* owner = required<void*>(member(out, key, owner_field, key_size, "ClassProperty"), 0);
                        const auto variable = required<FName>(member(out, key, variable_field, key_size, "NameProperty"), 0);
                        auto* guid = member(out, key, guid_field, key_size, "StructProperty");
                        const auto guid_info = metadata(out, guid_field);
                        reference(out, required<void*>(guid_field, 0x78));
                        if (guid_info.element_size != 16 || object_path_cached(out, required<void*>(guid_field, 0x78)) != "/Script/CoreUObject.Guid") fail(Code::TypeMismatch, "inspection override GUID incompatible");
                        std::string guid_json = "[";
                        for (int n = 0; n < 4; ++n) { if (n) guid_json += ','; guid_json += std::to_string(required<std::uint32_t>(guid, 4*n)); }
                        items.push_back("\"source\":\"inherited_override_record\",\"origin_class\":" + path(out, klass) + ",\"handler\":" + path(out, handler) +
                            ",\"owner_class\":" + path(out, owner) + ",\"variable_name\":" + quote(fname_to_string(runtime_, variable)) + ",\"variable_guid\":" + guid_json + ']' +
                            ",\"template\":" + path(out, component) + ",\"component_class\":" + path(out, component_class) + ",\"effective_resolution\":\"not_computed\"");
                        break;
                    }
                }
                if (!items.empty()) break;
            }
            if (items.empty()) fail(Code::OutOfRange, "inspection component/override index out of range");
            out.json += ',' + items.front();
        } else fail(Code::InvalidArgument, "unknown inspection query");
        out.json += '}';
        if (out.json.size() > 4096) fail(Code::LimitExceeded, "inspection descriptor exceeds 4096 bytes");
        return out;
    }

    std::uint64_t frame_{1}, sequence_{};
    std::unordered_map<std::string, Binding> bindings_;
};
