#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "runtime/runtime.hpp"
#include "bootstrap/startup.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>

namespace {

constexpr std::size_t kExportCount = 192;
constexpr wchar_t kRealDllName[] = L"winmm.dll";

INIT_ONCE g_real_dll_once = INIT_ONCE_STATIC_INIT;
HMODULE g_real_dll = nullptr;
HMODULE g_self_module = nullptr;
DWORD g_bootstrap_thread_id = 0;
smtvv::StartupAdmission g_startup_admission;
bool g_initial_import = false;

void debug_message(const wchar_t* message) noexcept
{
    ::OutputDebugStringW(message);
}

BOOL CALLBACK load_real_winmm(
    PINIT_ONCE,
    PVOID,
    PVOID*) noexcept
{
    wchar_t path[MAX_PATH] = {};
    const UINT system_directory_length =
        ::GetSystemDirectoryW(path, static_cast<UINT>(std::size(path)));
    if (system_directory_length == 0 ||
        system_directory_length >= std::size(path)) {
            debug_message(L"Infinifusion: GetSystemDirectoryW failed.\n");
        return FALSE;
    }

    std::size_t cursor = system_directory_length;
    if (cursor != 0 && path[cursor - 1] != L'\\') {
        if (cursor + 1 >= std::size(path)) {
            debug_message(L"Infinifusion: System32 path is too long.\n");
            return FALSE;
        }
        path[cursor++] = L'\\';
    }

    if (cursor + std::size(kRealDllName) > std::size(path)) {
            debug_message(L"Infinifusion: System32 winmm.dll path is too long.\n");
        return FALSE;
    }

    for (const wchar_t character : kRealDllName) {
        path[cursor++] = character;
    }

    HMODULE module = ::LoadLibraryExW(
        path,
        nullptr,
        LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr &&
        ::GetLastError() == ERROR_INVALID_PARAMETER) {

        module = ::LoadLibraryW(path);
    }

    if (module == nullptr || module == g_self_module) {
        if (module != nullptr) {
            ::FreeLibrary(module);
        }
        debug_message(L"Infinifusion: failed to load the real System32 winmm.dll.\n");
        return FALSE;
    }

    g_real_dll = module;
    return TRUE;
}

std::uintptr_t WINAPI missing_export() noexcept
{
    ::SetLastError(ERROR_PROC_NOT_FOUND);
    return 0;
}

DWORD WINAPI dumper_worker(void* parameter) noexcept
{
    const auto self_module = static_cast<HMODULE>(parameter);
    try {
        smtvv::run(self_module, g_bootstrap_thread_id,
            g_initial_import ? &g_startup_admission : nullptr);
    } catch (...) {
        debug_message(L"Infinifusion: unhandled exception in runtime worker.\n");
    }
    return 0;
}

void CALLBACK startup_apc(ULONG_PTR) noexcept
{

    if (!g_startup_admission.wait(15'000))
        debug_message(L"Infinifusion: startup admission timed out; early overlays unavailable.\n");
}

} // namespace

extern "C" {

alignas(void*) FARPROC g_winmm_exports[kExportCount] = {};

FARPROC proxy_resolve_export(
    const std::uint32_t export_index,
    const char* export_name) noexcept
{
    const auto fallback = reinterpret_cast<FARPROC>(&missing_export);
    if (export_index >= kExportCount || export_name == nullptr) {
        return fallback;
    }

    auto* const slot =
        reinterpret_cast<PVOID volatile*>(&g_winmm_exports[export_index]);
    if (const PVOID cached =
            ::InterlockedCompareExchangePointer(slot, nullptr, nullptr);
        cached != nullptr) {
        return reinterpret_cast<FARPROC>(cached);
    }

    const DWORD caller_last_error = ::GetLastError();
    FARPROC target = fallback;
    if (::InitOnceExecuteOnce(
            &g_real_dll_once,
            &load_real_winmm,
            nullptr,
            nullptr) != FALSE) {
        if (const FARPROC resolved =
                ::GetProcAddress(g_real_dll, export_name);
            resolved != nullptr) {
            target = resolved;
        }
    }

    const PVOID target_pointer = reinterpret_cast<PVOID>(target);
    const PVOID previous =
        ::InterlockedCompareExchangePointer(slot, target_pointer, nullptr);
    const FARPROC forwarded_target = previous != nullptr
        ? reinterpret_cast<FARPROC>(previous)
        : target;
    if (forwarded_target != fallback) {
        ::SetLastError(caller_last_error);
    }
    return forwarded_target;
}

BOOL WINAPI DllMain(
    const HMODULE module,
    const DWORD reason,
    void* reserved) noexcept
{
    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }

    g_self_module = module;
    g_bootstrap_thread_id = ::GetCurrentThreadId();
    ::DisableThreadLibraryCalls(module);

    // Only process-initial imports have the before-entry APC guarantee.
    // Dynamic LoadLibrary remains asynchronous (including proxy test hosts).
    if (reserved != nullptr && g_startup_admission.arm()) {
        g_initial_import = true;
        if (!QueueUserAPC(startup_apc, GetCurrentThread(), 0)) {
            (void)g_startup_admission.wait(0); // Cancel; do not promise early publication.
            debug_message(L"Infinifusion: could not queue startup admission APC.\n");
        }
    } else if (reserved != nullptr) {
        debug_message(L"Infinifusion: could not create startup admission event; early overlays unavailable.\n");
    }

    // A new thread cannot begin executing user code until this DllMain returns
    // and the loader lock is released. Keep all dumper work in that thread.
    const HANDLE worker = ::CreateThread(
        nullptr,
        0,
        &dumper_worker,
        module,
        0,
        nullptr);
    if (worker != nullptr) {
        ::CloseHandle(worker);
    } else {
        g_startup_admission.complete();
        debug_message(L"Infinifusion: failed to create runtime worker.\n");
    }

    // Proxying remains usable even if the optional dumper worker cannot start.
    return TRUE;
}

} // extern "C"
