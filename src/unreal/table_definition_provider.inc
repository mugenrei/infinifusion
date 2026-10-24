std::vector<infinifusion::core::TablePatchProvider::Definition> runtime_table_definitions();
void runtime_table_observed(const std::string&, IF_TableState, const std::string&);
bool runtime_initial_definitions_ready() noexcept;

class NativeTableDefinitions {
    Runtime runtime_;
    EngineAllocator allocator_;
    ManagedValueBridge values_;
    DWORD thread_{};
    std::recursive_mutex mutex_;
    std::set<void*> active_;
    std::set<void*> quarantined_;
    std::uint64_t applications_{};
    struct Identity { void* object{}; void* klass{}; void* outer{}; FName name{}; int index{},serial{}; };
    const std::uint8_t* item(void* object) {
        const auto index=safe_read_at<std::int32_t>(object,0x0C);
        const auto registry=capture_object_registry(runtime_);
        if (!index || !registry || *index<0 || *index>=registry->header.num_elements) return nullptr;
        const auto chunk=safe_read<void*>(registry->header.objects+*index/kElementsPerChunk).value_or(nullptr);
        return chunk ? static_cast<const std::uint8_t*>(chunk)+(*index%kElementsPerChunk)*kObjectItemSize : nullptr;
    }
    Identity identity(void* object, bool loading) {
        const auto slot=item(object);
        const auto flags=safe_read_at<std::uint32_t>(object,8).value_or(0xFFFFFFFF);
        const auto internal=safe_read_at<std::uint32_t>(slot,8).value_or(0xFFFFFFFF);
        if (!slot || safe_read<void*>(slot).value_or(nullptr)!=object ||
            internal&((1u<<28)|(1u<<29)|(1u<<31)) ||
            ((internal&(1u<<26)) && !(loading && (internal&(1u<<27)))) ||
            flags&(0x10u|0x200u|0x400u|0x4000u|0x8000u|0x10000u|0x20000u|0x08000000u) ||
            (loading && (!(flags&0x1000u) || !(flags&0x80000u))))
            throw std::runtime_error("table source identity/load state unavailable");
        const auto klass=safe_read_at<void*>(object,0x10),outer=safe_read_at<void*>(object,0x20);
        const auto name=safe_read_at<FName>(object,0x18);
        const auto index=safe_read_at<std::int32_t>(object,0x0C),serial=safe_read_at<std::int32_t>(slot,0x10);
        if (!klass || !*klass || !outer || !name || !index || !serial || *serial<0)
            throw std::runtime_error("table source identity unreadable");
        return {object,*klass,*outer,*name,*index,*serial};
    }
    void recertify(const Identity& before, void* structure) {
        const auto now=identity(before.object,true);
        if (before.object!=now.object || before.klass!=now.klass || before.outer!=now.outer ||
            before.index!=now.index || before.serial!=now.serial ||
            std::memcmp(&before.name,&now.name,sizeof(FName)) ||
            safe_read_at<void*>(before.object,0x28).value_or(nullptr)!=structure)
            throw std::runtime_error("table source changed during staging");
    }
    void apply(void* object) {
        if (GetCurrentThreadId()!=thread_ || !runtime_initial_definitions_ready() || quarantined_.contains(object) ||
            !is_asset_class(runtime_,object,"/Script/Engine.DataTable")) return;
        const auto asset=object_path(runtime_,object);
        const auto all=runtime_table_definitions();
        std::vector<infinifusion::core::TablePatchProvider::Definition> selected;
        for (const auto& definition:all) if (definition.asset==asset) selected.push_back(definition);
        if (selected.empty()) return;
        const auto started=std::chrono::steady_clock::now();
        IF_TableState outcome{}; outcome.size=sizeof(outcome); outcome.application=++applications_;
        bool mutation_started{};
        const auto unknown_before=values_.unknown_events();
        try {
            const auto before=identity(object,true);
            auto* structure=safe_read_at<void*>(object,0x28).value_or(nullptr);
            if (!structure) throw std::runtime_error("table RowStruct unavailable");
            const auto before_struct=identity(structure,false);
            const auto map=safe_read_at<SparseContainerHeader>(object,0x30);
            if (!map || map->num_slots>65536 || !sparse_header_valid(*map)) throw std::runtime_error("table RowMap invalid");
            const auto size=safe_read_at<std::int32_t>(structure,0x58).value_or(0);
            if (size<=0 || size>65536) throw std::runtime_error("table row size invalid");
            const auto row_path=object_path(runtime_,structure);
            values_.validate_table(object);
            struct Original { std::string row; FName key{}; void* pointer{}; ManagedValueBridge::Scratch storage; };
            std::vector<Original> originals;
            originals.reserve(selected.size()*2);
            const auto remember=[&](const std::string& row,FName key,void* pointer) {
                for(const auto& prior:originals)
                    if(prior.key.comparison_index==key.comparison_index && prior.key.number==key.number) return;
                Original original; original.row=row; original.key=key; original.pointer=pointer;
                if(pointer) original.storage=values_.scratch(structure,pointer);
                originals.push_back(std::move(original));
            };
            const auto recertify_source=[&] {
                recertify(before,structure);
                const auto now=identity(structure,false);
                if(now.klass!=before_struct.klass || now.outer!=before_struct.outer || now.index!=before_struct.index ||
                    now.serial!=before_struct.serial || std::memcmp(&now.name,&before_struct.name,sizeof(FName)) ||
                    safe_read_at<std::int32_t>(structure,0x58).value_or(0)!=size)
                    throw std::runtime_error("table RowStruct changed during staging");
                values_.validate_table(object);
            };
            struct Prepared {
                FName key{};
                bool remove{}, existed{};
                std::string row;
                ManagedValueBridge::Scratch storage;
            };
            std::vector<Prepared> staged; staged.reserve(selected.size());

            for (const auto& d:selected) {
                for (const auto& guard:d.guards) if (guard!=row_path) throw std::runtime_error("RowStruct guard mismatch row="+d.row);
                const auto key=values_.make_name(d.row);
                auto* existing=find_row(runtime_,object,d.row,key);
                const bool remove=d.operation=="remove";
                if(remove && !d.fields.empty()) throw std::runtime_error("remove row cannot import fields");
                remember(d.row,key,existing);
                if(!d.expected_fields.empty() && (!existing || !values_.matches_expected(structure,existing,d.expected_fields,object)))
                    throw std::runtime_error("table expected fields mismatch/missing row: "+d.row);
                const void* source=existing;
                if (d.operation.starts_with("clone:")) {
                    const auto source_name=d.operation.substr(6);
                    const auto source_key=values_.make_name(source_name);
                    source=find_row(runtime_,object,source_name,source_key);
                    remember(source_name,source_key,const_cast<void*>(source));
                    if (!source) throw std::runtime_error("clone source missing row="+d.row);
                } else if (d.operation=="update" && !existing) throw std::runtime_error("update row missing: "+d.row);
                else if (d.operation!="update" && d.operation!="upsert" && !remove)
                    throw std::runtime_error("unknown row operation: "+d.operation);
                for (const auto& prior:staged) if (prior.key.comparison_index==key.comparison_index && prior.key.number==key.number)
                    throw std::runtime_error("row spellings alias one FName outside the logical ownership normalization: "+d.row);
                Prepared p; p.key=key; p.remove=remove; p.existed=existing!=nullptr; p.row=d.row;
                if (!remove) {
                    p.storage=values_.scratch(structure,source);
                    for (const auto& [name,text]:d.fields) {
                        auto* property=values_.field(structure,name);
                        if (!property) throw std::runtime_error("row="+d.row+" reflected field missing: "+name);
                        const auto info=inspect_property(runtime_,property);
                        if (info.offset<0 || info.offset>=size || info.array_dim<=0 || info.element_size<=0 ||
                            static_cast<std::uint64_t>(info.element_size)*info.array_dim>static_cast<std::uint64_t>(size-info.offset))
                            throw std::runtime_error("row field layout invalid: "+name);
                        values_.import(property,text,static_cast<std::uint8_t*>(p.storage.data())+info.offset,object);
                        ++outcome.imported_fields;
                    }
                }
                staged.push_back(std::move(p));
            }
            recertify_source();
            for(const auto& original:originals) {
                const auto now=find_row(runtime_,object,original.row,original.key);
                if(now!=original.pointer || (now && !values_.identical(structure,now,original.storage.data())))
                    throw std::runtime_error("table original row changed during staging: "+original.row);
            }
            for (auto& p:staged) {
                recertify_source();
                const auto original=std::find_if(originals.begin(),originals.end(),[&](const Original& row) {
                    return row.key.comparison_index==p.key.comparison_index && row.key.number==p.key.number;
                });
                const auto current=find_row(runtime_,object,p.row,p.key);
                if(original==originals.end() || current!=original->pointer ||
                    (current && !values_.identical(structure,current,original->storage.data())))
                    throw std::runtime_error("table target changed before mutation: "+p.row);
                if (p.remove) {
                    if (p.existed) { mutation_started=true; values_.remove_row(object,p.key); ++outcome.removed_rows; }
                    if (find_row(runtime_,object,p.row,p.key)) throw std::runtime_error("row removal readback failed: "+p.row);
                } else {
                    mutation_started=true; values_.add_row(object,p.key,p.storage.data());
                    const auto* published=find_row(runtime_,object,p.row,p.key);
                    if (!published || !values_.identical(structure,published,p.storage.data())) throw std::runtime_error("row publication value readback failed: "+p.row);
                    if (p.existed) ++outcome.updated_rows; else ++outcome.added_rows;
                }
            }
            for(auto& p:staged) p.storage.cleanup();
            for(auto& original:originals) original.storage.cleanup();
            outcome.state=IF_TBCR_APPLIED;
            runtime_table_observed(asset,outcome,"fresh load; staged imports and row membership verified; elapsed_us="+
                std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count()));
        } catch (const std::exception& error) {
            const bool unknown=mutation_started || values_.unknown_events()!=unknown_before;
            if(unknown) quarantined_.insert(object);
            outcome.state=unknown ? IF_TBCR_OUTCOME_UNKNOWN : IF_TBCR_REJECTED;
            outcome.error=unknown ? IF_WRITE_FAILED : IF_INVALID_ARGUMENT;
            runtime_table_observed(asset,outcome,error.what());
        }
    }
    void observe_cpp(void* object) noexcept {
        try { std::unique_lock lock(mutex_,std::try_to_lock); if (lock.owns_lock() && active_.insert(object).second) {
            struct Guard { std::set<void*>& active; void* object; ~Guard(){active.erase(object);} } guard{active_,object};
            apply(object);
        } }
        catch(const std::exception& error) { log_line("WARN",std::string("TABLE source publication unavailable: ")+error.what()); }
        catch(...) { log_line("WARN","TABLE source publication fault; no retry in callback"); }
    }
public:
    NativeTableDefinitions(Runtime& source,DWORD thread):allocator_(runtime_),values_(runtime_,allocator_),thread_(thread) {
        runtime_.image_base=source.image_base; runtime_.image_size=source.image_size;
        runtime_.exe_timestamp=source.exe_timestamp; runtime_.object_array=source.object_array;
        runtime_.append_string=source.append_string;
    }
    void observe(void* object) noexcept {
        __try { observe_cpp(object); }
        __except(EXCEPTION_EXECUTE_HANDLER) { log_line("WARN","TABLE source publication SEH fault; no retry"); }
    }
};
