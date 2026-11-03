#pragma once

#include "chronos/core/Types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

namespace chronos {

template <typename T>
class MpscRingBuffer {
    static_assert(std::is_nothrow_copy_assignable_v<T>, "MpscRingBuffer payload copy assignment must be noexcept");
    static_assert(std::is_nothrow_move_assignable_v<T>, "MpscRingBuffer payload move assignment must be noexcept");

    struct alignas(kCacheLineSize) Slot {
        std::atomic<std::uint64_t> sequence{0};
        T payload{};
    };

public:
    explicit MpscRingBuffer(std::size_t capacity)
        : capacity_(capacity), mask_(capacity - 1), buffer_(capacity) {
        if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("MpscRingBuffer capacity must be a power of two >= 2");
        }
        for (std::size_t i = 0; i < capacity_; ++i) {
            buffer_[i].sequence.store(static_cast<std::uint64_t>(i), std::memory_order_relaxed);
        }
    }

    // Nonblocking reservation. False means the ring is currently full.
    [[nodiscard]] bool try_push(const T& item) noexcept {
        std::uint64_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = buffer_[static_cast<std::size_t>(pos) & mask_];
            const std::uint64_t sequence = slot.sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<std::int64_t>(sequence - pos);
            if (difference == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
                    slot.payload = item;
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }

    // Blocking convenience API. It makes progress only while the consumer
    // continues draining; use try_push when the caller needs nonblocking flow.
    void push(const T& item) noexcept {
        while (!try_push(item)) {
            pause();
        }
    }

    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::uint64_t pos = tail_;
        Slot& slot = buffer_[static_cast<std::size_t>(pos) & mask_];
        const std::uint64_t sequence = slot.sequence.load(std::memory_order_acquire);
        const auto difference = static_cast<std::int64_t>(sequence - (pos + 1));
        if (difference < 0) {
            return false;
        }
        if (difference > 0) {
            return false;
        }
        out = std::move(slot.payload);
        slot.sequence.store(pos + capacity_, std::memory_order_release);
        ++tail_;
        return true;
    }

    [[nodiscard]] T pop() noexcept {
        T out{};
        while (!try_pop(out)) {
            pause();
        }
        return out;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    static void pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        _mm_pause();
#else
        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    std::vector<Slot> buffer_;
    alignas(kCacheLineSize) std::atomic<std::uint64_t> head_{0};
    alignas(kCacheLineSize) std::uint64_t tail_{0};
};

}  // namespace chronos
