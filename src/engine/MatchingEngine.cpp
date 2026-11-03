#include "chronos/utils/PerfCounters.hpp"

#include <cstdlib>
#include <new>

#if defined(__linux__)
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace chronos {
namespace {
thread_local bool trackAllocations = false;
thread_local std::uint64_t trackedAllocations = 0;
}

namespace detail {
void noteHeapAllocation() noexcept {
    if (trackAllocations) {
        ++trackedAllocations;
    }
}

std::uint64_t heapAllocationCount() noexcept { return trackedAllocations; }
void setHeapTracking(bool enabled) noexcept { trackAllocations = enabled; }
}  // namespace detail

HeapAllocationTracker::HeapAllocationTracker() noexcept : previousTracking_(trackAllocations) {
    trackedAllocations = 0;
    trackAllocations = true;
}

HeapAllocationTracker::~HeapAllocationTracker() { (void)stop(); }

std::uint64_t HeapAllocationTracker::stop() noexcept {
    if (active_) {
        trackAllocations = previousTracking_;
        active_ = false;
    }
    return trackedAllocations;
}

std::uint64_t HeapAllocationTracker::count() const noexcept { return trackedAllocations; }

}  // namespace chronos

void* operator new(std::size_t size) {
    chronos::detail::noteHeapAllocation();
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    chronos::detail::noteHeapAllocation();
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

void* operator new(std::size_t size, std::align_val_t alignment) {
    chronos::detail::noteHeapAllocation();
    const std::size_t align = static_cast<std::size_t>(alignment);
#if defined(_WIN32)
    if (void* memory = _aligned_malloc(size == 0 ? 1 : size, align)) {
        return memory;
    }
#else
    const std::size_t requested = size == 0 ? align : size;
    const std::size_t rounded = ((requested + align - 1) / align) * align;
    if (void* memory = std::aligned_alloc(align, rounded)) {
        return memory;
    }
#endif
    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete(void* memory, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

void operator delete[](void* memory, std::align_val_t alignment) noexcept { ::operator delete(memory, alignment); }
void operator delete(void* memory, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}
void operator delete[](void* memory, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}

namespace chronos {

LinuxPerfCounters::LinuxPerfCounters() noexcept {
#if defined(__linux__)
    constexpr std::array<std::uint64_t, 4> events{{PERF_COUNT_HW_CPU_CYCLES, PERF_COUNT_HW_INSTRUCTIONS,
                                                  PERF_COUNT_HW_CACHE_REFERENCES, PERF_COUNT_HW_CACHE_MISSES}};
    for (std::size_t i = 0; i < events.size(); ++i) {
        perf_event_attr attributes{};
        attributes.type = PERF_TYPE_HARDWARE;
        attributes.size = sizeof(attributes);
        attributes.config = events[i];
        attributes.disabled = 1;
        attributes.exclude_kernel = 1;
        attributes.exclude_hv = 1;
        fileDescriptors_[i] = static_cast<int>(syscall(__NR_perf_event_open, &attributes, 0, -1, -1, 0));
        if (fileDescriptors_[i] < 0) {
            for (int& descriptor : fileDescriptors_) {
                if (descriptor >= 0) {
                    close(descriptor);
                    descriptor = -1;
                }
            }
            break;
        }
    }
#endif
}

LinuxPerfCounters::~LinuxPerfCounters() {
#if defined(__linux__)
    for (const int descriptor : fileDescriptors_) {
        if (descriptor >= 0) {
            close(descriptor);
        }
    }
#endif
}

bool LinuxPerfCounters::available() const noexcept {
    for (const int descriptor : fileDescriptors_) {
        if (descriptor < 0) {
            return false;
        }
    }
    return true;
}

bool LinuxPerfCounters::start() noexcept {
#if defined(__linux__)
    if (!available()) {
        return false;
    }
    for (const int descriptor : fileDescriptors_) {
        if (ioctl(descriptor, PERF_EVENT_IOC_RESET, 0) != 0 || ioctl(descriptor, PERF_EVENT_IOC_ENABLE, 0) != 0) {
            running_ = false;
            return false;
        }
    }
    running_ = true;
    return true;
#else
    return false;
#endif
}

HardwareCounterValues LinuxPerfCounters::stop() noexcept {
    HardwareCounterValues values{};
#if defined(__linux__)
    if (!running_) {
        return values;
    }
    std::array<std::uint64_t, 4> raw{};
    bool success = true;
    for (std::size_t i = 0; i < fileDescriptors_.size(); ++i) {
        const int descriptor = fileDescriptors_[i];
        if (ioctl(descriptor, PERF_EVENT_IOC_DISABLE, 0) != 0 || read(descriptor, &raw[i], sizeof(raw[i])) != sizeof(raw[i])) {
            success = false;
        }
    }
    running_ = false;
    if (success) {
        values = {raw[0], raw[1], raw[2], raw[3], true};
    }
#endif
    return values;
}

}  // namespace chronos
