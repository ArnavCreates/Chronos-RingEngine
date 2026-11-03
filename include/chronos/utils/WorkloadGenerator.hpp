#pragma once

#include "chronos/core/Types.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace chronos {

struct NamedWorkload {
    std::string name;
    std::vector<EventCommand> commands;
    std::size_t warmupCommands{0};
};

class WorkloadGenerator {
public:
    [[nodiscard]] static std::vector<EventCommand> generateDifferential(std::size_t count, std::uint64_t seed) {
        return generate(count, seed, 9000, 11000, 70, 20, false, true);
    }

    [[nodiscard]] static NamedWorkload generateBalanced(std::size_t count, std::uint64_t seed) {
        return makeNamed("Balanced_OrderFlow", count, seed, 9000, 11000, 75, 25, false);
    }

    [[nodiscard]] static NamedWorkload generateCancelHeavy(std::size_t count, std::uint64_t seed) {
        return makeNamed("Cancel_Heavy_Burst", count, seed, 9500, 10500, 55, 45, false);
    }

    [[nodiscard]] static NamedWorkload generateSingleTick(std::size_t count, std::uint64_t seed) {
        return makeNamed("Single_Tick_Contention", count, seed, 10000, 10000, 75, 25, true);
    }

    [[nodiscard]] static std::vector<EventCommand> generateProducerCommands(std::size_t count, std::size_t producer,
                                                                             std::uint64_t seed) {
        std::mt19937_64 random(seed);
        std::uniform_int_distribution<Price> price(9900, 10100);
        std::uniform_int_distribution<Quantity> quantity(1, 100);
        std::uniform_int_distribution<int> side(0, 1);
        std::vector<EventCommand> commands;
        commands.reserve(count);
        std::vector<OrderId> priorIds;
        priorIds.reserve(count / 2);
        std::uint64_t localCounter = 1;
        const OrderId base = static_cast<OrderId>(producer) * 1'000'000ULL;

        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t bucket = i % 10;
            if (bucket < 7 || priorIds.empty()) {
                const OrderId id = base + localCounter++;
                Order order{id, price(random), quantity(random), side(random) == 0 ? Side::Buy : Side::Sell,
                            OrderType::Limit};
                commands.push_back({RequestType::Add, order});
                priorIds.push_back(id);
            } else if (bucket < 9) {
                const OrderId id = priorIds[static_cast<std::size_t>(random() % priorIds.size())];
                commands.push_back({RequestType::Cancel, Order{id}});
            } else {
                const OrderId id = priorIds[static_cast<std::size_t>(random() % priorIds.size())];
                Order order{id, price(random), quantity(random), side(random) == 0 ? Side::Buy : Side::Sell,
                            OrderType::Limit};
                commands.push_back({RequestType::Modify, order});
            }
        }
        return commands;
    }

private:
    static NamedWorkload makeNamed(const std::string& name, std::size_t count, std::uint64_t seed, Price low,
                                   Price high, int addPercent, int cancelPercent, bool singleTick) {
        NamedWorkload workload;
        workload.name = name;
        workload.commands = generate(count, seed, low, high, addPercent, cancelPercent, singleTick, true);
        workload.warmupCommands = count / 11;  // 100,000 for a 1.1M action stream.
        return workload;
    }

    static std::vector<EventCommand> generate(std::size_t count, std::uint64_t seed, Price low, Price high,
                                              int addPercent, int cancelPercent, bool singleTick, bool cancelMix) {
        std::mt19937_64 random(seed);
        std::uniform_int_distribution<Price> price(low, high);
        std::uniform_int_distribution<Quantity> quantity(1, 100);
        std::uniform_int_distribution<int> side(0, 9);
        std::vector<EventCommand> commands;
        commands.reserve(count);
        std::vector<OrderId> priorIds;
        priorIds.reserve(count / 2);
        OrderId nextId = 1;

        for (std::size_t i = 0; i < count; ++i) {
            const int bucket = static_cast<int>(i % 100);
            const bool shouldAdd = !cancelMix || bucket < addPercent || priorIds.empty();
            const bool shouldCancel = cancelMix && !shouldAdd && bucket < addPercent + cancelPercent;
            if (shouldAdd) {
                Side orderSide = side(random) < (singleTick ? 9 : 5) ? Side::Buy : Side::Sell;
                Order order{nextId++, singleTick ? low : price(random), quantity(random), orderSide, OrderType::Limit};
                commands.push_back({RequestType::Add, order});
                priorIds.push_back(order.id);
            } else if (shouldCancel) {
                const OrderId id = priorIds[static_cast<std::size_t>(random() % priorIds.size())];
                commands.push_back({RequestType::Cancel, Order{id}});
            } else {
                // The differential workload's remaining 10% are Modify.
                const OrderId id = priorIds[static_cast<std::size_t>(random() % priorIds.size())];
                Order order{id, singleTick ? low : price(random), quantity(random),
                            side(random) < (singleTick ? 9 : 5) ? Side::Buy : Side::Sell, OrderType::Limit};
                commands.push_back({RequestType::Modify, order});
            }
        }
        return commands;
    }
};

}  // namespace chronos
