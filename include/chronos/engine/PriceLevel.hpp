#pragma once

#include "chronos/core/Types.hpp"

#include <list>

namespace chronos {

struct PriceLevelList {
    Price price{0};
    Quantity activeQuantity{0};
    std::size_t activeOrderCount{0};
    std::list<Order> orders;
};

struct IntrusivePriceLevel {
    Quantity activeQuantity{0};
    std::size_t activeOrderCount{0};
    std::uint32_t headIdx{kInvalidIndex};
    std::uint32_t tailIdx{kInvalidIndex};
};

struct OrderLocationV1 {
    Side side{Side::Buy};
    Price price{0};
    std::list<Order>::iterator iterator{};
};

struct OrderLocationV2 {
    Side side{Side::Buy};
    Price price{0};
    std::uint32_t nodeIdx{kInvalidIndex};
};

struct OrderLocationV3 {
    Side side{Side::Buy};
    std::size_t tick{0};
    std::uint32_t nodeIdx{kInvalidIndex};
};

struct OrderLocationV4 {
    std::uint32_t nodeIdx{kInvalidIndex};
    // The bounded flat-price domain fits in 31 bits; the high bit stores side.
    std::uint32_t tickAndSide{0};

    static constexpr std::uint32_t kSideBit = 1U << 31;
    static constexpr std::uint32_t kTickMask = kSideBit - 1;

    [[nodiscard]] Side side() const noexcept { return (tickAndSide & kSideBit) == 0 ? Side::Buy : Side::Sell; }
    [[nodiscard]] std::uint32_t tick() const noexcept { return tickAndSide & kTickMask; }
    void set(Side orderSide, std::size_t priceTick, std::uint32_t nodeIndex) noexcept {
        nodeIdx = nodeIndex;
        tickAndSide = static_cast<std::uint32_t>(priceTick) | (orderSide == Side::Sell ? kSideBit : 0U);
    }
};
static_assert(sizeof(OrderLocationV4) == 8, "V4 cancellation index entries must stay compact");

}  // namespace chronos
