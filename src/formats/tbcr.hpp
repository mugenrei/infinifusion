#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace smtvv::tbcr
{
using Bytes = std::vector<std::uint8_t>;
inline constexpr std::size_t max_payload_bytes = 64 * 1024 * 1024;
inline constexpr std::size_t max_sections = 512;

struct Section
{
    std::size_t index{};
    std::size_t offset{};
    std::size_t data_offset{};
    std::uint32_t data_bytes{};
    std::uint32_t record_stride{};
    std::uint32_t record_count{};
};

struct RecordPatch
{
    std::size_t table{}, record{};
    std::span<const std::uint8_t> bytes, active_mask;
};
struct RecordEdit
{
    std::size_t offset{};
    Bytes before, after;
};

class Document
{
public:
    [[nodiscard]] static Document parse(std::span<const std::uint8_t> payload);
    [[nodiscard]] std::uint32_t header_size() const noexcept { return header_size_; }
    [[nodiscard]] std::span<const Section> sections() const noexcept { return sections_; }
    [[nodiscard]] std::span<const std::uint8_t> record(
        std::size_t section_index, std::size_t record_index) const;
    void update(std::size_t section_index, std::size_t record_index,
        std::size_t record_offset, std::span<const std::uint8_t> bytes);

    [[nodiscard]] std::vector<RecordEdit> prepare_updates(std::span<const RecordPatch>) const;

    void append(std::size_t section_index, std::span<const std::uint8_t> full_record);
    [[nodiscard]] Bytes serialize() const { return bytes_; }

private:
    Document() = default;
    [[nodiscard]] const Section& section(std::size_t index) const;
    [[nodiscard]] bool supports_growth() const noexcept;
    Bytes bytes_;
    std::vector<Section> sections_;
    std::uint32_t header_size_{};
};
}
