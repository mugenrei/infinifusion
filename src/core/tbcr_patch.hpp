#pragma once
#include "core/provider.hpp"
#include <map>

namespace infinifusion::core {

class TbcrPatchProvider final : public Provider {
public:
    struct Definition {
        std::string asset;
        std::uint32_t table_index = 0, record_index = 0, record_size = 0;
        std::vector<std::uint8_t> bytes, active_mask;
        bool append = false;
        std::string owner_id, append_id;
    };
    std::string_view id() const noexcept override { return "smtvv.tbcr"; }
    Result<Region> bind(const ResourceLocator&, const PropertyPath&) override;
    Result<Region> bind_owned(const ResourceLocator&, const PropertyPath&, std::string_view owner_id) override;
    Error validate(const Region&) override;
    Result<EncodedValue> prepare(const Region&, const Value&) override;
    Result<Value> decode(const Region&, std::span<const std::uint8_t>) override;
    Result<std::vector<std::uint8_t>> read(const Region&) override;
    Error write(const Region&, std::span<const std::uint8_t>) override;
    std::vector<Definition> definitions() const;
private:
    struct Slot { Definition record; std::vector<std::uint8_t> storage; TargetIdentity identity; };
    std::map<std::string, Slot> slots_;
    Result<const Slot*> slot(const Region&) const;
};
}
