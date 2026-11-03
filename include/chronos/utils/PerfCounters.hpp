#pragma once

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sched.h>
#endif

namespace chronos {

[[nodiscard]] inline std::uint64_t readTscSerialized() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _mm_lfence();
    const std::uint64_t value = __rdtsc();
    _mm_lfence();
    return value;
#else
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// Keep per-operation probes light: compiler fences preserve source ordering
// without serializing the hardware pipeline around every short operation.
[[nodiscard]] inline std::uint64_t readTscFast() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const std::uint64_t value = __rdtsc();
    std::atomic_signal_fence(std::memory_order_seq_cst);
    return value;
#else
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

[[nodiscard]] inline std::uint64_t readTsc() noexcept { return readTscSerialized(); }

[[nodiscard]] inline bool pinCurrentThreadToCore0() noexcept {
#if defined(_WIN32)
    const BOOL prioritySet = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    const BOOL threadPrioritySet = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    const DWORD_PTR affinitySet = SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1ULL));
    return prioritySet != 0 && threadPrioritySet != 0 && affinitySet != 0;
#elif defined(__linux__)
    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);
    CPU_SET(0, &cpuSet);
    return sched_setaffinity(0, sizeof(cpu_set_t), &cpuSet) == 0;
#else
    return false;
#endif
}

[[nodiscard]] inline std::uint64_t calibrateTscProbeOverheadCycles() {
    constexpr std::size_t warmupPairs = 2000;
    constexpr std::size_t samplePairs = 100000;
    for (std::size_t i = 0; i < warmupPairs; ++i) {
        const std::uint64_t start = readTscFast();
        const std::uint64_t end = readTscFast();
        (void)(end - start);
    }
    std::vector<std::uint64_t> deltas;
    deltas.reserve(samplePairs);
    for (std::size_t i = 0; i < samplePairs; ++i) {
        const std::uint64_t start = readTscFast();
        const std::uint64_t end = readTscFast();
        deltas.push_back(end > start ? end - start : 1ULL);
    }
    std::sort(deltas.begin(), deltas.end());
    return deltas[deltas.size() / 2];
}

[[nodiscard]] inline double calibrateTscFrequency(std::chrono::milliseconds interval = std::chrono::milliseconds(200)) {
    const auto startTime = std::chrono::steady_clock::now();
    const std::uint64_t startCycles = readTscSerialized();
    std::this_thread::sleep_for(interval);
    const std::uint64_t endCycles = readTscSerialized();
    const auto endTime = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(endTime - startTime).count();
    return seconds > 0.0 ? static_cast<double>(endCycles - startCycles) / seconds : 0.0;
}

struct HardwareCounterValues {
    std::uint64_t cycles{0};
    std::uint64_t instructions{0};
    std::uint64_t cacheReferences{0};
    std::uint64_t cacheMisses{0};
    bool available{false};
};

class LinuxPerfCounters {
public:
    LinuxPerfCounters() noexcept;
    ~LinuxPerfCounters();
    LinuxPerfCounters(const LinuxPerfCounters&) = delete;
    LinuxPerfCounters& operator=(const LinuxPerfCounters&) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool start() noexcept;
    [[nodiscard]] HardwareCounterValues stop() noexcept;

private:
    std::array<int, 4> fileDescriptors_{{-1, -1, -1, -1}};
    bool running_{false};
};

namespace detail {
void noteHeapAllocation() noexcept;
std::uint64_t heapAllocationCount() noexcept;
void setHeapTracking(bool enabled) noexcept;
}

class HeapAllocationTracker {
public:
    HeapAllocationTracker() noexcept;
    ~HeapAllocationTracker();
    HeapAllocationTracker(const HeapAllocationTracker&) = delete;
    HeapAllocationTracker& operator=(const HeapAllocationTracker&) = delete;

    [[nodiscard]] std::uint64_t stop() noexcept;
    [[nodiscard]] std::uint64_t count() const noexcept;

private:
    bool previousTracking_{false};
    bool active_{true};
};

}  // namespace chronos
