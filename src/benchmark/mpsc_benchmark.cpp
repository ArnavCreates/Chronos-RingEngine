#include "chronos/engine/ChronosPipeline.hpp"
#include "chronos/utils/WorkloadGenerator.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <latch>
#include <thread>
#include <vector>

int main() {
    constexpr std::size_t producerCount = 10;
    constexpr std::size_t commandsPerProducer = 1'000'000;
    constexpr std::size_t totalCommands = producerCount * commandsPerProducer;

    std::cout << "Preparing " << totalCommands << " commands across " << producerCount << " producers...\n";
    std::vector<std::vector<chronos::EventCommand>> producerCommands(producerCount);
    for (std::size_t producer = 0; producer < producerCount; ++producer) {
        producerCommands[producer] = chronos::WorkloadGenerator::generateProducerCommands(
            commandsPerProducer, producer, 0xC001D00DULL + producer);
    }

    chronos::ChronosPipeline pipeline(1U << 20, 9000, 11000, 1, 4'000'000, 12'000'000, 4'000'000);
    std::latch startSignal(1);
    std::vector<std::thread> producers;
    producers.reserve(producerCount);
    for (std::size_t producer = 0; producer < producerCount; ++producer) {
        producers.emplace_back([&, producer] {
            startSignal.wait();
            for (const auto& command : producerCommands[producer]) {
                pipeline.publish(command);
            }
        });
    }

    const auto start = std::chrono::steady_clock::now();
    startSignal.count_down();
    for (auto& producer : producers) {
        producer.join();
    }
    pipeline.shutdown();
    const auto end = std::chrono::steady_clock::now();

    const double elapsedSeconds = std::chrono::duration<double>(end - start).count();
    const double throughput = static_cast<double>(totalCommands) / elapsedSeconds;
    const double averageNs = elapsedSeconds * 1.0e9 / static_cast<double>(totalCommands);
    constexpr double targetOpsPerSecond = 3'100'000.0;
    const bool targetMet = throughput >= targetOpsPerSecond;

    std::filesystem::create_directories("assets");
    std::ofstream json("assets/mpsc_throughput_results.json");
    if (!json) {
        std::cerr << "Unable to write assets/mpsc_throughput_results.json\n";
        return 2;
    }
    json << std::fixed << std::setprecision(3)
         << "{\n"
         << "  \"producers\": " << producerCount << ",\n"
         << "  \"commands_per_producer\": " << commandsPerProducer << ",\n"
         << "  \"total_commands\": " << totalCommands << ",\n"
         << "  \"queue_capacity\": " << (1U << 20) << ",\n"
         << "  \"elapsed_seconds\": " << elapsedSeconds << ",\n"
         << "  \"throughput_ops_sec\": " << throughput << ",\n"
         << "  \"average_ns_per_op\": " << averageNs << ",\n"
         << "  \"matched_trades\": " << pipeline.matchedTrades() << ",\n"
         << "  \"processed_commands\": " << pipeline.processedCommands() << ",\n"
         << "  \"ghost_cancellations_reclaimed\": " << pipeline.ghostReclaimedCount() << ",\n"
         << "  \"target_ops_sec\": " << targetOpsPerSecond << ",\n"
         << "  \"target_met\": " << (targetMet ? "true" : "false") << "\n"
         << "}\n";
    json.close();

    std::cout << std::fixed << std::setprecision(3)
              << "Chronos MPSC benchmark\n"
              << "-----------------------\n"
              << "Producers                 : " << producerCount << "\n"
              << "Processed commands        : " << pipeline.processedCommands() << " / " << totalCommands << "\n"
              << "Elapsed                   : " << elapsedSeconds << " s\n"
              << "Throughput                : " << throughput << " ops/s\n"
              << "Average latency           : " << averageNs << " ns/op\n"
              << "Matched trades            : " << pipeline.matchedTrades() << "\n"
              << "Ghost cancellations freed : " << pipeline.ghostReclaimedCount() << "\n"
              << "3.1M ops/s target         : " << (targetMet ? "PASS" : "BELOW TARGET") << "\n"
              << "JSON                      : assets/mpsc_throughput_results.json\n";
    if (!pipeline.consumerDrainComplete()) {
        std::cerr << "Consumer did not report a completed drain.\n";
        return 3;
    }
    return targetMet ? 0 : 1;
}
