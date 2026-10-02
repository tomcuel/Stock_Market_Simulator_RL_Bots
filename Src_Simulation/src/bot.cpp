#include "bot.hpp"

#include <algorithm>

namespace sim {

void NoiseTraderBot::step() {
    Symbol symbol = pick_symbol();

    // engine_.last_price() is updated synchronously inside MatchingEngine::submit_order() itself,
    // so it's always at least as fresh as market_data_.last_price() (which only updates once the notification bus's async worker thread gets around to processing the trade event
    // an eventually-consistent lag that, in a tight loop like a bot's, can mean sampling the same stale price two ticks in a row even though the "real" price already moved)
    // Preferred here as the primary source for exactly that reason; market_data is kept as a defensive fallback only
    Price reference = engine_.last_price(symbol);
    if (reference <= 0.0) reference = market_data_.last_price(symbol);
    if (reference <= 0.0) return; // no price information yet for this symbol, skip this tick

    // heuristic per-tick offset scale from annualized volatility: a 20%/year vol becomes a +/-1% per-order price range (roughly volatility/20), 
    // so a historically calmer or wilder ticker visibly trades calmer or wilder here too, instead of every symbol using the same fixed range
    double offset_scale = std::clamp(volatility_for(symbol) / 20.0, 0.002, 0.10);

    // 45% market (immediate), 35% limit (rests in the book), 10% stop, 10% limit_stop
    // so the simulation continuously exercises every order kind the engine supports, the same way a real participant mix would (warning the launcher can configure those to different ratios if desired)
    std::discrete_distribution<int> kind_dist({45, 35, 10, 10});
    std::bernoulli_distribution buy_or_sell(0.5);
    std::uniform_int_distribution<Quantity> qty_dist(1, 10);
    std::uniform_real_distribution<double> offset_dist(-offset_scale, offset_scale);

    Side side = buy_or_sell(rng_) ? Side::BUY : Side::SELL;
    Quantity qty = qty_dist(rng_);

    OrderRequest request;
    request.client = client_;
    request.side = side;
    request.symbol = symbol;
    request.quantity = qty;

    switch (kind_dist(rng_)) {
        case 0:
            request.kind = OrderKind::MARKET;
            break;
        case 1: {
            request.kind = OrderKind::LIMIT;
            request.price = std::max(0.01, reference * (1.0 + offset_dist(rng_)));
            break;
        }
        case 2: {
            // a stop order a little further from the reference price than a plain limit would be, so it usually sits waiting for a bit before releasing
            // e.g. a stop-loss a few percent below the current price for a SELL, or a breakout buy above it for a BUY
            request.kind = OrderKind::STOP;
            double direction = (side == Side::SELL) ? -1.0 : 1.0;
            std::uniform_real_distribution<double> stop_offset_dist(offset_scale, offset_scale * 3.0);
            request.price = std::max(0.01, reference * (1.0 + direction * stop_offset_dist(rng_)));
            break;
        }
        default: {
            request.kind = OrderKind::LIMIT_STOP;
            std::uniform_real_distribution<double> band_offset_dist(offset_scale, offset_scale * 2.5);
            double lower = reference * (1.0 - band_offset_dist(rng_));
            double upper = reference * (1.0 + band_offset_dist(rng_));
            request.price = reference;
            request.trigger_lower = std::max(0.01, lower);
            request.trigger_upper = upper;
            break;
        }
    }

    // roughly a quarter of orders carry an expiry, like a real "good-till-date" order
    std::bernoulli_distribution has_expiry(0.25);
    if (has_expiry(rng_)) {
        std::uniform_int_distribution<int> expiry_seconds_dist(5, 60);
        request.expires_in = std::chrono::seconds(expiry_seconds_dist(rng_));
    }

    engine_.submit_order(request);
}


void MomentumBot::step() {
    Symbol symbol = pick_symbol();

    // engine_.last_price() is updated synchronously inside MatchingEngine::submit_order() itself,
    // so it's always at least as fresh as market_data_.last_price() (which only updates once the notification bus's async worker thread gets around to processing the trade event
    // an eventually-consistent lag that, in a tight loop like a bot's, can mean sampling the same stale price two ticks in a row even though the "real" price already moved)
    // Preferred here as the primary source for exactly that reason; market_data is kept as a defensive fallback only
    Price reference = engine_.last_price(symbol);
    if (reference <= 0.0) {
        reference = market_data_.last_price(symbol);
    }
    if (reference <= 0.0) {
        return; // no price information yet for this symbol, skip this tick
    }

    // sample this symbol's price into its own rolling window (see the class comment in bot.hpp for why this is sampled directly rather than read from MarketData's time-bucketed candles)
    auto& history = price_history_[symbol];
    history.push_back(reference);
    if (history.size() > kWindowSize) {
        history.pop_front();
    }
    if (history.size() < 2) {
        return; // not enough of our own samples yet to judge a trend
    }

    double change = history.back() - history.front();
    if (change == 0.0) {
        return;
    }

    std::uniform_int_distribution<Quantity> qty_dist(1, 5);
    Quantity qty = qty_dist(rng_);
    Side side = (change > 0.0) ? Side::BUY : Side::SELL;

    OrderRequest request;
    request.client = client_;
    request.side = side;
    request.kind = OrderKind::MARKET;
    request.symbol = symbol;
    request.quantity = qty;
    engine_.submit_order(request);

    // attach a protective stop on the opposite side a few percent through the trade, the way a momentum strategy would guard against the trend reversing
    double offset_scale = std::clamp(volatility_for(symbol) / 20.0, 0.002, 0.05);
    std::uniform_real_distribution<double> stop_offset_dist(offset_scale, std::max(offset_scale * 2.0, offset_scale + 0.005));
    OrderRequest stop_request;
    stop_request.client = client_;
    stop_request.side = (side == Side::BUY) ? Side::SELL : Side::BUY;
    stop_request.kind = OrderKind::STOP;
    stop_request.symbol = symbol;
    stop_request.quantity = qty;
    double direction = (side == Side::BUY) ? -1.0 : 1.0; // protect a long with a lower stop, a short with a higher one
    stop_request.price = std::max(0.01, reference * (1.0 + direction * stop_offset_dist(rng_)));
    stop_request.expires_in = std::chrono::seconds(120);
    engine_.submit_order(stop_request);
}


void MarketMakerBot::step() {
    // cancel last tick's quotes (on whichever symbol they were posted on) before picking a new symbol and posting fresh ones
    // without this the book would accumulate an ever-growing pile of stale resting orders from every symbol this bot has ever visited, 
    // instead of maintaining just its current two-sided quote the way a real market maker would
    if (last_buy_order_ != 0) {
        engine_.cancel_order(last_quote_symbol_, last_buy_order_);
        last_buy_order_ = 0;
    }
    if (last_sell_order_ != 0) {
        engine_.cancel_order(last_quote_symbol_, last_sell_order_);
        last_sell_order_ = 0;
    }

    Symbol symbol = pick_symbol();
    last_quote_symbol_ = symbol;

    // see the comment on the equivalent lines in NoiseTraderBot::step()/MomentumBot::step() above:
    // engine_.last_price() is always at least as fresh as market_data_.last_price(), so it's the primary source here too
    Price mid = engine_.last_price(symbol);
    if (mid <= 0.0) {
        mid = market_data_.last_price(symbol);
    }
    if (mid <= 0.0) {
        return;
    }

    std::uniform_int_distribution<Quantity> qty_dist(5, 20);
    Price half_spread = mid * spread_fraction_ / 2.0;

    Price bid = std::max(0.01, mid - half_spread);
    Price ask = mid + half_spread;

    OrderRequest buy_request;
    buy_request.client = client_;
    buy_request.side = Side::BUY;
    buy_request.kind = OrderKind::LIMIT;
    buy_request.symbol = symbol;
    buy_request.quantity = qty_dist(rng_);
    buy_request.price = bid;
    auto buy_result = engine_.submit_order(buy_request);
    if (buy_result.accepted && buy_result.filled_quantity < buy_request.quantity) {
        last_buy_order_ = buy_result.order_id; // only remember it if something is actually left resting
    }

    OrderRequest sell_request;
    sell_request.client = client_;
    sell_request.side = Side::SELL;
    sell_request.kind = OrderKind::LIMIT;
    sell_request.symbol = symbol;
    sell_request.quantity = qty_dist(rng_);
    sell_request.price = ask;
    auto sell_result = engine_.submit_order(sell_request);
    if (sell_result.accepted && sell_result.filled_quantity < sell_request.quantity) {
        last_sell_order_ = sell_result.order_id;
    }
}

} // namespace sim
