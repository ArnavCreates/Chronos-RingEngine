#include "chronos/engine/MatchingEngine.hpp"
#include "chronos/engine/Variants.hpp"
#include "chronos/utils/PerfCounters.hpp"
#include "chronos/utils/WorkloadGenerator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Metrics {
    std::string variant;
    std::string workload;
    double throughputOpsSec{0};
    double amortizedNsPerOp{0};
    double cyclesPerOp{0};
    double meanNs{0};
    std::uint64_t p50Ns{0};
    std::uint64_t p90Ns{0};
    std::uint64_t p99Ns{0};
    std::uint64_t p999Ns{0};
    std::uint64_t matchedTrades{0};
    std::uint64_t tradeLedgerHash{0};
    std::uint64_t finalStateHash{0};
    std::uint64_t hotPathAllocations{0};
    chronos::HardwareCounterValues hardware{};
};

std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0;
    }
    std::size_t index = static_cast<std::size_t>(fraction * static_cast<double>(sorted.size()));
    if (index >= sorted.size()) {
        index = sorted.size() - 1;
    }
    return sorted[index];
}

template <typename Book>
Metrics benchmarkVariant(const std::string& variantName, Book book, const chronos::NamedWorkload& workload,
                         double tscFrequencyHz, std::uint64_t probeOverheadCycles) {
    chronos::MatchingEngine<Book> engine(book, workload.commands.size());
    for (std::size_t i = 0; i < workload.warmupCommands; ++i) {
        engine.executeCommand(workload.commands[i]);
    }

    const std::size_t measuredCount = workload.commands.size() - workload.warmupCommands;
    chronos::LinuxPerfCounters counters;
    const bool perfStarted = counters.start();
    const auto batchStart = std::chrono::steady_clock::now();
    const std::uint64_t cycleStart = chronos::readTscSerialized();
    std::uint64_t matchedTrades = 0;
    chronos::HeapAllocationTracker allocationTracker;
    for (std::size_t i = workload.warmupCommands; i < workload.commands.size(); ++i) {
        engine.executeCommand(workload.commands[i]);
        matchedTrades += engine.lastTrades().size();
    }
    const std::uint64_t hotPathAllocations = allocationTracker.stop();
    const std::uint64_t cycleEnd = chronos::readTscSerialized();
    const auto batchEnd = std::chrono::steady_clock::now();
    const chronos::HardwareCounterValues hardware = perfStarted ? counters.stop() : chronos::HardwareCounterValues{};
    const double batchSeconds = std::chrono::duration<double>(batchEnd - batchStart).count();
    const double cyclesPerOp = measuredCount == 0 ? 0.0 : static_cast<double>(cycleEnd - cycleStart) / measuredCount;

    const chronos::BookSnapshot snapshot = engine.getSnapshot();

    chronos::MatchingEngine<Book> ledgerEngine(book, workload.commands.size());
    for (std::size_t i = 0; i < workload.warmupCommands; ++i) {
        ledgerEngine.executeCommand(workload.commands[i]);
    }
    chronos::TradeLedgerHasher ledgerHasher;
    for (std::size_t i = workload.warmupCommands; i < workload.commands.size(); ++i) {
        ledgerEngine.executeCommand(workload.commands[i]);
        ledgerHasher.update(ledgerEngine.lastTrades());
    }
    const std::uint64_t tradeLedgerHash = ledgerHasher.value();

    chronos::MatchingEngine<Book> latencyEngine(book, workload.commands.size());
    for (std::size_t i = 0; i < workload.warmupCommands; ++i) {
        latencyEngine.executeCommand(workload.commands[i]);
    }
    std::vector<std::uint64_t> latenciesNs;
    latenciesNs.reserve(measuredCount);
    for (std::size_t i = workload.warmupCommands; i < workload.commands.size(); ++i) {
        const std::uint64_t opStart = chronos::readTscFast();
        latencyEngine.executeCommand(workload.commands[i]);
        const std::uint64_t opEnd = chronos::readTscFast();
        const std::uint64_t rawCycles = opEnd > opStart ? opEnd - opStart : 1ULL;
        const std::uint64_t adjustedCycles = rawCycles > probeOverheadCycles ? rawCycles - probeOverheadCycles : 1ULL;
        const double nanoseconds = tscFrequencyHz > 0.0
                                      ? static_cast<double>(adjustedCycles) * 1.0e9 / tscFrequencyHz
                                      : 0.0;
        latenciesNs.push_back(nanoseconds > 0.0 ? std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(nanoseconds)))
                                                : 0);
    }
    std::sort(latenciesNs.begin(), latenciesNs.end());
    const std::uint64_t sumNs = std::accumulate(latenciesNs.begin(), latenciesNs.end(), std::uint64_t{0});
    const double meanNs = latenciesNs.empty() ? 0.0 : static_cast<double>(sumNs) / latenciesNs.size();

    return Metrics{variantName,
                   workload.name,
                   batchSeconds > 0.0 ? static_cast<double>(measuredCount) / batchSeconds : 0.0,
                   measuredCount > 0 ? batchSeconds * 1.0e9 / measuredCount : 0.0,
                   cyclesPerOp,
                   meanNs,
                   percentile(latenciesNs, 0.50),
                   percentile(latenciesNs, 0.90),
                   percentile(latenciesNs, 0.99),
                   percentile(latenciesNs, 0.999),
                   matchedTrades,
                   tradeLedgerHash,
                   snapshot.stateHash,
                   hotPathAllocations,
                   hardware};
}

void writeMetrics(std::ofstream& output, const Metrics& metric, bool last) {
    output << std::fixed << std::setprecision(3)
           << "    {\"variant\": \"" << metric.variant << "\", \"workload\": \"" << metric.workload
           << "\", \"throughput_ops_sec\": " << metric.throughputOpsSec
           << ", \"amortized_ns_per_op\": " << metric.amortizedNsPerOp << ", \"cycles_per_op\": "
           << metric.cyclesPerOp << ", \"mean_ns\": " << metric.meanNs << ", \"p50_ns\": " << metric.p50Ns
           << ", \"p90_ns\": " << metric.p90Ns << ", \"p99_ns\": " << metric.p99Ns
           << ", \"p99_9_ns\": " << metric.p999Ns << ", \"matched_trades\": " << metric.matchedTrades
           << ", \"trade_ledger_hash\": " << metric.tradeLedgerHash << ", \"final_state_hash\": "
           << metric.finalStateHash << ", \"hot_path_heap_allocations\": "
           << metric.hotPathAllocations << ", \"hardware_counters_available\": "
           << (metric.hardware.available ? "true" : "false") << ", \"hardware_cycles\": "
           << metric.hardware.cycles << ", \"hardware_instructions\": " << metric.hardware.instructions
           << ", \"cache_references\": " << metric.hardware.cacheReferences << ", \"cache_misses\": "
           << metric.hardware.cacheMisses << "}" << (last ? "\n" : ",\n");
}

void printMetric(const Metrics& metric) {
    std::cout << std::left << std::setw(22) << metric.workload << std::setw(7) << metric.variant << std::right
              << std::setw(13) << std::fixed << std::setprecision(0) << metric.throughputOpsSec << std::setw(11)
              << metric.amortizedNsPerOp << std::setw(9) << metric.cyclesPerOp << std::setw(9) << metric.p50Ns
              << std::setw(9) << metric.p99Ns << std::setw(11) << metric.p999Ns << std::setw(14)
              << metric.hotPathAllocations << std::setw(19) << metric.matchedTrades << "\n";
}

}  // namespace

int main() {
    constexpr std::size_t totalActions = 1'100'000;
    constexpr chronos::Price minPrice = 9000;
    constexpr chronos::Price maxPrice = 11000;
    constexpr chronos::Price tickSize = 1;
    constexpr std::size_t slabCapacity = totalActions;
    constexpr std::size_t orderIdCapacity = totalActions + 1;
    const bool pinnedToCore0 = chronos::pinCurrentThreadToCore0();
    const double tscFrequencyHz = chronos::calibrateTscFrequency();
    const std::uint64_t probeOverheadCycles = chronos::calibrateTscProbeOverheadCycles();
    const double probeOverheadNs = tscFrequencyHz > 0.0
                                       ? static_cast<double>(probeOverheadCycles) * 1.0e9 / tscFrequencyHz
                                       : 0.0;
    std::cout << "Benchmark thread pinned to logical CPU 0: " << (pinnedToCore0 ? "yes" : "no") << "\n"
              << "TSC probe median overhead: " << probeOverheadCycles << " cycles (" << std::fixed
              << std::setprecision(2) << probeOverheadNs << " ns)\n";

    std::vector<chronos::NamedWorkload> workloads;
    workloads.push_back(chronos::WorkloadGenerator::generateBalanced(totalActions, 0x1234));
    workloads.push_back(chronos::WorkloadGenerator::generateCancelHeavy(totalActions, 0x5678));
    workloads.push_back(chronos::WorkloadGenerator::generateSingleTick(totalActions, 0x9ABC));

    std::vector<Metrics> metrics;
    metrics.reserve(workloads.size() * 4);
    for (const auto& workload : workloads) {
        std::cout << "\n" << workload.name << " (" << workload.commands.size() - workload.warmupCommands
                  << " measured commands)\n";
        std::cout << std::left << std::setw(22) << "Workload" << std::setw(7) << "Variant" << std::right
                  << std::setw(13) << "Ops/sec" << std::setw(11) << "ns/op" << std::setw(9) << "cy/op"
                  << std::setw(9) << "P50 ns" << std::setw(9) << "P99 ns" << std::setw(11) << "P99.9 ns"
                  << std::setw(14) << "new calls" << std::setw(19) << "trades" << "\n";

        metrics.push_back(benchmarkVariant("V1_MapList", chronos::Variant1_MapListBook{}, workload, tscFrequencyHz,
                                           probeOverheadCycles));
        metrics.push_back(benchmarkVariant("V2_MapSlab", chronos::Variant2_MapSlabBook{slabCapacity}, workload,
                                           tscFrequencyHz, probeOverheadCycles));
        metrics.push_back(benchmarkVariant("V3_FlatBitmap",
                                           chronos::Variant3_FlatBitmapBook{minPrice, maxPrice, tickSize, slabCapacity},
                                           workload, tscFrequencyHz, probeOverheadCycles));
        metrics.push_back(benchmarkVariant("V4_ChronosFull",
                                           chronos::Variant4_ChronosBook{minPrice, maxPrice, tickSize, slabCapacity,
                                                                         orderIdCapacity},
                                           workload, tscFrequencyHz, probeOverheadCycles));
        for (std::size_t i = metrics.size() - 4; i < metrics.size(); ++i) {
            printMetric(metrics[i]);
        }
    }

    bool parityPassed = true;
    bool v4TargetsPassed = true;
    for (std::size_t base = 0; base < metrics.size(); base += 4) {
        const Metrics& baseline = metrics[base];
        for (std::size_t variant = 1; variant < 4; ++variant) {
            const Metrics& actual = metrics[base + variant];
            parityPassed = parityPassed && actual.matchedTrades == baseline.matchedTrades &&
                           actual.tradeLedgerHash == baseline.tradeLedgerHash &&
                           actual.finalStateHash == baseline.finalStateHash;
        }
        const Metrics& v4 = metrics[base + 3];
        v4TargetsPassed = v4TargetsPassed && v4.amortizedNsPerOp < 60.0 && v4.p50Ns < 60 &&
                          v4.hotPathAllocations == 0;
    }

    std::filesystem::create_directories("assets");
    std::ofstream json("assets/variant_latency_comparison.json");
    if (!json) {
        std::cerr << "Unable to write assets/variant_latency_comparison.json\n";
        return 2;
    }
    json << std::fixed << std::setprecision(3)
         << "{\n  \"tsc_frequency_hz\": " << tscFrequencyHz << ",\n  \"tsc_probe_overhead_cycles\": "
         << probeOverheadCycles << ",\n  \"tsc_probe_calibration_samples\": 100000,\n  \"tsc_probe_overhead_ns\": " << probeOverheadNs
         << ",\n  \"benchmark_thread_pinned_to_core0\": " << (pinnedToCore0 ? "true" : "false")
         << ",\n  \"hardware_counters_note\": \"Linux perf_event_open counters are unavailable on non-Linux hosts or when permissions deny access.\",\n  \"results\": [\n";
    for (std::size_t i = 0; i < metrics.size(); ++i) {
        writeMetrics(json, metrics[i], i + 1 == metrics.size());
    }
    json << "  ],\n  \"parity_passed\": " << (parityPassed ? "true" : "false")
         << ",\n  \"v4_targets_passed\": " << (v4TargetsPassed ? "true" : "false") << "\n}\n";
    json.close();

    std::cout << "\nTrade-ledger and snapshot parity : " << (parityPassed ? "PASS" : "FAIL") << "\n"
              << "V4 latency/allocation goals: " << (v4TargetsPassed ? "PASS" : "BELOW TARGET") << "\n"
              << "TSC frequency             : " << std::fixed << std::setprecision(0) << tscFrequencyHz << " Hz\n"
              << "TSC probe overhead        : " << probeOverheadCycles << " cycles (" << std::setprecision(2)
              << probeOverheadNs << " ns)\n"
              << "Linux hardware counters    : recorded when perf_event_open is available\n"
              << "JSON                      : assets/variant_latency_comparison.json\n";
    return parityPassed && v4TargetsPassed ? 0 : 1;
}
