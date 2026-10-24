class CertifiedEntryHook {
    Runtime& runtime_;
    const std::string label_;
    const std::uintptr_t entry_rva_, destination_rva_;
    const std::size_t entry_size_, destination_size_;
    const std::array<std::uint8_t, 32> entry_bytes_, destination_bytes_;
    void* const original_;
    void* const wrapper_;
    const bool certificate_valid_;
    void* relay_{};
    bool installed_{};
    static std::array<std::uint8_t, 32> certificate(std::span<const std::uint8_t> bytes) {
        std::array<std::uint8_t, 32> copy{};
        if (bytes.size() <= copy.size()) std::copy(bytes.begin(), bytes.end(), copy.begin());
        return copy;
    }
    static bool copy_raw(const void* from, void* to, std::size_t size) noexcept {
        __try { std::memcpy(to, from, size); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool exact_bytes() const noexcept {
        std::array<std::uint8_t, 32> entry{}, destination{};
        return copy_raw(runtime_.image_base + entry_rva_, entry.data(), entry_size_) &&
            std::memcmp(entry.data(), entry_bytes_.data(), entry_size_) == 0 &&
            copy_raw(runtime_.image_base + destination_rva_, destination.data(), destination_size_) &&
            std::memcmp(destination.data(), destination_bytes_.data(), destination_size_) == 0;
    }
    bool executable(std::uintptr_t rva, std::size_t size) const {
        if (!runtime_.image_base || runtime_.image_size < sizeof(IMAGE_NT_HEADERS64) || rva >= runtime_.image_size || size > runtime_.image_size - rva) return false;
        MEMORY_BASIC_INFORMATION memory{};
        auto* address = runtime_.image_base + rva;
        if (!VirtualQuery(address, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
            reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + size > memory.RegionSize) return false;
        const auto dos = safe_read<IMAGE_DOS_HEADER>(runtime_.image_base);
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
            static_cast<std::uint32_t>(dos->e_lfanew) > runtime_.image_size - sizeof(IMAGE_NT_HEADERS64)) return false;
        const auto nt = safe_read<IMAGE_NT_HEADERS64>(runtime_.image_base + dos->e_lfanew);
        if (!nt || nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt->FileHeader.TimeDateStamp != kKnownExeTimestamp || nt->OptionalHeader.SizeOfImage != kKnownImageSize ||
            nt->FileHeader.NumberOfSections > 96) return false;
        const auto section_start = static_cast<std::size_t>(dos->e_lfanew) +
            offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader;
        if (section_start > runtime_.image_size || nt->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER) > runtime_.image_size - section_start) return false;
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const auto section = safe_read<IMAGE_SECTION_HEADER>(runtime_.image_base + section_start + i * sizeof(IMAGE_SECTION_HEADER));
            if (section && rva >= section->VirtualAddress && rva - section->VirtualAddress < section->Misc.VirtualSize &&
                size <= section->Misc.VirtualSize - (rva - section->VirtualAddress))
                return (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        }
        return false;
    }
    void* allocate_relay(std::uintptr_t next) {
        SYSTEM_INFO system{}; GetSystemInfo(&system);
        const auto granularity = static_cast<std::uintptr_t>(system.dwAllocationGranularity);
        if (!granularity) return nullptr;
        const auto aligned = next - next % granularity;

        for (std::uintptr_t step = 1; step <= 16384; ++step) {
            const auto distance = step * granularity;
            for (unsigned side = 0; side < 2; ++side) {
                if ((!side && aligned < distance) || (side && aligned > UINTPTR_MAX - distance)) continue;
                const auto candidate = side ? aligned + distance : aligned - distance;
                const auto displacement = static_cast<std::int64_t>(candidate) - static_cast<std::int64_t>(next);
                if (displacement < INT32_MIN || displacement > INT32_MAX) continue;
                MEMORY_BASIC_INFORMATION info{};
                if (!VirtualQuery(reinterpret_cast<void*>(candidate), &info, sizeof(info)) || info.State != MEM_FREE ||
                    info.RegionSize < granularity) continue;
                if (auto* allocation = VirtualAlloc(reinterpret_cast<void*>(candidate), granularity,
                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return allocation;
            }
        }
        return nullptr;
    }
public:
    CertifiedEntryHook(Runtime& runtime, const char* label, std::uintptr_t entry_rva,
        std::span<const std::uint8_t> entry_bytes, std::uintptr_t destination_rva,
        std::span<const std::uint8_t> destination_bytes, void* original, void* wrapper)
        : runtime_(runtime), label_(label ? label : "Certified entry hook"),
          entry_rva_(entry_rva), destination_rva_(destination_rva),
          entry_size_(entry_bytes.size()), destination_size_(destination_bytes.size()),
          entry_bytes_(certificate(entry_bytes)), destination_bytes_(certificate(destination_bytes)),
          original_(original), wrapper_(wrapper), certificate_valid_(entry_size_ >= 5 &&
              entry_size_ <= 32 && destination_size_ > 0 && destination_size_ <= 32) {}
    CertifiedEntryHook(Runtime& runtime, const char* label, std::uintptr_t entry_rva,
        std::span<const std::uint8_t> entry_bytes, std::uintptr_t destination_rva,
        std::span<const std::uint8_t> destination_bytes, void* wrapper)
        : CertifiedEntryHook(runtime, label, entry_rva, entry_bytes, destination_rva,
            destination_bytes, runtime.image_base ? runtime.image_base + destination_rva : nullptr, wrapper) {}
    CertifiedEntryHook(const CertifiedEntryHook&) = delete;
    CertifiedEntryHook& operator=(const CertifiedEntryHook&) = delete;
    void* original() const noexcept { return original_; }
    bool installed() const noexcept { return installed_; }
    bool install() {
        if (installed_) return true;
        if (runtime_.exe_timestamp != kKnownExeTimestamp || runtime_.image_size != kKnownImageSize ||
            !certificate_valid_ || !original_ || !wrapper_ ||
            !executable(entry_rva_, entry_size_) || !executable(destination_rva_, destination_size_) || !exact_bytes()) {
            log_line("WARN", label_ + " rejected executable fingerprint/entry/destination integrity stage=integrity error=0 thread=0"); return false;
        }
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(wrapper_), &pinned)) {
            log_line("WARN", label_ + " refused stage=wrapper-pin error=" + std::to_string(GetLastError()) + " thread=0"); return false;
        }
        auto* entry = runtime_.image_base + entry_rva_;
        const auto next = reinterpret_cast<std::uintptr_t>(entry + 5);
        relay_ = allocate_relay(next);
        if (!relay_) { log_line("WARN", label_ + " found no bounded near relay stage=relay-allocation error=" + std::to_string(GetLastError()) + " thread=0"); return false; }
        std::array<std::uint8_t, 14> relay_bytes{0xFF,0x25,0,0,0,0};
        const auto wrapper = reinterpret_cast<std::uintptr_t>(wrapper_);
        std::memcpy(relay_bytes.data() + 6, &wrapper, 8);
        DWORD relay_protection{};
        if (!copy_raw(relay_bytes.data(), relay_, relay_bytes.size()) ||
            !VirtualProtect(relay_, relay_bytes.size(), PAGE_EXECUTE_READ, &relay_protection) ||
            !FlushInstructionCache(GetCurrentProcess(), relay_, relay_bytes.size())) {
            const auto error = GetLastError();
            VirtualFree(relay_, 0, MEM_RELEASE); relay_ = nullptr;
            log_line("WARN", label_ + " refused stage=relay-preparation error=" + std::to_string(error) + " thread=0"); return false;
        }
        const auto displacement = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(relay_)) - static_cast<std::int64_t>(next);
        if (displacement < INT32_MIN || displacement > INT32_MAX) {
            VirtualFree(relay_, 0, MEM_RELEASE); relay_ = nullptr;
            log_line("WARN", label_ + " refused stage=relay-range error=0 thread=0"); return false;
        }
        std::array<std::uint8_t, 32> patch{};
        patch.fill(0x90); patch[0] = 0xE9;
        const auto relative = static_cast<std::int32_t>(displacement);
        std::memcpy(patch.data() + 1, &relative, 4);

        struct ThreadHandle { HANDLE handle{}; DWORD id{}; bool paused{}; };
        std::array<ThreadHandle, 1024> threads{};
        std::size_t count{}, terminated{};
        const char* failure = "none";
        DWORD failure_error{}, failure_thread{};
        auto failed = [&](const char* stage, DWORD error = 0, DWORD id = 0) noexcept {
            failure = stage; failure_error = error; failure_thread = id;
        };
        bool ready{};

        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            ready = snapshot != INVALID_HANDLE_VALUE;
            if (!ready) failed("thread-snapshot", GetLastError());
            THREADENTRY32 thread{}; thread.dwSize = sizeof(thread);
            if (ready) {
                ready = Thread32First(snapshot, &thread) != FALSE;
                if (!ready) failed("thread-first", GetLastError());
                while (ready) {
                    if (thread.th32OwnerProcessID == GetCurrentProcessId() && thread.th32ThreadID != GetCurrentThreadId()) {
                        if (count == threads.size()) { failed("thread-capacity"); ready = false; break; }
                        const auto handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                            THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, thread.th32ThreadID);
                        if (!handle) { failed("thread-open", GetLastError(), thread.th32ThreadID); ready = false; break; }
                        threads[count++] = {handle, thread.th32ThreadID, false};
                    }
                    if (!Thread32Next(snapshot, &thread)) {
                        const auto error = GetLastError();
                        if (error != ERROR_NO_MORE_FILES) { failed("thread-next", error); ready = false; }
                        break;
                    }
                }
            }
            if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
            if (ready || attempt || std::strcmp(failure, "thread-open") || failure_error != ERROR_INVALID_PARAMETER) break;
            for (std::size_t i = 0; i < count; ++i) CloseHandle(threads[i].handle);
            count = 0;
        }

        for (std::size_t i = 0; ready && i < count; ++i) {
            auto& thread = threads[i];
            if (WaitForSingleObject(thread.handle, 0) == WAIT_OBJECT_0) { ++terminated; continue; }
            if (SuspendThread(thread.handle) == DWORD(-1)) {
                const auto error = GetLastError();
                if (WaitForSingleObject(thread.handle, 0) == WAIT_OBJECT_0) { ++terminated; continue; }
                failed("thread-suspend", error, thread.id); ready = false; break;
            }
            thread.paused = true;
        }
        for (std::size_t i = 0; ready && i < count; ++i) {
            auto& thread = threads[i];
            if (!thread.paused) continue;
            CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(thread.handle, &context)) {
                const auto error = GetLastError();
                if (WaitForSingleObject(thread.handle, 0) == WAIT_OBJECT_0) { thread.paused = false; ++terminated; continue; }
                failed("thread-context", error, thread.id); ready = false;
            } else if (context.Rip >= reinterpret_cast<DWORD64>(entry) && context.Rip < reinterpret_cast<DWORD64>(entry + entry_size_)) {
                failed("thread-in-entry", 0, thread.id); ready = false;
            }
        }
        bool wrote{}, restored{}, flushed{};
        DWORD protection{}, ignored{};
        if (ready && !exact_bytes()) { failed("entry-reread"); ready = false; }
        if (ready && !VirtualProtect(entry, entry_size_, PAGE_EXECUTE_READWRITE, &protection)) {
            failed("entry-protection", GetLastError()); ready = false;
        }
        if (ready) {
            wrote = true;
            const bool write_ok = copy_raw(patch.data(), entry, entry_size_);
            flushed = write_ok && FlushInstructionCache(GetCurrentProcess(), entry, entry_size_);
            restored = VirtualProtect(entry, entry_size_, protection, &ignored) != FALSE;
            if (!write_ok || !flushed || !restored) {
                failed(!write_ok ? "entry-write" : (!flushed ? "entry-flush" : "entry-protection-restore"), GetLastError());

                if (VirtualProtect(entry, entry_size_, PAGE_EXECUTE_READWRITE, &ignored)) {
                    const bool rollback = copy_raw(entry_bytes_.data(), entry, entry_size_);
                    const bool rollback_flush = FlushInstructionCache(GetCurrentProcess(), entry, entry_size_) != FALSE;
                    const bool rollback_protect = VirtualProtect(entry, entry_size_, protection, &ignored) != FALSE;
                    if (rollback && rollback_flush && rollback_protect) wrote = false;
                }
            }
        }
        bool resume_ok = true;
        for (std::size_t i = count; i > 0; --i) {
            const auto& thread = threads[i - 1];
            if (thread.paused && ResumeThread(thread.handle) == DWORD(-1)) {
                const auto error = GetLastError();
                if (WaitForSingleObject(thread.handle, 0) != WAIT_OBJECT_0) {
                    failed("thread-resume", error, thread.id); resume_ok = false;
                }
            }
        }
        for (std::size_t i = 0; i < count; ++i) CloseHandle(threads[i].handle);
        installed_ = wrote;
        if (!installed_) {
            VirtualFree(relay_, 0, MEM_RELEASE); relay_ = nullptr;
        }
        if (installed_ && restored && flushed && resume_ok)
            log_line("INFO", label_ + " installed entryRva=" + std::to_string(entry_rva_) +
                " destinationRva=" + std::to_string(destination_rva_) + " threads=" +
                std::to_string(count) + " exited=" + std::to_string(terminated) + "; coverage pending actual call");
        else log_line("WARN", label_ + (installed_ ?
            " installation/rollback or thread resume uncertain; pinned state retained" :
            " installation refused/rolled back; no coverage established") +
            " stage=" + failure + " error=" + std::to_string(failure_error) + " thread=" + std::to_string(failure_thread));
        return installed_;
    }
};
