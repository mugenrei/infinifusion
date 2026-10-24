#pragma once

#include <Windows.h>

namespace smtvv
{
class StartupAdmission;

void run(HMODULE self_module, DWORD bootstrap_thread_id, StartupAdmission* admission = nullptr) noexcept;
}
