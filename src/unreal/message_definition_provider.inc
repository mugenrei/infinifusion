std::vector<infinifusion::core::TablePatchProvider::Definition> runtime_message_definitions();
class NativeMessageDefinitions {
    Runtime runtime_;
    EngineAllocator allocator_;
    ManagedValueBridge values_;
    DWORD thread_{};
    std::recursive_mutex mutex_;
    std::set<void*> active_;
    std::set<void*> quarantined_;
    std::uint64_t applications_{};
    void apply(void* object) {
        if (GetCurrentThreadId()!=thread_ || !runtime_initial_definitions_ready() || quarantined_.contains(object)) return;
        auto* klass=safe_read_at<void*>(object,0x10).value_or(nullptr);
        if (!klass || !is_script_message_asset(runtime_,object)) return;
        const auto asset=object_path(runtime_,object);
        const auto definitions=runtime_message_definitions();
        std::vector<infinifusion::core::TablePatchProvider::Definition> selected;
        for(const auto& d:definitions) if(d.asset==asset) selected.push_back(d);
        if(selected.empty()) return;
        const auto started=std::chrono::steady_clock::now();
        IF_TableState outcome{}; outcome.size=sizeof(outcome); outcome.application=++applications_;
        bool mutation_started{};
        std::size_t staged_copies{},expected_copies{},live_copies{},array_copies{},retained_copies{};
        const auto unknown_before=values_.unknown_events();
        try {
            const auto registry=capture_object_registry(runtime_);
            const auto index=safe_read_at<std::int32_t>(object,0x0C).value_or(-1);
            if(!registry || index<0 || index>=registry->header.num_elements || object_at(*registry,index)!=object)
                throw std::runtime_error("message identity unavailable");
            const auto chunk=safe_read<void*>(registry->header.objects+index/kElementsPerChunk).value_or(nullptr);
            const auto slot=chunk ? static_cast<const std::uint8_t*>(chunk)+(index%kElementsPerChunk)*kObjectItemSize : nullptr;
            const auto serial=safe_read_at<std::int32_t>(slot,0x10);
            const auto outer=safe_read_at<void*>(object,0x20);
            const auto object_name=safe_read_at<FName>(object,0x18);
            const auto flags=safe_read_at<std::uint32_t>(object,8).value_or(0xFFFFFFFF);
            const auto internal=safe_read_at<std::uint32_t>(slot,8).value_or(0xFFFFFFFF);
            if(!serial || *serial<0 || !outer || !object_name || internal&((1u<<28)|(1u<<29)|(1u<<31)) ||
                ((internal&(1u<<26)) && !(internal&(1u<<27))) ||
                flags&(0x10u|0x200u|0x400u|0x4000u|0x8000u|0x10000u|0x20000u|0x08000000u) || !(flags&0x1000u) || !(flags&0x80000u))
                throw std::runtime_error("message serialized load state unavailable");
            std::string reason;
            auto* property=match_property(runtime_,klass,"Messages",reason);
            const auto info=property ? inspect_property(runtime_,property) : PropertyInfo{};
            auto* inner=property ? safe_read_at<void*>(property,0x78).value_or(nullptr) : nullptr;
            const auto element=inner ? inspect_property(runtime_,inner) : PropertyInfo{};
            auto* structure=inner ? safe_read_at<void*>(inner,0x78).value_or(nullptr) : nullptr;
            if(info.type!="ArrayProperty" || info.offset<0 || info.array_dim!=1 || info.element_size!=sizeof(FStringHeader) || !structure || element.type!="StructProperty" || element.array_dim!=1 || element.element_size<=0 || element.element_size>65536)
                throw std::runtime_error("message element schema invalid");
            auto* header_address=static_cast<std::uint8_t*>(object)+info.offset;
            const auto header=safe_read<FStringHeader>(header_address);
            if(!header || header->num<0 || header->max<header->num || (!header->data && header->num))
                throw std::runtime_error("Messages array invalid");
            auto* label_prop=match_property(runtime_,structure,"MessageLabel",reason);
            const auto label=label_prop ? inspect_property(runtime_,label_prop) : PropertyInfo{};
            if(label.type!="NameProperty" || label.offset<0 || label.offset>element.element_size-static_cast<int>(sizeof(FName)))
                throw std::runtime_error("message label layout invalid");
            const auto stride=values_.struct_stride(structure);
            if(stride!=static_cast<std::size_t>(element.element_size) ||
                static_cast<std::size_t>(header->max)>std::numeric_limits<std::size_t>::max()/stride ||
                (header->max && (!header->data || !is_readable_range(header->data,stride*static_cast<std::size_t>(header->max)))) ||
                !is_writable_range(header_address,sizeof(FStringHeader)))
                throw std::runtime_error("Messages array storage/stride invalid");
            EngineAllocator::Certificate old_allocator;
            if(!allocator_.certify(old_allocator)) throw std::runtime_error("message allocator certificate unavailable");
            const auto same_key=[](FName a,FName b) { return a.comparison_index==b.comparison_index && a.number==b.number; };
            auto* original=reinterpret_cast<std::uint8_t*>(header->data);
            std::vector<FName> original_labels;
            original_labels.reserve(static_cast<std::size_t>(header->num));
            std::set<std::pair<std::int32_t,std::int32_t>> unique_labels;
            std::map<int,ManagedValueBridge::Scratch> baseline;
            ManagedValueBridge::Scratch retained;
            for(int i=0;i<header->num;++i) {
                auto* source=original+static_cast<std::size_t>(i)*stride;
                const auto key=safe_read_at<FName>(source,label.offset);
                if(!key) throw std::runtime_error("message label unreadable");
                if(!unique_labels.emplace(key->comparison_index,key->number).second)
                    throw std::runtime_error("ambiguous original message FName labels");
                original_labels.push_back(*key);
            }
            const auto find=[&](FName key)->int {
                for(int i=0;i<header->num;++i) if(same_key(original_labels[i],key)) return i;
                return -1;
            };
            const auto base_cell=[&](int i)->void* {
                if(retained.data()) return static_cast<std::uint8_t*>(retained.data())+static_cast<std::size_t>(i)*stride;
                const auto found=baseline.find(i);
                if(found!=baseline.end()) return found->second.data();
                return baseline.emplace(i,values_.scratch(structure,original+static_cast<std::size_t>(i)*stride)).first->second.data();
            };
            struct Entry { int original{-1}; FName label{}; bool remove{}; ManagedValueBridge::Scratch scratch; };
            std::vector<Entry> prepared;
            prepared.reserve(selected.size());
            for(const auto& d:selected) {
                for(const auto& guard:d.guards) if(guard!=object_path(runtime_,structure))
                    throw std::runtime_error("message struct guard mismatch");
                Entry entry; entry.label=values_.make_name(d.row); entry.original=find(entry.label);
                for(const auto& prior:prepared) if(same_key(prior.label,entry.label))
                    throw std::runtime_error("label spellings alias one FName outside logical ownership normalization");
                entry.remove=d.operation=="remove";
                const void* source=entry.original>=0 ? base_cell(entry.original) : nullptr;
                if(!d.expected_fields.empty()) {
                    if(!source || !values_.matches_expected(structure,source,d.expected_fields,object))
                        throw std::runtime_error("message expected fields mismatch/missing label: "+d.row);
                    ++expected_copies;
                }
                if(d.operation.starts_with("clone:")) {
                    const auto from=find(values_.make_name(d.operation.substr(6)));
                    if(from<0) throw std::runtime_error("message clone source missing: "+d.row);
                    source=base_cell(from);
                } else if(d.operation=="update" && !source) throw std::runtime_error("message label missing: "+d.row);
                else if(d.operation!="update" && d.operation!="upsert" && !entry.remove)
                    throw std::runtime_error("unknown message operation: "+d.operation);
                if(entry.remove && !d.fields.empty()) throw std::runtime_error("remove message cannot import fields");
                if(!entry.remove) {
                    entry.scratch=values_.scratch(structure,source);
                    if(source) ++staged_copies;
                    if(!write_bytes_safely(static_cast<std::uint8_t*>(entry.scratch.data())+label.offset,reinterpret_cast<const std::uint8_t*>(&entry.label),sizeof(FName)))
                        throw std::runtime_error("message staged label write failed");
                    for(const auto& [name,text]:d.fields) {
                        auto* field=values_.field(structure,name);
                        const auto leaf=field ? inspect_property(runtime_,field) : PropertyInfo{};
                        if(!field || leaf.offset<0 || leaf.offset>=element.element_size || leaf.element_size<=0 || leaf.array_dim<=0 ||
                            static_cast<std::uint64_t>(leaf.element_size)*leaf.array_dim>static_cast<std::uint64_t>(element.element_size-leaf.offset))
                            throw std::runtime_error("message field layout unavailable: "+name);
                        values_.import(field,text,static_cast<std::uint8_t*>(entry.scratch.data())+leaf.offset,object);
                        ++outcome.imported_fields;
                    }
                    const auto final_key=safe_read_at<FName>(entry.scratch.data(),label.offset);
                    if(!final_key || !same_key(*final_key,entry.label))
                        throw std::runtime_error("MessageLabel field conflicts with label locator: "+d.row);
                }
                prepared.push_back(std::move(entry));
            }
            const auto unchanged=[&] {
                const auto current=safe_read<FStringHeader>(header_address);
                const auto now_serial=safe_read_at<std::int32_t>(slot,0x10);
                const auto now_name=safe_read_at<FName>(object,0x18);
                const auto now_info=inspect_property(runtime_,property);
                const auto now_element=inspect_property(runtime_,inner);
                const auto now_label=inspect_property(runtime_,label_prop);
                if(!current || !now_serial || !now_name || std::memcmp(&*now_name,&*object_name,sizeof(FName)) ||
                    safe_read_at<void*>(object,0x20).value_or(nullptr)!=*outer ||
                    safe_read_at<std::uint32_t>(object,8).value_or(0xFFFFFFFF)!=flags ||
                    safe_read_at<std::uint32_t>(slot,8).value_or(0xFFFFFFFF)!=internal ||
                    now_info.type!=info.type || now_info.offset!=info.offset ||
                    now_info.array_dim!=info.array_dim || now_info.element_size!=info.element_size ||
                    now_label.type!=label.type || now_label.offset!=label.offset ||
                    now_label.array_dim!=label.array_dim || now_label.element_size!=label.element_size ||
                    now_element.type!=element.type || now_element.array_dim!=element.array_dim || now_element.element_size!=element.element_size || *now_serial!=*serial || safe_read<void*>(slot).value_or(nullptr)!=object ||
                    current->data!=header->data || current->num!=header->num || current->max!=header->max ||
                    safe_read_at<void*>(object,0x10).value_or(nullptr)!=klass ||
                    match_property(runtime_,klass,"Messages",reason)!=property ||
                    safe_read_at<void*>(property,0x78).value_or(nullptr)!=inner ||
                    safe_read_at<void*>(inner,0x78).value_or(nullptr)!=structure ||
                    !allocator_.unchanged(old_allocator))
                    throw std::runtime_error("message source changed during staging");
            };
            const auto original_unchanged=[&](int i) {
                const auto found=baseline.find(i);
                const auto snapshot=found!=baseline.end() ? found->second.data() :
                    (retained.data() ? static_cast<std::uint8_t*>(retained.data())+static_cast<std::size_t>(i)*stride : nullptr);
                if(!snapshot || !values_.identical(structure,original+static_cast<std::size_t>(i)*stride,snapshot))
                    throw std::runtime_error("message original value changed during staging");
            };
            const bool membership_changed=std::any_of(prepared.begin(),prepared.end(),[](const Entry& entry) {
                return entry.remove ? entry.original>=0 : entry.original<0;
            });
            if(!membership_changed) {
                unchanged();
                for(const auto& [i,value]:baseline) { (void)value; original_unchanged(i); }
                for(auto& entry:prepared) {
                    if(entry.remove) continue;
                    unchanged();
                    original_unchanged(entry.original);
                    auto* destination=original+static_cast<std::size_t>(entry.original)*stride;
                    if(!values_.identical(structure,destination,entry.scratch.data())) {
                        mutation_started=true;
                        values_.copy(structure,destination,entry.scratch.data());
                        ++live_copies;
                    }
                    if(!values_.identical(structure,destination,entry.scratch.data()))
                        throw std::runtime_error("message whole-entry publication readback failed");
                    ++outcome.updated_rows;
                }
            } else {

                retained=values_.scratch_array(structure,header->num);
                for(int i=0;i<header->num;++i) {
                    const auto touched=baseline.find(i);
                    const auto source=touched!=baseline.end() ? touched->second.data() :
                        original+static_cast<std::size_t>(i)*stride;
                    values_.copy(structure,static_cast<std::uint8_t*>(retained.data())+static_cast<std::size_t>(i)*stride,source);
                    ++retained_copies;
                }
                std::vector<const void*> composed;
                composed.reserve(static_cast<std::size_t>(header->num)+prepared.size());
                for(int i=0;i<header->num;++i) {
                    const auto operation=std::find_if(prepared.begin(),prepared.end(),[&](const Entry& e){return e.original==i;});
                    if(operation==prepared.end()) composed.push_back(base_cell(i));
                    else if(!operation->remove) { composed.push_back(operation->scratch.data()); ++outcome.updated_rows; }
                    else ++outcome.removed_rows;
                }
                for(const auto& entry:prepared) if(entry.original<0 && !entry.remove) {
                    composed.push_back(entry.scratch.data()); ++outcome.added_rows;
                }
                if(composed.size()>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
                    throw std::runtime_error("message count exceeds native int32 capacity");
                auto publication=values_.scratch_array(structure,static_cast<std::int32_t>(composed.size()));
                for(std::size_t i=0;i<composed.size();++i) {
                    values_.copy(structure,static_cast<std::uint8_t*>(publication.data())+i*stride,composed[i]);
                    ++array_copies;
                }
                unchanged();
                for(int i=0;i<header->num;++i) original_unchanged(i);
                FStringHeader replacement{reinterpret_cast<wchar_t*>(publication.data()),
                    static_cast<std::int32_t>(composed.size()),static_cast<std::int32_t>(composed.size())};
                mutation_started=true;

                publication.quarantine();
                if(!write_bytes_safely(header_address,reinterpret_cast<const std::uint8_t*>(&replacement),sizeof(replacement)))
                    throw std::runtime_error("message array header publication outcome unknown");
                const auto observed=safe_read<FStringHeader>(header_address);
                if(!observed || observed->data!=replacement.data || observed->num!=replacement.num || observed->max!=replacement.max)
                    throw std::runtime_error("message array header readback failed");
                for(std::size_t i=0;i<composed.size();++i)
                    if(!values_.identical(structure,reinterpret_cast<std::uint8_t*>(observed->data)+i*stride,composed[i]))
                        throw std::runtime_error("message array entry readback failed");
                publication.release();
                values_.retire_array(structure,header->data,header->num,old_allocator);
            }
            for(auto& entry:prepared) entry.scratch.cleanup();
            for(auto& [i,value]:baseline) { (void)i; value.cleanup(); }
            retained.cleanup();
            outcome.state=IF_TBCR_APPLIED;
            runtime_table_observed(asset,outcome,std::string("fresh ScriptMessage load; ")+(membership_changed ? "engine-owned array membership publication" : "engine-owned touched whole-entry publication")+"; source_copies="+std::to_string(baseline.size())+
                " retained_copies="+std::to_string(retained_copies)+" staged_copies="+std::to_string(staged_copies)+" expected_copies="+std::to_string(expected_copies)+
                " live_copies="+std::to_string(live_copies)+" array_copies="+std::to_string(array_copies)+"; elapsed_us="+
                std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count()));
        } catch(const std::exception& error) {
            const bool unknown=mutation_started || values_.unknown_events()!=unknown_before;
            if(unknown) quarantined_.insert(object);
            outcome.state=unknown ? IF_TBCR_OUTCOME_UNKNOWN : IF_TBCR_REJECTED;
            outcome.error=unknown ? IF_WRITE_FAILED : IF_INVALID_ARGUMENT;
            runtime_table_observed(asset,outcome,error.what());
        }
    }
    void observe_cpp(void* object) noexcept {
        try { std::unique_lock lock(mutex_,std::try_to_lock); if(lock.owns_lock() && active_.insert(object).second) {
            struct Guard { std::set<void*>& active; void* object; ~Guard(){active.erase(object);} } guard{active_,object};
            apply(object);
        } }
        catch(const std::exception& error) { log_line("WARN",std::string("MESSAGE source publication unavailable: ")+error.what()); }
        catch(...) { log_line("WARN","MESSAGE source publication fault; no retry"); }
    }
public:
    NativeMessageDefinitions(Runtime& source,DWORD thread):allocator_(runtime_),values_(runtime_,allocator_),thread_(thread) {
        runtime_.image_base=source.image_base; runtime_.image_size=source.image_size;
        runtime_.exe_timestamp=source.exe_timestamp; runtime_.object_array=source.object_array;
        runtime_.append_string=source.append_string;
    }
    void observe(void* object) noexcept {
        __try { observe_cpp(object); }
        __except(EXCEPTION_EXECUTE_HANDLER) { log_line("WARN","MESSAGE source publication SEH fault; no retry"); }
    }
};
