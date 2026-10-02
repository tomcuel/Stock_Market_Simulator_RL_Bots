#include "matching_engine.hpp"

#include <chrono>

#include "logger.hpp"

namespace sim {

void MatchingEngine::register_symbol(const Symbol& symbol, Price initial_price) {
    {
        std::unique_lock lock(books_mutex_);
        books_.emplace(symbol, std::make_unique<OrderBook>(symbol));
    }
    {
        std::unique_lock lock(prices_mutex_);
        last_prices_[symbol] = initial_price;
    }
    LOG_INFO("registered symbol ", symbol, " reference_price=", initial_price);
}

void MatchingEngine::ensure_client(ClientId client, double initial_cash) {
    std::lock_guard<std::mutex> lock(portfolios_mutex_);
    portfolios_.try_emplace(client, Portfolio{initial_cash, {}});
}

void MatchingEngine::grant_initial_holdings(ClientId client, const Symbol& symbol, Quantity quantity) {
    std::lock_guard<std::mutex> lock(portfolios_mutex_);
    portfolios_[client].holdings[symbol] += quantity;
}

OrderBook& MatchingEngine::book_for(const Symbol& symbol) {
    std::shared_lock lock(books_mutex_);
    return *books_.at(symbol);
}

const OrderBook& MatchingEngine::book_for(const Symbol& symbol) const {
    std::shared_lock lock(books_mutex_);
    return *books_.at(symbol);
}

bool MatchingEngine::symbol_exists(const Symbol& symbol) const {
    std::shared_lock lock(books_mutex_);
    return books_.count(symbol) > 0;
}

Price MatchingEngine::last_price(const Symbol& symbol) const {
    std::shared_lock lock(prices_mutex_);
    auto it = last_prices_.find(symbol);
    return it == last_prices_.end() ? 0.0 : it->second;
}

std::unordered_map<Symbol, Price> MatchingEngine::all_last_prices() const {
    std::shared_lock lock(prices_mutex_);
    return last_prices_;
}

BookSnapshot MatchingEngine::snapshot(const Symbol& symbol, std::size_t depth) const {
    // book_for() throws std::out_of_range for an unknown symbol 
    // Every other public method either validates the symbol first (submit_order) or get an empty/false result
    if (!symbol_exists(symbol)) {
        return BookSnapshot{};
    }
    return book_for(symbol).snapshot(depth);
}

Portfolio MatchingEngine::portfolio_snapshot(ClientId client) const {
    std::lock_guard<std::mutex> lock(portfolios_mutex_);
    auto it = portfolios_.find(client);
    return it == portfolios_.end() ? Portfolio{} : it->second;
}

std::unordered_map<ClientId, Portfolio> MatchingEngine::all_portfolios() const {
    std::lock_guard<std::mutex> lock(portfolios_mutex_);
    return portfolios_;
}

std::vector<Symbol> MatchingEngine::symbols() const {
    std::shared_lock lock(books_mutex_);
    std::vector<Symbol> result;
    result.reserve(books_.size());
    for (const auto& [symbol, book] : books_) {
        result.push_back(symbol);
    }
    return result;
}

bool MatchingEngine::passes_risk_check(ClientId client, Side side, OrderKind kind, const Symbol& symbol, Quantity quantity, Price price) const {
    std::lock_guard<std::mutex> lock(portfolios_mutex_);
    auto it = portfolios_.find(client);
    if (it == portfolios_.end()) {
        return false; // unknown client
    }
    const Portfolio& portfolio = it->second;

    if (side == Side::SELL) {
        return portfolio.holding(symbol) >= quantity;
    }

    // BUY: for an order that already carries a real limit price (LIMIT/STOP/LIMIT_STOP), we know the exact worst-case cost
    // For a MARKET order we don't know the fill price yet, so we estimate using the last traded price plus a safety buffer 
    Price reference = (kind == OrderKind::MARKET) ? last_price(symbol) * kMarketOrderCashSafetyFactor : price;
    return portfolio.cash >= static_cast<double>(quantity) * reference;
}

void MatchingEngine::settle(const Trade& trade) {
    std::lock_guard<std::mutex> lock(portfolios_mutex_);
    double notional = static_cast<double>(trade.quantity) * trade.price;

    auto& buyer = portfolios_[trade.buyer];
    buyer.cash -= notional;
    buyer.holdings[trade.symbol] += trade.quantity;

    auto& seller = portfolios_[trade.seller];
    seller.cash += notional;
    seller.holdings[trade.symbol] -= trade.quantity;
}

Order MatchingEngine::build_order(const OrderRequest& request, OrderId id, TimePoint now) const {
    Order order;
    order.id = id;
    order.client = request.client;
    order.side = request.side;
    order.kind = request.kind;
    order.symbol = request.symbol;
    order.quantity = request.quantity;
    order.price = request.price;
    order.submitted_at = now;

    if (request.not_before_in.has_value()) {
        order.not_before = now + *request.not_before_in;
    }
    if (request.expires_in.has_value()) {
        order.expires_at = now + *request.expires_in;
    }

    switch (request.kind) {
        case OrderKind::MARKET:
        case OrderKind::LIMIT:
            // released immediately regardless of reference price, exactly like before this order  model existed, unless the caller explicitly narrowed the band (rare, but supported for parity: a LIMIT order can itself carry a release trigger)
            order.release_lower = request.trigger_lower.value_or(-std::numeric_limits<Price>::infinity());
            order.release_upper = request.trigger_upper.value_or(std::numeric_limits<Price>::infinity());
            break;
        case OrderKind::STOP: {
            // classic, side-aware stop order (the standard market convention):
            // - SELL stop (stop-loss) sits BELOW the market, released once the price falls to the trigger
            // - BUY stop (breakout / short cover) sits ABOVE the market, released once the price rises to it
            // This used to release every STOP on "price <= trigger" regardless of side, so a BUY stop placed above the market (exactly how the bots use it) was already "triggered" on submission and went straight into the book as an above-market buy
            // This created a systematic buy bias that pushed every symbol up and left sell-side books empty (before but this fixes it)
            Price trigger = request.trigger_upper.value_or(request.price);
            if (request.side == Side::BUY) {
                order.release_lower = trigger;
                order.release_upper = std::numeric_limits<Price>::infinity();
            } 
            else { // SELL
                order.release_lower = -std::numeric_limits<Price>::infinity();
                order.release_upper = trigger;
            }
            break;
        }
        case OrderKind::LIMIT_STOP:
            // explicit band on both sides
            order.release_lower = request.trigger_lower.value_or(-std::numeric_limits<Price>::infinity());
            order.release_upper = request.trigger_upper.value_or(std::numeric_limits<Price>::infinity());
            break;
    }
    return order;
}

void MatchingEngine::mark_dirty(const Symbol& symbol) {
    std::lock_guard<std::mutex> lock(dirty_mutex_);
    dirty_symbols_.insert(symbol);
}

std::vector<Symbol> MatchingEngine::drain_dirty_symbols() {
    std::lock_guard<std::mutex> lock(dirty_mutex_);
    std::vector<Symbol> drained(dirty_symbols_.begin(), dirty_symbols_.end());
    dirty_symbols_.clear();
    return drained;
}

SubmitResult MatchingEngine::match_and_settle(Order order) {
    auto start = std::chrono::steady_clock::now();

    std::vector<Trade> trades = book_for(order.symbol).match(order);

    SubmitResult result;
    result.accepted = true;
    result.order_id = order.id;
    metrics_.order_accepted();
    bus_.publish(OrderAcceptedEvent{order.id, order.client, order.symbol});

    for (const auto& trade : trades) {
        settle(trade);
        result.filled_quantity += trade.quantity;
        {
            std::unique_lock lock(prices_mutex_);
            last_prices_[trade.symbol] = trade.price;
        }
        mark_dirty(trade.symbol); // a STOP/LIMIT_STOP order on this symbol may now be releasable
        bus_.publish(TradeEvent{trade});
    }
    result.trades = std::move(trades);
    metrics_.trades_executed(result.trades.size());
    for (const auto& t : result.trades) {
        metrics_.volume_traded(static_cast<std::uint64_t>(t.quantity));
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start);
    metrics_.record_submit_latency(elapsed);

    return result;
}

SubmitResult MatchingEngine::submit_order(const OrderRequest& request) {
    metrics_.order_submitted();

    SubmitResult result;
    result.order_id = next_order_id_.fetch_add(1, std::memory_order_relaxed);

    if (request.quantity <= 0) {
        result.reject_reason = "quantity must be positive";
    } 
    else if (request.kind != OrderKind::MARKET && request.price <= 0.0) {
        result.reject_reason = "LIMIT/STOP/LIMIT_STOP order requires a positive price";
    } 
    else if (!symbol_exists(request.symbol)) {
        result.reject_reason = "unknown symbol";
    }

    if (!result.reject_reason.empty()) {
        metrics_.order_rejected();
        bus_.publish(OrderRejectedEvent{result.order_id, request.client, result.reject_reason});
        return result;
    }

    if (!passes_risk_check(request.client, request.side, request.kind, request.symbol, request.quantity, request.price)) {
        result.reject_reason = (request.side == Side::BUY) ? "insufficient cash" : "insufficient holdings";
        metrics_.order_rejected();
        bus_.publish(OrderRejectedEvent{result.order_id, request.client, result.reject_reason});
        return result;
    }

    TimePoint now = Clock::now();
    Order order = build_order(request, result.order_id, now);

    bool started = !order.not_before.has_value() || now >= *order.not_before;
    bool band_satisfied = order.release_band_contains(last_price(order.symbol));

    if (started && band_satisfied) {
        return match_and_settle(std::move(order));
    }

    // not ready yet: hold it in the waiting registry until try_release_waiting_orders() picks it up
    result.accepted = true;
    result.queued = true;
    metrics_.order_queued();
    registry_.add(std::move(order));
    bus_.publish(OrderQueuedEvent{result.order_id, request.client, request.symbol});
    return result;
}

MatchingEngine::ReleaseSummary MatchingEngine::try_release_waiting_orders(bool full_scan) {
    TimePoint now = Clock::now();
    auto get_reference_price = [this](const Symbol& symbol) { return last_price(symbol); };

    OrderRegistry::ScanResult scan;
    if (full_scan) {
        scan = registry_.scan_and_release(now, get_reference_price);
    } 
    else {
        for (const auto& symbol : drain_dirty_symbols()) {
            auto partial = registry_.scan_and_release_symbol(symbol, now, get_reference_price);
            scan.released.insert(scan.released.end(), std::make_move_iterator(partial.released.begin()), std::make_move_iterator(partial.released.end()));
            scan.expired.insert(scan.expired.end(), std::make_move_iterator(partial.expired.begin()), std::make_move_iterator(partial.expired.end()));
        }
    }

    ReleaseSummary summary;
    summary.released.reserve(scan.released.size());
    for (auto& order : scan.released) {
        summary.released.push_back(match_and_settle(std::move(order)));
    }

    for (const auto& order : scan.expired) {
        metrics_.order_expired();
        bus_.publish(OrderExpiredEvent{order.id, order.client, order.symbol});
    }
    summary.expired = std::move(scan.expired);

    return summary;
}

bool MatchingEngine::cancel_order(const Symbol& symbol, Side side, Price price, OrderId order_id) {
    if (!symbol_exists(symbol)) return false;
    return book_for(symbol).cancel(side, price, order_id);
}

bool MatchingEngine::cancel_order(const Symbol& symbol, OrderId order_id) {
    if (!symbol_exists(symbol)) return false;
    return book_for(symbol).cancel(order_id);
}

bool MatchingEngine::cancel_waiting_order(OrderId order_id) {
    return registry_.cancel(order_id);
}

std::vector<Order> MatchingEngine::resting_orders(const Symbol& symbol) const {
    if (!symbol_exists(symbol)) return {};
    return book_for(symbol).resting_orders();
}

} // namespace sim
