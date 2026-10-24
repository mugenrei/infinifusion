void runtime_tbcr_observed(const std::string&, IF_TbcrState, const std::string&);

bool runtime_initial_definitions_ready() noexcept;
std::vector<infinifusion::core::TbcrPatchProvider::Definition> runtime_tbcr_definitions();
using ConditionalPostLoadFn = void(__fastcall*)(void*);
void runtime_object_loaded(void*) noexcept;
class AssetLoadBoundary;
std::atomic<AssetLoadBoundary*> g_asset_load_boundary{};
void __fastcall asset_postload_wrapper(void*);

class AssetLoadBoundary {
    static constexpr std::uintptr_t entry_rva = 0x01056840;

    static constexpr std::array<std::uint8_t,6> prologue{0x40,0x53,0x48,0x83,0xEC,0x20};
    static constexpr std::array<std::uint8_t,26> following{
        0x48,0x8B,0xD9,0x8B,0x49,0x08,0x8B,0xC1,0xC1,0xE8,0x0C,0xA8,0x01,0x74,0x63,
        0x0F,0xBA,0xF1,0x0C,0x89,0x4B,0x08,0x48,0x8B,0xCB,0xE8};
    Runtime runtime_;
    EngineAllocator allocator_;
#ifdef INFINIFUSION_DEVELOPMENT
    TableEngineProbe table_probe_;
#endif
    NativeTableDefinitions table_definitions_;
    NativeMessageDefinitions message_definitions_;

    struct Projection {
        std::uintptr_t object{}, data{}, outer{};
        std::int32_t index{}, serial{};
        FName name{};
        smtvv::tbcr::Bytes base, last;
    };
    std::map<std::string, Projection> projections_;
    std::size_t cached_bytes_{};
    std::uint64_t applications_{};
    Runtime observation_;
    DWORD expected_thread_{};
    std::mutex mutex_;
    void* trampoline_{};
    std::unique_ptr<CertifiedEntryHook> hook_;
    ConditionalPostLoadFn original_{};

    std::optional<PropertyInfo> field(void* type, std::string_view name) {
        for (unsigned depth=0; type && depth<8; ++depth) {
            auto* current=safe_read_at<void*>(type,0x50).value_or(nullptr);
            for (unsigned count=0; current && count<128; ++count) {
                const auto info=inspect_property(observation_,current);
                if (info.name==name) return info;
                current=safe_read_at<void*>(current,0x20).value_or(nullptr);
            }
            type=safe_read_at<void*>(type,0x40).value_or(nullptr);
        }
        return std::nullopt;
    }
    const std::uint8_t* item(void* object) const {
        const auto index=safe_read_at<std::int32_t>(object,0x0C).value_or(-1);
        const auto registry=safe_read<UObjectArrayHeader>(observation_.object_array);
        if (!registry || !registry->objects || index<0 || index>=registry->num_elements ||
            registry->num_elements>registry->max_elements || index/kElementsPerChunk>=registry->num_chunks) return nullptr;
        const auto chunk=safe_read<void*>(registry->objects+index/kElementsPerChunk).value_or(nullptr);
        return chunk ? static_cast<const std::uint8_t*>(chunk)+(index%kElementsPerChunk)*kObjectItemSize : nullptr;
    }
    bool live(void* object, bool loading_callback=false) const {
        const auto slot=item(object);
        const auto internal=safe_read_at<std::uint32_t>(slot,8).value_or(0xFFFFFFFF);
        const auto flags=safe_read_at<std::uint32_t>(object,8).value_or(0xFFFFFFFF);

        return slot && safe_read<void*>(slot).value_or(nullptr)==object &&
            !(internal&((1u<<28)|(1u<<29)|(1u<<31))) &&
            (!(internal&(1u<<26)) || (loading_callback && (internal&(1u<<27)) && (flags&0x1000u))) &&
            !(flags&(0x200u|0x400u|0x4000u|0x8000u|0x10000u|0x20000u|0x08000000u)) &&
            (flags&0x00080000u);
    }
    bool observe_cpp(void* object, bool fresh_source) {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if (!lock.owns_lock()) return false;
        auto* klass=safe_read_at<void*>(object,0x10).value_or(nullptr);
        if (!klass || !is_asset_class(observation_,object,"/Script/Project.MMIAsset")) return false;
        auto* base_class=klass;
        for (unsigned depth=0; base_class && depth<128; ++depth) {
            if (object_path(observation_,base_class)=="/Script/Project.MMIAsset") break;
            base_class=safe_read_at<void*>(base_class,0x40).value_or(nullptr);
        }
        const auto class_slot=item(base_class);
        const auto class_internal=safe_read_at<std::uint32_t>(class_slot,8).value_or(0);
        constexpr auto rooted_native=(1u<<25)|(1u<<30);
        if (!class_slot || safe_read<void*>(class_slot).value_or(nullptr)!=base_class || (class_internal&rooted_native)!=rooted_native) return false;
        const auto path=object_path(observation_,object);
        if (GetCurrentThreadId()!=expected_thread_) {
            log_line("WARN","TBCR source application unavailable off game thread asset="+path); return false;
        }
        if (!runtime_initial_definitions_ready()) {
            log_line("WARN","TBCR source application preceded client admission asset="+path); return false;
        }
        const auto started=std::chrono::steady_clock::now();
        const auto definitions=runtime_tbcr_definitions();
        const bool selected=std::any_of(definitions.begin(),definitions.end(),[&](const auto& d){return d.asset==path;});
        auto projection=projections_.find(path);
        if (!selected && projection==projections_.end()) return false;
        IF_TbcrState outcome{}; outcome.size=sizeof(outcome); outcome.application=++applications_;
        bool source_published{};
        try {
            if (!live(object,fresh_source)) throw std::runtime_error("TBCR source identity/load state unavailable flags="+
                hex_value(safe_read_at<std::uint32_t>(object,8).value_or(0xFFFFFFFF))+" internal="+
                hex_value(safe_read_at<std::uint32_t>(item(object),8).value_or(0xFFFFFFFF)));
            const auto serial=safe_read_at<std::int32_t>(item(object),0x10);
            const auto index=safe_read_at<std::int32_t>(object,0x0C);
            const auto name=safe_read_at<FName>(object,0x18);
            const auto outer=safe_read_at<void*>(object,0x20);
            const auto vtable=safe_read<void*>(object);
            const auto postload=vtable ? safe_read_at<void*>(*vtable,0x70) : std::nullopt;
            if (!serial || !index || !name || !outer || !postload || *postload!=runtime_.image_base+0x01062740)
                throw std::runtime_error("TBCR source class/serial certificate unavailable");
            const auto info=field(klass,"MyFmtData");
            const auto inner=info ? safe_read_at<void*>(info->address,0x78).value_or(nullptr) : nullptr;
            const auto element=inner ? inspect_property(observation_,inner) : PropertyInfo{};
            if (!info || info->type!="ArrayProperty" || info->offset<0 || info->element_size!=16 || info->array_dim!=1 ||
                element.type!="ByteProperty" || element.element_size!=1 || element.array_dim!=1)
                throw std::runtime_error("TBCR reflected byte array unavailable");
            auto* header_address=static_cast<std::uint8_t*>(object)+info->offset;
            using Header=infinifusion::unreal::TbcrBufferHeader;
            const auto header=safe_read<Header>(header_address);
            if (!header || !header->data || header->num<12 || header->max<header->num || header->max>16*1024*1024)
                throw std::runtime_error("TBCR source header unavailable");
            const auto same_identity=[&]() noexcept {
                const auto now_serial=safe_read_at<std::int32_t>(item(object),0x10);
                const auto now_index=safe_read_at<std::int32_t>(object,0x0C);
                const auto now_name=safe_read_at<FName>(object,0x18);
                const auto now_outer=safe_read_at<void*>(object,0x20);
                return live(object,fresh_source) && now_serial && *now_serial==*serial && now_index && *now_index==*index &&
                    now_name && std::memcmp(&*now_name,&*name,sizeof(FName))==0 && now_outer && *now_outer==*outer &&
                    safe_read_at<void*>(object,0x10).value_or(nullptr)==klass;
            };
            const auto same_source=[&]() noexcept {
                const auto current=safe_read<Header>(header_address); return same_identity() && current && *current==*header;
            };
            smtvv::tbcr::Bytes original(static_cast<std::size_t>(header->num));
            if (!same_source() || !safe_copy(header->data,original.data(),original.size()) || !same_source())
                throw std::runtime_error("TBCR source copy unavailable");
            const auto current_document=smtvv::tbcr::Document::parse(original);
            const auto address=reinterpret_cast<std::uintptr_t>(object);
            const auto outer_address=reinterpret_cast<std::uintptr_t>(*outer);
            const bool new_identity=projection==projections_.end() || projection->second.object!=address ||
                projection->second.index!=*index || projection->second.serial!=*serial || projection->second.outer!=outer_address ||
                std::memcmp(&projection->second.name,&*name,sizeof(FName))!=0;
            if (fresh_source || new_identity) {
                const auto old_cost=projection==projections_.end() ? 0 : projection->second.base.size()+projection->second.last.size();
                if ((projection==projections_.end() && projections_.size()>=128) || cached_bytes_-old_cost+2*original.size()>64*1024*1024)
                    throw std::runtime_error("TBCR copied source baseline capacity exceeded");
                Projection next; next.object=address; next.index=*index; next.serial=*serial; next.name=*name;
                next.outer=outer_address; next.data=reinterpret_cast<std::uintptr_t>(header->data);
                next.base=original; next.last=original;
                projection=projections_.insert_or_assign(path,std::move(next)).first;
                cached_bytes_=cached_bytes_-old_cost+2*original.size();
            } else if (projection->second.data!=reinterpret_cast<std::uintptr_t>(header->data) || projection->second.last!=original) {
                throw std::runtime_error("TBCR source changed outside the owned image; no reappend or baseline overwrite");
            }
            auto candidate=infinifusion::unreal::compose_tbcr_source(projection->second.base,path,definitions);
            if (candidate.bytes.size()>16*1024*1024 || cached_bytes_-projection->second.last.size()+candidate.bytes.size()>64*1024*1024)
                throw std::runtime_error("TBCR composed source capacity exceeded");
            outcome.before_bytes=static_cast<std::uint32_t>(original.size()); outcome.after_bytes=static_cast<std::uint32_t>(candidate.bytes.size());
            outcome.update_records=candidate.update_records; outcome.append_records=candidate.append_records;
            const auto target_document=smtvv::tbcr::Document::parse(candidate.bytes);
            bool same_framing=original.size()==candidate.bytes.size() && current_document.header_size()==target_document.header_size() &&
                current_document.sections().size()==target_document.sections().size() &&
                std::equal(original.begin(),original.begin()+current_document.header_size(),candidate.bytes.begin());
            for (std::size_t i=0;same_framing && i<current_document.sections().size();++i) {
                const auto& before=current_document.sections()[i]; const auto& after=target_document.sections()[i];
                same_framing=before.offset==after.offset &&
                    std::equal(original.begin()+before.offset,original.begin()+before.offset+16,candidate.bytes.begin()+after.offset);
            }
            using Publication=infinifusion::unreal::TbcrPublication;
            Publication publication=Publication::Unchanged; bool cleanup_certain=true;
            if (candidate.bytes!=original && same_framing) {
                std::vector<smtvv::tbcr::RecordEdit> edits;
                std::size_t scratch_size=16;
                for (const auto& section:current_document.sections()) {
                    if (std::equal(original.begin()+section.data_offset,original.begin()+section.data_offset+section.data_bytes,
                        candidate.bytes.begin()+section.data_offset)) continue;
                    for (std::size_t record=0;record<section.record_count;++record) {
                        const auto before=current_document.record(section.index,record), after=target_document.record(section.index,record);
                        if (!std::equal(before.begin(),before.end(),after.begin())) {
                            edits.push_back({section.data_offset+record*section.record_stride,{before.begin(),before.end()},{after.begin(),after.end()}});
                            scratch_size=std::max(scratch_size,before.size());
                        }
                    }
                }
                smtvv::tbcr::Bytes scratch(scratch_size),observed_header(current_document.header_size());
                const auto valid=[&]() noexcept {
                    if (!same_source() || !safe_copy(header->data,observed_header.data(),observed_header.size()) ||
                        !std::equal(observed_header.begin(),observed_header.end(),original.begin())) return false;
                    std::array<std::uint8_t,16> section_header{};
                    for (const auto& section:current_document.sections())
                        if (!safe_copy(static_cast<std::uint8_t*>(header->data)+section.offset,section_header.data(),16) ||
                            !std::equal(section_header.begin(),section_header.end(),original.begin()+section.offset)) return false;
                    return true;
                };
                for (const auto& edit:edits) if (!is_writable_range(static_cast<std::uint8_t*>(header->data)+edit.offset,edit.after.size()))
                    throw std::runtime_error("TBCR source record storage not writable");
                publication=infinifusion::unreal::publish_tbcr_records(edits,scratch,valid,
                    [&](std::size_t offset,std::span<std::uint8_t> bytes) noexcept {return safe_copy(static_cast<std::uint8_t*>(header->data)+offset,bytes.data(),bytes.size());},
                    [&](std::size_t offset,std::span<const std::uint8_t> bytes) noexcept {return write_bytes_safely(static_cast<std::uint8_t*>(header->data)+offset,bytes.data(),bytes.size());});
            } else if (candidate.bytes!=original) {
                EngineAllocator::Certificate certificate;
                if (!is_writable_range(header_address,sizeof(Header)) || !allocator_.certify(certificate))
                    throw std::runtime_error("TBCR engine allocation/header certificate unavailable");
                smtvv::tbcr::Bytes scratch(candidate.bytes.size()), source_check(original.size());
                const auto valid=[&]() noexcept {
                    return same_source() && safe_copy(header->data,source_check.data(),source_check.size()) && source_check==original;
                };
                const auto result=infinifusion::unreal::publish_tbcr_buffer(*header,candidate.bytes,scratch,valid,
                    [&](std::size_t count,void** pointer) noexcept {return allocator_.resize(certificate,nullptr,count,pointer);},
                    [&](void* pointer) noexcept {void* released{}; return allocator_.resize(certificate,pointer,0,&released) && !released;},
                    [&](void* pointer,std::span<std::uint8_t> bytes) noexcept {return safe_copy(pointer,bytes.data(),bytes.size());},
                    [&](void* pointer,std::span<const std::uint8_t> bytes) noexcept {return is_writable_range(pointer,bytes.size()) && write_bytes_safely(pointer,bytes.data(),bytes.size());},
                    [&](Header& output) noexcept {const auto h=same_identity() ? safe_read<Header>(header_address) : std::nullopt; if (!h) return false; output=*h; return true;},
                    [&](const Header& h) noexcept {return same_identity() && write_bytes_safely(header_address,reinterpret_cast<const std::uint8_t*>(&h),sizeof(h));});
                publication=result.state; cleanup_certain=result.cleanup_certain;
            }
            if (publication==Publication::RecoveryUncertain) {
                log_line("ERROR","TBCR source header/record recovery uncertain asset="+path);
                RaiseFailFastException(nullptr,nullptr,0); std::terminate();
            }
            if (publication==Publication::Rejected || publication==Publication::Recovered) {
                outcome.state=cleanup_certain ? IF_TBCR_REJECTED : IF_TBCR_OUTCOME_UNKNOWN; outcome.error=IF_WRITE_FAILED;
                runtime_tbcr_observed(path,outcome,publication==Publication::Recovered ? "publication failed; source recovered" : "publication rejected; source unchanged; allocation cleanup may be uncertain");
                return false;
            }
            source_published=publication==Publication::Applied;
            const auto published=safe_read<Header>(header_address);
            if (!published || !same_identity()) throw std::runtime_error("TBCR source publication identity outcome unknown");
            cached_bytes_=cached_bytes_-projection->second.last.size()+candidate.bytes.size();
            projection->second.last=std::move(candidate.bytes); projection->second.data=reinterpret_cast<std::uintptr_t>(published->data);
            const auto fingerprint=[](std::span<const std::uint8_t> bytes) {std::uint32_t hash=2166136261u; for (auto b:bytes) hash=(hash^b)*16777619u; return hash;};
            const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count();
            outcome.state=cleanup_certain ? IF_TBCR_APPLIED : IF_TBCR_OUTCOME_UNKNOWN; outcome.error=cleanup_certain ? IF_OK : IF_WRITE_FAILED;
            runtime_tbcr_observed(path,outcome,publication==Publication::Unchanged ? "source already matches current definitions" :
                cleanup_certain ? "source published with matched readback before consumer" : "source published; old allocation retirement uncertain");
            log_line(cleanup_certain ? "INFO" : "WARN","TBCR source asset="+path+" phase="+(fresh_source ? "PostLoad" : "before-parser")+
                " application="+std::to_string(outcome.application)+" updates="+std::to_string(outcome.update_records)+" appends="+std::to_string(outcome.append_records)+
                " bytes="+std::to_string(outcome.before_bytes)+"->"+std::to_string(outcome.after_bytes)+
                " before_hash="+hex_value(fingerprint(original))+" after_hash="+hex_value(fingerprint(projection->second.last))+
                " flags="+hex_value(safe_read_at<std::uint32_t>(object,8).value_or(0xFFFFFFFF))+
                " elapsed_us="+std::to_string(elapsed)+" readback="+(publication==Publication::Unchanged ? "unchanged" : "matched"));
            return true;
        } catch (const std::exception& error) {
            outcome.state=source_published ? IF_TBCR_OUTCOME_UNKNOWN : IF_TBCR_REJECTED;
            outcome.error=source_published ? IF_WRITE_FAILED : IF_GUARD_MISMATCH;
            runtime_tbcr_observed(path,outcome,error.what());
            throw;
        }
    }
    bool observe_guarded_cpp(void* object, bool fresh_source) noexcept {
        try { return observe_cpp(object, fresh_source); }
        catch (const std::exception& error) { log_line("WARN",error.what()); }
        catch (...) { log_line("WARN","TBCR initial publication preparation failed"); }
        return false;
    }
    bool observe(void* object, bool fresh_source) noexcept {
        __try { return observe_guarded_cpp(object, fresh_source); }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
public:
    AssetLoadBoundary(Runtime& runtime,DWORD expected_thread):runtime_(runtime),allocator_(runtime_),
#ifdef INFINIFUSION_DEVELOPMENT
        table_probe_(runtime_,expected_thread),
#endif
        table_definitions_(runtime_,expected_thread),message_definitions_(runtime_,expected_thread),expected_thread_(expected_thread) {
        observation_.image_base=runtime.image_base; observation_.image_size=runtime.image_size;
        observation_.exe_timestamp=runtime.exe_timestamp; observation_.object_array=runtime.object_array;
        observation_.append_string=runtime.append_string;
    }
    bool install() {
        trampoline_=VirtualAlloc(nullptr,20,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        if (!trampoline_) return false;
        auto* bytes=static_cast<std::uint8_t*>(trampoline_);
        std::copy(prologue.begin(),prologue.end(),bytes);
        emit_absolute_jump(bytes+prologue.size(),runtime_.image_base+entry_rva+prologue.size());
        DWORD protection{};
        if (!VirtualProtect(trampoline_,20,PAGE_EXECUTE_READ,&protection) ||
            !FlushInstructionCache(GetCurrentProcess(),trampoline_,20)) {
            VirtualFree(trampoline_,0,MEM_RELEASE); trampoline_=nullptr; return false;
        }
        original_=reinterpret_cast<ConditionalPostLoadFn>(trampoline_);
        hook_=std::make_unique<CertifiedEntryHook>(runtime_,"Shared asset boundary",entry_rva,prologue,
            entry_rva+prologue.size(),following,trampoline_,reinterpret_cast<void*>(&asset_postload_wrapper));
        AssetLoadBoundary* expected{};
        if (!g_asset_load_boundary.compare_exchange_strong(expected,this)) return false;
        if (hook_->install()) return true;
        g_asset_load_boundary.store(nullptr);
        VirtualFree(trampoline_,0,MEM_RELEASE); trampoline_=nullptr;
        return false;
    }
    bool before_parser(void* object) noexcept { return observe(object, false); }
    void invoke(void* object) {
        bool selected{};
        const auto flags=safe_read_at<std::uint32_t>(object,8).value_or(0);

        if ((flags&0x1000u) && !(flags&0x10u)) {
#ifdef INFINIFUSION_DEVELOPMENT
            table_probe_.observe(object);
#endif
            table_definitions_.observe(object);
            message_definitions_.observe(object);
            selected=observe(object, true);
        }
        original_(object);
        if ((flags&0x1000u) && GetCurrentThreadId()==expected_thread_) runtime_object_loaded(object);
        if (selected) log_line("INFO","TBCR initial ConditionalPostLoad returned thread="+std::to_string(GetCurrentThreadId()));
    }
};
void __fastcall asset_postload_wrapper(void* object) {
    g_asset_load_boundary.load(std::memory_order_acquire)->invoke(object);
}

void runtime_tbcr_before_parser(void* object) noexcept {
    if (auto* boundary=g_asset_load_boundary.load(std::memory_order_acquire)) boundary->before_parser(object);
}
