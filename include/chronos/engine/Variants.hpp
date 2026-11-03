#pragma once

#include "chronos/core/Types.hpp"
#include "chronos/engine/PriceLevel.hpp"
#include "chronos/memory/SlabPool.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chronos {
namespace detail {

inline void appendTrade(std::vector<Trade>& tape, const Trade& trade) {
    if (tape.size() == tape.capacity()) {
        throw std::overflow_error("TradeTape capacity exhausted; configure it for the maximum resting order count");
    }
    tape.push_back(trade);
}

inline BookSnapshot finishSnapshot(BookSnapshot snapshot) {
    snapshot.stateHash = computeSnapshotHash(snapshot.bids, snapshot.asks);
    return snapshot;
}

inline void unlinkIntrusive(SlabPool& pool, IntrusivePriceLevel& level, std::uint32_t index) noexcept {
    SlabNodeView node = pool.get(index);
    if (node.prev != kInvalidIndex) {
        pool.get(node.prev).next = node.next;
    } else {
        level.headIdx = node.next;
    }
    if (node.next != kInvalidIndex) {
        pool.get(node.next).prev = node.prev;
    } else {
        level.tailIdx = node.prev;
    }
}

inline void appendIntrusive(SlabPool& pool, IntrusivePriceLevel& level, std::uint32_t index) noexcept {
    SlabNodeView node = pool.get(index);
    node.prev = level.tailIdx;
    node.next = kInvalidIndex;
    if (level.tailIdx != kInvalidIndex) {
        pool.get(level.tailIdx).next = index;
    } else {
        level.headIdx = index;
    }
    level.tailIdx = index;
}

template <typename Map>
inline void addSummary(const Map& levels, std::vector<LevelSummary>& output) {
    for (const auto& [price, level] : levels) {
        if (level.activeOrderCount != 0) {
            output.push_back({price, level.activeQuantity, level.activeOrderCount});
        }
    }
}

inline std::size_t checkedTickCount(Price minPrice, Price maxPrice, Price tickSize) {
    if (tickSize == 0 || maxPrice < minPrice || (maxPrice - minPrice) % tickSize != 0) {
        throw std::invalid_argument("price bounds must be ordered and separated by a positive whole number of ticks");
    }
    const std::uint64_t count = (maxPrice - minPrice) / tickSize + 1;
    if (count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::length_error("price range exceeds addressable tick count");
    }
    return static_cast<std::size_t>(count);
}

inline std::size_t checkedCompactTickCount(Price minPrice, Price maxPrice, Price tickSize) {
    const std::size_t count = checkedTickCount(minPrice, maxPrice, tickSize);
    if (count > static_cast<std::size_t>(OrderLocationV4::kSideBit)) {
        throw std::length_error("V4 flat price range exceeds its 31-bit tick index");
    }
    return count;
}

struct FlatSideV3 {
    explicit FlatSideV3(std::size_t ticks) : levels(ticks), bitmap((ticks + 63) / 64, 0) {}

    std::vector<IntrusivePriceLevel> levels;
    std::vector<std::uint64_t> bitmap;

    void setOccupied(std::size_t tick) noexcept { bitmap[tick / 64] |= (1ULL << (tick % 64)); }
    void clearOccupied(std::size_t tick) noexcept { bitmap[tick / 64] &= ~(1ULL << (tick % 64)); }

    [[nodiscard]] std::size_t nextOccupied(std::size_t from) const noexcept {
        if (from >= levels.size()) {
            return levels.size();
        }
        std::size_t wordIndex = from / 64;
        std::uint64_t word = bitmap[wordIndex] & (~0ULL << (from % 64));
        for (;;) {
            if (word != 0) {
                const std::size_t tick = wordIndex * 64 + static_cast<std::size_t>(std::countr_zero(word));
                return tick < levels.size() ? tick : levels.size();
            }
            if (++wordIndex >= bitmap.size()) {
                return levels.size();
            }
            word = bitmap[wordIndex];
        }
    }

    [[nodiscard]] std::size_t prevOccupied(std::size_t from) const noexcept {
        if (from >= levels.size()) {
            return levels.size();
        }
        std::size_t wordIndex = from / 64;
        const std::size_t bit = from % 64;
        std::uint64_t word = bitmap[wordIndex] & (bit == 63 ? ~0ULL : ((1ULL << (bit + 1)) - 1));
        for (;;) {
            if (word != 0) {
                return wordIndex * 64 + (63U - static_cast<unsigned>(std::countl_zero(word)));
            }
            if (wordIndex == 0) {
                return levels.size();
            }
            word = bitmap[--wordIndex];
        }
    }
};

struct FlatSideV4 : FlatSideV3 {
    FlatSideV4(std::size_t ticks, bool bidSide) : FlatSideV3(ticks), isBid(bidSide) {}

    bool isBid;
    std::size_t bestTick{std::numeric_limits<std::size_t>::max()};

    void noteInserted(std::size_t tick) noexcept {
        if (bestTick == std::numeric_limits<std::size_t>::max() || (isBid ? tick > bestTick : tick < bestTick)) {
            bestTick = tick;
        }
    }

    void noteMaybeCleared(std::size_t tick) noexcept {
        if (tick != bestTick) {
            return;
        }
        if (isBid) {
            bestTick = tick == 0 ? std::numeric_limits<std::size_t>::max() : prevOccupied(tick - 1);
            if (bestTick == levels.size()) {
                bestTick = std::numeric_limits<std::size_t>::max();
            }
        } else {
            const std::size_t next = tick + 1 < levels.size() ? nextOccupied(tick + 1) : levels.size();
            bestTick = next < levels.size() ? next : std::numeric_limits<std::size_t>::max();
        }
    }
};

}  // namespace detail

class Variant1_MapListBook {
public:
    void validateOrder(const Order&) const noexcept {}

    [[nodiscard]] bool hasOrder(OrderId id) const { return locations_.find(id) != locations_.end(); }

    [[nodiscard]] bool addOrder(const Order& order) {
        if (locations_.find(order.id) != locations_.end()) {
            return false;
        }
        if (order.side == Side::Buy) {
            addToSide(bids_, order);
        } else {
            addToSide(asks_, order);
        }
        return true;
    }

    [[nodiscard]] bool cancelOrder(OrderId id) {
        const auto locationIt = locations_.find(id);
        if (locationIt == locations_.end()) {
            return false;
        }
        const OrderLocationV1 location = locationIt->second;
        if (location.side == Side::Buy) {
            cancelFromSide(bids_, location);
        } else {
            cancelFromSide(asks_, location);
        }
        locations_.erase(locationIt);
        return true;
    }

    void matchAgainst(Order& incoming, std::optional<Price> limitPrice, std::vector<Trade>& trades) {
        if (incoming.side == Side::Buy) {
            matchSide(incoming, asks_, true, limitPrice, trades);
        } else {
            matchSide(incoming, bids_, false, limitPrice, trades);
        }
    }

    [[nodiscard]] BookSnapshot extractSnapshot() const {
        BookSnapshot snapshot;
        detail::addSummary(bids_, snapshot.bids);
        detail::addSummary(asks_, snapshot.asks);
        return detail::finishSnapshot(std::move(snapshot));
    }

    [[nodiscard]] Price topBidPrice() const noexcept { return bids_.empty() ? 0 : bids_.begin()->first; }
    [[nodiscard]] Price topAskPrice() const noexcept { return asks_.empty() ? 0 : asks_.begin()->first; }
    [[nodiscard]] std::size_t orderCount() const noexcept { return locations_.size(); }

private:
    template <typename Map>
    void addToSide(Map& levels, const Order& order) {
        auto [levelIt, inserted] = levels.try_emplace(order.price);
        PriceLevelList& level = levelIt->second;
        if (inserted) {
            level.price = order.price;
        }
        level.orders.push_back(order);
        auto orderIt = std::prev(level.orders.end());
        level.activeQuantity += order.quantity;
        ++level.activeOrderCount;
        locations_.emplace(order.id, OrderLocationV1{order.side, order.price, orderIt});
    }

    template <typename Map>
    void cancelFromSide(Map& levels, const OrderLocationV1& location) {
        const auto levelIt = levels.find(location.price);
        if (levelIt != levels.end()) {
            PriceLevelList& level = levelIt->second;
            level.activeQuantity -= location.iterator->quantity;
            --level.activeOrderCount;
            level.orders.erase(location.iterator);
            if (level.orders.empty()) {
                levels.erase(levelIt);
            }
        }
    }

    template <typename Map>
    void matchSide(Order& incoming, Map& levels, bool isBuy, std::optional<Price> limitPrice,
                   std::vector<Trade>& trades) {
        auto levelIt = levels.begin();
        while (levelIt != levels.end() && incoming.quantity > 0) {
            const Price levelPrice = levelIt->first;
            if (limitPrice && (isBuy ? levelPrice > *limitPrice : levelPrice < *limitPrice)) {
                break;
            }
            PriceLevelList& level = levelIt->second;
            auto orderIt = level.orders.begin();
            while (orderIt != level.orders.end() && incoming.quantity > 0) {
                const Quantity fill = std::min(incoming.quantity, orderIt->quantity);
                detail::appendTrade(trades, Trade{orderIt->id, incoming.id, orderIt->price, fill});
                incoming.quantity -= fill;
                orderIt->quantity -= fill;
                level.activeQuantity -= fill;
                if (orderIt->quantity == 0) {
                    locations_.erase(orderIt->id);
                    --level.activeOrderCount;
                    orderIt = level.orders.erase(orderIt);
                } else {
                    break;
                }
            }
            if (level.orders.empty()) {
                levelIt = levels.erase(levelIt);
            } else {
                ++levelIt;
            }
        }
    }

    std::map<Price, PriceLevelList, std::greater<Price>> bids_;
    std::map<Price, PriceLevelList, std::less<Price>> asks_;
    std::unordered_map<OrderId, OrderLocationV1> locations_;
};

class Variant2_MapSlabBook {
public:
    explicit Variant2_MapSlabBook(std::size_t slabCapacity) : pool_(slabCapacity) {}

    void validateOrder(const Order&) const noexcept {}
    [[nodiscard]] bool hasOrder(OrderId id) const { return locations_.find(id) != locations_.end(); }

    [[nodiscard]] bool addOrder(const Order& order) {
        if (hasOrder(order.id)) {
            return false;
        }
        const std::uint32_t index = pool_.acquire(order);
        if (index == kInvalidIndex) {
            throw std::overflow_error("Variant2_MapSlabBook slab exhausted");
        }
        try {
            if (order.side == Side::Buy) {
                addToSide(bids_, order, index);
            } else {
                addToSide(asks_, order, index);
            }
        } catch (...) {
            pool_.release(index);
            throw;
        }
        locations_.emplace(order.id, OrderLocationV2{order.side, order.price, index});
        return true;
    }

    [[nodiscard]] bool cancelOrder(OrderId id) {
        const auto locationIt = locations_.find(id);
        if (locationIt == locations_.end()) {
            return false;
        }
        const OrderLocationV2 location = locationIt->second;
        if (location.side == Side::Buy) {
            cancelFromSide(bids_, location);
        } else {
            cancelFromSide(asks_, location);
        }
        locations_.erase(locationIt);
        return true;
    }

    void matchAgainst(Order& incoming, std::optional<Price> limitPrice, std::vector<Trade>& trades) {
        if (incoming.side == Side::Buy) {
            matchSide(incoming, asks_, true, limitPrice, trades);
        } else {
            matchSide(incoming, bids_, false, limitPrice, trades);
        }
    }

    [[nodiscard]] BookSnapshot extractSnapshot() const {
        BookSnapshot snapshot;
        detail::addSummary(bids_, snapshot.bids);
        detail::addSummary(asks_, snapshot.asks);
        return detail::finishSnapshot(std::move(snapshot));
    }

    [[nodiscard]] Price topBidPrice() const noexcept { return bids_.empty() ? 0 : bids_.begin()->first; }
    [[nodiscard]] Price topAskPrice() const noexcept { return asks_.empty() ? 0 : asks_.begin()->first; }
    [[nodiscard]] std::size_t orderCount() const noexcept { return locations_.size(); }
    [[nodiscard]] SlabPool& pool() noexcept { return pool_; }
    [[nodiscard]] const SlabPool& pool() const noexcept { return pool_; }

private:
    template <typename Map>
    void addToSide(Map& levels, const Order& order, std::uint32_t index) {
        auto [levelIt, inserted] = levels.try_emplace(order.price);
        (void)inserted;
        IntrusivePriceLevel& level = levelIt->second;
        detail::appendIntrusive(pool_, level, index);
        level.activeQuantity += order.quantity;
        ++level.activeOrderCount;
    }

    template <typename Map>
    void cancelFromSide(Map& levels, const OrderLocationV2& location) {
        const auto levelIt = levels.find(location.price);
        if (levelIt == levels.end()) {
            return;
        }
        IntrusivePriceLevel& level = levelIt->second;
        level.activeQuantity -= pool_.get(location.nodeIdx).order.quantity;
        --level.activeOrderCount;
        detail::unlinkIntrusive(pool_, level, location.nodeIdx);
        pool_.release(location.nodeIdx);
        if (level.activeOrderCount == 0) {
            levels.erase(levelIt);
        }
    }

    template <typename Map>
    void matchSide(Order& incoming, Map& levels, bool isBuy, std::optional<Price> limitPrice,
                   std::vector<Trade>& trades) {
        auto levelIt = levels.begin();
        while (levelIt != levels.end() && incoming.quantity > 0) {
            const Price levelPrice = levelIt->first;
            if (limitPrice && (isBuy ? levelPrice > *limitPrice : levelPrice < *limitPrice)) {
                break;
            }
            IntrusivePriceLevel& level = levelIt->second;
            std::uint32_t index = level.headIdx;
            while (index != kInvalidIndex && incoming.quantity > 0) {
                SlabNodeView node = pool_.get(index);
                const Quantity fill = std::min(incoming.quantity, node.order.quantity);
                detail::appendTrade(trades, Trade{node.order.id, incoming.id, node.order.price, fill});
                incoming.quantity -= fill;
                node.order.quantity -= fill;
                level.activeQuantity -= fill;
                if (node.order.quantity == 0) {
                    locations_.erase(node.order.id);
                    --level.activeOrderCount;
                    const std::uint32_t next = node.next;
                    detail::unlinkIntrusive(pool_, level, index);
                    pool_.release(index);
                    index = next;
                } else {
                    break;
                }
            }
            if (level.activeOrderCount == 0) {
                levelIt = levels.erase(levelIt);
            } else {
                ++levelIt;
            }
        }
    }

    std::map<Price, IntrusivePriceLevel, std::greater<Price>> bids_;
    std::map<Price, IntrusivePriceLevel, std::less<Price>> asks_;
    std::unordered_map<OrderId, OrderLocationV2> locations_;
    SlabPool pool_;
};

class Variant3_FlatBitmapBook {
public:
    Variant3_FlatBitmapBook(Price minPrice, Price maxPrice, Price tickSize, std::size_t slabCapacity)
        : minPrice_(minPrice), tickSize_(tickSize), numTicks_(detail::checkedTickCount(minPrice, maxPrice, tickSize)),
          bids_(numTicks_), asks_(numTicks_), pool_(slabCapacity) {}

    void validateOrder(const Order& order) const {
        if (order.type == OrderType::Limit) {
            (void)tickIndex(order.price);
        }
    }
    [[nodiscard]] bool hasOrder(OrderId id) const { return locations_.find(id) != locations_.end(); }

    [[nodiscard]] bool addOrder(const Order& order) {
        if (hasOrder(order.id)) {
            return false;
        }
        const std::size_t tick = tickIndex(order.price);
        auto& side = order.side == Side::Buy ? bids_ : asks_;
        IntrusivePriceLevel& level = side.levels[tick];
        const bool wasEmpty = level.activeOrderCount == 0;
        const std::uint32_t index = pool_.acquire(order);
        if (index == kInvalidIndex) {
            throw std::overflow_error("Variant3_FlatBitmapBook slab exhausted");
        }
        detail::appendIntrusive(pool_, level, index);
        level.activeQuantity += order.quantity;
        ++level.activeOrderCount;
        if (wasEmpty) {
            side.setOccupied(tick);
        }
        locations_.emplace(order.id, OrderLocationV3{order.side, tick, index});
        return true;
    }

    [[nodiscard]] bool cancelOrder(OrderId id) {
        const auto locationIt = locations_.find(id);
        if (locationIt == locations_.end()) {
            return false;
        }
        const OrderLocationV3 location = locationIt->second;
        auto& side = location.side == Side::Buy ? bids_ : asks_;
        IntrusivePriceLevel& level = side.levels[location.tick];
        level.activeQuantity -= pool_.get(location.nodeIdx).order.quantity;
        --level.activeOrderCount;
        detail::unlinkIntrusive(pool_, level, location.nodeIdx);
        pool_.release(location.nodeIdx);
        if (level.activeOrderCount == 0) {
            side.clearOccupied(location.tick);
        }
        locations_.erase(locationIt);
        return true;
    }

    void matchAgainst(Order& incoming, std::optional<Price> limitPrice, std::vector<Trade>& trades) {
        if (incoming.side == Side::Buy) {
            matchSide(incoming, asks_, true, limitPrice, trades);
        } else {
            matchSide(incoming, bids_, false, limitPrice, trades);
        }
    }

    [[nodiscard]] BookSnapshot extractSnapshot() const {
        BookSnapshot snapshot;
        appendSideSnapshot(bids_, true, snapshot.bids);
        appendSideSnapshot(asks_, false, snapshot.asks);
        return detail::finishSnapshot(std::move(snapshot));
    }

    [[nodiscard]] Price topBidPrice() const noexcept {
        const std::size_t tick = bids_.prevOccupied(numTicks_ - 1);
        return tick == numTicks_ ? 0 : priceAt(tick);
    }
    [[nodiscard]] Price topAskPrice() const noexcept {
        const std::size_t tick = asks_.nextOccupied(0);
        return tick == numTicks_ ? 0 : priceAt(tick);
    }
    [[nodiscard]] std::size_t orderCount() const noexcept { return locations_.size(); }
    [[nodiscard]] SlabPool& pool() noexcept { return pool_; }
    [[nodiscard]] const SlabPool& pool() const noexcept { return pool_; }

private:
    [[nodiscard]] std::size_t tickIndex(Price price) const {
        if (price < minPrice_) {
            throw std::out_of_range("price is below the configured range or is not aligned to tickSize");
        }
        const Price delta = price - minPrice_;
        if (tickSize_ == 1) {
            const std::size_t tick = static_cast<std::size_t>(delta);
            if (tick >= numTicks_) {
                throw std::out_of_range("price is above the configured range");
            }
            return tick;
        }
        if (delta % tickSize_ != 0) {
            throw std::out_of_range("price is below the configured range or is not aligned to tickSize");
        }
        const std::size_t tick = static_cast<std::size_t>(delta / tickSize_);
        if (tick >= numTicks_) {
            throw std::out_of_range("price is above the configured range");
        }
        return tick;
    }

    [[nodiscard]] Price priceAt(std::size_t tick) const noexcept {
        return tickSize_ == 1 ? minPrice_ + static_cast<Price>(tick)
                              : minPrice_ + static_cast<Price>(tick) * tickSize_;
    }

    template <typename Output>
    void appendSideSnapshot(const detail::FlatSideV3& side, bool bids, Output& output) const {
        if (bids) {
            for (std::size_t tick = numTicks_; tick-- > 0;) {
                const auto& level = side.levels[tick];
                if (level.activeOrderCount != 0) {
                    output.push_back({priceAt(tick), level.activeQuantity, level.activeOrderCount});
                }
            }
        } else {
            for (std::size_t tick = 0; tick < numTicks_; ++tick) {
                const auto& level = side.levels[tick];
                if (level.activeOrderCount != 0) {
                    output.push_back({priceAt(tick), level.activeQuantity, level.activeOrderCount});
                }
            }
        }
    }

    void matchSide(Order& incoming, detail::FlatSideV3& side, bool isBuy, std::optional<Price> limitPrice,
                   std::vector<Trade>& trades) {
        std::size_t tick = isBuy ? side.nextOccupied(0) : side.prevOccupied(numTicks_ - 1);
        while (tick != numTicks_ && incoming.quantity > 0) {
            const Price levelPrice = priceAt(tick);
            if (limitPrice && (isBuy ? levelPrice > *limitPrice : levelPrice < *limitPrice)) {
                break;
            }
            IntrusivePriceLevel& level = side.levels[tick];
            std::uint32_t index = level.headIdx;
            while (index != kInvalidIndex && incoming.quantity > 0) {
                SlabNodeView node = pool_.get(index);
                const Quantity fill = std::min(incoming.quantity, node.order.quantity);
                detail::appendTrade(trades, Trade{node.order.id, incoming.id, node.order.price, fill});
                incoming.quantity -= fill;
                node.order.quantity -= fill;
                level.activeQuantity -= fill;
                if (node.order.quantity == 0) {
                    locations_.erase(node.order.id);
                    --level.activeOrderCount;
                    const std::uint32_t next = node.next;
                    detail::unlinkIntrusive(pool_, level, index);
                    pool_.release(index);
                    index = next;
                } else {
                    break;
                }
            }
            if (level.activeOrderCount == 0) {
                side.clearOccupied(tick);
                tick = isBuy ? side.nextOccupied(tick + 1)
                             : (tick == 0 ? numTicks_ : side.prevOccupied(tick - 1));
            } else {
                break;
            }
        }
    }

    Price minPrice_;
    Price tickSize_;
    std::size_t numTicks_;
    detail::FlatSideV3 bids_;
    detail::FlatSideV3 asks_;
    std::unordered_map<OrderId, OrderLocationV3> locations_;
    SlabPool pool_;
};

class Variant4_ChronosBook {
public:
    Variant4_ChronosBook(Price minPrice, Price maxPrice, Price tickSize, std::size_t slabCapacity,
                         std::size_t maxOrderIdCapacity)
        : minPrice_(minPrice), tickSize_(tickSize),
          numTicks_(detail::checkedCompactTickCount(minPrice, maxPrice, tickSize)),
          bids_(numTicks_, true), asks_(numTicks_, false), pool_(slabCapacity), flatIndex_(maxOrderIdCapacity) {}

    void validateOrder(const Order& order) const {
        (void)idIndex(order.id);
        if (order.type == OrderType::Limit) {
            (void)tickIndex(order.price);
        }
    }

    [[nodiscard]] bool hasOrder(OrderId id) const noexcept {
        if (id >= flatIndex_.size()) {
            return false;
        }
        const std::uint32_t index = flatIndex_[static_cast<std::size_t>(id)].nodeIdx;
        return index != kInvalidIndex;
    }

    [[nodiscard]] bool addOrder(const Order& order) {
        return addOrderImpl(order, true);
    }

    // MatchingEngine calls this only after validateOrder()/hasOrder() have
    // already proved the ID and price are valid and the ID is unused.
    [[nodiscard]] bool addOrderAfterPreflight(const Order& order) {
        return addOrderImpl(order, false);
    }

    [[nodiscard]] bool cancelOrder(OrderId id) noexcept {
        if (id >= flatIndex_.size()) {
            return false;
        }
        const std::size_t idSlot = static_cast<std::size_t>(id);
        const OrderLocationV4 location = flatIndex_[idSlot];
        if (location.nodeIdx == kInvalidIndex) {
            return false;
        }
        const std::size_t tick = location.tick();
        auto& side = location.side() == Side::Buy ? bids_ : asks_;
        IntrusivePriceLevel& level = side.levels[tick];
        const Quantity remaining = pool_.get(location.nodeIdx).order.quantity;
        level.activeQuantity -= remaining;
        --level.activeOrderCount;
        pool_.markGhostKnownActive(location.nodeIdx);
        flatIndex_[idSlot].nodeIdx = kInvalidIndex;
        if (level.activeOrderCount == 0) {
            side.clearOccupied(tick);
            side.noteMaybeCleared(tick);
            // No future matching cursor is guaranteed to visit an inactive
            // level, so reclaim its all-ghost chain now.
            sweepWholeLevel(level);
        } else if (location.nodeIdx == level.headIdx) {
            // Keep cancellation O(1): a cancelled FIFO head cannot block any
            // live order, so unlink and reclaim it immediately. Ghosts behind
            // a live head remain lazily reclaimed during matching.
            detail::unlinkIntrusive(pool_, level, location.nodeIdx);
            pool_.release(location.nodeIdx, true);
        }
        return true;
    }

    void matchAgainst(Order& incoming, std::optional<Price> limitPrice, std::vector<Trade>& trades) {
        if (incoming.side == Side::Buy) {
            matchSide(incoming, asks_, true, limitPrice, trades);
        } else {
            matchSide(incoming, bids_, false, limitPrice, trades);
        }
    }

    [[nodiscard]] BookSnapshot extractSnapshot() const {
        BookSnapshot snapshot;
        appendSideSnapshot(bids_, true, snapshot.bids);
        appendSideSnapshot(asks_, false, snapshot.asks);
        return detail::finishSnapshot(std::move(snapshot));
    }

    [[nodiscard]] Price topBidPrice() const noexcept {
        return bids_.bestTick == kNoTick ? 0 : priceAt(bids_.bestTick);
    }
    [[nodiscard]] Price topAskPrice() const noexcept {
        return asks_.bestTick == kNoTick ? 0 : priceAt(asks_.bestTick);
    }
    [[nodiscard]] std::size_t orderCount() const noexcept {
        std::size_t count = 0;
        for (const auto& location : flatIndex_) {
            if (location.nodeIdx != kInvalidIndex) {
                ++count;
            }
        }
        return count;
    }
    [[nodiscard]] std::size_t maxOrderIdCapacity() const noexcept { return flatIndex_.size(); }
    [[nodiscard]] SlabPool& pool() noexcept { return pool_; }
    [[nodiscard]] const SlabPool& pool() const noexcept { return pool_; }
    [[nodiscard]] std::uint32_t nodeIndexFor(OrderId id) const noexcept {
        return id < flatIndex_.size() ? flatIndex_[static_cast<std::size_t>(id)].nodeIdx : kInvalidIndex;
    }

private:
    static constexpr std::size_t kNoTick = std::numeric_limits<std::size_t>::max();

    [[nodiscard]] bool addOrderImpl(const Order& order, bool checkDuplicate) {
        const std::size_t id = checkDuplicate ? idIndex(order.id) : static_cast<std::size_t>(order.id);
        if (checkDuplicate && flatIndex_[id].nodeIdx != kInvalidIndex) {
            return false;
        }
        const std::size_t tick = tickIndex(order.price);
        auto& side = order.side == Side::Buy ? bids_ : asks_;
        IntrusivePriceLevel& level = side.levels[tick];
        const bool wasEmpty = level.activeOrderCount == 0;
        const std::uint32_t index = pool_.acquire(order);
        if (index == kInvalidIndex) {
            throw std::overflow_error("Variant4_ChronosBook slab exhausted");
        }
        detail::appendIntrusive(pool_, level, index);
        level.activeQuantity += order.quantity;
        ++level.activeOrderCount;
        if (wasEmpty) {
            side.setOccupied(tick);
            side.noteInserted(tick);
        }
        flatIndex_[id].set(order.side, tick, index);
        return true;
    }

    [[nodiscard]] std::size_t idIndex(OrderId id) const {
        if (id >= flatIndex_.size()) {
            throw std::out_of_range("OrderId exceeds configured flat-index capacity");
        }
        return static_cast<std::size_t>(id);
    }

    [[nodiscard]] std::size_t tickIndex(Price price) const {
        if (price < minPrice_) {
            throw std::out_of_range("price is below the configured range or is not aligned to tickSize");
        }
        const Price delta = price - minPrice_;
        if (tickSize_ == 1) {
            const std::size_t tick = static_cast<std::size_t>(delta);
            if (tick >= numTicks_) {
                throw std::out_of_range("price is above the configured range");
            }
            return tick;
        }
        if (delta % tickSize_ != 0) {
            throw std::out_of_range("price is below the configured range or is not aligned to tickSize");
        }
        const std::size_t tick = static_cast<std::size_t>(delta / tickSize_);
        if (tick >= numTicks_) {
            throw std::out_of_range("price is above the configured range");
        }
        return tick;
    }

    [[nodiscard]] Price priceAt(std::size_t tick) const noexcept {
        return tickSize_ == 1 ? minPrice_ + static_cast<Price>(tick)
                              : minPrice_ + static_cast<Price>(tick) * tickSize_;
    }

    void sweepWholeLevel(IntrusivePriceLevel& level) noexcept {
        while (level.headIdx != kInvalidIndex) {
            const std::uint32_t index = level.headIdx;
            detail::unlinkIntrusive(pool_, level, index);
            pool_.release(index, true);
        }
        level.activeQuantity = 0;
        level.activeOrderCount = 0;
    }

    template <typename Output>
    void appendSideSnapshot(const detail::FlatSideV4& side, bool bids, Output& output) const {
        if (bids) {
            for (std::size_t tick = numTicks_; tick-- > 0;) {
                const auto& level = side.levels[tick];
                if (level.activeOrderCount != 0) {
                    output.push_back({priceAt(tick), level.activeQuantity, level.activeOrderCount});
                }
            }
        } else {
            for (std::size_t tick = 0; tick < numTicks_; ++tick) {
                const auto& level = side.levels[tick];
                if (level.activeOrderCount != 0) {
                    output.push_back({priceAt(tick), level.activeQuantity, level.activeOrderCount});
                }
            }
        }
    }

    void matchSide(Order& incoming, detail::FlatSideV4& side, bool isBuy, std::optional<Price> limitPrice,
                   std::vector<Trade>& trades) {
        while (side.bestTick != kNoTick && incoming.quantity > 0) {
            const std::size_t tick = side.bestTick;
            const Price levelPrice = priceAt(tick);
            if (limitPrice && (isBuy ? levelPrice > *limitPrice : levelPrice < *limitPrice)) {
                break;
            }
            IntrusivePriceLevel& level = side.levels[tick];
            std::uint32_t index = level.headIdx;
            while (index != kInvalidIndex && incoming.quantity > 0) {
                SlabNodeView node = pool_.get(index);
                if (node.state == SlotState::GhostCancelled) {
                    const std::uint32_t next = node.next;
                    detail::unlinkIntrusive(pool_, level, index);
                    pool_.release(index, true);
                    index = next;
                    continue;
                }
                assert(node.state == SlotState::Active);
                const Quantity fill = std::min(incoming.quantity, node.order.quantity);
                detail::appendTrade(trades, Trade{node.order.id, incoming.id, node.order.price, fill});
                incoming.quantity -= fill;
                node.order.quantity -= fill;
                level.activeQuantity -= fill;
                if (node.order.quantity == 0) {
                    const std::size_t id = static_cast<std::size_t>(node.order.id);
                    if (id < flatIndex_.size() && flatIndex_[id].nodeIdx == index) {
                        flatIndex_[id].nodeIdx = kInvalidIndex;
                    }
                    --level.activeOrderCount;
                    pool_.markFilled(index);
                    const std::uint32_t next = node.next;
                    detail::unlinkIntrusive(pool_, level, index);
                    pool_.release(index);
                    index = next;
                } else {
                    break;
                }
            }
            if (level.activeOrderCount == 0) {
                side.clearOccupied(tick);
                side.noteMaybeCleared(tick);
                sweepWholeLevel(level);
            } else if (incoming.quantity == 0) {
                break;
            } else {
                // A live level can only remain after its active orders have
                // been consumed by the incoming order.
                break;
            }
        }
    }

    Price minPrice_;
    Price tickSize_;
    std::size_t numTicks_;
    detail::FlatSideV4 bids_;
    detail::FlatSideV4 asks_;
    SlabPool pool_;
    std::vector<OrderLocationV4> flatIndex_;
};

}  // namespace chronos
