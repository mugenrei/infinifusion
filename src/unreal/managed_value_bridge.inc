class ManagedValueBridge {
    using StructLifecycle = void(__fastcall*)(void*,void*,std::int32_t);
    using StructCopy = void(__fastcall*)(void*,void*,const void*,std::int32_t);
    using StructCompare = bool(__fastcall*)(void*,const void*,const void*,std::uint32_t);
    using PropertyLifecycle = void(__fastcall*)(void*,void*);
    using PropertyExport = void(__fastcall*)(void*,FStringHeader*,const void*,const void*,void*,std::int32_t,void*);
    using Import = const wchar_t*(__fastcall*)(void*,const wchar_t*,void*,std::int32_t,void*,void*);
    using NameConversion = FName*(__fastcall*)(FName*,const FStringHeader*);
    using AddRow = void(__fastcall*)(void*,FName,const void*);
    using RemoveRow = void(__fastcall*)(void*,FName);
    struct Parameter {
        void* field{}; void* vtable{}; PropertyInfo info;
        void* initialize{}; void* destroy{}; std::size_t size{};
        std::uint32_t alignment{}; std::uint8_t bool_byte{},bool_mask{};
        bool initialized{};
    };
    struct State {
        ManagedValueBridge* bridge{}; void* structure{}; void* memory{};
        std::size_t size{}, stride{}; std::uint32_t alignment{}; std::int32_t count{1}; EngineAllocator::Certificate allocator{};
        void* destroy{}; bool initialized{}; bool unknown{}; bool property{};
        void* property_vtable{}; std::uint64_t property_flags{};
        std::int32_t property_width{},property_dim{};
        bool parameters{},disposed{}; std::vector<Parameter> fields;
    };
    Runtime& runtime_;
    EngineAllocator& allocator_;
    std::vector<State*> owned_;
    std::uint64_t unknown_events_{};
    FStringHeader export_buffer_{};
    EngineAllocator::Certificate export_allocator_{};
    bool export_quarantined_{},export_active_{};
    struct ExportTimings { std::uint64_t layout_us{},buffer_us{},native_us{},copy_us{}; } export_timings_;
    static std::uint64_t elapsed(std::chrono::steady_clock::time_point from) {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-from).count();
    }
    void quarantine_export() noexcept {
        export_buffer_={};export_quarantined_=true;++unknown_events_;
    }
    static bool reset_export(FStringHeader& text) noexcept {
        __try { text.num=0;if(text.data)text.data[0]=L'\0';return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void prepare_export_buffer() {
        if(export_quarantined_)fail("managed export buffer ownership quarantined");
        if(export_buffer_.data) {
            if(!allocator_.unchanged(export_allocator_) || export_buffer_.max<=0 ||
                !is_writable_range(export_buffer_.data,static_cast<std::size_t>(export_buffer_.max)*sizeof(wchar_t))) {
                quarantine_export();fail("managed export buffer allocator/range changed");
            }
        } else {
            if(!allocator_.certify(export_allocator_)) {quarantine_export();fail("managed export buffer allocator unavailable");}
            void* allocated{};
            if(!allocator_.resize(export_allocator_,nullptr,256*sizeof(wchar_t),&allocated) || !allocated ||
                !allocator_.unchanged(export_allocator_) || reinterpret_cast<std::uintptr_t>(allocated)%alignof(wchar_t) ||
                !is_writable_range(allocated,256*sizeof(wchar_t))) {
                quarantine_export();fail("managed export buffer allocation outcome unknown");
            }
            export_buffer_={static_cast<wchar_t*>(allocated),0,256};
        }

        if(!reset_export(export_buffer_)) {quarantine_export();fail("managed export buffer reset outcome unknown");}
    }

    struct ErrorDevice {
        const void* const* vtable{};
        bool suppress_event_tag{true}; bool auto_line_terminator{false};
        std::uint8_t padding[6]{};
        std::uint32_t messages{}; bool unreadable{},truncated{};
        std::array<wchar_t,1024> first{};
    };
    static_assert(offsetof(ErrorDevice,messages)==16);
    static void capture(ErrorDevice* device,const wchar_t* text) noexcept {
        if (++device->messages!=1) return;
        if (!text) { device->unreadable=true; return; }
        for (std::size_t i=0;i+1<device->first.size();++i) {
            const auto unit=safe_read<wchar_t>(text+i);
            if (!unit) { device->unreadable=true; return; }
            device->first[i]=*unit;
            if (!*unit) return;
        }
        device->truncated=true;
    }
    static void* __fastcall sink_destructor(ErrorDevice* device,unsigned int) noexcept { return device; }
    static void __fastcall sink_serialize(ErrorDevice* d,const wchar_t* text,std::uint8_t,const FName*) noexcept { capture(d,text); }
    static void __fastcall sink_timed(ErrorDevice* d,const wchar_t* text,std::uint8_t,const FName*,double) noexcept { capture(d,text); }
    static void __fastcall sink_noop(ErrorDevice*) noexcept {}
    static void __fastcall sink_dump(ErrorDevice*,void*) noexcept {}
    static bool __fastcall sink_true(const ErrorDevice*) noexcept { return true; }
    static bool __fastcall sink_false(const ErrorDevice*) noexcept { return false; }
    static const void* const* sink_vtable() noexcept {
        static const std::array<const void*,9> table{
            reinterpret_cast<const void*>(&sink_destructor),reinterpret_cast<const void*>(&sink_timed),
            reinterpret_cast<const void*>(&sink_serialize),reinterpret_cast<const void*>(&sink_noop),
            reinterpret_cast<const void*>(&sink_noop),reinterpret_cast<const void*>(&sink_dump),
            reinterpret_cast<const void*>(&sink_true),reinterpret_cast<const void*>(&sink_false),
            reinterpret_cast<const void*>(&sink_false)};
        return table.data();
    }
    [[noreturn]] static void fail(const char* message) { throw std::runtime_error(message); }
    bool executable(const void* pointer) const noexcept {
        const auto address=reinterpret_cast<std::uintptr_t>(pointer),base=reinterpret_cast<std::uintptr_t>(runtime_.image_base);
        MEMORY_BASIC_INFORMATION memory{};
        return address>=base && address-base<runtime_.image_size &&
            VirtualQuery(pointer,&memory,sizeof(memory))==sizeof(memory) && memory.State==MEM_COMMIT &&
            !(memory.Protect&(PAGE_GUARD|PAGE_NOACCESS)) &&
            (memory.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
    }
    bool bytes(std::uintptr_t rva,std::string_view expected) const noexcept {
        if (runtime_.exe_timestamp!=kKnownExeTimestamp || runtime_.image_size!=kKnownImageSize ||
            expected.empty() || expected.size()%2 || expected.size()>2048 || !executable(runtime_.image_base+rva)) return false;
        std::array<std::uint8_t,1024> current{};
        const auto count=expected.size()/2;
        if (!safe_copy(runtime_.image_base+rva,current.data(),count)) return false;
        const auto digit=[](char c)->unsigned { return c<='9'?static_cast<unsigned>(c-'0'):static_cast<unsigned>(c-'a'+10); };
        for (std::size_t i=0;i<count;++i)
            if (current[i]!=(digit(expected[i*2])*16+digit(expected[i*2+1]))) return false;
        return true;
    }
    void* entry(std::uintptr_t rva,std::string_view certificate) const {
        if (!bytes(rva,certificate)) fail("managed engine entry certificate mismatch");
        return runtime_.image_base+rva;
    }
    static std::wstring input(std::string_view text,std::size_t limit=16384) {
        if (text.size()>limit*4 || text.find('\0')!=std::string_view::npos) fail("managed import input exceeds bounds or contains NUL");
        auto wide=widen(text);
        if ((!text.empty() && wide.empty()) || wide.size()>limit) fail("managed import invalid UTF-8 or UTF-16 size");
        return wide;
    }
    struct Lifecycle { void* initialize{}; void* destroy{}; std::size_t size{}; std::uint32_t alignment{}; };
    Lifecycle lifecycle(void* structure) const {
        const auto size=safe_read_at<std::int32_t>(structure,0x58).value_or(0);
        const auto alignment=safe_read_at<std::int32_t>(structure,0x5C).value_or(0);
        const auto vtable=safe_read<void*>(structure).value_or(nullptr);
        const auto initialize=vtable?safe_read_at<void*>(vtable,0x2B0).value_or(nullptr):nullptr;
        const auto destroy=vtable?safe_read_at<void*>(vtable,0x2B8).value_or(nullptr):nullptr;
        if (size<=0 || size>65536 || alignment<=0 || alignment>65536 || (alignment&(alignment-1)))
            fail("managed RowStruct size/alignment unsupported");
        const auto regular=entry(0xF4DCC0,initialize_bytes);
        const auto user=entry(0x26B2F20,user_initialize_bytes);
        if ((initialize!=regular && initialize!=user) || destroy!=entry(0xF487F0,destroy_bytes))
            fail("managed RowStruct lifecycle dispatch mismatch");
        const auto aligned=(static_cast<std::size_t>(size)+alignment-1)&~static_cast<std::size_t>(alignment-1);
        if (aligned>65536) fail("managed RowStruct aligned size exceeds bounds");
        return {initialize,destroy,aligned,static_cast<std::uint32_t>(alignment)};
    }
    Lifecycle property_lifecycle(void* property) const {
        const auto info=inspect_property(runtime_,property);
        const auto vtable=safe_read<void*>(property).value_or(nullptr);
        const auto initialize=vtable?safe_read_at<void*>(vtable,0xF8).value_or(nullptr):nullptr;
        const auto destroy=vtable?safe_read_at<void*>(vtable,0xF0).value_or(nullptr):nullptr;
        if(info.element_size<=0 || info.array_dim<=0 || info.array_dim>4096 ||
            static_cast<std::uint64_t>(info.element_size)*info.array_dim>65536 ||
            !executable(initialize) || !executable(destroy))
            fail("managed property lifecycle/layout unavailable");
        const auto alignment_function=vtable?safe_read_at<void*>(vtable,0x110).value_or(nullptr):nullptr;
        std::int32_t alignment{};
        if(!executable(alignment_function) || !invoke_property_alignment(alignment_function,property,&alignment) ||
           alignment<=0 || alignment>65536 || (alignment&(alignment-1)))
            fail("managed property alignment unavailable");
        return {initialize,destroy,static_cast<std::size_t>(info.element_size)*info.array_dim,static_cast<std::uint32_t>(alignment)};
    }
    static bool invoke_property_alignment(void* function,void* property,std::int32_t* alignment) noexcept {
        __try { *alignment=reinterpret_cast<std::int32_t(__fastcall*)(void*)>(function)(property); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool invoke_property_lifecycle(void* function,void* property,void* value) noexcept {
        __try { reinterpret_cast<PropertyLifecycle>(function)(property,value); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool invoke_export(void* function,void* property,FStringHeader* text,const void* source,void* owner) noexcept {
        __try { reinterpret_cast<PropertyExport>(function)(property,text,source,nullptr,owner,1,nullptr); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void quarantine(void* destination) noexcept {
        const auto address=reinterpret_cast<std::uintptr_t>(destination);
        for (auto* state:owned_) {
            const auto begin=reinterpret_cast<std::uintptr_t>(state->memory);
            if (address>=begin && address-begin<state->size) state->unknown=true;
        }
        ++unknown_events_;
    }
    static bool invoke_lifecycle(void* function,void* structure,void* value,std::int32_t count=1) noexcept {
        __try { reinterpret_cast<StructLifecycle>(function)(structure,value,count); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool invoke_copy(void* function,void* structure,void* dest,const void* source) noexcept {
        __try { reinterpret_cast<StructCopy>(function)(structure,dest,source,1); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool invoke_copy_elements(void* function,void* property,void* dest,const void* source,std::int32_t count) noexcept {
        __try { reinterpret_cast<StructCopy>(function)(property,dest,source,count); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool invoke_identical(void* function,void* structure,const void* first,const void* second,bool* result) noexcept {
        __try { *result=reinterpret_cast<StructCompare>(function)(structure,first,second,0); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool invoke_import(void* function,void* property,const wchar_t* text,void* dest,void* owner,ErrorDevice* errors,const wchar_t** result) noexcept {
        __try { *result=reinterpret_cast<Import>(function)(property,text,dest,1,owner,errors); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool invoke_name(void* function,FName* name,const FStringHeader* text,FName** result) noexcept {
        __try { *result=reinterpret_cast<NameConversion>(function)(name,text); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool invoke_add(void* function,void* table,FName name,const void* source) noexcept {
        __try { reinterpret_cast<AddRow>(function)(table,name,source); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool invoke_remove(void* function,void* table,FName name) noexcept {
        __try { reinterpret_cast<RemoveRow>(function)(table,name); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool dispose(State& state) noexcept {
        if(state.disposed)return !state.unknown;
        state.disposed=true;
        const auto at=std::find(owned_.begin(),owned_.end(),&state);
        if (at!=owned_.end()) owned_.erase(at);
        if (!state.memory) { if(state.unknown) ++unknown_events_; return !state.unknown; }
        if(state.parameters) {
            if(!state.unknown) {
                try {
                    if(!allocator_.unchanged(state.allocator) || !is_writable_range(state.memory,state.size) ||
                       reinterpret_cast<std::uintptr_t>(state.memory)%state.alignment)
                        fail("managed parameter storage certificate changed");

                    for(const auto& field:state.fields)validate_parameter(field);
                    for(auto it=state.fields.rbegin();it!=state.fields.rend();++it) {
                        if(!it->initialized || (it->info.flags&0x1000000000ull))continue;
                        validate_parameter(*it);
                        if(!invoke_property_lifecycle(it->destroy,it->field,
                            static_cast<std::uint8_t*>(state.memory)+it->info.offset))
                            fail("managed parameter destruction outcome unknown");
                        it->initialized=false;
                    }
                    void* freed{};
                    if(!allocator_.unchanged(state.allocator) ||
                       !allocator_.resize(state.allocator,state.memory,0,&freed) || freed)
                        fail("managed parameter free outcome unknown");
                }catch(...) {state.unknown=true;}
            }
            if(state.unknown) {
                ++unknown_events_;
                try {log_line("ERROR","managed parameter frame quarantined: no cleanup retry");}catch(...) {}
            }
            state.memory=nullptr;
            return !state.unknown;
        }
        if (!state.unknown) {
            try {
                const auto live=state.property?property_lifecycle(state.structure):lifecycle(state.structure);
                const auto info=state.property?inspect_property(runtime_,state.structure):PropertyInfo{};
                if (live.destroy!=state.destroy || live.size!=state.stride || live.alignment!=state.alignment || !allocator_.unchanged(state.allocator) ||
                    (state.property && (info.flags!=state.property_flags || info.element_size!=state.property_width ||
                    info.array_dim!=state.property_dim || safe_read<void*>(state.structure).value_or(nullptr)!=state.property_vtable))) state.unknown=true;
                else if (state.initialized) {
                    const auto flags=state.property?inspect_property(runtime_,state.structure).flags:0;
                    const bool destroyed=state.property ? ((flags&0x1000000000ull) ||
                        invoke_property_lifecycle(state.destroy,state.structure,state.memory)) :
                        invoke_lifecycle(state.destroy,state.structure,state.memory,state.count);
                    if(!destroyed) state.unknown=true;
                }
                if (!state.unknown) {
                    void* result{};
                    if (!allocator_.resize(state.allocator,state.memory,0,&result) || result) state.unknown=true;
                }
            } catch (...) { state.unknown=true; }
        }
        if (state.unknown) {
            ++unknown_events_;
            try { log_line("ERROR","managed scratch quarantined: engine ownership/lifecycle outcome unknown; no cleanup retry"); } catch (...) {}
        }
        state.memory=nullptr;
        return !state.unknown;
    }
    void validate_parameter(const Parameter& field) const {
        const auto current=inspect_property(runtime_,field.field);
        if(current.name!=field.info.name || current.type!=field.info.type || current.offset!=field.info.offset ||
           current.flags!=field.info.flags || current.element_size!=field.info.element_size ||
           current.array_dim!=field.info.array_dim || safe_read<void*>(field.field).value_or(nullptr)!=field.vtable)
            fail("managed parameter schema changed");
        const auto live=property_lifecycle(field.field);
        if(live.initialize!=field.initialize || live.destroy!=field.destroy || live.size!=field.size ||
           live.alignment!=field.alignment)
            fail("managed parameter lifecycle changed");
        if(field.bool_mask && (safe_read_at<std::uint8_t>(field.field,0x79).value_or(255)!=field.bool_byte ||
            safe_read_at<std::uint8_t>(field.field,0x7B).value_or(0)!=field.bool_mask))
            fail("managed parameter boolean layout changed");
    }
    void* table_entry(void* table,std::size_t slot,std::uintptr_t expected,std::string_view certificate) const {
        const auto klass=safe_read_at<void*>(table,0x10).value_or(nullptr);
        const auto vtable=safe_read<void*>(table).value_or(nullptr);
        if (!klass || !is_asset_class(runtime_,table,"/Script/Engine.DataTable"))
            fail("managed table class ancestry mismatch");
        const auto function=vtable?safe_read_at<void*>(vtable,slot*sizeof(void*)).value_or(nullptr):nullptr;
        if (function!=entry(expected,certificate)) fail("managed table operation dispatch mismatch");
        return function;
    }
public:
    class Scratch {
        friend class ManagedValueBridge;
        std::unique_ptr<State> state_;
        explicit Scratch(std::unique_ptr<State> state):state_(std::move(state)) {}
    public:
        Scratch()=default;
        Scratch(const Scratch&)=delete;
        Scratch& operator=(const Scratch&)=delete;
        Scratch(Scratch&&)=default;
        Scratch& operator=(Scratch&& other) noexcept {
            if (this!=&other) { if (state_) state_->bridge->dispose(*state_); state_=std::move(other.state_); }
            return *this;
        }
        ~Scratch() noexcept { if (state_) state_->bridge->dispose(*state_); }
        void* data() const noexcept { return state_?state_->memory:nullptr; }
        void* structure() const noexcept { return state_?state_->structure:nullptr; }
        bool uncertain() const noexcept { return state_ && state_->unknown; }
        void quarantine() noexcept { if (state_) state_->unknown=true; }
        void release() noexcept {
            if (!state_) return;
            auto& owned=state_->bridge->owned_;
            owned.erase(std::remove(owned.begin(),owned.end(),state_.get()),owned.end());
            state_->memory=nullptr; state_.reset();
        }
        void cleanup() {
            if (state_ && !state_->bridge->dispose(*state_)) fail("managed scratch cleanup outcome unknown");
            state_.reset();
        }
    };
    using ParameterFrame=Scratch;
    ManagedValueBridge(Runtime& runtime,EngineAllocator& allocator):runtime_(runtime),allocator_(allocator) {}
    ~ManagedValueBridge() noexcept { try {release_export_buffer();} catch(...) {} }
    void release_export_buffer() {
        if(export_active_)fail("managed export buffer release during export");
        if(export_quarantined_)return;
        if(!export_buffer_.data)return;
        EngineAllocator::Certificate fresh{};void* freed{};
        auto* memory=export_buffer_.data;export_buffer_={};
        if(!allocator_.unchanged(export_allocator_) || !allocator_.certify(fresh) ||
            !allocator_.unchanged(fresh) || !allocator_.resize(fresh,memory,0,&freed) || freed) {
            quarantine_export();fail("managed export buffer free outcome unknown");
        }
    }
    ManagedValueBridge(const ManagedValueBridge&)=delete;
    ManagedValueBridge& operator=(const ManagedValueBridge&)=delete;
    std::uint64_t unknown_events() const noexcept { return unknown_events_; }
    auto export_timings() const {return export_timings_;}
    void reset_export_timings() {export_timings_={};}
    std::size_t struct_stride(void* structure) const { return lifecycle(structure).size; }
    ParameterFrame parameter_frame(std::size_t size,std::span<void* const> fields) {
        if(size>65536 || fields.size()>512 || (!size && !fields.empty()))
            fail("managed parameter frame size/count unsupported");
        auto state=std::make_unique<State>();state->bridge=this;state->parameters=true;
        state->size=size;state->alignment=1;
        for(auto* property:fields) {
            const auto info=inspect_property(runtime_,property);
            const auto live=property_lifecycle(property);
            if(!(info.flags&0x80ull) || info.offset<0 || static_cast<std::size_t>(info.offset)>size ||
               live.size>size-static_cast<std::size_t>(info.offset) ||
               static_cast<std::size_t>(info.offset)%live.alignment)
                fail("managed parameter offset/size/alignment invalid");
            Parameter field;field.field=property;field.info=info;field.vtable=safe_read<void*>(property).value_or(nullptr);
            field.initialize=live.initialize;field.destroy=live.destroy;field.size=live.size;field.alignment=live.alignment;
            if(info.type=="BoolProperty" && info.array_dim==1 && info.element_size==1 &&
               (info.flags&0x200ull) && (info.flags&0x1000000000ull)) {
                field.bool_byte=safe_read_at<std::uint8_t>(property,0x79).value_or(255);
                field.bool_mask=safe_read_at<std::uint8_t>(property,0x7B).value_or(0);
                if(field.bool_byte || !field.bool_mask)fail("managed parameter boolean layout unsupported");
            }
            for(const auto& prior:state->fields) {
                if(prior.field==property)fail("duplicate managed parameter property");
                const auto start=static_cast<std::size_t>(info.offset),other=static_cast<std::size_t>(prior.info.offset);
                if(start<other+prior.size && other<start+live.size &&
                   !(field.bool_mask && prior.bool_mask && start==other && !(field.bool_mask&prior.bool_mask)))
                    fail("overlapping managed parameter properties");
            }
            state->alignment=std::max(state->alignment,live.alignment);state->fields.push_back(std::move(field));
        }
        if(!allocator_.certify(state->allocator))fail("managed parameter allocator unavailable");
        owned_.push_back(state.get());ParameterFrame frame(std::move(state));auto& current=*frame.state_;
        if(!size)return frame;
        if(!allocator_.resize(current.allocator,nullptr,size,&current.memory,current.alignment) || !current.memory) {
            current.unknown=true;fail("managed parameter allocation outcome unknown");
        }
        if(!allocator_.unchanged(current.allocator) || reinterpret_cast<std::uintptr_t>(current.memory)%current.alignment ||
           !is_writable_range(current.memory,size)) {
            current.unknown=true;fail("managed parameter allocation certificate changed");
        }
        std::memset(current.memory,0,size);
        for(auto& field:current.fields) {
            try {validate_parameter(field);}catch(...) {current.unknown=true;throw;}
            if(!(field.info.flags&0x200ull) && !invoke_property_lifecycle(field.initialize,field.field,
                static_cast<std::uint8_t*>(current.memory)+field.info.offset)) {
                current.unknown=true;fail("managed parameter initialization outcome unknown");
            }
            field.initialized=true;
        }
        return frame;
    }
    Scratch scratch(void* structure,const void* source=nullptr) {
        auto result=scratch_array(structure,1);
        if (source) copy(structure,result.data(),source);
        return result;
    }
    Scratch scratch_property(void* property,const void* source=nullptr) {
        const auto live=property_lifecycle(property);
        auto state=std::make_unique<State>();
        state->bridge=this; state->structure=property; state->stride=live.size;
        state->size=live.size; state->alignment=live.alignment; state->destroy=live.destroy; state->property=true;
        const auto info=inspect_property(runtime_,property);
        state->property_flags=info.flags; state->property_width=info.element_size;
        state->property_dim=info.array_dim; state->property_vtable=safe_read<void*>(property).value_or(nullptr);
        if(!allocator_.certify(state->allocator)) fail("managed property allocator certificate mismatch");
        owned_.push_back(state.get()); Scratch result(std::move(state)); auto& current=*result.state_;
        if(!allocator_.resize(current.allocator,nullptr,current.size,&current.memory,live.alignment) || !current.memory) {
            current.unknown=true; fail("managed property allocation outcome unknown");
        }
        if(reinterpret_cast<std::uintptr_t>(current.memory)%live.alignment || !is_writable_range(current.memory,current.size))
            fail("managed property allocation range/alignment unavailable");
        const auto flags=inspect_property(runtime_,property).flags;

        if(flags&0x200ull) std::memset(current.memory,0,current.size);
        else if(!invoke_property_lifecycle(live.initialize,property,current.memory)) {
            current.unknown=true; fail("managed property initialization outcome unknown");
        }
        current.initialized=true;
        if(source) copy_property(property,current.memory,source);
        return result;
    }
    void copy_property(void* property,void* destination,const void* source) {
        const auto live=property_lifecycle(property);
        if(!destination || !source || !is_writable_range(destination,live.size) || !is_readable_range(source,live.size))
            fail("managed property copy range unavailable");
        if(destination==source) return;
        const auto info=inspect_property(runtime_,property);
        if(info.flags&0x40000000ull) std::memcpy(destination,source,live.size);
        else {
            const auto vtable=safe_read<void*>(property).value_or(nullptr);
            const auto function=vtable?safe_read_at<void*>(vtable,0xB8).value_or(nullptr):nullptr;
            if(!executable(function)) fail("managed property copy dispatch unavailable");
            if(!invoke_copy_elements(function,property,destination,source,info.array_dim)) {
                quarantine(destination); fail("managed property copy outcome unknown");
            }
        }
    }
    bool identical_property(void* property,const void* first,const void* second,bool single_element=false) {
        const auto live=property_lifecycle(property);
        const auto info=inspect_property(runtime_,property);
        const auto vtable=safe_read<void*>(property).value_or(nullptr);
        const auto function=vtable?safe_read_at<void*>(vtable,0x88).value_or(nullptr):nullptr;
        const auto width=single_element?static_cast<std::size_t>(info.element_size):live.size;
        if(!first || !second || !is_readable_range(first,width) || !is_readable_range(second,width) || !executable(function))
            fail("managed property comparison range/dispatch unavailable");
        for(int i=0;i<(single_element?1:info.array_dim);++i) {
            bool same{};
            if(!invoke_identical(function,property,static_cast<const std::uint8_t*>(first)+i*info.element_size,
                static_cast<const std::uint8_t*>(second)+i*info.element_size,&same))
                fail("managed property comparison outcome unknown");
            if(!same) return false;
        }
        return true;
    }
    std::string export_property(void* property,const void* source,void* owner,bool single_element=false) {
        const auto layout_started=std::chrono::steady_clock::now();
        const auto live=property_lifecycle(property);
        const auto info=inspect_property(runtime_,property);
        const auto vtable=safe_read<void*>(property).value_or(nullptr);
        const auto function=vtable?safe_read_at<void*>(vtable,0xA8).value_or(nullptr):nullptr;
        if(!source || !is_readable_range(source,single_element?static_cast<std::size_t>(info.element_size):live.size) || !executable(function))
            fail("managed property export range/dispatch unavailable");
        if(export_active_)fail("managed export buffer synchronous reentry unavailable");
        if(export_quarantined_)fail("managed export buffer ownership quarantined");
        export_active_=true;
        struct ExportGuard { bool& active;~ExportGuard(){active=false;} } guard{export_active_};
        export_timings_.layout_us+=elapsed(layout_started);
        std::string result;
        const auto count=single_element?1:info.array_dim;
        if(count>1) result='(';
        for(int i=0;i<count;++i) {
            const auto buffer_started=std::chrono::steady_clock::now();
            prepare_export_buffer();
            export_timings_.buffer_us+=elapsed(buffer_started);
            auto text=export_buffer_;

            export_buffer_={};
            const auto native_started=std::chrono::steady_clock::now();
            if(!invoke_export(function,property,&text,static_cast<const std::uint8_t*>(source)+i*info.element_size,owner)) {
                quarantine_export(); fail("managed property export allocation outcome unknown");
            }
            export_timings_.native_us+=elapsed(native_started);
            const auto copy_started=std::chrono::steady_clock::now();

            try {
                if(text.num<0 || text.max<text.num || text.max>1024*1024 ||
                    text.num>65536 || ((text.max>0)!=bool(text.data)) ||
                    !allocator_.unchanged(export_allocator_) ||
                    (text.data && (reinterpret_cast<std::uintptr_t>(text.data)%alignof(wchar_t) ||
                        !is_writable_range(text.data,static_cast<std::size_t>(text.max)*sizeof(wchar_t))))) {

                    fail("managed export string header/allocator invalid; quarantined");
                }
                std::wstring copied;
                if(!text.num && text.data && safe_read<wchar_t>(text.data).value_or(L'x')!=L'\0')
                    fail("managed empty export buffer unterminated");
                if(text.num) {
                    copied.resize(text.num);
                    if(!safe_copy(text.data,copied.data(),copied.size()*sizeof(wchar_t)) || copied.back()!=L'\0')
                        fail("managed export string unreadable or unterminated");
                    copied.pop_back();
                }
                const auto utf8=narrow(copied);
                const std::size_t punctuation=(i?1:0)+(count>1?1:0);
                if((!copied.empty() && utf8.empty()) || utf8.find('\0')!=std::string::npos ||
                    utf8.size()>65536 || result.size()>65536 || punctuation>65536-result.size() ||
                    utf8.size()>65536-result.size()-punctuation)
                    fail("managed export string encoding/size invalid");
                if(i) result+=',';
                result+=utf8;
                export_buffer_=text;
                export_timings_.copy_us+=elapsed(copy_started);
            } catch(...) {
                quarantine_export();
                throw;
            }
        }
        if(count>1) result+=')';
        return result;
    }
    Scratch scratch_array(void* structure,std::int32_t count) {
        if (count<0) fail("managed array count invalid");
        const auto live=lifecycle(structure);
        auto state=std::make_unique<State>();
        state->bridge=this; state->structure=structure; state->stride=live.size; state->alignment=live.alignment; state->count=count; state->destroy=live.destroy;
        if (static_cast<std::size_t>(count)>std::numeric_limits<std::size_t>::max()/live.size)
            fail("managed array byte size overflow");
        state->size=live.size*static_cast<std::size_t>(count);

        if (state->size>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
            fail("managed array exceeds native lifecycle byte capacity");
        if (!allocator_.certify(state->allocator)) fail("managed scratch allocator certificate mismatch");
        owned_.push_back(state.get());
        Scratch result(std::move(state));
        auto& current=*result.state_;
        if (!count) return result;
        if (!allocator_.resize(current.allocator,nullptr,current.size,&current.memory,live.alignment) || !current.memory) {
            current.unknown=true; fail("managed scratch allocation outcome unknown");
        }
        if (reinterpret_cast<std::uintptr_t>(current.memory)%live.alignment || !is_writable_range(current.memory,current.size)) {

            fail("managed scratch allocation range/alignment mismatch");
        }
        if (!invoke_lifecycle(live.initialize,structure,current.memory,count)) {
            current.unknown=true; fail("managed scratch initialization outcome unknown");
        }
        current.initialized=true;
        return result;
    }
    void retire_array(void* structure,void* memory,std::int32_t count,const EngineAllocator::Certificate& certificate) {
        const auto live=lifecycle(structure);
        if (count<0 || !allocator_.unchanged(certificate) ||
            static_cast<std::size_t>(count)>std::numeric_limits<std::size_t>::max()/live.size ||
            (count && (!memory || !is_writable_range(memory,live.size*static_cast<std::size_t>(count)))))
            { ++unknown_events_; fail("managed old array cleanup guard changed; quarantined"); }
        if (!memory) return;
        if (count && !invoke_lifecycle(live.destroy,structure,memory,count))
            { ++unknown_events_; fail("managed old array destruction outcome unknown; quarantined"); }
        void* returned{};
        if (!allocator_.resize(certificate,memory,0,&returned) || returned)
            { ++unknown_events_; fail("managed old array free outcome unknown; quarantined"); }
    }
    void copy(void* structure,void* destination,const void* source) {
        const auto live=lifecycle(structure);
        if (!destination || !source || !is_writable_range(destination,live.size) || !is_readable_range(source,live.size))
            fail("managed copy range unavailable");
        if (destination==source) return;
        if (!invoke_copy(entry(0xF47580,copy_bytes),structure,destination,source)) {
            quarantine(destination); fail("managed copy outcome unknown");
        }
    }
    FName make_name(std::string_view text) {
        auto wide=input(text,1023);
        if (wide.empty()) fail("managed row key is empty");
        FStringHeader borrowed{wide.data(),static_cast<std::int32_t>(wide.size()+1),static_cast<std::int32_t>(wide.size()+1)};
        FName name{}; FName* returned{};
        if (!invoke_name(entry(0x23146B0,name_bytes),&name,&borrowed,&returned)) fail("managed name construction outcome unknown");
        if (returned!=&name || name.comparison_index<=0) fail("managed row key resolved to None or invalid name");
        return name;
    }
    bool identical(void* structure,const void* first,const void* second) {
        const auto live=lifecycle(structure);
        if (!first || !second || !is_readable_range(first,live.size) || !is_readable_range(second,live.size))
            fail("managed row comparison range unavailable");
        bool result{};
        if (!invoke_identical(entry(0xF46BE0,identical_bytes),structure,first,second,&result))
            fail("managed row comparison outcome unknown");
        return result;
    }
    void* field(void* structure,std::string_view requested) {
        void* selected{};
        if(!visit_property_names(structure,[&](void* property,const FName& name) {
            if(infinifusion::unreal::reflected_name_equal(fname_to_string(runtime_,name),requested)) {selected=property;return false;}
            return true;
        })) fail("managed reflected property chain unreadable or cyclic");
        return selected;
    }
    void import(void* property,std::string_view text,void* destination,void* owner,bool single_element=false) {
        using namespace infinifusion::unreal;
        ImportCallbacks callbacks;
        callbacks.describe=[&](std::uintptr_t token) {
            auto* current=reinterpret_cast<void*>(token);
            const auto info=inspect_property(runtime_,current);
            if(info.array_dim<=0 || info.array_dim>4096 || info.element_size<=0 || info.element_size>65536)
                fail("managed import reflected layout invalid");
            ImportSchema schema; schema.fixed_count=single_element && current==property?1:static_cast<std::size_t>(info.array_dim);
            if(info.type=="StructProperty") schema.kind=ImportKind::Struct;
            else if(info.type=="ArrayProperty") schema.kind=ImportKind::Array;
            else if(info.type=="SetProperty") schema.kind=ImportKind::Set;
            else if(info.type=="MapProperty") schema.kind=ImportKind::Map;
            if(schema.kind==ImportKind::Array || schema.kind==ImportKind::Set)
                schema.inner=reinterpret_cast<std::uintptr_t>(safe_read_at<void*>(current,0x78).value_or(nullptr));
            else if(schema.kind==ImportKind::Map) {
                schema.key=reinterpret_cast<std::uintptr_t>(safe_read_at<void*>(current,0x78).value_or(nullptr));
                schema.value=reinterpret_cast<std::uintptr_t>(safe_read_at<void*>(current,0x80).value_or(nullptr));
            }
            return schema;
        };
        callbacks.member=[&](std::uintptr_t token,std::string_view name) {
            auto* structure=safe_read_at<void*>(reinterpret_cast<void*>(token),0x78).value_or(nullptr);
            return reinterpret_cast<std::uintptr_t>(structure ? field(structure,name) : nullptr);
        };
        callbacks.scalar_validation=[&](std::uintptr_t token,std::string_view leaf) {
            auto* current=reinterpret_cast<void*>(token);
            const auto type=field_class_name(runtime_,current);
            if(type=="EnumProperty") {
                if(leaf.empty() || leaf.front()!='"')
                    validate_property_numeric(field_class_name(runtime_,safe_read_at<void*>(current,0x78).value_or(nullptr)),leaf);
            } else if(type!="ByteProperty" || leaf.empty() || leaf.front()!='"' || !safe_read_at<void*>(current,0x78).value_or(nullptr))
                validate_property_numeric(type,leaf);
            if(type=="ObjectProperty" || type=="ClassProperty") {

                void* resolved{};
                if(safe_read_at<std::int32_t>(current,0x3C).value_or(0)!=sizeof(resolved)) fail("hard reference layout invalid");
                import_one(current,leaf,&resolved,owner);
            }
        };
        const auto prepared=prepare_property_import(reinterpret_cast<std::uintptr_t>(property),text,callbacks);
        const auto stride=safe_read_at<std::int32_t>(property,0x3C).value_or(0);
        if(stride<=0 || !destination || !is_writable_range(destination,static_cast<std::size_t>(stride)*prepared.size()))
            fail("managed import destination range unavailable");
        for(std::size_t i=0;i<prepared.size();++i)
            import_one(property,prepared[i],static_cast<std::uint8_t*>(destination)+i*stride,owner);
    }
    void import_one(void* property,std::string_view text,void* destination,void* owner) {
        auto wide=input(text);
        const auto vtable=safe_read<void*>(property).value_or(nullptr);
        const auto function=vtable?safe_read_at<void*>(vtable,0xB0).value_or(nullptr):nullptr;
        const auto size=safe_read_at<std::int32_t>(property,0x3C).value_or(0);
        if (!destination || size<=0 || size>65536 || !is_writable_range(destination,static_cast<std::size_t>(size)) || !executable(function))
            fail("managed import property dispatch/range unavailable");
        ErrorDevice errors; errors.vtable=sink_vtable(); const wchar_t* cursor{};
        if (!invoke_import(entry(0x803620,import_bytes),property,wide.c_str(),destination,owner,&errors,&cursor)) {
            quarantine(destination); fail("managed import outcome unknown");
        }
        if (errors.messages) {
            const auto diagnostic=narrow(std::wstring_view(errors.first.data()));
            throw std::runtime_error("managed import rejected: "+diagnostic+(errors.truncated?" [diagnostic truncated]":""));
        }
        const auto begin=reinterpret_cast<std::uintptr_t>(wide.c_str()),end=begin+wide.size()*sizeof(wchar_t),at=reinterpret_cast<std::uintptr_t>(cursor);
        if (!cursor || at<begin || at>end || (at-begin)%sizeof(wchar_t)) fail("managed import returned invalid/failure cursor");

        while (*cursor && std::iswspace(*cursor)) ++cursor;
        if (*cursor) fail("managed import did not consume complete value");
        const auto type=field_class_name(runtime_,property);
        if ((type=="ObjectProperty" || type=="ClassProperty") &&
            !safe_read<void*>(destination).value_or(nullptr) && text!="None" && text!="\"None\"")
            fail("managed hard reference did not resolve");
    }
    bool matches_expected(void* structure,const void* source,
        const std::map<std::string,std::string>& expected,void* owner) {
        if (expected.empty()) return true;
        if (!source) return false;
        auto staged=scratch(structure,source);
        const auto size=struct_stride(structure);
        for (const auto& [name,text]:expected) {
            auto* property=field(structure,name);
            const auto info=property ? inspect_property(runtime_,property) : PropertyInfo{};
            if (!property || info.offset<0 || static_cast<std::size_t>(info.offset)>=size ||
                info.array_dim<=0 || info.element_size<=0 ||
                static_cast<std::uint64_t>(info.element_size)*info.array_dim>size-static_cast<std::size_t>(info.offset))
                throw std::runtime_error("expected field layout unavailable: "+name);
            import(property,text,static_cast<std::uint8_t*>(staged.data())+info.offset,owner);
        }
        const bool matches=identical(structure,staged.data(),source);
        staged.cleanup();
        return matches;
    }
    void validate_table(void* table) const {
        (void)table_entry(table,85,0x21FD920,add_bytes);
        (void)table_entry(table,84,0x2216290,remove_bytes);
    }
    void add_row(void* table,FName key,const void* source) {
        const auto structure=safe_read_at<void*>(table,0x28).value_or(nullptr);
        const auto live=lifecycle(structure);
        const auto staged=std::find_if(owned_.begin(),owned_.end(),[&](const State* state) {
            return state->memory==source && state->structure==structure && state->initialized && !state->unknown;
        });
        if (!source || staged==owned_.end() || !is_readable_range(source,live.size))
            fail("managed AddRow requires a compatible owned scratch row");
        if (!invoke_add(table_entry(table,85,0x21FD920,add_bytes),table,key,source)) fail("managed AddRow mutation outcome unknown");
    }
    void remove_row(void* table,FName key) {
        if (!invoke_remove(table_entry(table,84,0x2216290,remove_bytes),table,key)) fail("managed RemoveRow mutation outcome unknown");
    }
private:

    static constexpr std::string_view import_bytes = "48895c240848896c2410488974241848897c242041564883ec30488b6c24684d8bf0488bda4c8bc5418bd1418bf9488bf1e8da27890084c074304885db742b488b4424600fbaef1b4c8b16448bcf48896c24284d8bc6488bd34889442420488bce41ff92b0000000eb0233c0488b5c2440488b6c2448488b742450488b7c24584883c430415ec3";
    static constexpr std::string_view copy_bytes = "405341544155415641574883ec40448b515c4d8bf0458be14c8bfa488bd9458d6aff41f7da440369584523eaf781b000000000100000741b488b89b8000000458bcc488b01ff90b000000084c00f850d010000f783b000000000200000741f450fafec498bd6498bcf4d63c54883c440415f415e415d415c5be95ceb0702488b4350488d4c242048894424284889bc248000000048895c2420c7442430ffffffff66c74424340101c644243600e8eecf8bff488b7c24284885ff0f849800000048896c24704889742478660f1f4400004585e47e5c33db4963ed498bf40f1f0048634f4c4803cb4a8d14314e8d14394c3bd274348b4740480fbae01e73148b473c498bca0faf47384c63c0e8caea0702eb16488b074c8bc2448b4f38498bd2488bcfff90b80000004803dd4883ee0175af488b4720488d4c24204889442428e85ccf8bff488b7c24284885ff7582488b742478488b6c2470488bbc24800000004883c440415f415e415d415c5bc3";
    static constexpr std::string_view initialize_bytes = "4889542410535556574154415541564883ec20448b495c488bda8b71584c8bf1ffce458be04103f133d241f7d9488bcb4123f18bc689742470410fafc04c63c0e861840702498bbeb80000004533ed4885ff7439488b07488bcfff501084c075284585e47e234863ee418bf40f1f4000488b07488bd3488bcfff50184803dd4883ee0175eb8b742470448b6f0845396e580f8e93000000498b5e704c897c24604532ff4885db0f84790000004c8b7424684584ff756f8b43380faf433c03434c413bc57e504585e47e4e4863ee498bfe498bf48b4340480fbae009731a8b433833d20faf433c48634b4c4803cf4c63c0e8b1830702eb134863534c488bcb488b034803d7ff90f80000004803fd4883ee0175c0eb0341b701488b5b588b7424704885db758c4c8b7c24604883c420415e415d415c5f5e5d5bc3";
    static constexpr std::string_view user_initialize_bytes = "405355564883ec20418be8488bda488bf1e88aad89fe4885db0f84af00000048897c2440488dbed8000000488b07488bcf4c89742450ff503884c07409488b07488bcfff5020488b07488bcfff50104c8bf04885c0746d8b565c448d42fff7da440346584423c285ed7e594c897c2458488bfd4d63f866660f1f84000000000041b9010000004d8bc6488bd3488bcee8cc4589fe488bd3488d4c2448e8af099afe488bd3488bcee814bc85fe488d4c2448e8ea0c9afe4903df4883ef0175c14c8b7c2458488b7c24404c8b7424504883c4205e5d5bc3";
    static constexpr std::string_view destroy_bytes = "4055415441574883ec30f781b0000000006000004c8be2458bf8488be90f852f0100008b415c8b495848895c2458488b9db800000048897424608d70ff03f148897c2468f7d84c896c242823f04c897424204533ed897424504885db7442488b03488bcbff502884c0742e4585ff7e294c63f6498bfc418bf70f1f8000000000488b03488bd7488bcbff50304903fe4883ee0175eb8b742450448b6b088b4d58413bcd0f8e90000000488b9d800000004532f64885db0f847d00000048b900000000100000004584f6756e8b433c0faf433803434c413bc57e4f48854b40754c4585ff7e474863ee33ff498bf70f1f00488b434048c1e824f6d0a80174164863534c488bcb488b034803d74903d4ff90f00000004803fd4883ee0175d348b90000000010000000eb0341b601488b5b688b7424504885db758d4c8b6c2428488b7c2468488b742460488b5c24584c8b7424204883c430415f415c5dc3";
    static constexpr std::string_view name_bytes = "40534883ec20837a0800488bd97405488b12eb07488d1595a9d90041b801000000e8aa29bafe488bc34883c4205bc3";
    static constexpr std::string_view add_bytes = "48895c241048896c2418565741564883ec20488bda488bf1488bd1498be8488d4c2440e868caffff4c8b76284d85f6751f8b4608c1e804a801750d488bcee81d07ebfee80817e6fee8136054004c8bf04c8b06488bd3488bce41ff90a0020000418b565c458b465841ffc84403c2f7da4423c233d24963c8e83389bcfe4d8b1641b801000000488bd0498bce488bf841ff92b002000041b9010000004c8bc5488bd7498bcee8b69bd4fe4c8b0e4c8bc7488bd3488bce41ff9178020000488d0d94c93102ff15966de700488b5c2440488d5424404c8bc3488d0d7270f101e87dda6cfe48634424404533c083f8ff7411488d1440488b055570f101488d0cd0eb03498bc883690801751f498bd04c89442440488bcbe8a6c30000488bd3488d0d2c70f101e887215bfe488d0d28c93102488b5c2448488b6c24504883c420415e5f5e48ff251f6de700";
    static constexpr std::string_view remove_bytes = "48895c241848897424204889542410574883ec20488bda488bf9488bd1488d4c2430e8f940feff488b77284885f6751f8b4708c1e804a801750d488bcfe8ae7de9fee8998de4fee8a4d65200488bf08b4f383b4f640f84da0000008bcbe80e77cafe0344243c4c8d4768488b4f704c635f784885c948984c0f45c1498d4bff4823c8418b148883faff0f84a60000004c8b4f304863ca488d044949391cc1740f418b54c11083faff75e9e986000000488d0449498b5cc1084d8d14c18b47383b47647440418d43ff4863c8496342144823c8418b04884d8d048883f8ff7425903bc2741948984c8d40014e8d0440438b04c14f8d04c183f8ff75e5eb07418b421041890041b801000000488d4f30e81d9c59fe4885db741d488b0641b801000000488bd3488bceff90b8020000488bcbe88b3dbafe488d0dac3f3002ff15aee3e500488b5c2430488d5424304c8bc3488d0d8ae6ef01e895506bfe48634424304533c083f8ff7411488d1440488b056de6ef01488d0cd0eb03498bc883690801751f498bd04c89442430488bcbe8be39ffff488bd3488d0d44e6ef01e89f9759fe488d0d403f3002488b5c2440488b7424484883c4205f48ff253ae3e500";
    static constexpr std::string_view identical_bytes = "4c8bdc49895b0849896b10565741564883ec50418be9498bf04c8bf2488bd94d85c00f84ca000000f681b0000000027431488b89b8000000498d531841c6431800498953b8498bd6488b01ff90c000000084c0740d0fb6842480000000e992000000488b4350488d4c2430488944243848895c2430c7442440ffffffff66c74424440101c644244600e8b2d98bff488b7c24384885ff745633db395f387e32908bc3448bcd0faf473c488bcf4863d04863474c4803d0488b074c8d04324903d6ff908800000084c07428ffc33b5f387ccf488b4720488d4c24304889442438e85cd98bff488b7c24384885ff75aab001eb0232c0488b5c2470488b6c24784883c450415e5f5ec3";
};
