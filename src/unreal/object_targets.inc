class NativeObjectTargets final : private NativeObjectReflection {
public:
    struct Selection {
        NativeObjectDiscovery::Resolved identity;
        std::string object_path;
        Snapshot certificate;
    };
    NativeObjectTargets(Runtime& runtime,NativeObjectDiscovery& discovery):NativeObjectReflection(runtime,discovery) {}
    infinifusion::core::Result<Selection> select(const infinifusion::core::ResourceLocator& locator) {
        const auto found=discovery_.resolve({locator.provider,locator.target,{}, {}});
        if(!found) return found.error();
        try {
            Observation observation;
            RecordScope recording(*this,observation);
            const auto registry=capture_object_registry(runtime_);
            if(!registry) fail(Code::NotReady,"object target registry unavailable");
            observation.registry=*registry;
            auto* object=found->object;
            reference(observation,object);
            if(!locator.row.empty() && locator.row!="object") {
                if(!is_a(observation,object,"/Script/CoreUObject.Class"))
                    fail(Code::TypeMismatch,"default/component selector requires a class target");
                if(locator.row=="defaults") object=class_default(observation,object);
                else if(locator.row.starts_with("component:") && locator.row.size()>10)
                    object=component(observation,object,std::string_view(locator.row).substr(10)).object;
                else fail(Code::InvalidArgument,"unknown object selector");
            }
            const auto identity=discovery_.certify(object,*registry);
            if(!identity) return identity.error();
            if(!locator.class_guard.empty() && object_path_cached(observation,identity->klass)!=locator.class_guard)
                fail(Code::GuardMismatch,"selected object class guard mismatch");
            Selection result;
            result.identity=identity.value();
            result.object_path=object_path_cached(observation,object);
            result.certificate=std::move(static_cast<Snapshot&>(observation));
            return result;
        } catch(const Error& error) {return error;}
    }
    void owned_serial_assigned(void* object) { discovery_.owned_serial_assigned(object); }
    infinifusion::core::Error validate(const infinifusion::core::ResourceLocator& locator,const Selection& prior) {
        if(locator.row=="defaults" && !certified_class_default_layout(runtime_))
            return error(Code::CapabilityUnavailable,"class-default layout certificate unavailable");
        const auto registry=capture_object_registry(runtime_);
        if(!registry) return error(Code::NotReady,"object target registry unavailable");
        const auto selected=discovery_.certify(prior.identity.object,*registry);
        if(!selected) return selected.error();
        if(selected->index!=prior.identity.index || selected->serial!=prior.identity.serial || selected->klass!=prior.identity.klass)
            return error(Code::TargetExpired,"selected object identity changed");

        for(const auto& old:prior.certificate.references) {
            const auto now=discovery_.certify(old.object,*registry);
            if(!now) return now.error();
            if(now->index!=old.index || now->serial!=old.serial || now->klass!=old.klass || now->flags!=old.flags ||
               now->name.comparison_index!=old.name.comparison_index || now->name.number!=old.name.number || now->outer!=old.outer)
                return error(Code::LifetimeUnverifiable,"object role reference changed");
        }
        for(const auto& fact:prior.certificate.reads) {
            std::array<std::uint8_t,80> bytes{};
            if(!safe_copy(fact.address,bytes.data(),fact.size) || std::memcmp(bytes.data(),fact.bytes.data(),fact.size))
                return error(Code::LifetimeUnverifiable,"object role source changed");
        }
        for(const auto& [field,old]:prior.certificate.properties) {
            const auto now=inspect_property(runtime_,field);
            if(now.name!=old.name || now.type!=old.type || now.offset!=old.offset || now.element_size!=old.element_size ||
               now.array_dim!=old.array_dim || now.flags!=old.flags)
                return error(Code::LifetimeUnverifiable,"object role field schema changed");
        }
        return {};
    }
};
