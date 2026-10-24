#pragma once
#include "infinifusion/api.h"
namespace infinifusion::host {
uint32_t IF_CALL tbcr_compile_append(const IF_TbcrAppend*, IF_TbcrCompiled*) noexcept;
uint32_t IF_CALL tbcr_compile_update(const IF_TbcrUpdate*, IF_TbcrCompiled*) noexcept;
}
