#pragma once

#include "chronos/concurrency/MpscRingBuffer.hpp"
#include "chronos/engine/MatchingEngine.hpp"
#include "chronos/engine/Variants.hpp"

#include <atomic>
#include <cstddef>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>

namespace chronos {

class ChronosPipeline {
public:
    ChronosPipeline(std::size_t queueCapacity, Price minPrice, Price maxPrice, Price tickSize,
                    std::size_t slabCapacity, std::size_t maxOrderIdCapacity, std::size_t maxRestingOrders)
        : queue_(queueCapacity),
          engine_(Variant4_ChronosBook(minPrice, maxPrice, tickSize, slabCapacity, maxOrderIdCapacity),
                  maxRestingOrders),
          worker_([this] { consume(); }) {}

    ChronosPipeline(const ChronosPipeline&) = delete;
    ChronosPipeline& operator=(const ChronosPipeline&) = delete;

    ~ChronosPipeline() {
        try {
            shutdown();
        } catch (...) {
        }
    }

    void publish(const EventCommand& command) {
        if (stopRequested_.load(std::memory_order_acquire)) {
            throw std::logic_error("cannot publish after pipeline shutdown has started");
        }
        queue_.push(command);
    }

    // Call only after all producer threads have completed their pushes. The
    // stop command receives a later queue ticket, so the consumer drains all
    // preceding commands before exiting.
    void shutdown() {
        bool expected = false;
        if (stopRequested_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            queue_.push(EventCommand{RequestType::Stop, {}});
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        if (workerError_) {
            std::rethrow_exception(workerError_);
        }
    }

    [[nodiscard]] std::uint64_t processedCommands() const noexcept {
        return processedCommands_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t matchedTrades() const noexcept {
        return matchedTrades_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool consumerDrainComplete() const noexcept {
        return consumerDrainComplete_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t ghostReclaimedCount() const noexcept {
        return engine_.getBook().pool().ghostReclaimedCount();
    }
    [[nodiscard]] BookSnapshot getSnapshot() const { return engine_.getSnapshot(); }

private:
    void consume() noexcept {
        bool failed = false;
        for (;;) {
            const EventCommand command = queue_.pop();
            if (command.type == RequestType::Stop) {
                break;
            }
            if (!failed) {
                try {
                    engine_.executeCommand(command);
                    matchedTrades_.fetch_add(engine_.lastTrades().size(), std::memory_order_relaxed);
                    processedCommands_.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    workerError_ = std::current_exception();
                    failed = true;
                }
            }
        }
        consumerDrainComplete_.store(true, std::memory_order_release);
    }

    MpscRingBuffer<EventCommand> queue_;
    MatchingEngine<Variant4_ChronosBook> engine_;
    std::thread worker_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> consumerDrainComplete_{false};
    std::atomic<std::uint64_t> processedCommands_{0};
    std::atomic<std::uint64_t> matchedTrades_{0};
    std::exception_ptr workerError_;
};

}  // namespace chronos
