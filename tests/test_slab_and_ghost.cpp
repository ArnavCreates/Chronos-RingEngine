#include "chronos/engine/MatchingEngine.hpp"
#include "chronos/engine/Variants.hpp"
#include "chronos/memory/SlabPool.hpp"
#include "chronos/utils/PerfCounters.hpp"

#include <gtest/gtest.h>

static_assert(sizeof(chronos::SlabNode) == 32);
static_assert(alignof(chronos::SlabNode) == 32);

TEST(SlabPool, AcquireExhaustReleaseAndTelemetry) {
    chronos::SlabPool pool(2);
    const std::uint32_t first = pool.acquire({1, 100, 3, chronos::Side::Buy, chronos::OrderType::Limit});
    const std::uint32_t second = pool.acquire({2, 101, 4, chronos::Side::Sell, chronos::OrderType::Limit});
    EXPECT_NE(first, chronos::kInvalidIndex);
    EXPECT_NE(second, chronos::kInvalidIndex);
    EXPECT_EQ(pool.acquire({3, 102, 5, chronos::Side::Buy, chronos::OrderType::Limit}), chronos::kInvalidIndex);
    EXPECT_EQ(pool.activeCount(), 2u);
    EXPECT_EQ(pool.freeCount(), 0u);
    EXPECT_EQ(pool.totalAcquired(), 2u);
    pool.markGhost(first);
    EXPECT_EQ(pool.get(first).state, chronos::SlotState::GhostCancelled);
    pool.release(first, true);
    pool.release(second);
    EXPECT_EQ(pool.get(first).state, chronos::SlotState::Free);
    EXPECT_EQ(pool.activeCount(), 0u);
    EXPECT_EQ(pool.freeCount(), 2u);
    EXPECT_EQ(pool.ghostMarkedCount(), 1u);
    EXPECT_EQ(pool.ghostReclaimedCount(), 1u);
}

TEST(SlabAndGhost, HeadGhostReclaimsImmediatelyAndInteriorGhostIsSweptLazily) {
    chronos::MatchingEngine engine(chronos::Variant4_ChronosBook{9000, 11000, 1, 16, 64}, 16);
    (void)engine.processOrderHot({10, 10000, 5, chronos::Side::Sell, chronos::OrderType::Limit});
    (void)engine.processOrderHot({11, 10000, 3, chronos::Side::Sell, chronos::OrderType::Limit});
    (void)engine.processOrderHot({12, 10000, 7, chronos::Side::Sell, chronos::OrderType::Limit});
    const std::uint32_t headIndex = engine.getBook().nodeIndexFor(10);
    const std::uint32_t interiorGhostIndex = engine.getBook().nodeIndexFor(11);
    ASSERT_NE(headIndex, chronos::kInvalidIndex);
    ASSERT_NE(interiorGhostIndex, chronos::kInvalidIndex);

    EXPECT_TRUE(engine.cancelOrder(11));
    EXPECT_EQ(engine.getBook().pool().get(interiorGhostIndex).state, chronos::SlotState::GhostCancelled);
    EXPECT_TRUE(engine.cancelOrder(10));
    EXPECT_EQ(engine.getBook().pool().get(headIndex).state, chronos::SlotState::Free);
    EXPECT_EQ(engine.getBook().pool().get(interiorGhostIndex).state, chronos::SlotState::GhostCancelled);
    EXPECT_EQ(engine.getBook().topAskPrice(), 10000u);
    auto snapshot = engine.getSnapshot();
    ASSERT_EQ(snapshot.asks.size(), 1u);
    EXPECT_EQ(snapshot.asks[0], (chronos::LevelSummary{10000, 7, 1}));

    const auto trades = engine.processOrderHot({20, 10000, 7, chronos::Side::Buy, chronos::OrderType::Limit});
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (chronos::Trade{12, 20, 10000, 7}));
    EXPECT_EQ(engine.getBook().pool().get(interiorGhostIndex).state, chronos::SlotState::Free);
    EXPECT_EQ(engine.getBook().pool().ghostReclaimedCount(), 2u);
    EXPECT_EQ(engine.getBook().topAskPrice(), 0u);
}

TEST(SlabAndGhost, CancellingLastActiveOrderImmediatelyReclaimsGhostOnlyLevel) {
    chronos::MatchingEngine engine(chronos::Variant4_ChronosBook{9000, 11000, 1, 8, 32}, 8);
    (void)engine.processOrderHot({3, 10001, 2, chronos::Side::Buy, chronos::OrderType::Limit});
    const std::uint32_t nodeIndex = engine.getBook().nodeIndexFor(3);
    ASSERT_NE(nodeIndex, chronos::kInvalidIndex);
    EXPECT_TRUE(engine.cancelOrder(3));
    EXPECT_EQ(engine.getBook().topBidPrice(), 0u);
    EXPECT_EQ(engine.getBook().pool().get(nodeIndex).state, chronos::SlotState::Free);
    EXPECT_EQ(engine.getBook().pool().ghostMarkedCount(), 1u);
    EXPECT_EQ(engine.getBook().pool().ghostReclaimedCount(), 1u);
    EXPECT_TRUE(engine.getSnapshot().bids.empty());
}

TEST(SlabAndGhost, VariantFourHotAddMatchAndCancelUseNoRuntimeNewCalls) {
    chronos::MatchingEngine engine(chronos::Variant4_ChronosBook{9000, 11000, 1, 32, 128}, 32);
    (void)engine.processOrderHot({1, 10000, 4, chronos::Side::Sell, chronos::OrderType::Limit});

    chronos::HeapAllocationTracker tracker;
    (void)engine.processOrderHot({2, 10000, 4, chronos::Side::Buy, chronos::OrderType::Limit});
    (void)engine.processOrderHot({3, 10001, 1, chronos::Side::Buy, chronos::OrderType::Limit});
    (void)engine.cancelOrder(3);
    const std::uint64_t allocations = tracker.stop();
    EXPECT_EQ(allocations, 0u);
}
