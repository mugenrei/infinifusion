#pragma once

#include "infinifusion/api.h"
#include <string_view>

namespace infinifusion::sdk {
inline IF_String text(std::string_view value) {
    return {value.data(), static_cast<uint32_t>(value.size())};
}
inline IF_Value floating(double value) {
    IF_Value result{}; result.size = sizeof(result); result.kind = IF_FLOAT64;
    result.float_value = value; return result;
}
inline IF_Value boolean(bool value) {
    IF_Value result{}; result.size = sizeof(result); result.kind = IF_BOOL;
    result.unsigned_value = value; return result;
}
inline IF_Value string(std::string_view value) {
    IF_Value result{}; result.size = sizeof(result); result.kind = IF_STRING;
    result.text = text(value); return result;
}
inline IF_Value integer(int64_t value) {
    IF_Value result{}; result.size = sizeof(result); result.kind = IF_INT64;
    result.signed_value = value; return result;
}
inline IF_Value unsigned_integer(uint64_t value) {
    IF_Value result{}; result.size = sizeof(result); result.kind = IF_UINT64;
    result.unsigned_value = value; return result;
}
inline IF_PropertySpec object_field(std::string_view path, std::string_view class_guard,
                                    std::string_view field, uint32_t kind = IF_FLOAT64) {
    return {sizeof(IF_PropertySpec), kind,
        {text("unreal.object"), text(path), {}, text(class_guard)}, text(field)};
}

inline IF_Value property_text(std::string_view expression) {
    IF_Value result{}; result.size=sizeof(result); result.kind=IF_BYTES;
    result.bytes={reinterpret_cast<const uint8_t*>(expression.data()), static_cast<uint32_t>(expression.size())};
    return result;
}
inline IF_PropertySpec selected_object_field(std::string_view path, std::string_view selector,
                                             std::string_view class_guard, std::string_view field,
                                             uint32_t kind=IF_BYTES) {
    return {sizeof(IF_PropertySpec),kind,
        {text("unreal.object"),text(path),text(selector),text(class_guard)},text(field)};
}

inline IF_Assignment definition(std::string_view provider, std::string_view asset,
                                std::string_view row, std::string_view guard,
                                std::string_view property, std::string_view import_text) {
    IF_Assignment result{}; result.size = sizeof(result);
    result.target = {text(provider), text(asset), text(row), text(guard)};
    result.property = text(property); result.value = string(import_text); return result;
}
inline IF_Assignment table_operation(std::string_view asset, std::string_view row,
                                     std::string_view operation = "update", std::string_view guard = {}) {
    return definition("unreal.table_definition", asset, row, guard, "Operation", operation);
}
inline IF_Assignment table_definition_field(std::string_view asset, std::string_view row,
                                            std::string_view fields_path, std::string_view import_text,
                                            std::string_view guard = {}) {
    return definition("unreal.table_definition", asset, row, guard, fields_path, import_text);
}
inline IF_Assignment message_operation(std::string_view asset, std::string_view label) {
    return definition("unreal.message_definition", asset, label, {}, "Operation", "update");
}
inline IF_Assignment message_definition_field(std::string_view asset, std::string_view label,
                                              std::string_view fields_path, std::string_view import_text) {
    return definition("unreal.message_definition", asset, label, {}, fields_path, import_text);
}
inline bool compatible(const IF_Api* api) {
    return api && api->size == sizeof(IF_Api) && api->version == IF_API_VERSION &&
        api->property_open && api->property_get && api->property_refresh && api->property_set &&
        api->property_reset && api->property_remove && api->property_close && api->property_resume;
}

class Property {
public:
    uint32_t open(const IF_Api* api, IF_Owner owner, const IF_PropertySpec& spec) {
        if (!compatible(api)) return IF_ABI_MISMATCH;
        if (id_) return IF_INVALID_ARGUMENT;
        const auto error = api->property_open(owner, &spec, &id_);
        if (error == IF_OK) { api_ = *api; owner_ = owner; }
        return error;
    }
    uint32_t get(IF_PropertyState& result) const {
        result.size = sizeof(result);
        return id_ ? api_.property_get(owner_, id_, &result) : IF_INVALID_HANDLE;
    }
    uint32_t set(const IF_Value& value, uint32_t flags = 0, uint64_t revision = 0) const {
        return id_ ? api_.property_set(owner_, id_, &value, flags, revision) : IF_INVALID_HANDLE;
    }
    uint32_t reset(uint32_t flags = 0, uint64_t revision = 0) const {
        return id_ ? api_.property_reset(owner_, id_, flags, revision) : IF_INVALID_HANDLE;
    }
    uint32_t refresh() const { return id_ ? api_.property_refresh(owner_, id_) : IF_INVALID_HANDLE; }
    uint32_t resume(const IF_Value& observed) const {
        return id_ ? api_.property_resume(owner_, id_, &observed) : IF_INVALID_HANDLE;
    }
    uint32_t remove() const { return id_ ? api_.property_remove(owner_, id_) : IF_INVALID_HANDLE; }
    uint32_t close() {
        if (!id_) return IF_INVALID_HANDLE;
        const auto error = api_.property_close(owner_, id_);
        if (error == IF_OK) id_ = 0;
        return error;
    }
    IF_Property handle() const { return id_; }
private:
    IF_Api api_{};
    IF_Owner owner_{};
    IF_Property id_{};
};

inline uint32_t plugin(const IF_Api* api, IF_Plugin* output, const char* id,
                       const char* name, int32_t priority,
                       uint32_t (IF_CALL *initialize)(const IF_Api*, IF_Owner),
                       void (IF_CALL *poll)(const IF_Api*, IF_Owner) = nullptr) {
    if (!compatible(api) || !output || output->size != sizeof(*output)) return IF_ABI_MISMATCH;
    *output = {}; output->size = sizeof(*output); output->required_api_version = IF_API_VERSION;
    output->owner = {sizeof(IF_OwnerInfo), priority, text(id), text(name), text(id)};
    output->initialize = initialize; output->poll = poll;
    return IF_OK;
}
}
