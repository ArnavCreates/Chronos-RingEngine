#include "chronos/engine/MatchingEngine.hpp"
#include "chronos/engine/Variants.hpp"
#include "chronos/replay/EventStreamParser.hpp"
#include "chronos/utils/WorkloadGenerator.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <utility>
#include <vector>

namespace {

template <typename Book>
struct ReplayOutput {
    std::vector<chronos::Trade> ledger;
    chronos::BookSnapshot snapshot;
};

template <typename Book>
ReplayOutput<Book> runCommands(Book book, std::size_t tradeCapacity,
                               const std::vector<chronos::EventCommand>& commands) {
    chronos::MatchingEngine<Book> engine(std::move(book), tradeCapacity);
    std::vector<chronos::Trade> ledger;
    for (const auto& command : commands) {
        engine.executeCommand(command);
        const auto trades = engine.lastTrades();
        ledger.insert(ledger.end(), trades.begin(), trades.end());
    }
    return {std::move(ledger), engine.getSnapshot()};
}

}  // namespace

TEST(DifferentialReplay, AllFourVariantsMatchTradeLedgerAndRestingSnapshot) {
    constexpr std::size_t actionCount = 250'000;
    const auto commands = chronos::WorkloadGenerator::generateDifferential(actionCount, 0xC0FFEE);

    const auto v1 = runCommands(chronos::Variant1_MapListBook{}, actionCount,
                               commands);
    const auto v2 = runCommands(chronos::Variant2_MapSlabBook{actionCount}, actionCount,
                               commands);
    const auto v3 = runCommands(chronos::Variant3_FlatBitmapBook{9000, 11000, 1, actionCount}, actionCount,
                               commands);
    const auto v4 = runCommands(chronos::Variant4_ChronosBook{9000, 11000, 1, actionCount, actionCount + 1},
                               actionCount, commands);

    EXPECT_EQ(v2.ledger, v1.ledger);
    EXPECT_EQ(v3.ledger, v1.ledger);
    EXPECT_EQ(v4.ledger, v1.ledger);
    EXPECT_EQ(v2.snapshot, v1.snapshot);
    EXPECT_EQ(v3.snapshot, v1.snapshot);
    EXPECT_EQ(v4.snapshot, v1.snapshot);
    EXPECT_EQ(v1.snapshot.stateHash, chronos::computeSnapshotHash(v1.snapshot.bids, v1.snapshot.asks));
}

TEST(DifferentialReplay, ReplayingSameSeedProducesSameSnapshotHash) {
    constexpr std::size_t actionCount = 30'000;
    const auto commands = chronos::WorkloadGenerator::generateDifferential(actionCount, 42);
    const auto first = runCommands(chronos::Variant4_ChronosBook{9000, 11000, 1, actionCount, actionCount + 1},
                                   actionCount, commands);
    const auto second = runCommands(chronos::Variant4_ChronosBook{9000, 11000, 1, actionCount, actionCount + 1},
                                    actionCount, commands);
    EXPECT_EQ(first.ledger, second.ledger);
    EXPECT_EQ(first.snapshot, second.snapshot);
    EXPECT_EQ(first.snapshot.stateHash, second.snapshot.stateHash);
}

TEST(DifferentialReplay, CsvStreamParsesAndReplaysDeterministically) {
    std::istringstream input(
        "Action,OrderId,Price,Quantity,Side,OrderType\n"
        "A,1,10000,5,B,L\n"
        "A,2,10000,3,S,L\n"
        "C,1,0,0,B,L\n"
        "M,2,10001,2,S,L\n");
    const auto commands = chronos::EventStreamParser::parseStream(input);
    ASSERT_EQ(commands.size(), 4u);
    EXPECT_EQ(commands[0].type, chronos::RequestType::Add);
    EXPECT_EQ(commands[2].type, chronos::RequestType::Cancel);
    EXPECT_EQ(commands[3].type, chronos::RequestType::Modify);

    const auto first = runCommands(chronos::Variant4_ChronosBook{9000, 11000, 1, 16, 32}, 16, commands);
    const auto second = runCommands(chronos::Variant4_ChronosBook{9000, 11000, 1, 16, 32}, 16, commands);
    EXPECT_EQ(first.ledger, second.ledger);
    EXPECT_EQ(first.snapshot.stateHash, second.snapshot.stateHash);
}

TEST(DifferentialReplay, CsvRejectsMalformedRowsWithLineNumber) {
    std::istringstream input("Action,OrderId,Price,Quantity,Side\nA,abc,10000,1,B\n");
    try {
        (void)chronos::EventStreamParser::parseStream(input);
        FAIL() << "expected malformed numeric field to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("line 2"), std::string::npos);
    }
}
