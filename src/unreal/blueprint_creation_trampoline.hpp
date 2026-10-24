#pragma once
#include <array>
#include <cstdint>
#include <cstring>
namespace infinifusion { namespace unreal {

inline std::array<std::uint8_t,33> blueprint_creation_trampoline(const void* null_exit,const void* nonnull_exit) {
    std::array<std::uint8_t,33> code{0x48,0x85,0xD2,0x75,0x0E};
    for (auto offset:{5u,19u}) {code[offset]=0xFF;code[offset+1]=0x25;}
    const auto a=reinterpret_cast<std::uintptr_t>(null_exit),b=reinterpret_cast<std::uintptr_t>(nonnull_exit);
    std::memcpy(code.data()+11,&a,8);std::memcpy(code.data()+25,&b,8);
    return code;
}
}}
