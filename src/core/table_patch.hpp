#pragma once
#include "core/provider.hpp"
#include <map>

namespace infinifusion::core {

class TablePatchProvider final : public Provider {
public:
    explicit TablePatchProvider(std::string provider_id = "unreal.table_definition");
    static constexpr std::size_t max_payload_bytes = 4096;
    static constexpr std::size_t max_guard_bytes = 2048;
    static constexpr std::size_t value_offset = 9 + max_guard_bytes;
    static constexpr std::size_t storage_bytes = value_offset + max_payload_bytes;

    static constexpr std::size_t max_slots = 1024;
    struct Definition {
        std::string asset, row, struct_guard;
        std::string operation = "update";
        std::map<std::string, std::string> fields;

        std::map<std::string, std::string> expected_fields;

        std::vector<std::string> guards;
    };
    std::string_view id() const noexcept override { return id_; }
    Result<Region> bind(const ResourceLocator&, const PropertyPath&) override;
    Error validate(const Region&) override;
    Result<EncodedValue> prepare(const Region&, const Value&) override;
    Result<Value> decode(const Region&, std::span<const std::uint8_t>) override;
    Result<std::vector<std::uint8_t>> read(const Region&) override;
    Error write(const Region&, std::span<const std::uint8_t>) override;
    std::vector<Definition> definitions() const;
private:
    const std::string id_;
    struct Slot {
        std::string asset, row, property;
        TargetIdentity identity;
        std::vector<std::uint8_t> storage;
    };
    std::map<std::string, Slot> slots_;

    std::map<std::string, std::string> row_display_;
    Result<const Slot*> slot(const Region&) const;
};
}
