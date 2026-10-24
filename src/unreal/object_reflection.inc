class NativeObjectReflection {
protected:
    using Error=infinifusion::core::Error;
    using Code=infinifusion::core::ErrorCode;
    struct ReadFact { const void* address{}; std::array<std::uint8_t,80> bytes{}; std::uint8_t size{}; };
    struct Snapshot {
        std::string json; std::vector<std::uintptr_t> trace; std::vector<std::string> paths;
        std::vector<ReadFact> reads;
        std::vector<NativeObjectDiscovery::Resolved> references;
        std::vector<std::pair<void*,PropertyInfo>> properties;
    };

    struct Observation : Snapshot {
        ObjectRegistryView registry;
        std::unordered_set<void*> certified;
        std::unordered_map<void*, std::size_t> outer_depths;
        std::unordered_map<void*, std::string> object_paths, object_names;
        std::unordered_map<void*, std::vector<void*>> hierarchies, property_lists;
        std::unordered_map<void*, std::string> field_names;
        std::unordered_set<void*> checked_fields;
        std::unordered_map<void*, PropertyInfo> metadata;
        std::unordered_map<const void*,std::size_t> read_indices;
    };
    Observation* recording_{};
    struct RecordScope {
        NativeObjectReflection& owner; Observation* previous;
        RecordScope(NativeObjectReflection& owner,Observation& out):owner(owner),previous(owner.recording_){owner.recording_=&out;}
        ~RecordScope(){owner.recording_=previous;}
    };
    static Error error(Code code, std::string message) { return infinifusion::core::make_error(code, std::move(message)); }
    [[noreturn]] static void fail(Code code, std::string message) { throw error(code, std::move(message)); }
    template<class T> T required(const void* base, std::ptrdiff_t offset) {
        static_assert(sizeof(T)<=80);
        const auto value = safe_read_at<T>(base, offset);
        if (!value) fail(Code::ReadFailed, "inspection reflected layout unreadable");
        if(recording_) {
            const auto* address=static_cast<const std::uint8_t*>(base)+offset;
            const auto [found,inserted]=recording_->read_indices.emplace(address,recording_->reads.size());
            if(inserted) {
                if(recording_->reads.size()>=16384) fail(Code::LimitExceeded,"object role source certificate limit");
                ReadFact fact;fact.address=address;fact.size=sizeof(T);std::memcpy(fact.bytes.data(),&*value,sizeof(T));
                recording_->reads.push_back(fact);
            } else {
                auto& fact=recording_->reads[found->second];
                if(std::memcmp(fact.bytes.data(),&*value,std::min<std::size_t>(fact.size,sizeof(T))))
                    fail(Code::LifetimeUnverifiable,"object role changed during observation");
                if(sizeof(T)>fact.size){fact.size=sizeof(T);std::memcpy(fact.bytes.data(),&*value,sizeof(T));}
            }
        }
        return *value;
    }
    static std::string quote(std::string_view input) {
        std::string out = "\"";
        for (const unsigned char ch : input) {
            if (ch == '"' || ch == '\\') { out += '\\'; out += static_cast<char>(ch); }
            else if (ch < 32) {
                constexpr char hex[] = "0123456789abcdef";
                out += "\\u00"; out += hex[ch >> 4]; out += hex[ch & 15];
            } else out += static_cast<char>(ch);
            if (out.size() > 4096) fail(Code::LimitExceeded, "inspection descriptor exceeds 4096 bytes");
        }
        return out + '"';
    }
    void reference(Observation& out, void* object) {
        if (out.certified.contains(object)) return;
        std::unordered_set<void*> outers;
        std::vector<void*> chain;
        std::size_t tail_depth{};
        for (auto* current = object; current;) {
            if (out.certified.contains(current)) {
                tail_depth = out.outer_depths.at(current);
                if (chain.size() + tail_depth > 128) fail(Code::LimitExceeded, "inspection outer chain cycle/limit");
                break;
            }
            if (outers.size() >= 128 || !outers.insert(current).second) fail(Code::LimitExceeded, "inspection outer chain cycle/limit");
            chain.push_back(current);
            const auto live = discovery_.certify(current, out.registry);
            if (!live) throw live.error();
            if(recording_==&out) out.references.push_back(live.value());
            const auto name = live->name;

            out.trace.insert(out.trace.end(), {reinterpret_cast<std::uintptr_t>(current), static_cast<std::uintptr_t>(live->index),
                static_cast<std::uintptr_t>(live->serial), reinterpret_cast<std::uintptr_t>(live->klass), live->flags,
                static_cast<std::uintptr_t>(name.comparison_index), static_cast<std::uintptr_t>(name.number),
                reinterpret_cast<std::uintptr_t>(live->outer)});
            current = live->outer;

        }
        out.certified.insert(outers.begin(), outers.end());
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) out.outer_depths.emplace(*it, ++tail_depth);
    }
    const std::string& object_path_cached(Observation& out, void* object) {
        reference(out, object);
        const auto old = out.object_paths.find(object);
        if (old != out.object_paths.end()) return old->second;
        auto text = object_path(runtime_, object);
        if (text.empty()) fail(Code::NotReady, "inspection object path unavailable");
        if (text.size() > 4096) fail(Code::LimitExceeded, "inspection object path exceeds bounds");
        out.paths.push_back(text);
        return out.object_paths.emplace(object, std::move(text)).first->second;
    }
    const std::string& object_name_cached(Observation& out, void* object) {
        reference(out, object);
        const auto old = out.object_names.find(object);
        if (old != out.object_names.end()) return old->second;
        return out.object_names.emplace(object, object_name(runtime_, object)).first->second;
    }
    std::string path(Observation& out, void* object) {
        return object ? quote(object_path_cached(out, object)) : "null";
    }
    const std::vector<void*>& hierarchy(Observation& out, void* klass) {
        const auto old = out.hierarchies.find(klass);
        if (old != out.hierarchies.end()) return old->second;
        const auto root = klass;
        std::vector<void*> result; std::unordered_set<void*> seen;
        while (klass) {
            if (result.size() >= 128 || !seen.insert(klass).second) fail(Code::LimitExceeded, "inspection class hierarchy cycle/limit");
            reference(out, klass); result.push_back(klass);
            klass = required<void*>(klass, 0x40);
        }
        return out.hierarchies.emplace(root, std::move(result)).first->second;
    }
    bool is_a(Observation& out, void* object, std::string_view qualified) {
        reference(out, object);
        for (auto* klass : hierarchy(out, required<void*>(object, 0x10)))
            if (object_path_cached(out, klass) == qualified) return true;
        return false;
    }
    std::vector<void*> functions(Observation& out,void* klass) {
        std::unordered_set<void*> seen;std::unordered_set<std::string> names;
        std::vector<void*> result;
        for(auto* current:hierarchy(out,klass)) {
            auto* child=required<void*>(current,0x48);
            while(child) {
                if(seen.size()>=4096 || !seen.insert(child).second)
                    fail(Code::LimitExceeded,"reflected UField function chain cycle/limit");
                reference(out,child);
                if(is_a(out,child,"/Script/CoreUObject.Function") && names.insert(object_name_cached(out,child)).second)
                    result.push_back(child);
                child=required<void*>(child,0x28);
            }
        }
        return result;
    }
    const PropertyInfo& metadata(Observation& out, void* property) {
        const auto old = out.metadata.find(property);
        if (old != out.metadata.end()) return old->second;
        if(recording_==&out) {

            const auto header=required<std::array<std::uint8_t,0x50>>(property,0);
            void* type{};std::memcpy(&type,header.data()+8,sizeof(type));
            if(type) required<FName>(type,0);
        }
        const auto& info=out.metadata.emplace(property, inspect_property(runtime_, property)).first->second;
        if(recording_==&out) out.properties.emplace_back(property,info);
        return info;
    }
    const std::vector<void*>& field_chain(Observation& out, void* structure) {
        const auto old = out.property_lists.find(structure);
        if (old != out.property_lists.end()) return old->second;
        const auto& classes = hierarchy(out, structure);
        std::vector<void*> result; std::unordered_set<void*> seen;
        for (auto* klass : classes) {
            auto* field = required<void*>(klass, 0x50);
            while (field) {
                if (result.size() >= 8192 || !seen.insert(field).second) fail(Code::LimitExceeded, "inspection FProperty chain cycle/limit");

                const auto header = required<std::array<std::uint8_t, 0x30>>(field, 0);
                void* next{}; FName field_name{};
                std::memcpy(&next, header.data()+0x20, sizeof(next));
                std::memcpy(&field_name, header.data()+0x28, sizeof(field_name));
                auto name = fname_to_string(runtime_, field_name);
                if (name.empty()) fail(Code::ReadFailed, "inspection FProperty name unavailable");
                out.field_names.emplace(field, std::move(name));
                out.trace.insert(out.trace.end(), {reinterpret_cast<std::uintptr_t>(field), reinterpret_cast<std::uintptr_t>(next),
                    static_cast<std::uintptr_t>(field_name.comparison_index), static_cast<std::uintptr_t>(field_name.number)});
                result.push_back(field); field = next;
            }
        }
        return out.property_lists.emplace(structure, std::move(result)).first->second;
    }
    void certify_field(Observation& out, void* field) {
        if (out.checked_fields.contains(field)) return;
        const auto& info = metadata(out, field);

        const auto header = required<std::array<std::uint8_t, 0x50>>(field, 0);
        std::int32_t dim{}, width{}, offset{}; std::uint64_t flags{}; void* klass{};
        std::memcpy(&dim, header.data()+0x38, sizeof(dim));
        std::memcpy(&width, header.data()+0x3C, sizeof(width));
        std::memcpy(&offset, header.data()+0x4C, sizeof(offset));
        std::memcpy(&flags, header.data()+0x40, sizeof(flags));
        std::memcpy(&klass, header.data()+8, sizeof(klass));
        if (info.name.empty() || info.type.empty() || dim <= 0 || width <= 0 || offset < 0 || dim > 65536 || width > 1048576)
            fail(Code::ReadFailed, "inspection FProperty layout incompatible");
        out.trace.insert(out.trace.end(), {reinterpret_cast<std::uintptr_t>(field), reinterpret_cast<std::uintptr_t>(klass),
            static_cast<std::uintptr_t>(dim), static_cast<std::uintptr_t>(width), static_cast<std::uintptr_t>(offset), static_cast<std::uintptr_t>(flags)});
        out.checked_fields.insert(field);
    }
    const std::vector<void*>& fields(Observation& out, void* structure) {
        const auto& result = field_chain(out, structure);
        for (auto* candidate : result) certify_field(out, candidate);
        return result;
    }
    void* field(Observation& out, void* structure, std::string_view name, bool optional = false) {
        for (auto* candidate : field_chain(out, structure)) if (out.field_names.at(candidate) == name) {
            certify_field(out, candidate); return candidate;
        }
        if (!optional) fail(Code::CapabilityUnavailable, "inspection reflected field unavailable: " + std::string(name));
        return nullptr;
    }
    void* object_field(Observation& out, void* object, std::string_view name, bool optional = false) {
        reference(out, object);
        auto* property = field(out, required<void*>(object, 0x10), name, optional);
        if (!property) return nullptr;
        const auto info = metadata(out, property);
        if ((info.type != "ObjectProperty" && info.type != "ClassProperty") || info.array_dim != 1 || info.element_size != sizeof(void*))
            fail(Code::TypeMismatch, "inspection reflected object field incompatible: " + std::string(name));
        auto* value = required<void*>(object, info.offset);
        if (value) reference(out, value);
        return value;
    }
    struct Array { void* data{}; std::int32_t count{}; void* inner{}; std::int32_t stride{}; };
    Array array(Observation& out, void* object, std::string_view name) {
        reference(out, object);
        auto* property = field(out, required<void*>(object, 0x10), name);
        const auto info = metadata(out, property);
        if (info.type != "ArrayProperty" || info.array_dim != 1 || info.element_size != 16)
            fail(Code::TypeMismatch, "inspection reflected array incompatible: " + std::string(name));
        const auto header = required<FStringHeader>(object, info.offset);
        auto* inner = required<void*>(property, 0x78);
        if (!inner || header.num < 0 || header.max < header.num || header.max > 4096 || (header.num && !header.data))
            fail(Code::ReadFailed, "inspection reflected array header invalid");
        const auto stride = required<std::int32_t>(inner, 0x3C);
        if (stride <= 0 || stride > 65536 || required<std::int32_t>(inner, 0x38) != 1)
            fail(Code::ReadFailed, "inspection reflected array inner invalid");
        out.trace.insert(out.trace.end(), {reinterpret_cast<std::uintptr_t>(header.data), static_cast<std::uintptr_t>(header.num),
            static_cast<std::uintptr_t>(header.max), reinterpret_cast<std::uintptr_t>(inner), static_cast<std::uintptr_t>(stride)});
        return {header.data, header.num, inner, stride};
    }
    static void* element(const Array& values, std::int32_t index) {
        return static_cast<std::uint8_t*>(values.data) + static_cast<std::size_t>(index) * values.stride;
    }
    std::string optional_scalar(Observation& out, void* object, std::string_view name) {
        auto* property = field(out, required<void*>(object, 0x10), name, true);
        if (!property) return "null";
        const auto info = metadata(out, property);
        if (info.type == "NameProperty" && info.array_dim == 1 && info.element_size == sizeof(FName))
            return quote(fname_to_string(runtime_, required<FName>(object, info.offset)));
        if (info.type == "StructProperty" && info.array_dim == 1 && info.element_size == 16) {
            auto* structure = required<void*>(property, 0x78); reference(out, structure);
            if (object_path_cached(out, structure) != "/Script/CoreUObject.Guid") fail(Code::TypeMismatch, "inspection GUID field has incompatible struct");
            std::string value = "[";
            for (int i = 0; i < 4; ++i) { if (i) value += ','; value += std::to_string(required<std::uint32_t>(object, info.offset + 4 * i)); }
            return value + ']';
        }
        fail(Code::TypeMismatch, "inspection optional field incompatible: " + std::string(name));
    }
    void* member(Observation& out, void* data, void* property, std::int32_t stride, std::string_view type) {

        const auto offset = required<std::int32_t>(property, 0x4C), width = required<std::int32_t>(property, 0x3C), dim = required<std::int32_t>(property, 0x38);
        if (offset < 0 || width <= 0 || dim != 1 || width > stride || offset > stride - width)
            fail(Code::ReadFailed, "inspection record member range invalid");
        const auto info = metadata(out, property);
        const bool pointer = type == "ObjectProperty" || type == "ClassProperty";
        if ((info.type != type && !(pointer && (info.type == "ObjectProperty" || info.type == "ClassProperty"))) ||
            (pointer && width != sizeof(void*)) || (type == "NameProperty" && width != sizeof(FName)))
            fail(Code::TypeMismatch, "inspection record member type incompatible");
        return static_cast<std::uint8_t*>(data) + offset;
    }
    void* class_default(Observation& out,void* klass) {
        reference(out,klass);
        if(!is_a(out,klass,"/Script/CoreUObject.Class") || !certified_class_default_layout(runtime_))
            fail(Code::CapabilityUnavailable,"class-default layout certificate unavailable");
        auto* object=required<void*>(klass,0x118);
        out.trace.push_back(reinterpret_cast<std::uintptr_t>(object));
        if(!object) fail(Code::NotReady,"class default object not constructed/loaded");
        reference(out,object);
        if(!(required<std::uint32_t>(object,8)&0x10u) || required<void*>(object,0x10)!=klass)
            fail(Code::GuardMismatch,"class default object identity mismatch");
        return object;
    }
    std::array<std::uint32_t,4> guid(Observation& out,void* base,void* property,std::int32_t stride) {
        const auto info=metadata(out,property);
        auto* address=member(out,base,property,stride,"StructProperty");
        auto* structure=required<void*>(property,0x78); reference(out,structure);
        if(info.element_size!=16 || object_path_cached(out,structure)!="/Script/CoreUObject.Guid")
            fail(Code::TypeMismatch,"component node GUID layout incompatible");
        auto result=required<std::array<std::uint32_t,4>>(address,0);
        for(auto word:result) out.trace.push_back(word);
        return result;
    }
    struct ComponentTarget { void* object{}; void* defining_class{}; void* node{}; void* override_class{}; void* cooked_data{}; void* cooked_type{}; };
    ComponentTarget component(Observation& out,void* actual_class,std::string_view variable) {

        const auto& classes=hierarchy(out,actual_class);
        for(std::size_t owner_index=0;owner_index<classes.size();++owner_index) {
            auto* owner=classes[owner_index];
            if(!is_a(out,owner,"/Script/Engine.BlueprintGeneratedClass")) continue;
            auto* scs=object_field(out,owner,"SimpleConstructionScript",true);
            if(!scs) continue;
            const auto nodes=array(out,scs,"AllNodes");
            if(metadata(out,nodes.inner).type!="ObjectProperty" || nodes.stride!=sizeof(void*))
                fail(Code::TypeMismatch,"SCS node array incompatible");
            for(int i=0;i<nodes.count;++i) {
                auto* node=required<void*>(element(nodes,i),0); reference(out,node);
                const auto node_name=optional_scalar(out,node,"InternalVariableName");
                if(node_name!=quote(variable)) continue;
                auto* guid_field=field(out,required<void*>(node,0x10),"VariableGuid");
                const auto node_size=required<std::int32_t>(required<void*>(node,0x10),0x58);
                const auto key=guid(out,node,guid_field,node_size);
                ComponentTarget result{object_field(out,node,"ComponentTemplate"),owner,node};
                auto* cooked_field=field(out,required<void*>(node,0x10),"CookedComponentInstancingData",true);
                if(cooked_field) {
                    result.cooked_data=member(out,node,cooked_field,node_size,"StructProperty");
                    result.cooked_type=required<void*>(cooked_field,0x78);
                }
                bool cooked_override{};
                for(std::size_t derived=0;derived<owner_index;++derived) {
                    auto* handler=object_field(out,classes[derived],"InheritableComponentHandler",true);
                    if(!handler) continue;
                    const auto records=array(out,handler,"Records");
                    if(metadata(out,records.inner).type!="StructProperty") fail(Code::TypeMismatch,"component override array incompatible");
                    auto* record_type=required<void*>(records.inner,0x78); reference(out,record_type);
                    auto* key_field=field(out,record_type,"ComponentKey");
                    auto* key_type=required<void*>(key_field,0x78); reference(out,key_type);
                    auto* owner_field=field(out,key_type,"OwnerClass");
                    auto* override_guid_field=field(out,key_type,"AssociatedGuid");
                    auto* template_field=field(out,record_type,"ComponentTemplate");
                    for(int record_index=0;record_index<records.count;++record_index) {
                        auto* record=element(records,record_index);
                        auto* record_key=member(out,record,key_field,records.stride,"StructProperty");
                        const auto key_size=metadata(out,key_field).element_size;
                        auto* key_owner=required<void*>(member(out,record_key,owner_field,key_size,"ClassProperty"),0);
                        reference(out,key_owner);
                        if(key_owner!=owner || guid(out,record_key,override_guid_field,key_size)!=key) continue;
                        auto* candidate=required<void*>(member(out,record,template_field,records.stride,"ObjectProperty"),0);
                        if(!result.override_class && candidate) { reference(out,candidate); result.object=candidate; result.override_class=classes[derived]; }

                        if(!cooked_override) {
                            auto* data_field=field(out,record_type,"CookedComponentInstancingData",true);
                            result.cooked_data=data_field?member(out,record,data_field,records.stride,"StructProperty"):nullptr;
                            result.cooked_type=data_field?required<void*>(data_field,0x78):nullptr;
                            cooked_override=true;
                        }
                        break;
                    }
                }
                if(!result.object || !is_a(out,result.object,"/Script/Engine.ActorComponent"))
                    fail(Code::GuardMismatch,"effective SCS component template unavailable/incompatible");
                return result;
            }
        }
        for(auto* owner:classes) {
            if(!is_a(out,owner,"/Script/Engine.BlueprintGeneratedClass")) continue;
            const auto templates=array(out,owner,"ComponentTemplates");
            if(metadata(out,templates.inner).type!="ObjectProperty" || templates.stride!=sizeof(void*))
                fail(Code::TypeMismatch,"AddComponent template array incompatible");
            for(int i=0;i<templates.count;++i) {
                auto* candidate=required<void*>(element(templates,i),0); reference(out,candidate);
                if(object_name_cached(out,candidate)!=variable) continue;
                if(!is_a(out,candidate,"/Script/Engine.ActorComponent"))
                    fail(Code::GuardMismatch,"AddComponent template class incompatible");
                return {candidate,owner};
            }
        }
        auto* defaults=class_default(out,actual_class);
        auto* native=object_field(out,defaults,variable,true);
        if(!native || !is_a(out,native,"/Script/Engine.ActorComponent") ||
            !(required<std::uint32_t>(native,8)&0x40000u))
            fail(Code::PathNotFound,"selected component not found in SCS or native defaults");
        return {native,actual_class};
    }
    Runtime& runtime_; NativeObjectDiscovery& discovery_;
    NativeObjectReflection(Runtime& runtime,NativeObjectDiscovery& discovery):runtime_(runtime),discovery_(discovery) {}
};
