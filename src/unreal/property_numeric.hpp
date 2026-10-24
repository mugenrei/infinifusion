#pragma once
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace infinifusion::unreal {

inline void validate_property_numeric(std::string_view type, std::string_view text) {
    while (!text.empty() && (text.front()==' ' || text.front()=='\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back()==' ' || text.back()=='\t')) text.remove_suffix(1);
    auto invalid=[] { throw std::invalid_argument("numeric import exceeds reflected type or has invalid syntax"); };
    if (type=="BoolProperty") {
        if (text!="True" && text!="False" && text!="true" && text!="false" && text!="1" && text!="0") invalid();
        return;
    }
    unsigned bits{}; bool signed_value{};
    if(type=="Int8Property") {bits=8; signed_value=true;}
    else if(type=="Int16Property") {bits=16; signed_value=true;}
    else if(type=="IntProperty") {bits=32; signed_value=true;}
    else if(type=="Int64Property") {bits=64; signed_value=true;}
    else if(type=="ByteProperty" || type=="UInt8Property") bits=8;
    else if(type=="UInt16Property") bits=16;
    else if(type=="UInt32Property") bits=32;
    else if(type=="UInt64Property") bits=64;
    else if(type=="FloatProperty" || type=="DoubleProperty") {
        double number{};
        const auto parsed=std::from_chars(text.data(),text.data()+text.size(),number,std::chars_format::fixed);
        if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || !std::isfinite(number) ||
           (type=="FloatProperty" && std::abs(number)>std::numeric_limits<float>::max())) invalid();
        return;
    } else return;
    if(text.empty()) invalid();
    if(text.front()=='+') {
        text.remove_prefix(1);
        if(text.empty() || text.front()=='+' || text.front()=='-') invalid();
    }
    if(signed_value) {
        std::int64_t value{};
        const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
        if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size()) invalid();
        if(bits<64 && (value < -(std::int64_t{1}<<(bits-1)) || value > (std::int64_t{1}<<(bits-1))-1)) invalid();
    } else {
        std::uint64_t value{};
        const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
        if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() ||
           (bits<64 && value>((std::uint64_t{1}<<bits)-1))) invalid();
    }
}
}
