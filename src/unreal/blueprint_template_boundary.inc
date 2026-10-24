void runtime_object_before_component(void* actor,void* source) noexcept;

struct BlueprintTemplateSourceContext {
    void* actor;
    void* source;
    const BlueprintTemplateSourceContext* previous;
    static thread_local const BlueprintTemplateSourceContext* current;
    BlueprintTemplateSourceContext(void* owner,void* value) noexcept
        :actor(owner),source(value),previous(current) {current=this;}
    ~BlueprintTemplateSourceContext() {current=previous;}
    BlueprintTemplateSourceContext(const BlueprintTemplateSourceContext&)=delete;
    BlueprintTemplateSourceContext& operator=(const BlueprintTemplateSourceContext&)=delete;
};
thread_local const BlueprintTemplateSourceContext* BlueprintTemplateSourceContext::current{};

void* blueprint_template_source(void* actor) noexcept {
    const auto* context=BlueprintTemplateSourceContext::current;
    return context && context->actor==actor ? context->source : nullptr;
}

class BlueprintTemplateBoundary;
std::atomic<BlueprintTemplateBoundary*> g_blueprint_template_boundary{};
void* __fastcall blueprint_template_wrapper(void* actor,void* source,FName name);

class BlueprintTemplateBoundary final {
    using Original=void*(__fastcall*)(void*,void*,FName);
    static_assert(sizeof(FName)==8,"certified slow-template ABI requires 8-byte FName");
    Runtime runtime_;
    void* trampoline_{};
    Original original_{};
    std::unique_ptr<CertifiedEntryHook> hook_;
    bool exposed_{};
    static constexpr std::array<std::uint8_t,10> entry{
        0x48,0x89,0x5C,0x24,0x20,0x4C,0x89,0x44,0x24,0x18};
    static constexpr std::array<std::uint8_t,10> following{
        0x55,0x56,0x57,0x48,0x81,0xEC,0xE0,0,0,0};
    void discard_unexposed() noexcept {
        hook_.reset();
        if (trampoline_) VirtualFree(trampoline_,0,MEM_RELEASE);
        trampoline_=nullptr;
        original_=nullptr;
    }
public:
    explicit BlueprintTemplateBoundary(const Runtime& runtime):runtime_(runtime) {}
    bool install() {
        if (exposed_) return true;
        if (trampoline_) return false;
        const auto code=infinifusion::unreal::blueprint_template_trampoline(runtime_.image_base+0x1FEB79A);
        trampoline_=VirtualAlloc(nullptr,code.size(),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        if (!trampoline_) return false;
        std::copy(code.begin(),code.end(),static_cast<std::uint8_t*>(trampoline_));
        DWORD protection{};
        if (!VirtualProtect(trampoline_,code.size(),PAGE_EXECUTE_READ,&protection) ||
            !FlushInstructionCache(GetCurrentProcess(),trampoline_,code.size())) {
            discard_unexposed();return false;
        }
        original_=reinterpret_cast<Original>(trampoline_);
        try {
            hook_=std::make_unique<CertifiedEntryHook>(runtime_,"Blueprint slow-template boundary",
                0x1FEB790,entry,0x1FEB79A,following,trampoline_,
                reinterpret_cast<void*>(&blueprint_template_wrapper));
        } catch (...) {discard_unexposed();throw;}
        BlueprintTemplateBoundary* expected{};
        if (!g_blueprint_template_boundary.compare_exchange_strong(expected,this,
            std::memory_order_release,std::memory_order_relaxed)) {
            discard_unexposed();return false;
        }

        try {exposed_=hook_->install();}
        catch (...) {

            exposed_=true;
            try {log_line("WARN","Blueprint slow-template boundary installation exception; pinned state retained, coverage unknown");}
            catch (...) {}
        }
        if (exposed_) return true;
        g_blueprint_template_boundary.store(nullptr,std::memory_order_release);
        discard_unexposed();return false;
    }
    void* invoke(void* actor,void* source,FName name) {
        const BlueprintTemplateSourceContext context(actor,source);
        runtime_object_before_component(actor,source);

        return original_(actor,source,name);
    }
};

void* __fastcall blueprint_template_wrapper(void* actor,void* source,FName name) {
    return g_blueprint_template_boundary.load(std::memory_order_acquire)->invoke(actor,source,name);
}
