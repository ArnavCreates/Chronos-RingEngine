#pragma once

#include "chronos/core/Types.hpp"

#include <stdexcept>
#include <vector>

namespace chronos {

// The full public order value occupies 32 bytes. Sidecar metadata keeps the
// aligned payload dense without narrowing the 64-bit public identifiers,
// prices, or quantities.
struct alignas(32) SlabNode {
    Order order{};
};
static_assert(sizeof(SlabNode) == 32, "SlabNode payload must occupy exactly 32 bytes");
static_assert(alignof(SlabNode) == 32, "SlabNode payload must be 32-byte aligned");

// Lightweight field view preserving the pool.get(index).order/prev/next/state
// access pattern while keeping link and state metadata in its own dense array.
struct SlabNodeView {
    Order& order;
    std::uint32_t& prev;
    std::uint32_t& next;
    SlotState& state;
};

struct ConstSlabNodeView {
    const Order& order;
    const std::uint32_t& prev;
    const std::uint32_t& next;
    const SlotState& state;
};

struct SlabNodeMetadata {
    std::uint32_t prev{kInvalidIndex};
    std::uint32_t next{kInvalidIndex};
    SlotState state{SlotState::Free};
};
static_assert(sizeof(SlabNodeMetadata) == 12, "metadata sidecar should remain compact");

class SlabPool {
public:
    explicit SlabPool(std::size_t capacity) : slots_(capacity), metadata_(capacity), capacity_(capacity) {
        if (capacity > static_cast<std::size_t>(kInvalidIndex)) {
            throw std::length_error("SlabPool capacity exceeds uint32 index range");
        }
        freeHead_ = capacity == 0 ? kInvalidIndex : 0;
        for (std::size_t i = 0; i < capacity; ++i) {
            metadata_[i].next = (i + 1 < capacity) ? static_cast<std::uint32_t>(i + 1) : kInvalidIndex;
        }
    }

    [[nodiscard]] std::uint32_t acquire(const Order& order) noexcept {
        if (freeHead_ == kInvalidIndex) {
            return kInvalidIndex;
        }
        const std::uint32_t index = freeHead_;
        SlabNodeMetadata& metadata = metadata_[index];
        freeHead_ = metadata.next;
        slots_[index].order = order;
        metadata.prev = kInvalidIndex;
        metadata.next = kInvalidIndex;
        metadata.state = SlotState::Active;
        ++occupiedCount_;  // Includes ghost nodes until physical reclamation.
        ++totalAcquired_;
        return index;
    }

    void markGhost(std::uint32_t index) noexcept {
        if (index < metadata_.size() && metadata_[index].state == SlotState::Active) {
            metadata_[index].state = SlotState::GhostCancelled;
            ++ghostMarkedCount_;
        }
    }

    // V4's flat cancellation index contains only live node indices. A valid
    // entry therefore proves the slot is active, avoiding a redundant state
    // read on the cancellation hot path.
    void markGhostKnownActive(std::uint32_t index) noexcept {
        metadata_[index].state = SlotState::GhostCancelled;
        ++ghostMarkedCount_;
    }

    void markFilled(std::uint32_t index) noexcept {
        if (index < metadata_.size() && metadata_[index].state == SlotState::Active) {
            metadata_[index].state = SlotState::Filled;
        }
    }

    void release(std::uint32_t index, bool wasGhost = false) noexcept {
        if (index >= metadata_.size()) {
            return;
        }
        SlabNodeMetadata& metadata = metadata_[index];
        if (metadata.state == SlotState::Free) {
            return;
        }
        const bool ghost = wasGhost || metadata.state == SlotState::GhostCancelled;
        metadata.state = SlotState::Free;
        metadata.prev = kInvalidIndex;
        metadata.next = freeHead_;
        freeHead_ = index;
        --occupiedCount_;
        if (ghost) {
            ++ghostReclaimedCount_;
        }
    }

    [[nodiscard]] SlabNodeView get(std::uint32_t index) noexcept {
        return {slots_[index].order, metadata_[index].prev, metadata_[index].next, metadata_[index].state};
    }
    [[nodiscard]] ConstSlabNodeView get(std::uint32_t index) const noexcept {
        return {slots_[index].order, metadata_[index].prev, metadata_[index].next, metadata_[index].state};
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t activeCount() const noexcept { return occupiedCount_; }
    [[nodiscard]] std::size_t freeCount() const noexcept { return capacity_ - occupiedCount_; }
    [[nodiscard]] std::uint64_t totalAcquired() const noexcept { return totalAcquired_; }
    [[nodiscard]] std::uint64_t ghostMarkedCount() const noexcept { return ghostMarkedCount_; }
    [[nodiscard]] std::uint64_t ghostReclaimedCount() const noexcept { return ghostReclaimedCount_; }

private:
    std::vector<SlabNode> slots_;
    std::vector<SlabNodeMetadata> metadata_;
    std::size_t capacity_{0};
    std::uint32_t freeHead_{kInvalidIndex};
    std::size_t occupiedCount_{0};
    std::uint64_t totalAcquired_{0};
    std::uint64_t ghostMarkedCount_{0};
    std::uint64_t ghostReclaimedCount_{0};
};

}  // namespace chronos
