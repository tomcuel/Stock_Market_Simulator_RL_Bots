#include "matching_engine.hpp"

#include <cmath>
#include <chrono>

#include "logger.hpp"

namespace sim {

void MatchingEngine::register_symbol(const Symbol& symbol, Price initial_price) {
    bool added = false;
    {
        std::unique_lock lock(books_mutex_);
        added = books_.emplace(symbol, std::make_unique<OrderBook>(symbol)).second;
    }
    {
        std::unique_lock lock(prices_mutex_);
        last_prices_[symbol] = initial_price;
    }
    if (added) {
        LOG_INFO("registered symbol ", symbol, " reference_price=", initial_price);
    } else {
        LOG_DEBUG("symbol ", symbol, " reference_price updated to ", initial_price); // e.g. a snapshot's closing price
    }
}

void MatchingEngine::ensure_client(ClientId client, double initial_cash) {
    std::lock_guard<std::mutex> lock(portfolios_mutex_);
    Portfolio portfolio;
    portfolio.cash = initial_cash;
    portfolios_.try_emplace(client, std::move(portfolio));
}

void MatchingEngine::grant_initial_holdings(ClientId client, const Symbol& symbol, Quantity quantity) {
    std::lock_guard<std::mutex> lock(portfolios_mutex_);
    portfolios_[client].holdings[symbol] += quantity;
}

double MatchingEngine::fund_balanced_account(ClientId client, double equity, double cash_fraction) {
    std::vector<Symbol> listed = symbols();
    auto prices = all_last_prices();
    double stock_budget = equity * std::clamp(1.0 - cash_fraction, 0.0, 1.0);
    double per_symbol = listed.empty() ? 0.0 : stock_budget / static_cast<double>(listed.size());

    double stock_value = 0.0;
    std::unordered_map<Symbol, Quantity> shares;
    for (const auto& symbol : listed){
        Price price = prices[symbol];
        if (price <= 0.0) {
            continue;
        }
        Quantity quantity = std::llround(per_symbol / price);
        if (quantity <= 0) {
            continue;
        }
        shares[symbol] = quantity;
        stock_value += static_cast<double>(quantity) * price;
    }
    // cash is the rest of the equity, so rounding the share counts never changes the account's value
    ensure_client(client, std::max(0.0, equity - stock_value));
    for (const auto& [symbol, quantity] : shares){
        grant_initial_holdings(client, symbol, quantity);
    }
    return stock_value;
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

bool MatchingEngine::try_reserve(const Order& order) {
    std::lock_guard<std::mutex> lock(portfolios_mutex_);
    auto it = portfolios_.find(order.client);
    if (it == portfolios_.end()) {
        return false; // unknown client
    }
    Portfolio& portfolio = it->second;

    if (order.side == Side::SELL){
        if (portfolio.available_shares(order.symbol) < order.quantity) {
            return false;
        }
        portfolio.reserved_shares[order.symbol] += order.quantity;
        return true;
    }
    // BUY: order.price is the most it can pay per share (its limit, or its collar for a MARKET order)
    double worst_case = static_cast<double>(order.quantity) * order.price;
    if (portfolio.available_cash() < worst_case) {
        return false;
    }
    portfolio.reserved_cash += worst_case;
    return true;
}

void MatchingEngine::release_reservation(const Order& order, Quantity quantity) {
    if (quantity <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(portfolios_mutex_);
    auto it = portfolios_.find(order.client);
    if (it == portfolios_.end()) {
        return;
    }
    Portfolio& portfolio = it->second;
    if (order.side == Side::SELL){
        portfolio.reserved_shares[order.symbol] = std::max<Quantity>(0, portfolio.reserved(order.symbol) - quantity);
    }
    else {
        // clamped at 0: repeated floating point adds and subtracts can leave a tiny negative residue
        portfolio.reserved_cash = std::max(0.0, portfolio.reserved_cash - static_cast<double>(quantity) * order.price);
    }
}

void MatchingEngine::settle(const Trade& trade, Price buyer_reserve_price) {
    std::lock_guard<std::mutex> lock(portfolios_mutex_);
    double notional = static_cast<double>(trade.quantity) * trade.price;

    auto& buyer = portfolios_[trade.buyer];
    buyer.cash -= notional;
    buyer.holdings[trade.symbol] += trade.quantity;
    // the filled shares' whole reservation is released: the buyer pays the trade price, which is at most what was reserved, so any difference simply becomes available again
    buyer.reserved_cash = std::max(0.0, buyer.reserved_cash - static_cast<double>(trade.quantity) * buyer_reserve_price);

    auto& seller = portfolios_[trade.seller];
    seller.cash += notional;
    seller.holdings[trade.symbol] -= trade.quantity;
    seller.reserved_shares[trade.symbol] = std::max<Quantity>(0, seller.reserved(trade.symbol) - trade.quantity);
}

Order MatchingEngine::build_order(const OrderRequest& request, OrderId id, TimePoint now) const {
    Order order;
    order.id = id;
    order.client = request.client;
    order.side = request.side;
    order.kind = request.kind;
    order.symbol = request.symbol;
    // an explicit TIF wins, otherwise an order with its own expiry date is good till that date (GTC until it expires) and an order without one is a DAY order, the usual exchange default
    order.time_in_force = request.time_in_force.value_or(request.expires_in ? TimeInForce::GTC : TimeInForce::DAY);
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
    if (request.kind == OrderKind::MARKET){
        // collar (see kMarketCollar): the price a MARKET order may not cross, and what a MARKET buy reserves
        Price reference = last_price(request.symbol);
        order.price = (request.side == Side::BUY) ? reference * (1.0 + kMarketCollar) : reference * (1.0 - kMarketCollar);
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
        // a resting buy always trades at its own limit, which is what it reserved
        // an incoming buy reserved its own price (limit or collar), which can be above the resting ask it hit
        Price buyer_reserve_price = (order.side == Side::BUY) ? order.price : trade.price;
        settle(trade, buyer_reserve_price);
        result.filled_quantity += trade.quantity;
        {
            std::unique_lock lock(prices_mutex_);
            last_prices_[trade.symbol] = trade.price;
        }
        mark_dirty(trade.symbol); // a STOP/LIMIT_STOP order on this symbol may now be releasable
        bus_.publish(TradeEvent{trade});
    }
    // a MARKET order never rests (IOC): whatever didn't fill inside its collar is discarded, and so is its reservation
    // A LIMIT remainder rests in the book and keeps its reservation until it trades, is cancelled, or the run ends
    if (order.kind == OrderKind::MARKET){
        release_reservation(order, order.quantity - result.filled_quantity);
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
    if (market_closed_.load()) {
        // after the closing auction nothing trades until the next session
        SubmitResult closed;
        closed.order_id = next_order_id_.fetch_add(1);
        closed.reject_reason = "market closed";
        metrics_.order_rejected();
        bus_.publish(OrderRejectedEvent{closed.order_id, request.client, closed.reject_reason});
        return closed;
    }
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

    TimePoint now = Clock::now();
    Order order = build_order(request, result.order_id, now);

    // reserve before the order can touch the book or the waiting registry
    // It is checked against what  is still available, not the raw balance, so open orders can't over-commit the account
    if (!try_reserve(order)){
        result.reject_reason = (request.side == Side::BUY) ? "insufficient cash" : "insufficient holdings";
        metrics_.order_rejected();
        bus_.publish(OrderRejectedEvent{result.order_id, request.client, result.reject_reason});
        return result;
    }

    bool started = !order.not_before.has_value() || now >= *order.not_before;
    bool band_satisfied = order.release_band_contains(last_price(order.symbol));

    if (started && band_satisfied) {
        if (auction_mode_.load()) {
            // pre-close: the order rests without matching, the closing auction will execute it
            SubmitResult result;
            result.accepted = true;
            result.order_id = order.id;
            metrics_.order_accepted();
            bus_.publish(OrderAcceptedEvent{order.id, order.client, order.symbol});
            book_for(order.symbol).add(std::move(order));
            return result;
        }
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
    if (auction_mode_.load()) {
        return {}; // pre-close: a released STOP would match continuously, it waits for the next session (or expires)
    }
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
        release_reservation(order, order.quantity);
        metrics_.order_expired();
        bus_.publish(OrderExpiredEvent{order.id, order.client, order.symbol});
    }
    summary.expired = std::move(scan.expired);

    return summary;
}

bool MatchingEngine::cancel_order(const Symbol& symbol, Side side, Price price, OrderId order_id, std::optional<ClientId> owner) {
    if (!symbol_exists(symbol)) {
        return false;
    }
    auto removed = book_for(symbol).cancel(side, price, order_id, owner);
    if (!removed) {
        return false;
    }
    release_reservation(*removed, removed->quantity); // what is left of it: filled shares were already settled
    return true;
}

bool MatchingEngine::cancel_order(const Symbol& symbol, OrderId order_id, std::optional<ClientId> owner) {
    if (!symbol_exists(symbol)) {
        return false;
    }
    auto removed = book_for(symbol).cancel(order_id, owner);
    if (!removed) {
        return false;
    }
    release_reservation(*removed, removed->quantity);
    return true;
}

bool MatchingEngine::cancel_waiting_order(OrderId order_id, std::optional<ClientId> owner) {
    auto removed = registry_.cancel(order_id, owner);
    if (!removed) {
        return false;
    }
    release_reservation(*removed, removed->quantity);
    return true;
}

std::vector<Order> MatchingEngine::resting_orders(const Symbol& symbol) const {
    if (!symbol_exists(symbol)) {
        return {};
    }
    return book_for(symbol).resting_orders();
}

// end of the trading day
std::vector<MatchingEngine::ClosingPrice> MatchingEngine::run_closing_auction() {
    std::vector<ClosingPrice> closes;
    for (const auto& symbol : symbols()) {
        auto auction = book_for(symbol).uncross(last_price(symbol));
        ClosingPrice close;
        close.symbol = symbol;
        close.crossed = auction.volume > 0;
        close.volume = auction.volume;
        for (const auto& fill : auction.fills) {
            settle(fill.trade, fill.buyer_reserve_price);
            metrics_.volume_traded(static_cast<std::uint64_t>(fill.trade.quantity));
            bus_.publish(TradeEvent{fill.trade});
        }
        metrics_.trades_executed(auction.fills.size());
        if (close.crossed) {
            std::unique_lock lock(prices_mutex_);
            last_prices_[symbol] = auction.price;
        }
        close.price = last_price(symbol);
        closes.push_back(close);
    }
    auction_mode_.store(false);
    return closes;
}

std::size_t MatchingEngine::expire_orders(bool day_only) {
    TimePoint now = Clock::now();
    auto expires = [day_only, now](const Order& order) {
        return !day_only || order.time_in_force == TimeInForce::DAY || order.kind == OrderKind::MARKET || (order.expires_at && *order.expires_at <= now);
    };
    std::size_t expired = 0;
    for (const auto& symbol : symbols()) {
        for (const auto& order : book_for(symbol).remove_if(expires)) {
            release_reservation(order, order.quantity); // what is left of it
            bus_.publish(OrderExpiredEvent{order.id, order.client, order.symbol});
            ++expired;
        }
    }
    for (const auto& order : registry_.all_waiting()) {
        if (!expires(order)) {
            continue;
        }
        if (auto removed = registry_.cancel(order.id)) {
            release_reservation(*removed, removed->quantity);
            bus_.publish(OrderExpiredEvent{removed->id, removed->client, removed->symbol});
            ++expired;
        }
    }
    return expired;
}

std::size_t MatchingEngine::expire_day_orders() {
    return expire_orders(true);
}

std::size_t MatchingEngine::expire_all_orders() {
    return expire_orders(false);
}

std::vector<MatchingEngine::OpenOrder> MatchingEngine::open_orders() const {
    std::vector<OpenOrder> result;
    for (const auto& symbol : symbols()) {
        for (const auto& order : resting_orders(symbol)) {
            result.push_back(OpenOrder{order, false});
        }
    }
    for (const auto& order : registry_.all_waiting()) {
        result.push_back(OpenOrder{order, true});
    }
    return result;
}

bool MatchingEngine::restore_order(const OpenOrder& open) {
    if (!symbol_exists(open.order.symbol) || !try_reserve(open.order)) {
        return false;
    }
    OrderId next = next_order_id_.load();
    while (next <= open.order.id && !next_order_id_.compare_exchange_weak(next, open.order.id + 1)) {
    }
    if (open.waiting) {
        registry_.add(open.order);
    } 
    else {
        book_for(open.order.symbol).add(open.order);
    }
    return true;
}

} // namespace sim
