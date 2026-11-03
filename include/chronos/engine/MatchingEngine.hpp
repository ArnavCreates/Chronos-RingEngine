#pragma once

#include "chronos/core/Types.hpp"

#include <cassert>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace chronos {

template <typename BookT>
class MatchingEngine {
public:
    MatchingEngine(BookT book, std::size_t maxRestingOrders) : book_(std::move(book)) {
        tradeBuffer_.reserve(maxRestingOrders);
    }

    [[nodiscard]] std::span<const Trade> processOrderHot(const Order& order) {
        tradeBuffer_.clear();
        if (order.quantity == 0) {
            throw std::invalid_argument("order quantity must be positive");
        }
        book_.validateOrder(order);
        if (book_.hasOrder(order.id)) {
            throw std::invalid_argument("duplicate active OrderId");
        }

        Order incoming = order;
        const std::optional<Price> limitPrice = order.type == OrderType::Limit ? std::optional<Price>(order.price)
                                                                               : std::nullopt;
        book_.matchAgainst(incoming, limitPrice, tradeBuffer_);
        if (order.type == OrderType::Limit && incoming.quantity > 0) {
            bool added = false;
            if constexpr (requires(BookT& book, const Order& resting) { book.addOrderAfterPreflight(resting); }) {
                added = book_.addOrderAfterPreflight(incoming);
            } else {
                added = book_.addOrder(incoming);
            }
            if (!added) {
                throw std::logic_error("book rejected an order after the engine validated its identifier");
            }
        }
        return tradeBuffer_;
    }

    [[nodiscard]] std::vector<Trade> processOrder(const Order& order) {
        const auto trades = processOrderHot(order);
        return {trades.begin(), trades.end()};
    }

    [[nodiscard]] bool cancelOrder(OrderId id) {
        tradeBuffer_.clear();
        return book_.cancelOrder(id);
    }

    // Replacement semantics are cancel-then-submit. A missing old ID is
    // treated as a new submission, matching the source repository behavior.
    [[nodiscard]] bool modifyOrder(const Order& order) {
        tradeBuffer_.clear();
        (void)book_.cancelOrder(order.id);
        (void)processOrderHot(order);
        return true;
    }

    void executeCommand(const EventCommand& command) {
        switch (command.type) {
            case RequestType::Add:
                (void)processOrderHot(command.order);
                break;
            case RequestType::Cancel:
                (void)cancelOrder(command.order.id);
                break;
            case RequestType::Modify:
                (void)modifyOrder(command.order);
                break;
            case RequestType::Stop:
                tradeBuffer_.clear();
                break;
        }
    }

    [[nodiscard]] std::span<const Trade> lastTrades() const noexcept { return tradeBuffer_; }
    [[nodiscard]] BookSnapshot getSnapshot() const { return book_.extractSnapshot(); }
    [[nodiscard]] const BookT& getBook() const noexcept { return book_; }
    [[nodiscard]] BookT& getBook() noexcept { return book_; }

private:
    BookT book_;
    std::vector<Trade> tradeBuffer_;
};

}  // namespace chronos
