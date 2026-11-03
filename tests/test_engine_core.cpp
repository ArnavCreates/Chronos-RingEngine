#include "chronos/engine/MatchingEngine.hpp"
#include "chronos/engine/Variants.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <utility>

namespace chronos::test {

template <typename Book>
void verifyFifoAndPartialFill(Book book) {
    MatchingEngine<Book> engine(std::move(book), 64);
    EXPECT_TRUE(engine.processOrder({1, 10000, 5, Side::Buy, OrderType::Limit}).empty());
    EXPECT_TRUE(engine.processOrder({2, 10000, 5, Side::Buy, OrderType::Limit}).empty());
    const auto trades = engine.processOrder({3, 10000, 7, Side::Sell, OrderType::Limit});
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0], (Trade{1, 3, 10000, 5}));
    EXPECT_EQ(trades[1], (Trade{2, 3, 10000, 2}));
    const BookSnapshot snapshot = engine.getSnapshot();
    ASSERT_EQ(snapshot.bids.size(), 1u);
    EXPECT_EQ(snapshot.bids[0], (LevelSummary{10000, 3, 1}));
    EXPECT_TRUE(snapshot.asks.empty());
    EXPECT_EQ(snapshot.stateHash, computeSnapshotHash(snapshot.bids, snapshot.asks));
}

}  // namespace chronos::test

TEST(EngineCore, FifoAndPartialFillAcrossAllVariants) {
    chronos::test::verifyFifoAndPartialFill(chronos::Variant1_MapListBook{});
    chronos::test::verifyFifoAndPartialFill(chronos::Variant2_MapSlabBook{64});
    chronos::test::verifyFifoAndPartialFill(chronos::Variant3_FlatBitmapBook{9000, 11000, 1, 64});
    chronos::test::verifyFifoAndPartialFill(chronos::Variant4_ChronosBook{9000, 11000, 1, 64, 128});
}

TEST(EngineCore, SweepsMultipleLevelsAndMarketRemainderDoesNotRest) {
    chronos::MatchingEngine engine(chronos::Variant1_MapListBook{}, 32);
    (void)engine.processOrder({1, 100, 2, chronos::Side::Sell, chronos::OrderType::Limit});
    (void)engine.processOrder({2, 101, 3, chronos::Side::Sell, chronos::OrderType::Limit});
    const auto trades = engine.processOrder({3, 0, 10, chronos::Side::Buy, chronos::OrderType::Market});
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0], (chronos::Trade{1, 3, 100, 2}));
    EXPECT_EQ(trades[1], (chronos::Trade{2, 3, 101, 3}));
    EXPECT_TRUE(engine.getSnapshot().asks.empty());
    EXPECT_FALSE(engine.getBook().hasOrder(3));
}

TEST(EngineCore, RejectsZeroQuantityAndDuplicateActiveIdentifier) {
    chronos::MatchingEngine engine(chronos::Variant1_MapListBook{}, 16);
    EXPECT_THROW((void)engine.processOrder({1, 100, 0, chronos::Side::Buy, chronos::OrderType::Limit}),
                 std::invalid_argument);
    (void)engine.processOrder({1, 100, 5, chronos::Side::Buy, chronos::OrderType::Limit});
    EXPECT_THROW((void)engine.processOrder({1, 101, 1, chronos::Side::Sell, chronos::OrderType::Limit}),
                 std::invalid_argument);
}

TEST(EngineCore, RejectsOutOfRangeAndUnalignedFlatPrices) {
    chronos::MatchingEngine engine(chronos::Variant3_FlatBitmapBook{9000, 11000, 5, 32}, 16);
    EXPECT_THROW((void)engine.processOrder({1, 9001, 1, chronos::Side::Buy, chronos::OrderType::Limit}),
                 std::out_of_range);
    EXPECT_THROW((void)engine.processOrder({2, 11005, 1, chronos::Side::Buy, chronos::OrderType::Limit}),
                 std::out_of_range);
}

TEST(EngineCore, ModifyCancelsAndReplacesTheRestingOrder) {
    chronos::MatchingEngine engine(chronos::Variant4_ChronosBook{9000, 11000, 1, 32, 64}, 32);
    (void)engine.processOrder({5, 10000, 5, chronos::Side::Buy, chronos::OrderType::Limit});
    EXPECT_TRUE(engine.modifyOrder({5, 10005, 7, chronos::Side::Buy, chronos::OrderType::Limit}));
    const auto snapshot = engine.getSnapshot();
    ASSERT_EQ(snapshot.bids.size(), 1u);
    EXPECT_EQ(snapshot.bids[0], (chronos::LevelSummary{10005, 7, 1}));
    EXPECT_EQ(engine.getBook().pool().ghostMarkedCount(), 1u);
    EXPECT_EQ(engine.getBook().pool().ghostReclaimedCount(), 1u);
}

TEST(EngineCore, RejectsOrderIdBeyondFlatLookupCapacity) {
    chronos::MatchingEngine engine(chronos::Variant4_ChronosBook{9000, 11000, 1, 32, 16}, 32);
    EXPECT_THROW((void)engine.processOrder({16, 10000, 1, chronos::Side::Buy, chronos::OrderType::Limit}),
                 std::out_of_range);
}
