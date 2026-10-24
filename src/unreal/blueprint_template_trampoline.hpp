#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace infinifusion { namespace unreal {

inline std::array<std::uint8_t,24> blueprint_template_trampoline(const void* continuation) {
    std::array<std::uint8_t,24> code{
        0x48,0x89,0x5C,0x24,0x20,0x4C,0x89,0x44,0x24,0x18,
        0xFF,0x25,0,0,0,0};
    const auto destination=reinterpret_cast<std::uintptr_t>(continuation);
    std::memcpy(code.data()+16,&destination,sizeof(destination));
    return code;
}
}}
