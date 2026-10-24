class NativeGameServices final : public infinifusion::host::GameServices, private NativeObjectReflection {
    NativeObjectTargets& targets_;
    DWORD thread_{};
    EngineAllocator allocator_;
    ManagedValueBridge bridge_;
    static constexpr std::uintptr_t process_event_rva=0x10B3830;
    static constexpr std::array<std::uint8_t,32> process_event_prefix{
        0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xEC,0xF0,
        0x00,0x00,0x00,0x48,0x8D,0x6C,0x24,0x30,0x48,0x89,0x9D,0x18,0x01,0x00,0x00,0x48};
    static constexpr std::uintptr_t actor_process_event_rva=0x1FFEB60;
    static constexpr std::array<std::uint8_t,32> actor_process_event_prefix{
        0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x57,0x48,0x83,0xEC,0x20,0xF7,
        0x82,0xB0,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x49,0x8B,0xE8,0x48,0x8B,0xFA,0x48};
    using ProcessEvent=void(__fastcall*)(void*,void*,void*);
    static bool dispatch(ProcessEvent entry,void* object,void* function,void* data) noexcept {
        __try {entry(object,function,data);return true;}
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    ProcessEvent certify_dispatch(void* object) {
        std::array<std::uint8_t,process_event_prefix.size()> code{};
        if(runtime_.exe_timestamp!=kKnownExeTimestamp || runtime_.image_size!=kKnownImageSize ||
           !safe_copy(runtime_.image_base+process_event_rva,code.data(),code.size()) || code!=process_event_prefix)
            fail(Code::CapabilityUnavailable,"ProcessEvent executable certificate unavailable");
        const auto table=required<void*>(object,0);
        const auto entry=required<void*>(table,68*sizeof(void*));
        if(entry!=runtime_.image_base+process_event_rva) {
            std::array<std::uint8_t,actor_process_event_prefix.size()> actor_code{};
            if(entry!=runtime_.image_base+actor_process_event_rva || !is_asset_class(runtime_,object,"/Script/Engine.Actor") ||
               !safe_copy(entry,actor_code.data(),actor_code.size()) || actor_code!=actor_process_event_prefix)
                fail(Code::CapabilityUnavailable,"target overrides ProcessEvent; dispatch not certified");

        }
        return reinterpret_cast<ProcessEvent>(entry);
    }
    Observation observation() {
        if(GetCurrentThreadId()!=thread_)fail(Code::NotReady,"game services require the certified game thread");
        const auto registry=capture_object_registry(runtime_);
        if(!registry)fail(Code::NotReady,"object registry unavailable");
        Observation out;out.registry=*registry;return out;
    }
public:
    NativeGameServices(Runtime& runtime,NativeObjectDiscovery& discovery,NativeObjectTargets& targets,DWORD thread):
        NativeObjectReflection(runtime,discovery),targets_(targets),thread_(thread),allocator_(runtime),bridge_(runtime,allocator_){}
    infinifusion::core::Result<std::string> search(const infinifusion::host::ObjectQuery& query,
        std::chrono::steady_clock::time_point deadline) override {
        try {
            auto out=observation();
            const auto count=static_cast<std::uint32_t>(out.registry.header.num_elements);
            if(query.start_index>count)fail(Code::OutOfRange,"object cursor beyond current registry");
            auto cursor=query.start_index;
            const auto stop=std::min<std::uint64_t>(count,static_cast<std::uint64_t>(cursor)+query.scan_limit);
            std::string items;std::uint32_t matched{};
            while(cursor<stop && matched<query.result_limit && std::chrono::steady_clock::now()<deadline) {
                auto* object=object_at(out.registry,static_cast<std::int32_t>(cursor));
                ++cursor;if(!object)continue;
                const auto live=discovery_.certify(object,out.registry);if(!live)continue;
                if(!(query.flags&IF_OBJECT_QUERY_INCLUDE_DEFAULTS) && (live->flags&(0x10u|0x20u)))continue;

                Observation candidate;candidate.registry=out.registry;
                if(!query.class_path.empty() && !is_a(candidate,object,query.class_path))continue;
                const auto path=object_path_cached(candidate,object);
                if(!query.path_contains.empty() && path.find(query.path_contains)==std::string::npos)continue;
                const auto klass=object_path_cached(candidate,live->klass);
                const auto item="{\"path\":"+quote(path)+",\"class\":"+quote(klass)+",\"object_index\":"+
                    std::to_string(live->index)+",\"object_serial\":"+std::to_string(live->serial)+"}";
                if(items.size()+item.size()>3700){--cursor;break;}
                if(matched++)items+=',';items+=item;
            }
            return "{\"items\":["+items+"],\"next_index\":"+std::to_string(cursor)+
                ",\"registry_count\":"+std::to_string(count)+",\"done\":"+(cursor>=count?"true":"false")+"}";
        }catch(const Error& error){return error;}
        catch(const std::exception& error){return infinifusion::core::make_error(Code::ReadFailed,error.what());}
    }
    infinifusion::host::InvokeOutcome invoke(const infinifusion::host::FunctionCall& call,
        std::chrono::steady_clock::time_point deadline) override {
        infinifusion::host::InvokeOutcome outcome;
        ManagedValueBridge::ParameterFrame frame;
        struct Condition {
            std::string path;
            ResolvedLeaf selected;
            PropertyInfo info;
            bool single_element{};
            ManagedValueBridge::Scratch expected;
        };
        std::vector<Condition> conditions;
        const auto quarantine=[&] {frame.quarantine();for(auto& condition:conditions)condition.expected.quarantine();};
        try {
            auto out=observation();
            if(std::chrono::steady_clock::now()>=deadline)fail(Code::NotReady,"invocation callback deadline elapsed before preparation");
            if(call.flags&IF_INVOKE_EXPECT_IDENTITY) {
                if(call.expected_object_index>=static_cast<std::uint64_t>(out.registry.header.num_elements))
                    fail(Code::TargetExpired,"expected target index outside registry");
                auto* candidate=object_at(out.registry,static_cast<std::int32_t>(call.expected_object_index));
                const auto live=discovery_.certify(candidate,out.registry);
                if(live && object_path_cached(out,candidate)==call.target.target) {
                    discovery_.watch_target(call.target.target);discovery_.loaded(candidate);
                }
            }
            const auto selected=targets_.select(call.target);if(!selected)throw selected.error();
            const auto& identity=selected->identity;
            outcome.object_index=identity.index;outcome.object_serial=identity.serial;
            if((call.flags&IF_INVOKE_EXPECT_IDENTITY) &&
               (call.expected_object_index!=outcome.object_index || call.expected_object_serial!=outcome.object_serial))
                fail(Code::TargetExpired,"invocation target identity differs from requested observation");
            auto* object=identity.object;
            if(is_a(out,object,"/Script/CoreUObject.Class"))
                fail(Code::TypeMismatch,"invoke an object or existing class defaults, not the UClass itself");
            void* function{};
            for(auto* candidate:functions(out,identity.klass))
                if(infinifusion::unreal::reflected_name_equal(object_name_cached(out,candidate),call.function)){function=candidate;break;}
            if(!function)fail(Code::PathNotFound,"reflected function not found on target class");
            const auto flags=required<std::uint32_t>(function,0xB0);
            if(flags&(0x40u|0x10000u|0x100000u))
                fail(Code::CapabilityUnavailable,"network/delegate dispatch requires a distinct supported contract");
            if((identity.flags&0x10u) && !(flags&0x2000u))
                fail(Code::InvalidArgument,"instance function cannot execute on class defaults");
            const auto parameter_bytes=required<std::uint16_t>(function,0xB6);
            const auto num_parameters=required<std::uint8_t>(function,0xB4);
            const auto return_offset=required<std::uint16_t>(function,0xB8);
            const auto parsed=smtvv::json::parse(call.arguments);
            if(!parsed.value || !parsed.value->object())fail(Code::InvalidArgument,"arguments require an object of import strings");
            std::map<std::string,std::string> arguments;
            for(const auto& [name,value]:*parsed.value->object()) {
                if(!value.string()||!arguments.emplace(name,*value.string()).second)
                    fail(Code::InvalidArgument,"duplicate/non-string function argument");
            }
            std::vector<std::pair<void*,PropertyInfo>> parameters;
            std::unordered_set<void*> seen;
            auto* property=required<void*>(function,0x50);
            bool found_return{};
            while(property) {
                if(seen.size()>=512||!seen.insert(property).second)fail(Code::LimitExceeded,"function property chain cycle/limit");
                certify_field(out,property);const auto info=metadata(out,property);
                if(info.flags&0x80u) {
                    if(static_cast<std::uint64_t>(info.offset)+static_cast<std::uint64_t>(info.element_size)*info.array_dim>parameter_bytes)
                        fail(Code::TypeMismatch,"parameter exceeds certified frame bounds");
                    if(info.flags&0x400u){found_return=true;if(info.offset!=return_offset)fail(Code::TypeMismatch,"return parameter offset mismatch");}
                    parameters.emplace_back(property,info);
                }
                property=required<void*>(property,0x20);
            }
            if(parameters.size()!=num_parameters || found_return!=(return_offset!=0xFFFF))
                fail(Code::TypeMismatch,"function parameter metadata inconsistent");
            const auto function_identity=discovery_.certify(function,out.registry);
            if(!function_identity)throw function_identity.error();
            auto entry=certify_dispatch(object);
            if(parameter_bytes) {
                std::vector<void*> parameter_fields;
                for(const auto& parameter:parameters)parameter_fields.push_back(parameter.first);
                frame=bridge_.parameter_frame(parameter_bytes,parameter_fields);
            }
            for(const auto& [field,info]:parameters) {
                const bool returned=bool(info.flags&0x400u);
                const bool input=!returned && (!(info.flags&0x100u) || (info.flags&0x8000000u) || (info.flags&2u));
                const auto arg=arguments.find(info.name);
                if(input && arg==arguments.end())fail(Code::InvalidArgument,"missing function input: "+info.name);
                if(!input && arg!=arguments.end())fail(Code::InvalidArgument,"output-only function argument: "+info.name);
                if(input) {bridge_.import(field,arg->second,static_cast<std::uint8_t*>(frame.data())+info.offset,object);arguments.erase(arg);}
            }
            if(!arguments.empty())fail(Code::PathNotFound,"unknown function argument: "+arguments.begin()->first);

            if(!call.expected_properties.empty()) {
                const auto expected=smtvv::json::parse(call.expected_properties);
                if(!expected.value || !expected.value->object() || expected.value->object()->size()>8)
                    fail(Code::InvalidArgument,"expected properties require at most8 import expressions");
                conditions.reserve(expected.value->object()->size());
                for(const auto& [path,value]:*expected.value->object()) {
                    if(path.empty() || !value.string())fail(Code::InvalidArgument,"expected property requires a path and import string");
                    std::string why;
                    auto leaf=resolve_leaf(runtime_,identity.klass,object,path,why,true,true);
                    if(!leaf)fail(Code::PathNotFound,"invocation condition: "+why);
                    conditions.push_back({path,*leaf,inspect_property(runtime_,leaf->property),path.back()==']',{}});
                    auto& condition=conditions.back();
                    condition.expected=bridge_.scratch_property(leaf->property);
                    bridge_.import(leaf->property,*value.string(),condition.expected.data(),object,condition.single_element);
                }
            }

            if(const auto current=targets_.validate(call.target,selected.value());!current.ok())throw current;
            const auto before_registry=capture_object_registry(runtime_);
            if(!before_registry){quarantine();fail(Code::LifetimeUnverifiable,"function registry unavailable before dispatch");}
            const auto live_function=discovery_.certify(function,*before_registry);
            if(!live_function || live_function->index!=function_identity->index || live_function->serial!=function_identity->serial ||
               required<std::uint32_t>(function,0xB0)!=flags || required<std::uint16_t>(function,0xB6)!=parameter_bytes ||
               required<std::uint8_t>(function,0xB4)!=num_parameters || required<std::uint16_t>(function,0xB8)!=return_offset)
                {quarantine();fail(Code::LifetimeUnverifiable,"function layout/identity changed before dispatch");}
            for(const auto& [field,info]:parameters) {
                const auto current=inspect_property(runtime_,field);
                if(current.name!=info.name || current.type!=info.type || current.offset!=info.offset || current.flags!=info.flags ||
                   current.element_size!=info.element_size || current.array_dim!=info.array_dim)
                    {quarantine();fail(Code::LifetimeUnverifiable,"parameter schema changed before dispatch");}
            }

            for(auto& condition:conditions) {
                std::string why;
                auto current=resolve_leaf(runtime_,identity.klass,object,condition.path,why,true,true);
                if(!current || current->property!=condition.selected.property ||
                   current->layout_trace!=condition.selected.layout_trace) {
                    quarantine();fail(Code::LifetimeUnverifiable,"invocation condition storage changed: "+condition.path);
                }
                const auto info=inspect_property(runtime_,current->property);
                const auto& before=condition.info;
                if(info.name!=before.name || info.type!=before.type || info.offset!=before.offset ||
                   info.flags!=before.flags || info.element_size!=before.element_size || info.array_dim!=before.array_dim) {
                    quarantine();fail(Code::LifetimeUnverifiable,"invocation condition schema changed: "+condition.path);
                }
                if(!bridge_.identical_property(current->property,current->address,condition.expected.data(),condition.single_element))
                    fail(Code::ExpectationFailed,"invocation condition failed: "+condition.path);
            }
            outcome.dispatched=true;
            if(!dispatch(entry,object,function,frame.data())) {
                quarantine();fail(Code::Faulted,"ProcessEvent faulted; game effects/parameter ownership uncertain");
            }

            const auto current_registry=capture_object_registry(runtime_);
            if(!current_registry){quarantine();fail(Code::LifetimeUnverifiable,"registry unavailable after dispatch");}
            const auto current_object=discovery_.certify(object,*current_registry);
            const auto current_function=discovery_.certify(function,*current_registry);
            if(!current_object || !current_function || current_object->index!=identity.index || current_object->serial!=identity.serial ||
               current_function->index!=live_function->index || current_function->serial!=live_function->serial)
                {quarantine();fail(Code::LifetimeUnverifiable,"receiver/function expired after dispatch; outputs unavailable");}
            std::string values;
            for(const auto& [field,info]:parameters) if(info.flags&(0x100u|0x400u)) {
                const auto current=inspect_property(runtime_,field);
                if(current.name!=info.name || current.type!=info.type || current.offset!=info.offset || current.flags!=info.flags ||
                   current.element_size!=info.element_size || current.array_dim!=info.array_dim)
                    {quarantine();fail(Code::LifetimeUnverifiable,"output parameter schema changed after dispatch");}
                if(!values.empty())values+=',';
                values+=quote(info.name)+":"+quote(bridge_.export_property(field,static_cast<const std::uint8_t*>(frame.data())+info.offset,object));
                if(values.size()>3600)fail(Code::LimitExceeded,"function outputs exceed copied result bound after dispatch");
            }
            frame.cleanup();
            outcome.json="{\"dispatched\":true,\"outputs\":{"+values+"}}";
        }catch(const Error& error){outcome.error=error;}
        catch(const std::exception& error){outcome.error=infinifusion::core::make_error(Code::ReadFailed,error.what());}

        try {frame.cleanup();}
        catch(const std::exception& error){outcome.error=infinifusion::core::make_error(Code::ReadFailed,error.what());}
        for(auto it=conditions.rbegin();it!=conditions.rend();++it) {
            try {it->expected.cleanup();}
            catch(const std::exception& error){outcome.error=infinifusion::core::make_error(Code::ReadFailed,error.what());}
        }
        if(outcome.json.empty())outcome.json="{\"dispatched\":"+std::string(outcome.dispatched?"true":"false")+"}";
        return outcome;
    }
};
