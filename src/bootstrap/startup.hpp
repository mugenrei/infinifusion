#pragma once

#include <Windows.h>
#include <atomic>

namespace smtvv {

class StartupAdmission {
    HANDLE event_{};
    std::atomic<bool> cancelled_{};
public:
    bool arm() noexcept {
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return event_ != nullptr;
    }
    void complete() noexcept { if (event_) SetEvent(event_); }
    bool cancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }
    bool wait(DWORD timeout_ms) noexcept {
        if (event_ && WaitForSingleObject(event_, timeout_ms) == WAIT_OBJECT_0) return true;
        cancelled_.store(true, std::memory_order_release);
        return false;
    }
};
}
