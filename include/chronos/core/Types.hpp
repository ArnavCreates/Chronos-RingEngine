#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace chronos {

inline constexpr std::size_t kCacheLineSize = 64;
using OrderId = std::uint64_t;
using Price = std::uint64_t;
using Quantity = std::uint64_t;
inline constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class OrderType : std::uint8_t { Limit = 0, Market = 1 };
enum class RequestType : std::uint8_t { Add = 0, Cancel = 1, Modify = 2, Stop = 3 };
enum class SlotState : std::uint8_t { Free = 0, Active = 1, GhostCancelled = 2, Filled = 3 };

struct Order {
    OrderId id{0};
    Price price{0};
    Quantity quantity{0};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    bool operator==(const Order&) const = default;
};

struct Trade {
    OrderId makerId{0};
    OrderId takerId{0};
    Price price{0};
    Quantity quantity{0};
    bool operator==(const Trade&) const = default;
};

struct EventCommand {
    RequestType type{RequestType::Add};
    Order order{};
};
using OrderRequest = EventCommand;

struct LevelSummary {
    Price price{0};
    Quantity totalQuantity{0};
    std::size_t activeOrderCount{0};
    bool operator==(const LevelSummary&) const = default;
};

struct BookSnapshot {
    std::vector<LevelSummary> bids;
    std::vector<LevelSummary> asks;
    std::uint64_t stateHash{0};
    bool operator==(const BookSnapshot&) const = default;
};

namespace detail {
inline void fnvFeedByte(std::uint64_t& hash, std::uint8_t byte) noexcept {
    hash ^= byte;
    hash *= 1099511628211ULL;
}

inline void fnvFeedU64Le(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        fnvFeedByte(hash, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

inline void fnvFeedLevels(std::uint64_t& hash, std::uint8_t sideTag,
                          const std::vector<LevelSummary>& levels) noexcept {
    fnvFeedByte(hash, sideTag);
    fnvFeedU64Le(hash, static_cast<std::uint64_t>(levels.size()));
    for (const auto& level : levels) {
        fnvFeedU64Le(hash, level.price);
        fnvFeedU64Le(hash, level.totalQuantity);
        fnvFeedU64Le(hash, static_cast<std::uint64_t>(level.activeOrderCount));
    }
}
}  // namespace detail

class TradeLedgerHasher {
public:
    void update(std::span<const Trade> trades) noexcept {
        for (const Trade& trade : trades) {
            detail::fnvFeedU64Le(hash_, trade.makerId);
            detail::fnvFeedU64Le(hash_, trade.takerId);
            detail::fnvFeedU64Le(hash_, trade.price);
            detail::fnvFeedU64Le(hash_, trade.quantity);
            ++tradeCount_;
        }
    }

    [[nodiscard]] std::uint64_t value() const noexcept {
        std::uint64_t result = hash_;
        detail::fnvFeedU64Le(result, tradeCount_);
        return result;
    }

private:
    std::uint64_t hash_{14695981039346656037ULL};
    std::uint64_t tradeCount_{0};
};

inline std::uint64_t computeTradeLedgerHash(std::span<const Trade> trades) noexcept {
    TradeLedgerHasher hasher;
    hasher.update(trades);
    return hasher.value();
}

// Hashes a canonical little-endian encoding, with side tags and level counts,
// so the result is stable across compilers and host endianness.
inline std::uint64_t computeSnapshotHash(const std::vector<LevelSummary>& bids,
                                         const std::vector<LevelSummary>& asks) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    detail::fnvFeedLevels(hash, 0x42, bids);
    detail::fnvFeedLevels(hash, 0x41, asks);
    return hash;
}

}  // namespace chronos
