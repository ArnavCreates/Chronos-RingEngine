#include "chronos/concurrency/MpscRingBuffer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <thread>
#include <vector>

TEST(MpscRing, ValidatesPowerOfTwoCapacityAndNonblockingFullResult) {
    EXPECT_THROW((chronos::MpscRingBuffer<std::uint64_t>{1}), std::invalid_argument);
    EXPECT_THROW((chronos::MpscRingBuffer<std::uint64_t>{3}), std::invalid_argument);

    chronos::MpscRingBuffer<std::uint64_t> queue(4);
    EXPECT_TRUE(queue.try_push(1));
    EXPECT_TRUE(queue.try_push(2));
    EXPECT_TRUE(queue.try_push(3));
    EXPECT_TRUE(queue.try_push(4));
    EXPECT_FALSE(queue.try_push(5));
    for (std::uint64_t expected = 1; expected <= 4; ++expected) {
        EXPECT_EQ(queue.pop(), expected);
    }
    EXPECT_TRUE(queue.try_push(5));
    EXPECT_EQ(queue.pop(), 5u);
}

TEST(MpscRing, SingleProducerWrapsAroundRepeatedly) {
    chronos::MpscRingBuffer<std::uint64_t> queue(8);
    for (std::uint64_t item = 0; item < 100'000; ++item) {
        queue.push(item);
        EXPECT_EQ(queue.pop(), item);
    }
}

TEST(MpscRing, TenProducersPublishEveryIdExactlyOnceAcrossWraps) {
    constexpr std::size_t producerCount = 10;
    constexpr std::size_t itemsPerProducer = 50'000;
    constexpr std::size_t totalItems = producerCount * itemsPerProducer;
    chronos::MpscRingBuffer<std::uint64_t> queue(1024);
    std::vector<std::uint32_t> seen(totalItems, 0);
    std::latch start(1);
    std::thread consumer([&] {
        start.wait();
        for (std::size_t received = 0; received < totalItems; ++received) {
            const std::uint64_t id = queue.pop();
            if (id < totalItems) {
                ++seen[static_cast<std::size_t>(id)];
            }
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(producerCount);
    for (std::size_t producer = 0; producer < producerCount; ++producer) {
        producers.emplace_back([&, producer] {
            start.wait();
            const std::size_t base = producer * itemsPerProducer;
            for (std::size_t i = 0; i < itemsPerProducer; ++i) {
                queue.push(static_cast<std::uint64_t>(base + i));
            }
        });
    }
    start.count_down();
    for (auto& producer : producers) {
        producer.join();
    }
    consumer.join();

    for (const std::uint32_t count : seen) {
        EXPECT_EQ(count, 1u);
    }
}
