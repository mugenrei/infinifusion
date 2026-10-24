namespace api_core = infinifusion::core;
class NativeObjectPropertyProvider final : public api_core::Provider {
    using Code=api_core::ErrorCode;
    using Value=api_core::Value;
    struct Timings { std::uint64_t validations{},roles_us{},membership_us{},schema_us{},exports_us{},preparations_us{},cache_hits{}; };
    struct Timer {
        std::uint64_t& total; std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
        ~Timer(){total+=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count();}
    };
    Timings timings_;
    static constexpr std::size_t text_limit=4096,payload_width=text_limit+4;
    struct Layout { void* property{}; PropertyInfo info; void* vtable{}; void* inner{}; void* second{}; };
    struct Binding {
        NativeObjectTargets::Selection selection;
        api_core::ResourceLocator locator; api_core::PropertyPath path;
        api_core::Region region; void* root{}; void* leaf{};
        std::uint8_t* root_address{}; std::uint8_t* address{};
        std::string root_name,leaf_type;
        PropertyInfo root_info; PropertyInfo leaf_info; std::uint32_t bool_layout{};
        std::vector<std::uintptr_t> trace;
        std::vector<Layout> metadata;
        std::uint64_t frame{}; bool ephemeral{},semantic{};

        std::optional<std::pair<Value,api_core::EncodedValue>> prepared_root;
    };
    Runtime& runtime_; NativeObjectTargets& targets_; EngineAllocator allocator_;
    ManagedValueBridge bridge_; std::uint64_t frame_{1};
    std::unordered_map<std::string,Binding> bindings_;
    std::unordered_map<std::string,std::vector<std::string>> aliases_;
    std::unordered_map<std::string,std::string> binding_lookup_;
    std::unordered_set<std::string> quarantined_;
    static api_core::Error error(Code code,std::string message) { return api_core::make_error(code,std::move(message)); }
    static std::string key(const api_core::Region& r) { return r.storage_key+":"+r.canonical_path; }
    static std::string lookup_key(const api_core::ResourceLocator& locator,const api_core::PropertyPath& path) {
        std::string result;
        for(const auto& part:{locator.provider,locator.target,locator.row,locator.class_guard,path.string()}) {
            result+=std::to_string(part.size());result+=':';result+=part;
        }
        return result;
    }
    void remember(const std::string& lookup,const std::string& certificate) {
        if(binding_lookup_.contains(lookup) || binding_lookup_.size()<2048)binding_lookup_[lookup]=certificate;
    }
    static bool same_layout(const PropertyInfo& a,const PropertyInfo& b) {
        return a.type==b.type && a.offset==b.offset && a.element_size==b.element_size && a.array_dim==b.array_dim && a.flags==b.flags;
    }
    static bool same_type(const api_core::FieldType& a,const api_core::FieldType& b) {
        if(a.kind!=b.kind || a.size!=b.size || a.bool_field_mask!=b.bool_field_mask || a.bool_true_bits!=b.bool_true_bits ||
            a.enum_signed!=b.enum_signed || a.enum_numeric_allowed!=b.enum_numeric_allowed || a.enum_type!=b.enum_type || a.enum_entries.size()!=b.enum_entries.size()) return false;
        for(std::size_t i=0;i<a.enum_entries.size();++i) if(a.enum_entries[i].name!=b.enum_entries[i].name || a.enum_entries[i].value!=b.enum_entries[i].value) return false;
        return true;
    }
    static bool single_element(const Binding& b) {
        return b.leaf_info.array_dim>1 && !b.path.segments.empty() && b.path.segments.back().kind==api_core::PathSegment::Kind::Index;
    }
    api_core::Result<std::vector<Layout>> metadata(const NativeObjectTargets::Selection& selected,const api_core::PropertyPath& path,void* checked_root) {

        std::vector<Layout> result; void* structure=selected.identity.klass;
        void* property{};
        const auto capture=[&](void* field) {
            const auto info=inspect_property(runtime_,field);
            const bool inner=info.type=="StructProperty" || info.type=="ArrayProperty" || info.type=="SetProperty" || info.type=="MapProperty" || info.type=="EnumProperty" || info.type=="ByteProperty";
            result.push_back({field,info,safe_read<void*>(field).value_or(nullptr),
                inner?safe_read_at<void*>(field,0x78).value_or(nullptr):nullptr,
                (info.type=="MapProperty" || info.type=="EnumProperty")?safe_read_at<void*>(field,0x80).value_or(nullptr):nullptr});
        };
        for(const auto& segment:path.segments) {
            if(segment.kind==api_core::PathSegment::Kind::Field) {
                if(property) {
                    if(inspect_property(runtime_,property).type!="StructProperty") return error(Code::LifetimeUnverifiable,"property hierarchy is not a struct");
                    structure=safe_read_at<void*>(property,0x78).value_or(nullptr);
                }

                property=result.empty()?checked_root:(structure?bridge_.field(structure,segment.field):nullptr);
                if(!property) return error(Code::LifetimeUnverifiable,"property hierarchy field unavailable");
                capture(property);
            } else {
                const auto info=inspect_property(runtime_,property);
                if(info.array_dim>1) {
                    if(segment.index>=static_cast<std::uint32_t>(info.array_dim)) return error(Code::LifetimeUnverifiable,"static property dimension changed");
                } else if(info.type=="ArrayProperty") {
                    property=safe_read_at<void*>(property,0x78).value_or(nullptr);
                    if(!property) return error(Code::LifetimeUnverifiable,"array element schema unavailable");
                    capture(property);
                } else return error(Code::LifetimeUnverifiable,"indexed property schema changed");
            }
        }
        return result;
    }
    api_core::Result<api_core::FieldType> field_type(const ResolvedLeaf& leaf) {
        api_core::FieldType type; auto name=leaf.type; void* numeric=leaf.property; void* enumeration{};
        if(name=="EnumProperty") {
            numeric=safe_read_at<void*>(leaf.property,0x78).value_or(nullptr);
            enumeration=safe_read_at<void*>(leaf.property,0x80).value_or(nullptr);
            if(!numeric) return error(Code::ReadFailed,"enum underlying property unavailable");
            name=inspect_property(runtime_,numeric).type;
        } else if(name=="ByteProperty") enumeration=safe_read_at<void*>(leaf.property,0x78).value_or(nullptr);
        type.size=static_cast<std::uint8_t>(std::clamp(inspect_property(runtime_,numeric).element_size,0,255));
        if(name=="BoolProperty") {
            type.kind=api_core::ScalarKind::Bool; type.size=1;
            const auto offset=safe_read_at<std::uint8_t>(leaf.property,0x79);
            if(!offset || *offset>=inspect_property(runtime_,leaf.property).element_size)
                return error(Code::ReadFailed,"bool byte offset exceeds reflected storage");
            const auto byte=safe_read_at<std::uint8_t>(leaf.property,0x7A).value_or(0);
            const auto mask=safe_read_at<std::uint8_t>(leaf.property,0x7B).value_or(0);
            type.bool_field_mask=mask==0xFF?0xFF:(byte?byte:mask);
            type.bool_true_bits=mask==0xFF?1:type.bool_field_mask;
            if(!type.bool_field_mask) return error(Code::ReadFailed,"bool field mask unavailable");
        } else if(name=="FloatProperty" || name=="DoubleProperty") type.kind=api_core::ScalarKind::Float;
        else if(name.starts_with("UInt") || name=="ByteProperty") type.kind=api_core::ScalarKind::UnsignedInt;
        else if(name=="IntProperty" || name=="Int8Property" || name=="Int16Property" || name=="Int64Property") type.kind=api_core::ScalarKind::SignedInt;
        else return error(Code::TypeMismatch,"managed reflected value requires property text");
        if(type.size!=1 && type.size!=2 && type.size!=4 && type.size!=8) return error(Code::ReadFailed,"invalid scalar storage width");
        if(type.kind==api_core::ScalarKind::Float && type.size!=4 && type.size!=8) return error(Code::ReadFailed,"invalid floating storage width");
        if(enumeration) {
            if(type.kind!=api_core::ScalarKind::SignedInt && type.kind!=api_core::ScalarKind::UnsignedInt)
                return error(Code::ReadFailed,"enum underlying storage must be integer");
            type.enum_signed=type.kind==api_core::ScalarKind::SignedInt;
            type.kind=api_core::ScalarKind::Enum; type.enum_numeric_allowed=true; type.enum_type=object_path(runtime_,enumeration);
            const auto array=safe_read_at<FStringHeader>(enumeration,0x40);
            struct Entry { FName name; std::int64_t value; };
            if(!array || array->num<0 || array->num>256) return error(Code::ReadFailed,"invalid enum metadata");
            for(int i=0;i<array->num;++i) {
                const auto entry=safe_read<Entry>(reinterpret_cast<const std::uint8_t*>(array->data)+i*sizeof(Entry));
                if(!entry) return error(Code::ReadFailed,"unreadable enum entry");
                type.enum_entries.push_back({fname_to_string(runtime_,entry->name),entry->value});
            }
        }
        return type;
    }
    api_core::Result<std::string> input_text(const Binding& b,const Value& value) {
        using namespace infinifusion::unreal;
        if(value.kind()==api_core::ValueKind::Bytes) {
            const auto& bytes=value.as_bytes();
            if(bytes.size()>text_limit || std::find(bytes.begin(),bytes.end(),0)!=bytes.end()) return error(Code::LimitExceeded,"property text exceeds4096 bytes or contains NUL");
            return std::string(bytes.begin(),bytes.end());
        }
        auto scalar=field_type(ResolvedLeaf{b.leaf,nullptr,b.leaf_type,{},{},false});
        if(b.leaf_info.array_dim>1 && !single_element(b)) scalar=error(Code::TypeMismatch,"complete static array requires import text");
        PropertyValue v;
        if(scalar) {
            const auto encoded=api_core::encode_value(value,scalar.value()); if(!encoded) return encoded.error();
            auto decoded=api_core::decode_value(encoded->bytes,scalar.value()); if(!decoded) return decoded.error();
            if(scalar->kind==api_core::ScalarKind::Enum) {
                if(value.kind()==api_core::ValueKind::EnumSymbol) v.value=PropertyEnum{value.as_enum().symbol};
                else if(value.kind()==api_core::ValueKind::String) v.value=PropertyEnum{value.as_string()};
                else if(decoded->kind()==api_core::ValueKind::EnumSymbol) v.value=PropertyEnum{decoded->as_enum().symbol};
                else if(decoded->kind()==api_core::ValueKind::SignedInteger) v.value=decoded->as_signed();
                else v.value=decoded->as_unsigned();
            } else if(decoded->kind()==api_core::ValueKind::Boolean) v.value=decoded->as_bool();
            else if(decoded->kind()==api_core::ValueKind::SignedInteger) v.value=decoded->as_signed();
            else if(decoded->kind()==api_core::ValueKind::UnsignedInteger) v.value=decoded->as_unsigned();
            else v.value=decoded->as_float();
        } else if(value.kind()==api_core::ValueKind::String) {
            if(b.leaf_type=="StrProperty") v.value=PropertyString{value.as_string()};
            else if(b.leaf_type=="NameProperty") v.value=PropertyName{value.as_string()};
            else if(b.leaf_type=="TextProperty") v.value=PropertyInvariantText{value.as_string()};
            else return error(Code::TypeMismatch,"complete value requires explicit Unreal import text");
        } else return error(Code::TypeMismatch,"managed value requires string or explicit import text");
        auto text=to_property_text(v); if(!text) return text.error();
        if(text->size()>text_limit) return error(Code::LimitExceeded,"property import exceeds4096 bytes");
        return text;
    }
    api_core::Result<ResolvedLeaf> projection(const Binding& b,void* root_memory) {
        std::string why;
        auto leaf=resolve_leaf(runtime_,b.selection.identity.klass,root_memory,b.path.string(),why,true,true,b.root);
        if(!leaf) return error(Code::PathNotFound,why);
        return *leaf;
    }
    static api_core::Result<std::vector<std::uint8_t>> pack(std::string_view text) {
        if(text.size()>text_limit || text.find('\0')!=std::string_view::npos) return error(Code::LimitExceeded,"root export exceeds4096 bytes or contains NUL");
        std::vector<std::uint8_t> bytes(payload_width); const auto n=static_cast<std::uint32_t>(text.size());
        std::memcpy(bytes.data(),&n,4); std::memcpy(bytes.data()+4,text.data(),text.size()); return bytes;
    }
    static api_core::Result<std::string> unpack(std::span<const std::uint8_t> bytes) {
        if(bytes.size()!=payload_width) return error(Code::InvalidArgument,"root payload width mismatch");
        std::uint32_t n{}; std::memcpy(&n,bytes.data(),4);
        if(n>text_limit || std::find(bytes.begin()+4,bytes.begin()+4+n,0)!=bytes.begin()+4+n ||
            std::any_of(bytes.begin()+4+n,bytes.end(),[](auto c){return c!=0;})) return error(Code::InvalidArgument,"invalid copied root payload");
        return std::string(reinterpret_cast<const char*>(bytes.data()+4),n);
    }
    api_core::Result<api_core::Region> bind_impl(const api_core::ResourceLocator& locator,const api_core::PropertyPath& path,bool owned) {
        try {
            const auto lookup=lookup_key(locator,path);
            const auto cached=binding_lookup_.find(lookup);
            if(cached!=binding_lookup_.end()) {
                const auto prior=bindings_.find(cached->second);
                if(prior!=bindings_.end() && !(owned && prior->second.selection.identity.serial==0)) {
                    const auto valid=validate(prior->second.region);
                    if(valid.ok())return prior->second.region;
                }

                binding_lookup_.erase(lookup);
            }
            auto selected=targets_.select(locator); if(!selected) return selected.error();
            if(owned && selected->identity.serial==0 && certify_owned_object_serial(runtime_,selected->identity.object)) {
                targets_.owned_serial_assigned(selected->identity.object);
                selected=targets_.select(locator); if(!selected) return selected.error();
            }
            if(path.segments.empty() || path.segments.front().kind!=api_core::PathSegment::Kind::Field) return error(Code::InvalidArgument,"property path requires a root field");
            auto* root=bridge_.field(selected->identity.klass,path.segments.front().field);
            if(!root) return error(Code::PathNotFound,"root property unavailable");
            const auto info=inspect_property(runtime_,root);
            const auto size=safe_read_at<std::int32_t>(selected->identity.klass,0x58).value_or(0);
            const auto width=static_cast<std::uint64_t>(std::max(0,info.element_size))*std::max(0,info.array_dim);
            if(info.offset<0 || info.element_size<=0 || info.array_dim<=0 || width>65536 || info.offset>size || width>static_cast<std::uint64_t>(size-info.offset)) return error(Code::ReadFailed,"root property exceeds class storage");
            std::string why; auto leaf=resolve_leaf(runtime_,selected->identity.klass,selected->identity.object,path.string(),why,true,true);
            if(!leaf) return error(Code::PathNotFound,why);
            const auto descriptor=field_type(*leaf);
            if(!descriptor && descriptor.error().code!=Code::TypeMismatch) return descriptor.error();
            const bool semantic=path.segments.size()!=1 || info.array_dim!=1 || !descriptor;
            const bool ephemeral=selected->identity.serial<=0;
            api_core::Region r; r.identity={ephemeral?frame_:1,static_cast<std::uint64_t>(selected->identity.index),static_cast<std::uint64_t>(std::max(0,selected->identity.serial)),0};
            r.canonical_path=leaf->canonical_path; r.restorable=!ephemeral;
            r.identity_tracking=ephemeral?api_core::IdentityTracking::ObservedOnly:api_core::IdentityTracking::ObjectSerial;
            r.whole_value=semantic; r.type=descriptor?descriptor.value():api_core::FieldType{};
            auto* root_address=static_cast<std::uint8_t*>(selected->identity.object)+info.offset;
            auto* address=static_cast<std::uint8_t*>(leaf->address);
            if(leaf->type=="BoolProperty") address+=safe_read_at<std::uint8_t>(leaf->property,0x79).value_or(0);
            r.storage_key=std::to_string(selected->identity.index)+":"+std::to_string(selected->identity.serial)+":"+std::to_string(reinterpret_cast<std::uintptr_t>(semantic?root_address:address));
            r.byte_mask.assign(semantic?payload_width:r.type.size,0xFF);
            if(!semantic && r.type.kind==api_core::ScalarKind::Bool) r.byte_mask[0]=r.type.bool_field_mask;
            auto layout=metadata(selected.value(),path,root); if(!layout) return layout.error();
            Binding b{selected.value(),locator,path,r,root,leaf->property,root_address,address,path.segments.front().field,leaf->type,info,inspect_property(runtime_,leaf->property),
                leaf->type=="BoolProperty"?safe_read_at<std::uint32_t>(leaf->property,0x78).value_or(0):0,semantic?std::vector<std::uintptr_t>{}:leaf->layout_trace,layout.value(),frame_,ephemeral,semantic};
            const auto certificate=key(r);
            if(bindings_.contains(certificate)) { const auto valid=validate(r); if(!valid.ok()) return valid; remember(lookup,certificate); return bindings_.at(certificate).region; }
            if(bindings_.size()>=2048) return error(Code::LimitExceeded,"provider binding limit2048");
            auto& aliases=aliases_[r.storage_key]; if(aliases.size()>=64) return error(Code::LimitExceeded,"physical alias limit64");
            bindings_.emplace(certificate,std::move(b)); aliases.push_back(certificate); remember(lookup,certificate); return r;
        } catch(const std::exception& e) { return error(Code::ReadFailed,e.what()); }
    }
    api_core::Error validate_one(const Binding& b,const api_core::Region& r) {
        ++timings_.validations;
        if(b.ephemeral && b.frame!=frame_) return error(Code::TargetExpired,"callback lease ended");
        if(r.identity!=b.region.identity) return error(Code::LifetimeUnverifiable,"binding identity changed");
        { Timer timer{timings_.roles_us}; auto target=targets_.validate(b.locator,b.selection); if(!target.ok()) return target; }
        if(safe_read_at<void*>(b.selection.identity.object,0x10).value_or(nullptr)!=b.selection.identity.klass)
            return error(Code::LifetimeUnverifiable,"selected object class changed");
        const auto class_size=safe_read_at<std::int32_t>(b.selection.identity.klass,0x58).value_or(0);
        const auto root_width=static_cast<std::uint64_t>(b.root_info.element_size)*b.root_info.array_dim;
        if(b.root_info.offset>class_size || root_width>static_cast<std::uint64_t>(class_size-b.root_info.offset))
            return error(Code::LifetimeUnverifiable,"class property storage changed");
        if(r.offset>b.region.byte_mask.size() || r.byte_mask.size()>b.region.byte_mask.size()-r.offset) return error(Code::InvalidArgument,"storage slice out of range");
        if(b.semantic && (r.offset || r.byte_mask.size()!=payload_width || !r.whole_value)) return error(Code::InvalidArgument,"root semantic slice unsupported");
        { Timer timer{timings_.membership_us}; if(!same_layout(inspect_property(runtime_,b.root),b.root_info) || bridge_.field(b.selection.identity.klass,b.root_name)!=b.root) return error(Code::LifetimeUnverifiable,"root reflected layout changed"); }
        Timer schema_timer{timings_.schema_us};
        auto layout=metadata(b.selection,b.path,b.root); if(!layout) return layout.error();
        if(layout->size()!=b.metadata.size()) return error(Code::LifetimeUnverifiable,"property hierarchy changed");
        for(std::size_t i=0;i<layout->size();++i) {
            const auto& now=(layout.value())[i]; const auto& prior=b.metadata[i];
            if(now.property!=prior.property || !same_layout(now.info,prior.info) || now.vtable!=prior.vtable || now.inner!=prior.inner || now.second!=prior.second)
                return error(Code::LifetimeUnverifiable,"property hierarchy layout changed");
        }
        if(b.semantic) {
            if(!same_layout(inspect_property(runtime_,b.leaf),b.leaf_info) ||
                (b.leaf_type=="BoolProperty" && safe_read_at<std::uint32_t>(b.leaf,0x78).value_or(0)!=b.bool_layout))
                return error(Code::LifetimeUnverifiable,"semantic leaf schema changed");
            auto type=field_type(ResolvedLeaf{b.leaf,nullptr,b.leaf_type,{},{},false});
            if(type && !same_type(type.value(),b.region.type)) return error(Code::LifetimeUnverifiable,"semantic scalar/enum schema changed");
            if(!type && type.error().code!=Code::TypeMismatch) return type.error();
            return {};
        }
        std::string why; auto leaf=resolve_leaf(runtime_,b.selection.identity.klass,b.selection.identity.object,b.path.string(),why,true,true);
        if(!leaf) return error(Code::LifetimeUnverifiable,why);
        auto* address=static_cast<std::uint8_t*>(leaf->address);
        if(leaf->type=="BoolProperty") address+=safe_read_at<std::uint8_t>(leaf->property,0x79).value_or(0);
        if(leaf->property!=b.leaf || leaf->type!=b.leaf_type || leaf->canonical_path!=b.region.canonical_path || !same_layout(inspect_property(runtime_,b.leaf),b.leaf_info) ||
            (leaf->type=="BoolProperty" && safe_read_at<std::uint32_t>(b.leaf,0x78).value_or(0)!=b.bool_layout) ||
            (!b.semantic && (address!=b.address || leaf->layout_trace!=b.trace))) return error(Code::LifetimeUnverifiable,"leaf reflected layout changed");
        auto type=field_type(*leaf);
        if(type && !same_type(type.value(),b.region.type)) return error(Code::LifetimeUnverifiable,"scalar/enum descriptor changed");
        if(!type && (type.error().code!=Code::TypeMismatch || !b.semantic)) return error(Code::LifetimeUnverifiable,"scalar descriptor unavailable");
        return {};
    }
    api_core::Error publish_scalar(const api_core::Region& r,std::span<const std::uint8_t> desired,bool& mutation) {

        for(const auto& alias:aliases_.at(r.storage_key)) {
            const auto& b=bindings_.at(alias); bool overlap{};
            for(std::size_t i=0;i<r.byte_mask.size();++i)
                if(r.offset+i<b.region.byte_mask.size() && (r.byte_mask[i]&b.region.byte_mask[r.offset+i])) overlap=true;
            if(!overlap) continue;
            std::vector<std::uint8_t> raw(b.region.type.size);
            if(!safe_copy(b.address,raw.data(),raw.size())) return error(Code::ReadFailed,"complete scalar read unavailable");
            for(std::size_t i=0;i<desired.size();++i) if(r.offset+i<raw.size()) {
                const auto mask=static_cast<std::uint8_t>(r.byte_mask[i]&b.region.byte_mask[r.offset+i]);
                raw[r.offset+i]=static_cast<std::uint8_t>((raw[r.offset+i]&~mask)|(desired[i]&mask));
            }
            auto value=api_core::decode_value(raw,b.region.type); if(!value) return value.error();
            auto text=input_text(b,value.value()); if(!text) return text.error();
            auto scratch=bridge_.scratch_property(b.root,b.root_address);
            bridge_.import(b.root,text.value(),scratch.data(),b.selection.identity.object);
            auto valid=validate(r); if(!valid.ok()) return valid;
            if(!bridge_.identical_property(b.root,b.root_address,scratch.data())) {
                mutation=true; bridge_.copy_property(b.root,b.root_address,scratch.data());
                valid=validate(r);
                if(!valid.ok()) {quarantined_.insert(r.storage_key);return valid;}
                if(!bridge_.identical_property(b.root,b.root_address,scratch.data())) {quarantined_.insert(r.storage_key);return error(Code::WriteFailed,"scalar publication readback mismatch");}
            }
            scratch.cleanup();
        }
        return {};
    }
public:
    NativeObjectPropertyProvider(Runtime& runtime,NativeObjectTargets& targets):runtime_(runtime),targets_(targets),allocator_(runtime),bridge_(runtime,allocator_) {}
    std::string_view id() const noexcept override { return "unreal.object"; }
    bool change_driven() const noexcept override { return true; }
    auto timings() const { return timings_; }
    auto export_timings() const {return bridge_.export_timings();}
    void frame(std::uint64_t epoch) {
        timings_={};
        bridge_.reset_export_timings();
        frame_=epoch;
        std::erase_if(bindings_,[&](const auto& item){return item.second.ephemeral && item.second.frame!=frame_;});
        std::erase_if(binding_lookup_,[&](const auto& item){return !bindings_.contains(item.second);});
        for(auto it=aliases_.begin();it!=aliases_.end();) { std::erase_if(it->second,[&](const auto& k){return !bindings_.contains(k);}); if(it->second.empty()) it=aliases_.erase(it); else ++it; }
    }
    api_core::Result<api_core::Region> bind(const api_core::ResourceLocator& locator,const api_core::PropertyPath& path) override { return bind_impl(locator,path,false); }
    api_core::Result<api_core::Region> bind_owned(const api_core::ResourceLocator& locator,const api_core::PropertyPath& path,std::string_view) override { return bind_impl(locator,path,true); }
    api_core::Error validate(const api_core::Region& r) override {
        try {
            if(r.identity_tracking==api_core::IdentityTracking::ObservedOnly && r.identity.epoch!=frame_)
                return error(Code::TargetExpired,"callback lease ended");
            const auto found=bindings_.find(key(r)); if(found==bindings_.end()) return error(Code::TargetExpired,"lease not found");
            if(quarantined_.contains(r.storage_key)) return error(Code::LifetimeUnverifiable,"native property outcome quarantined");
            const auto aliases=aliases_.find(r.storage_key); if(aliases==aliases_.end()) return error(Code::LifetimeUnverifiable,"alias certificates unavailable");
            bool found_checked{};
            for(const auto& k:aliases->second) {
                const auto& b=bindings_.at(k); bool overlap=b.semantic;
                if(!overlap) for(std::size_t i=0;i<r.byte_mask.size();++i)
                    if(r.offset+i<b.region.byte_mask.size() && (r.byte_mask[i]&b.region.byte_mask[r.offset+i])) overlap=true;
                if(overlap) { auto valid=validate_one(b,r); if(!valid.ok()) return valid; found_checked |= &b==&found->second; }
            }
            return found_checked ? api_core::Error{} : validate_one(found->second,r);
        } catch(const std::exception& e) { return error(Code::LifetimeUnverifiable,e.what()); }
    }
    api_core::Result<std::vector<std::uint8_t>> read(const api_core::Region& r) override {
        const auto valid=validate(r); if(!valid.ok()) return valid;
        try {
            const auto& b=bindings_.at(key(r));
            if(b.semantic) { Timer timer{timings_.exports_us}; return pack(bridge_.export_property(b.root,b.root_address,b.selection.identity.object)); }
            std::vector<std::uint8_t> bytes(r.byte_mask.size()); if(!safe_copy(b.address+r.offset,bytes.data(),bytes.size())) return error(Code::ReadFailed,"scalar read unavailable"); return bytes;
        } catch(const std::exception& e) { return error(Code::ReadFailed,e.what()); }
    }
    api_core::Result<api_core::EncodedValue> prepare(const api_core::Region& r,const Value& value) override {
        Timer timer{timings_.preparations_us};
        const auto valid=validate(r);
        if(!valid.ok()) {
            const auto prior=bindings_.find(key(r));if(prior!=bindings_.end())prior->second.prepared_root.reset();
            return valid;
        }
        try {
            auto& b=bindings_.at(key(r));
            const bool cacheable=b.semantic && b.path.segments.size()==1;
            if(cacheable && b.prepared_root && b.prepared_root->first==value) {++timings_.cache_hits;return b.prepared_root->second;}
            if(cacheable)b.prepared_root.reset();
            if(!b.semantic && value.kind()!=api_core::ValueKind::Bytes) return api_core::encode_value(value,r.type);
            auto text=input_text(b,value); if(!text) return text.error();
            auto scratch=bridge_.scratch_property(b.root,b.root_address); auto leaf=projection(b,scratch.data()); if(!leaf) return leaf.error();
            bridge_.import(leaf->property,text.value(),leaf->address,b.selection.identity.object,single_element(b));
            if(!b.semantic) {
                auto* address=static_cast<std::uint8_t*>(leaf->address);
                if(leaf->type=="BoolProperty") address+=safe_read_at<std::uint8_t>(leaf->property,0x79).value_or(0);
                std::vector<std::uint8_t> raw(r.type.size);
                if(!safe_copy(address,raw.data(),raw.size())) return error(Code::ReadFailed,"prepared scalar unavailable");
                scratch.cleanup(); return api_core::EncodedValue{std::move(raw),r.byte_mask};
            }
            auto bytes=pack(bridge_.export_property(b.root,scratch.data(),b.selection.identity.object)); scratch.cleanup();
            if(!bytes) return bytes.error();
            api_core::EncodedValue result{bytes.value(),r.byte_mask};
            if(cacheable)b.prepared_root.emplace(value,result);
            return result;
        } catch(const std::exception& e) { return error(Code::TypeMismatch,e.what()); }
    }
    api_core::Result<Value> decode(const api_core::Region& r,std::span<const std::uint8_t> bytes) override {
        const auto valid=validate(r); if(!valid.ok()) return valid;
        try {
            const auto& b=bindings_.at(key(r)); if(!b.semantic) return api_core::decode_value(bytes,r.type);
            auto text=unpack(bytes); if(!text) return text.error();

            if(b.path.segments.size()==1) return Value::bytes({text->begin(),text->end()});
            auto scratch=bridge_.scratch_property(b.root); bridge_.import(b.root,text.value(),scratch.data(),b.selection.identity.object);
            auto leaf=projection(b,scratch.data()); if(!leaf) return leaf.error(); auto type=field_type(leaf.value());
            if(b.leaf_info.array_dim>1 && !single_element(b)) type=error(Code::TypeMismatch,"complete static array projection");
            api_core::Result<Value> result=error(Code::ReadFailed,"projection unavailable");
            if(type) {
                auto* address=static_cast<std::uint8_t*>(leaf->address); if(leaf->type=="BoolProperty") address+=safe_read_at<std::uint8_t>(leaf->property,0x79).value_or(0);
                std::vector<std::uint8_t> raw(type->size); if(!safe_copy(address,raw.data(),raw.size())) return error(Code::ReadFailed,"scratch scalar projection unavailable"); result=api_core::decode_value(raw,type.value());
            } else {
                const auto exported=bridge_.export_property(leaf->property,leaf->address,b.selection.identity.object,single_element(b));
                if(exported.size()>text_limit) return error(Code::LimitExceeded,"projected property export exceeds4096 bytes");
                result=Value::bytes({exported.begin(),exported.end()});
            }
            scratch.cleanup(); return result;
        } catch(const std::exception& e) { return error(Code::ReadFailed,e.what()); }
    }
    api_core::Error write(const api_core::Region& r,std::span<const std::uint8_t> desired) override {
        const auto valid=validate(r); if(!valid.ok()) return valid;
        if(desired.size()!=r.byte_mask.size()) return error(Code::InvalidArgument,"write width mismatch");
        bool mutation{}; const auto unknown=bridge_.unknown_events();
        try {
            const auto& b=bindings_.at(key(r));
            if(!b.semantic) return publish_scalar(r,desired,mutation);
            auto scratch=bridge_.scratch_property(b.root,b.root_address);
            if(b.semantic) {
                auto text=unpack(desired); if(!text) return text.error(); bridge_.import(b.root,text.value(),scratch.data(),b.selection.identity.object);
                auto canonical=pack(bridge_.export_property(b.root,scratch.data(),b.selection.identity.object));
                if(!canonical || !std::equal(canonical->begin(),canonical->end(),desired.begin(),desired.end())) return error(Code::InvalidArgument,"root payload is not canonical engine text");
            }
            auto source=validate(r); if(!source.ok()) return source;
            if(!bridge_.identical_property(b.root,b.root_address,scratch.data())) {
                mutation=true; bridge_.copy_property(b.root,b.root_address,scratch.data());
                auto reread=validate(r); if(!reread.ok()) { quarantined_.insert(r.storage_key); return reread; }
                if(!bridge_.identical_property(b.root,b.root_address,scratch.data())) { quarantined_.insert(r.storage_key); return error(Code::WriteFailed,"property publication readback mismatch"); }
            }
            scratch.cleanup(); return {};
        } catch(const std::exception& e) {
            if(mutation || bridge_.unknown_events()!=unknown) quarantined_.insert(r.storage_key);
            return error(Code::WriteFailed,e.what());
        }
    }
};
