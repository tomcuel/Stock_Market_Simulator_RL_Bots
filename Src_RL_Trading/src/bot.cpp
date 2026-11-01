#include "bot.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#include "logger.hpp"

namespace sim {

namespace {

OrderRequest make_request(Side side, OrderKind kind, const Symbol& symbol, Quantity quantity, Price price = 0.0) {
    OrderRequest request;
    request.side = side;
    request.kind = kind;
    request.symbol = symbol;
    request.quantity = quantity;
    request.price = price;
    return request;
}

Side opposite(Side side) {
    return side == Side::BUY ? Side::SELL : Side::BUY;
}

} // namespace


// ---------------------------------------------------------------- base class
TradingStrategy::TradingStrategy(MarketGateway& gateway, std::vector<Symbol> symbols, unsigned seed, std::unordered_map<Symbol, double> volatilities) : gateway_(gateway), symbols_(std::move(symbols)), rng_(seed), volatilities_(std::move(volatilities)) {}

void TradingStrategy::step() {
    ++tick_;
    retract_stale();
    if (symbols_.empty()) {
        return;
    }
    if (lot_value_ <= 0.0) {
        // the bot's equity at its first tick (cash plus holdings at the current prices) sets its lot size
        Portfolio portfolio = gateway_.portfolio();
        double equity = portfolio.cash;
        for (const auto& symbol : symbols_) {
            equity += static_cast<double>(portfolio.holding(symbol)) * gateway_.quote(symbol).last;
        }
        lot_value_ = std::max(1.0, equity * kLotFraction);
    }
    on_tick();
}

Quantity TradingStrategy::lots(Price price, double units) const {
    if (price <= 0.0) {
        return 1;
    }
    return std::max<Quantity>(1, std::llround(units * lot_value_ / price));
}

Symbol TradingStrategy::pick_symbol() {
    std::uniform_int_distribution<std::size_t> dist(0, symbols_.size() - 1);
    return symbols_[dist(rng_)];
}

double TradingStrategy::volatility_for(const Symbol& symbol) const {
    auto it = volatilities_.find(symbol);
    return it == volatilities_.end() ? 0.20 : it->second;
}

double TradingStrategy::offset_scale(const Symbol& symbol) const {
    return std::clamp(volatility_for(symbol) / 20.0, 0.002, 0.10);
}

void TradingStrategy::observe_all(std::size_t keep) {
    for (const auto& symbol : symbols_) {
        Quote quote = gateway_.quote(symbol);
        Price price = (quote.last > 0.0) ? quote.last : quote.mid();
        if (price <= 0.0) {
            continue;
        }
        auto& prices = history_[symbol];
        prices.push_back(price);
        while (prices.size() > keep) {
            prices.pop_front();
        }
    }
}

SubmitResult TradingStrategy::place(OrderRequest request, int max_age_ticks) {
    SubmitResult result = gateway_.submit(request);
    if (!result.accepted) {
        return result;
    }
    ++placed_;
    // a MARKET order is IOC: nothing of it can remain open. Anything else may still rest or wait
    bool may_be_open = result.queued || (request.kind != OrderKind::MARKET && result.filled_quantity < request.quantity);
    if (may_be_open) {
        open_orders_.push_back(OpenOrder{request.symbol, result.order_id, tick_ + static_cast<std::uint64_t>(std::max(1, max_age_ticks))});
    }
    return result;
}

bool TradingStrategy::retract(OrderId order_id) {
    auto it = std::find_if(open_orders_.begin(), open_orders_.end(), [&](const OpenOrder& o) {return o.id == order_id;});
    if (it == open_orders_.end()) {
        return false;
    }
    bool cancelled = gateway_.cancel(it->symbol, it->id); // false: it already filled or expired
    if (cancelled) {
        ++retracted_;
    }
    open_orders_.erase(it);
    return cancelled;
}

void TradingStrategy::retract_all(const Symbol& symbol) {
    std::vector<OrderId> ids;
    for (const auto& order : open_orders_) {
        if (symbol.empty() || order.symbol == symbol) ids.push_back(order.id);
    }
    for (OrderId id : ids) retract(id);
}

void TradingStrategy::retract_stale() {
    std::vector<OrderId> stale;
    for (const auto& order : open_orders_) {
        if (order.expires_at_tick <= tick_) {
            stale.push_back(order.id);
        }
    }
    for (OrderId id : stale) {
        retract(id);
    }
}


// ---------------------------------------------------------------- noise
void NoiseTraderBot::on_tick() {
    Symbol symbol = pick_symbol();
    Price reference = gateway_.quote(symbol).last;
    if (reference <= 0.0) return;

    double scale = offset_scale(symbol);
    // 45% market, 35% limit, 10% stop, 10% limit_stop: every order kind stays exercised
    std::discrete_distribution<int> kind_dist({45, 35, 10, 10});
    std::uniform_real_distribution<double> units_dist(1.0, 10.0);
    std::uniform_real_distribution<double> offset_dist(-scale, scale);
    std::bernoulli_distribution buy_or_sell(buy_probability(symbol, reference));
    Side side = buy_or_sell(rng_) ? Side::BUY : Side::SELL;
    OrderRequest request = make_request(side, OrderKind::MARKET, symbol, lots(reference, units_dist(rng_)));
    int max_age = 20;
    switch (kind_dist(rng_)) {
        case 0:
            break;
        case 1:
            request.kind = OrderKind::LIMIT;
            request.price = std::max(0.01, reference * (1.0 + offset_dist(rng_)));
            break;
        case 2: {
            // a stop a little beyond the price: stop-loss below for a SELL, breakout above for a BUY
            request.kind = OrderKind::STOP;
            double direction = (side == Side::SELL) ? -1.0 : 1.0;
            std::uniform_real_distribution<double> stop_dist(scale, scale * 3.0);
            request.price = std::max(0.01, reference * (1.0 + direction * stop_dist(rng_)));
            max_age = 60;
            break;
        }
        default: {
            request.kind = OrderKind::LIMIT_STOP;
            std::uniform_real_distribution<double> band_dist(scale, scale * 2.5);
            request.price = reference;
            request.trigger_lower = std::max(0.01, reference * (1.0 - band_dist(rng_)));
            request.trigger_upper = reference * (1.0 + band_dist(rng_));
            max_age = 60;
            break;
        }
    }
    std::bernoulli_distribution has_expiry(0.25); // like a real "good-till-date" order
    if (has_expiry(rng_)) {
        std::uniform_int_distribution<int> expiry_dist(5, 60);
        request.expires_in = std::chrono::seconds(expiry_dist(rng_));
    }
    place(request, max_age);
}


double NoiseTraderBot::buy_probability(const Symbol& symbol, Price price) {
    Portfolio portfolio = gateway_.portfolio();
    if (start_cash_ < 0.0) {
        start_cash_ = std::max(1.0, portfolio.cash);
    }
    auto start = start_value_.try_emplace(symbol, std::max(1.0, static_cast<double>(portfolio.holding(symbol)) * price)).first->second;
    double cash_ratio = std::max(1e-3, portfolio.available_cash() / start_cash_);
    double stock_ratio = std::max(1e-3, static_cast<double>(portfolio.available_shares(symbol)) * price / start);
    // equal ratios: a fair coin, a symbol sold down to a third of its start against untouched cash: about 0.7
    return std::clamp(0.5 + 0.4 * std::tanh(0.5 * std::log(cash_ratio / stock_ratio)), 0.1, 0.9);
}


// ---------------------------------------------------------------- value investor
void ValueInvestorBot::on_tick() {
    if (!values_) {
        return;
    }
    Symbol symbol = pick_symbol();
    double sigma = values_->daily_vol(symbol);
    Price fundamental = values_->value(symbol);
    Quote quote = gateway_.quote(symbol);
    Price price = quote.mid();
    if (fundamental <= 0.0 || price <= 0.0 || sigma <= 0.0) {
        return;
    }
    auto error = error_.find(symbol);
    if (error == error_.end()) {
        std::normal_distribution<double> belief(0.0, 0.3 * sigma); // investors disagree, by about a third of a day's move
        error = error_.emplace(symbol, belief(rng_)).first;
    }
    double gap = std::log(fundamental / price) + error->second; // > 0: the stock is cheap for this investor
    if (std::abs(gap) < 0.25 * sigma) {
        return; // too close to call
    }
    Side side = gap > 0.0 ? Side::BUY : Side::SELL;
    double units = std::clamp(2.0 + 8.0 * std::abs(gap) / sigma, 1.0, 10.0);
    if (std::abs(gap) > 1.5 * sigma) {
        place(make_request(side, OrderKind::MARKET, symbol, lots(price, units)), 1); // far off: no time to wait
        return;
    }
    // patient: a limit a third of the way toward the value, retracted after a few ticks if nobody takes it
    Price limit = std::max(0.01, price * std::exp(gap / 3.0));
    place(make_request(side, OrderKind::LIMIT, symbol, lots(price, units), limit), 8);
}


// ---------------------------------------------------------------- momentum
void MomentumBot::on_tick() {
    constexpr std::size_t kLookback = 10;
    constexpr std::uint64_t kCooldownTicks = 10;
    observe_all(kLookback);
    Symbol symbol = pick_symbol();
    const auto& prices = history(symbol);
    if (prices.size() < kLookback) {
        return;
    }
    // only a significant move counts (at least the symbol's per-order scale), and never twice in a row on the same symbol: 
    // without that threshold and cooldown, momentum bots read their own price impact as a trend and chase it, which multiplied the price swings
    double change = prices.back() / prices.front() - 1.0;
    if (std::abs(change) < offset_scale(symbol)) {
        return;
    }
    auto last = last_trade_tick_.find(symbol);
    if (last != last_trade_tick_.end() && tick_ - last->second < kCooldownTicks) {
        return;
    }
    last_trade_tick_[symbol] = tick_;

    std::uniform_real_distribution<double> units_dist(1.0, 5.0);
    Side side = (change > 0.0) ? Side::BUY : Side::SELL;
    SubmitResult entry = place(make_request(side, OrderKind::MARKET, symbol, lots(prices.back(), units_dist(rng_))), 1);
    if (!entry.accepted || entry.filled_quantity == 0) {
        return;
    }

    // protective stop on the other side, a little through the price: guards against a reversal
    double scale = offset_scale(symbol);
    std::uniform_real_distribution<double> stop_dist(scale, std::max(scale * 2.0, scale + 0.005));
    double direction = (side == Side::BUY) ? -1.0 : 1.0;
    OrderRequest stop = make_request(opposite(side), OrderKind::STOP, symbol, entry.filled_quantity, std::max(0.01, prices.back() * (1.0 + direction * stop_dist(rng_))));
    stop.expires_in = std::chrono::seconds(120);
    place(stop, 80);
}


// ---------------------------------------------------------------- market maker
void MarketMakerBot::on_tick() {
    // requote: retract last tick's pair (wherever it was posted) before posting a new one
    for (OrderId id : live_quotes_) {
        retract(id);
    }
    live_quotes_.clear();

    Symbol symbol = pick_symbol();
    Quote quote = gateway_.quote(symbol);
    Price mid = quote.mid();
    if (mid <= 0.0) {
        return;
    }

    // inventory skew: every lot above the starting position leans both quotes 0.01% lower (so the maker sells it off), every lot below leans them higher
    // The lean is capped at 80% of the half spread, so the bid always stays below the mid and the ask above it: a market maker leans, it never crosses
    // (An uncapped 2% lean made makers post asks below the best bid and dump inventory every tick, which doubled the price swings in the measurements)
    const double kHalfSpread = half_spread_;
    Portfolio portfolio = gateway_.portfolio();
    Quantity target = target_position_.try_emplace(symbol, portfolio.holding(symbol)).first->second;
    double inventory = static_cast<double>(portfolio.holding(symbol) - target) * mid / lot_value(); // in lots
    double lean = std::clamp(-0.0001 * inventory, -0.8 * kHalfSpread, 0.8 * kHalfSpread);
    double center = mid * (1.0 + lean);
    double half_spread = mid * kHalfSpread;

    std::uniform_real_distribution<double> units_dist(5.0, 20.0);
    SubmitResult bid = place(make_request(Side::BUY, OrderKind::LIMIT, symbol, lots(mid, units_dist(rng_)), std::max(0.01, center - half_spread)), 5);
    SubmitResult ask = place(make_request(Side::SELL, OrderKind::LIMIT, symbol, lots(mid, units_dist(rng_)), center + half_spread), 5);
    if (bid.accepted) {
        live_quotes_.push_back(bid.order_id);
    }
    if (ask.accepted) {
        live_quotes_.push_back(ask.order_id);
    }
}


// ---------------------------------------------------------------- mean reversion
void MeanReversionBot::on_tick() {
    constexpr std::size_t kWindow = 20;
    observe_all(kWindow);
    Symbol symbol = pick_symbol();
    const auto& prices = history(symbol);
    if (prices.size() < kWindow) {
        return;
    }

    double mean = 0.0;
    for (double p : prices) {
        mean += p;
    }
    mean /= static_cast<double>(prices.size());
    double variance = 0.0;
    for (double p : prices) {
        variance += (p - mean) * (p - mean);
    }
    double sd = std::sqrt(variance / static_cast<double>(prices.size()));
    if (sd <= 0.0) {
        return;
    }

    double z = (prices.back() - mean) / sd;
    if (std::abs(z) < 1.5) {
        return; // not stretched enough to bet on a return to the mean
    } 

    // one working order per symbol: the new signal replaces the previous one
    retract_all(symbol);
    Quantity qty = lots(prices.back(), std::clamp(std::abs(z) * 3.0, 1.0, 15.0));
    double step = offset_scale(symbol) * 0.25;
    Side side = (z < 0.0) ? Side::BUY : Side::SELL;
    Price price = (side == Side::BUY) ? prices.back() * (1.0 - step) : prices.back() * (1.0 + step);
    place(make_request(side, OrderKind::LIMIT, symbol, qty, std::max(0.01, price)), 12); // passive, short-lived
}


// ---------------------------------------------------------------- trend following
void TrendFollowerBot::on_tick() {
    constexpr double kFast = 2.0 / (5.0 + 1.0);  // 5-sample EMA
    constexpr double kSlow = 2.0 / (20.0 + 1.0); // 20-sample EMA
    constexpr double kBand = 0.0005;             // ignore crossovers smaller than 0.05%
    observe_all(1);

    for (const auto& symbol : symbols_) {
        const auto& prices = history(symbol);
        if (prices.empty()) {
            continue;
        }
        Price price = prices.back();
        TrendState& state = trends_[symbol];
        if (state.samples == 0) {
            state.fast = state.slow = price;
        }
        else {
            state.fast += kFast * (price - state.fast);
            state.slow += kSlow * (price - state.slow);
        }
        ++state.samples;
        if (state.samples < 20) {
            continue; // warm-up
        } 

        int signal = (state.fast > state.slow * (1.0 + kBand)) ? 1 : (state.fast < state.slow * (1.0 - kBand)) ? -1 : 0;
        double trail = std::max(0.005, 2.0 * offset_scale(symbol));

        if (signal != 0 && signal != state.direction) {
            // crossover: drop the old protective stop, enter the new direction
            if (state.stop_id != 0) {
                retract(state.stop_id);
            }
            state.stop_id = 0;
            std::uniform_real_distribution<double> units_dist(2.0, 6.0);
            Side side = (signal > 0) ? Side::BUY : Side::SELL;
            SubmitResult entry = place(make_request(side, OrderKind::MARKET, symbol, lots(price, units_dist(rng_))), 1);
            if (!entry.accepted || entry.filled_quantity == 0) {
                continue;
            }
            state.direction = signal;
            state.quantity = entry.filled_quantity;
            state.stop_price = (signal > 0) ? price * (1.0 - trail) : price * (1.0 + trail);
            SubmitResult stop = place(make_request(opposite(side), OrderKind::STOP, symbol, entry.filled_quantity, state.stop_price), 400);
            if (stop.accepted) {
                state.stop_id = stop.order_id;
            }
        }
        else if (state.direction != 0 && state.stop_id != 0) {
            // trailing stop: when the price moved our way by more than 0.1%, move the stop with it
            Price wanted = (state.direction > 0) ? price * (1.0 - trail) : price * (1.0 + trail);
            bool better = (state.direction > 0) ? wanted > state.stop_price * 1.001 : wanted < state.stop_price * 0.999;
            if (better && retract(state.stop_id)) {
                // the stop protects the position the crossover opened, never more than what can still be sold
                Quantity qty = state.quantity;
                if (state.direction > 0) {
                    qty = std::min(qty, gateway_.portfolio().available_shares(symbol));
                }
                qty = std::max<Quantity>(1, qty);
                Side stop_side = (state.direction > 0) ? Side::SELL : Side::BUY;
                SubmitResult stop = place(make_request(stop_side, OrderKind::STOP, symbol, qty, wanted), 400);
                state.stop_id = stop.accepted ? stop.order_id : 0;
                state.stop_price = wanted;
            }
        }
    }
}


// ---------------------------------------------------------------- TWAP execution
void TwapExecutionBot::start_parent() {
    Symbol symbol = pick_symbol();
    Quote quote = gateway_.quote(symbol);
    Price arrival = quote.mid();
    if (arrival <= 0.0) {
        return;
    }
    Portfolio portfolio = gateway_.portfolio();

    std::bernoulli_distribution buy_or_sell(0.5);
    std::uniform_real_distribution<double> units_dist(15.0, 60.0); // a parent order of 3% to 12% of the equity
    std::uniform_int_distribution<int> horizon_dist(15, 30);
    Side side = buy_or_sell(rng_) ? Side::BUY : Side::SELL;
    Quantity total = lots(arrival, units_dist(rng_));
    if (side == Side::SELL) {
        total = std::min(total, portfolio.available_shares(symbol));
    }
    else {
        total = std::min<Quantity>(total, static_cast<Quantity>(portfolio.available_cash() / (arrival * 1.05)));
    }
    // too small to be worth slicing (under 5 lots, e.g. no cash or shares left), try again next tick
    if (total < 1 || static_cast<double>(total) * arrival < 5.0 * lot_value()) {
        return;
    }

    parent_.active = true;
    parent_.symbol = symbol;
    parent_.side = side;
    parent_.total = total;
    parent_.horizon_ticks = horizon_dist(rng_);
    parent_.started_tick = tick_;
    parent_.arrival_price = arrival;
    parent_.start_holding = portfolio.holding(symbol);
    parent_.start_cash = portfolio.cash;
    parent_.child_id = 0;
}

void TwapExecutionBot::finish_parent(Quantity filled, double cash_now) {
    if (filled > 0) {
        double average = std::abs(cash_now - parent_.start_cash) / static_cast<double>(filled);
        // positive = cost: paid more than the arrival price on a buy, received less on a sell
        double slippage_bps = (parent_.side == Side::BUY ? average / parent_.arrival_price - 1.0 : 1.0 - average / parent_.arrival_price) * 1e4;
        LOG_INFO("TWAP client=", client(), " ", to_string(parent_.side), " ", filled, "/", parent_.total, " ", parent_.symbol, " avg=", average, " arrival=", parent_.arrival_price, " slippage_bps=", slippage_bps, " ticks=", tick_ - parent_.started_tick);
    }
    ++parents_completed_;
    parent_.active = false;
}

void TwapExecutionBot::on_tick() {
    if (!parent_.active) {
        start_parent();
        return;
    }
    Portfolio portfolio = gateway_.portfolio();
    Quantity filled = std::abs(portfolio.holding(parent_.symbol) - parent_.start_holding);
    auto elapsed = static_cast<int>(tick_ - parent_.started_tick);

    if (parent_.child_id != 0) {
        retract(parent_.child_id); // the unfilled rest of the last slice is re-planned
    }
    parent_.child_id = 0;
    if (filled >= parent_.total || elapsed > parent_.horizon_ticks + 10) {
        finish_parent(filled, gateway_.portfolio().cash);
        return;
    }

    Quantity remaining = parent_.total - filled;
    int slices_left = std::max(1, parent_.horizon_ticks - elapsed);
    Quantity child = std::max<Quantity>(1, (remaining + slices_left - 1) / slices_left);
    double schedule = std::min(1.0, static_cast<double>(elapsed) / parent_.horizon_ticks);
    bool behind = static_cast<double>(filled) < schedule * static_cast<double>(parent_.total) - static_cast<double>(child);
    bool cross = behind || slices_left <= 1;

    // passive at the touch while on schedule (join the bid to buy, the ask to sell), cross the spread (take the ask to buy, the bid to sell) once behind or on the last slice
    Quote quote = gateway_.quote(parent_.symbol);
    Price last = (quote.last > 0.0) ? quote.last : parent_.arrival_price;
    Price price;
    if (parent_.side == Side::BUY) {
        price = cross ? (quote.best_ask > 0.0 ? quote.best_ask : last * 1.002) : (quote.best_bid > 0.0 ? quote.best_bid : last * 0.999);
    }
    else {
        price = cross ? (quote.best_bid > 0.0 ? quote.best_bid : last * 0.998) : (quote.best_ask > 0.0 ? quote.best_ask : last * 1.001);
    }
    SubmitResult result = place(make_request(parent_.side, OrderKind::LIMIT, parent_.symbol, child, std::max(0.01, price)), 3);
    if (result.accepted) {
        parent_.child_id = result.order_id;
    }
}


// ---------------------------------------------------------------- factory and runner
const std::vector<std::string>& strategy_names() {
    static const std::vector<std::string> names = {"noise", "momentum", "marketmaker", "meanreversion", "trend", "twap"};
    return names;
}

std::unique_ptr<TradingStrategy> make_strategy(const std::string& name, MarketGateway& gateway, std::vector<Symbol> symbols, unsigned seed, std::unordered_map<Symbol, double> volatilities) {
    if (name == "noise") {
        return std::make_unique<NoiseTraderBot>(gateway, std::move(symbols), seed, std::move(volatilities));
    }
    if (name == "momentum") {
        return std::make_unique<MomentumBot>(gateway, std::move(symbols), seed, std::move(volatilities));
    }
    if (name == "marketmaker") {
        return std::make_unique<MarketMakerBot>(gateway, std::move(symbols), seed, std::move(volatilities));
    }
    if (name == "meanreversion") {
        return std::make_unique<MeanReversionBot>(gateway, std::move(symbols), seed, std::move(volatilities));
    }
    if (name == "trend") {
        return std::make_unique<TrendFollowerBot>(gateway, std::move(symbols), seed, std::move(volatilities));
    }
    if (name == "twap") {
        return std::make_unique<TwapExecutionBot>(gateway, std::move(symbols), seed, std::move(volatilities));
    }
    return nullptr;
}

BotRunner::BotRunner(std::unique_ptr<TradingStrategy> strategy, std::chrono::milliseconds min_interval, std::chrono::milliseconds max_interval, unsigned seed) : strategy_(std::move(strategy)), min_interval_(min_interval), max_interval_(max_interval), rng_(seed) {}

BotRunner::~BotRunner() {
    stop();
}

void BotRunner::start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread([this]{
        std::uniform_int_distribution<int> jitter(static_cast<int>(min_interval_.count()), static_cast<int>(max_interval_.count()));
        while (running_.load()) {
            if (!session_ || session_->accepting_orders()) {
                strategy_->step();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(jitter(rng_)));
        }
    });
}

void BotRunner::stop() {
    running_.store(false);
    if (thread_.joinable()) {
        thread_.join();
    }
}

} // namespace sim
