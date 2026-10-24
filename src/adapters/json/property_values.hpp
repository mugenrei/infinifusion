#pragma once
#include "formats/json.hpp"
#include "unreal/property_text.hpp"
namespace infinifusion::json_mods {

unreal::PropertyValue property_value(const smtvv::json::Value& value, unsigned depth = 0);
std::string property_text(const smtvv::json::Value& value);
}
