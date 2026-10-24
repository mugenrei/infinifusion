#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace infinifusion { namespace runtime {

template<std::size_t Capacity, std::size_t RecordBytes>
class LogQueue {
public:
    using Record = std::array<char, RecordBytes>;
    bool push(const Record& record) {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || count_ == Capacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        records_[(head_ + count_) % Capacity] = record;
        ++count_;
        return true;
    }
    bool pop(Record& record) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!count_) return false;
        record = records_[head_];
        head_ = (head_ + 1) % Capacity;
        --count_;
        return true;
    }
    std::uint64_t take_dropped() { return dropped_.exchange(0, std::memory_order_relaxed); }
private:
    static_assert(Capacity && RecordBytes, "nonempty bounded log queue");
    std::array<Record, Capacity> records_{};
    std::mutex mutex_;
    std::atomic<std::uint64_t> dropped_{};
    std::size_t head_{}, count_{};
};
}}
