class EngineAllocator {
public:
    struct Certificate { void* allocator{}; void* vtable{}; void* function{}; };
private:
    using Realloc = void*(__fastcall*)(void*, std::size_t, std::uint32_t);
    struct Outcome { const char* state{"guards rejected"}; const char* cleanup{"not needed"}; Certificate cert{}; unsigned calls{}; };
    Runtime& runtime_;
    bool enabled_{};
    std::atomic<bool> attempted_{};
    static constexpr std::array<std::uint8_t, 79> wrapper_bytes{{
        0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,
        0x48,0x8B,0xF1,0x41,0x8B,0xD8,0x48,0x8B,0x0D,0x54,0xD0,0x5A,0x03,0x48,0x8B,
        0xFA,0x48,0x85,0xC9,0x75,0x0C,0xE8,0xF7,0x13,0xFF,0xFF,0x48,0x8B,0x0D,0x40,
        0xD0,0x5A,0x03,0x48,0x8B,0x01,0x44,0x8B,0xCB,0x4C,0x8B,0xC7,0x48,0x8B,0xD6,
        0x48,0x8B,0x5C,0x24,0x30,0x48,0x8B,0x74,0x24,0x38,0x48,0x83,0xC4,0x20,0x5F,
        0x48,0xFF,0x60,0x20
    }};
    bool executable(const void* pointer) const noexcept {
        const auto address=reinterpret_cast<std::uintptr_t>(pointer),base=reinterpret_cast<std::uintptr_t>(runtime_.image_base);
        MEMORY_BASIC_INFORMATION memory{};
        return address>=base && address-base<runtime_.image_size &&
            VirtualQuery(pointer,&memory,sizeof(memory))==sizeof(memory) && memory.State==MEM_COMMIT &&
            !(memory.Protect&(PAGE_GUARD|PAGE_NOACCESS)) &&
            (memory.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
    }
    bool guards(Certificate& result) const {
        if (runtime_.exe_timestamp != kKnownExeTimestamp || runtime_.image_size != kKnownImageSize) return false;
        auto* wrapper = runtime_.image_base + 0xDC8E40;
        std::array<std::uint8_t, wrapper_bytes.size()> bytes{};
        if (!executable(wrapper) || !safe_copy(wrapper, bytes.data(), bytes.size()) || bytes != wrapper_bytes) return false;
        const auto allocator = safe_read_at<void*>(runtime_.image_base, 0x4375EB0).value_or(nullptr);
        const auto vtable = allocator ? safe_read<void*>(allocator).value_or(nullptr) : nullptr;
        const auto function = vtable ? safe_read_at<void*>(vtable, 0x20).value_or(nullptr) : nullptr;
        if (!allocator || !vtable || !executable(function)) return false;
        result = {allocator, vtable, function}; return true;
    }
    bool same(const Certificate& expected) const {
        Certificate current;
        return guards(current) && current.allocator == expected.allocator && current.vtable == expected.vtable && current.function == expected.function;
    }
    static bool invoke_raw(Realloc wrapper, void* pointer, std::size_t count, void** result, std::uint32_t alignment = 0) {
        __try { *result = wrapper(pointer, count, alignment); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool invoke(Realloc wrapper, void* pointer, std::size_t count, void** result, std::uint32_t alignment = 0) noexcept {
        try { return invoke_raw(wrapper, pointer, count, result, alignment); } catch (...) { return false; }
    }
    void run(Outcome& out) {
        if (!guards(out.cert)) return;
        const auto wrapper = reinterpret_cast<Realloc>(runtime_.image_base + 0xDC8E40);
        void* owned{}; void* returned{};
        const auto call = [&](std::size_t count) {
            if (!same(out.cert)) { out.state = "guard changed"; return false; }
            ++out.calls;
            if (!invoke(wrapper, owned, count, &returned)) {
                owned = nullptr; out.state = "outcome unknown: wrapper fault"; out.cleanup = "not retried: ownership unknown"; return false;
            }
            if (count && !returned) {

                owned = nullptr; out.state = "outcome unknown: null allocation/resize"; out.cleanup = "not retried: ownership unknown"; return false;
            }
            owned = returned; return true;
        };
        if (!call(16)) return;
        out.cleanup = "owned allocation pending";
        std::array<std::uint8_t, 16> sentinel{}, observed{};
        for (std::size_t i = 0; i < sentinel.size(); ++i) sentinel[i] = static_cast<std::uint8_t>(0xA0 + i);
        bool verified = reinterpret_cast<std::uintptr_t>(owned) % 8 == 0 && is_readable_range(owned, 16) &&
            is_writable_range(owned, 16) && write_bytes_safely(owned, sentinel.data(), sentinel.size()) &&
            safe_copy(owned, observed.data(), observed.size()) && observed == sentinel;
        if (verified) {
            if (!call(32)) { if (!owned) return; verified = false; }
            else verified = reinterpret_cast<std::uintptr_t>(owned) % 8 == 0 && is_readable_range(owned, 32) &&
                safe_copy(owned, observed.data(), observed.size()) && observed == sentinel;
        }

        if (!same(out.cert)) { out.state = "failure: cleanup guard changed"; out.cleanup = "not called: owned buffer retained"; return; }
        ++out.calls;
        if (!invoke(wrapper, owned, 0, &returned)) { out.state = "outcome unknown: cleanup fault"; out.cleanup = "not retried"; return; }
        if (returned) { out.state = "outcome unknown: zero-size returned nonnull"; out.cleanup = "not retried"; return; }
        out.cleanup = "zero-size returned null";
        out.state = verified ? "pass" : "failure: sentinel/range/alignment";
    }
    void run_cpp(Outcome& out) noexcept {
        try { run(out); } catch (...) { out.state = "outcome unknown: diagnostic exception"; out.cleanup = "not retried"; }
    }
    void run_guarded(Outcome& out) noexcept {
        __try { run_cpp(out); }
        __except (EXCEPTION_EXECUTE_HANDLER) { out.state = "outcome unknown: diagnostic fault"; out.cleanup = "not retried"; }
    }
    void log_cpp(const Outcome& out, std::int64_t duration) noexcept {
        try {
            std::ostringstream message;
            message << "TBCR allocator probe " << out.state << " cleanup=" << out.cleanup << " duration_us=" << duration
                << " thread=" << GetCurrentThreadId() << " global=" << hex_value(reinterpret_cast<std::uintptr_t>(runtime_.image_base + 0x4375EB0))
                << " allocator=" << hex_value(reinterpret_cast<std::uintptr_t>(out.cert.allocator))
                << " vtable=" << hex_value(reinterpret_cast<std::uintptr_t>(out.cert.vtable))
                << " function=" << hex_value(reinterpret_cast<std::uintptr_t>(out.cert.function))
                << " wrapper=" << hex_value(reinterpret_cast<std::uintptr_t>(runtime_.image_base + 0xDC8E40))
                << " calls=" << out.calls << "; owned buffers only; no array mutation";
            log_line("INFO", message.str());
        } catch (...) {}
    }
    void log_guarded(const Outcome& out, std::int64_t duration) noexcept {
        __try { log_cpp(out, duration); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
public:
    explicit EngineAllocator(Runtime& runtime) : runtime_(runtime), enabled_(infinifusion::runtime::development_profile && GetPrivateProfileIntW(
        L"Diagnostics", L"TbcrAllocatorProbe", 0, (runtime.module_directory / L"infinifusion.ini").c_str()) != 0) {}
    void once() noexcept {
        if (!enabled_ || attempted_.exchange(true)) return;
        const auto started = std::chrono::steady_clock::now(); Outcome out; run_guarded(out);
        log_guarded(out, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    }
    bool certify(Certificate& result) const { return guards(result); }
    bool unchanged(const Certificate& result) const { return same(result); }

    bool resize(const Certificate& cert, void* pointer, std::size_t bytes, void** result, std::uint32_t alignment = 0) const {
        return (!alignment || !(alignment & (alignment-1))) && alignment<=65536 && same(cert) &&
            invoke(reinterpret_cast<Realloc>(runtime_.image_base + 0xDC8E40), pointer, bytes, result, alignment);
    }
};
