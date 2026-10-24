#include "formats/tbcr.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace smtvv::tbcr
{
namespace
{
[[noreturn]] void fail(const char* message)
{
    throw std::runtime_error(message);
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4)
        fail("TBCR: truncated u32");
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void write_u32(Bytes& bytes, std::size_t offset, std::size_t value)
{
    if (value > std::numeric_limits<std::uint32_t>::max() ||
        offset > bytes.size() || bytes.size() - offset < 4)
        fail("TBCR: u32 write overflow");
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

std::size_t align16(std::size_t value) noexcept
{

    return (value + 15) & ~std::size_t{15};
}

bool zero_range(const Bytes& bytes, std::size_t first, std::size_t last) noexcept
{
    return std::all_of(bytes.begin() + first, bytes.begin() + last,
        [](std::uint8_t value) { return value == 0; });
}
}

Document Document::parse(std::span<const std::uint8_t> payload)
{
    if (payload.size() > max_payload_bytes)
        fail("TBCR: payload exceeds 64 MiB limit");
    if (payload.size() < 12 || std::memcmp(payload.data(), "TBCR", 4) != 0)
        fail("TBCR: missing container magic/header");
    const auto header_size = read_u32(payload, 4);
    const auto count = read_u32(payload, 8);
    if (count > max_sections || header_size < 12 + count * 4 || header_size > payload.size())
        fail("TBCR: invalid header size or section count");

    Document result;
    result.header_size_ = header_size;
    result.sections_.reserve(count);
    std::vector<std::pair<std::size_t, std::size_t>> occupied;
    occupied.reserve(count + 1);
    occupied.emplace_back(0, header_size);
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto relative = read_u32(payload, 12 + index * 4);

        if (relative > payload.size() - header_size)
            fail("TBCR: section offset exceeds payload");
        const std::size_t start = header_size + relative;
        if (payload.size() - start < 16 || std::memcmp(payload.data() + start, "TBL1", 4) != 0)
            fail("TBCR: missing section magic/header");
        const auto size = read_u32(payload, start + 4);
        const auto stride = read_u32(payload, start + 8);
        if (read_u32(payload, start + 12) != 16)
            fail("TBCR: unsupported TBL1 header size");
        if (size && (!stride || size % stride))
            fail("TBCR: record size is not a multiple of stride");
        const std::size_t body = start + 16;
        if (size > payload.size() - body)
            fail("TBCR: records exceed payload bounds");
        result.sections_.push_back({index, start, body, size, stride, size ? size / stride : 0});
        occupied.emplace_back(start, body + size);
    }
    std::sort(occupied.begin(), occupied.end());
    for (std::size_t i = 1; i < occupied.size(); ++i)
        if (occupied[i - 1].second > occupied[i].first)
            fail("TBCR: overlapping sections");
    result.bytes_.assign(payload.begin(), payload.end());
    return result;
}

const Section& Document::section(std::size_t index) const
{
    if (index >= sections_.size())
        fail("TBCR: section index out of range");
    return sections_[index];
}

std::span<const std::uint8_t> Document::record(
    std::size_t section_index, std::size_t record_index) const
{
    const auto& selected = section(section_index);
    if (record_index >= selected.record_count)
        fail("TBCR: record index out of range");
    return std::span<const std::uint8_t>{bytes_}.subspan(
        selected.data_offset + record_index * selected.record_stride, selected.record_stride);
}

void Document::update(std::size_t section_index, std::size_t record_index,
    std::size_t record_offset, std::span<const std::uint8_t> bytes)
{
    const auto target = record(section_index, record_index);
    if (record_offset > target.size() || bytes.size() > target.size() - record_offset)
        fail("TBCR: write exceeds record bounds");
    if (!bytes.empty())
    {
        const auto& selected = section(section_index);

        std::memmove(bytes_.data() + selected.data_offset +
            record_index * selected.record_stride + record_offset, bytes.data(), bytes.size());
    }
}

std::vector<RecordEdit> Document::prepare_updates(std::span<const RecordPatch> patches) const
{
    std::set<std::pair<std::size_t, std::size_t>> selected;
    std::vector<RecordEdit> edits;
    edits.reserve(patches.size());
    for (const auto& patch : patches)
    {
        const auto source = record(patch.table, patch.record);
        if (patch.bytes.size() != source.size() || patch.active_mask.size() != source.size())
            fail("TBCR: composed update stride mismatch");
        if (!selected.emplace(patch.table, patch.record).second)
            fail("TBCR: duplicate composed record update");
        RecordEdit edit{section(patch.table).data_offset + patch.record * source.size(),
            {source.begin(), source.end()}, {source.begin(), source.end()}};
        for (std::size_t i = 0; i < source.size(); ++i)
        {
            if (patch.active_mask[i] != 0 && patch.active_mask[i] != 0xFF)
                fail("TBCR: invalid composed update mask");
            if (patch.active_mask[i]) edit.after[i] = patch.bytes[i];
        }
        if (edit.before != edit.after) edits.push_back(std::move(edit));
    }
    return edits;
}

bool Document::supports_growth() const noexcept
{
    if (header_size_ % 16)
        return false;
    std::size_t end = header_size_;
    for (const auto& selected : sections_)
    {
        if (selected.offset != align16(end) || !zero_range(bytes_, end, selected.offset))
            return false;
        end = selected.data_offset + selected.data_bytes;
    }
    return bytes_.size() == align16(end) && zero_range(bytes_, end, bytes_.size());
}

void Document::append(std::size_t section_index, std::span<const std::uint8_t> full_record)
{
    const auto& selected = section(section_index);
    if (!selected.record_stride || full_record.size() != selected.record_stride)
        fail("TBCR: append requires exactly one nonzero-stride record");
    if (!supports_growth())
        fail("TBCR: unsupported growth framing (requires ordered zero 16-byte alignment)");
    if (selected.record_stride > max_payload_bytes - bytes_.size() ||
        full_record.size() > std::numeric_limits<std::uint32_t>::max() - selected.data_bytes)
        fail("TBCR: append exceeds payload/data length limit");

    Bytes candidate(bytes_.begin(), bytes_.begin() + header_size_);
    candidate.reserve(bytes_.size() + full_record.size() + 15);
    for (const auto& current : sections_)
    {
        candidate.resize(align16(candidate.size()), 0);
        const auto start = candidate.size();
        write_u32(candidate, 12 + current.index * 4, start - header_size_);
        candidate.insert(candidate.end(), bytes_.begin() + current.offset,
            bytes_.begin() + current.data_offset + current.data_bytes);
        if (current.index == section_index)
        {
            candidate.insert(candidate.end(), full_record.begin(), full_record.end());
            write_u32(candidate, start + 4, current.data_bytes + full_record.size());
        }
    }
    candidate.resize(align16(candidate.size()), 0);
    if (candidate.size() > max_payload_bytes)
        fail("TBCR: rebuilt payload exceeds 64 MiB limit");
    auto validated = parse(candidate);
    *this = std::move(validated);
}
}
