#include "rl_bot.hpp"

#include <algorithm>
#include <cmath>
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

} // namespace

namespace {

// average price of an order's fills
Price average_fill(const SubmitResult& result) {
    double notional = 0.0;
    Quantity quantity = 0;
    for (const auto& trade : result.trades) {
        notional += trade.price * static_cast<double>(trade.quantity);
        quantity += trade.quantity;
    }
    return quantity > 0 ? notional / static_cast<double>(quantity) : 0.0;
}

} // namespace

void RLBot::on_tick() {
    if (!session_) {
        return; // a daily agent: it only trades with trading days
    }
    if (equity_ <= 0.0) {
        equity_ = lot_value() / kLotFraction; // the account's value at its first tick
    }
    int day = session_->day();
    SessionPhase phase = session_->phase();
    if (phase == SessionPhase::PRE_OPEN) {
        if (day != last_day_) {
            // the day's decisions, from the bars up to yesterday's close: their orders join the opening auction
            last_day_ = day;
            ++days_;
            for (const auto& symbol : symbols_) {
                decide(symbol);
            }
        }
        return;
    }
    if (phase != SessionPhase::OPEN) {
        return;
    }
    if (day != settled_day_) {
        settled_day_ = day;
        if (last_day_ != day) {
            ++missed_; // the pre-open went by without a tick of this bot: no decision today, the open trades keep their legs
        }
        else {
            settle_auction(day);
        }
    }
    Portfolio portfolio = gateway_.portfolio();
    for (auto& [symbol, open] : open_) {
        if (open.direction != 0) {
            watch(symbol, open, portfolio, day);
        }
    }
}

void RLBot::watch(const Symbol& symbol, Open& open, const Portfolio& portfolio, int day) {
    Quantity exposure = portfolio.holding(symbol) - open.base;
    if (exposure * open.direction > 0) {
        // still in the trade. A gap past the stop's limit leaves the released stop unfilled in the book: get out at market
        Price last = gateway_.quote(symbol).last;
        Price limit = open.stop * (1.0 - open.direction * kStopLimitThrough);
        if (last > 0.0 && (last - limit) * open.direction < 0.0) {
            close_at_market(symbol, open, "SL", day);
        }
        return;
    }
    // the position is gone: one leg filled. Cancelling a leg that already filled fails, which tells which one it was
    bool stop_cancelled = gateway_.cancel(symbol, open.stop_id);
    bool target_cancelled = gateway_.cancel(symbol, open.target_id);
    std::string reason = !target_cancelled && stop_cancelled ? "TP" : (!stop_cancelled && target_cancelled ? "SL" : "both");
    // the exit's real price: the legs' fills (both, when both filled)
    MarketGateway::Fill stop_fill = gateway_.filled(open.stop_id), target_fill = gateway_.filled(open.target_id);
    MarketGateway::Fill legs{stop_fill.quantity + target_fill.quantity, stop_fill.notional + target_fill.notional};
    Price last = gateway_.quote(symbol).last;
    Price exit = legs.quantity > 0 ? legs.average() : (reason == "TP" ? open.target : (open.direction == 1 ? std::min(open.stop, last) : std::max(open.stop, last)));
    if (exposure != 0) {
        // both legs filled (a gap through both): the position went the other way, back to flat at market
        place(make_request(exposure > 0 ? Side::SELL : Side::BUY, OrderKind::MARKET, symbol, std::abs(exposure)), 1);
    }
    record(symbol, open, exit, reason, day);
    open = Open{};
}

void RLBot::decide(const Symbol& symbol) {
    Open& open = open_[symbol];
    std::ostringstream state;
    state << std::setprecision(10) << "dir=" << open.direction << " entry=" << open.entry << " sl=" << open.stop << " tp=" << open.target
          << " days=" << open.days << " tpr=" << open.tp_r;
    PolicyAction action = client_->act(model_.name, symbol, state.str());
    if (action.not_ready) {
        ++waited_; // the server doesn't hold the agent's warm-up of daily bars yet
        return;
    }
    if (!action.ok) {
        ++failures_;
        return;
    }
    ++decisions_;
    int direction = action.position;
    Pending& pending = pending_[symbol];
    pending = Pending{};
    pending.base = open.direction != 0 ? open.base : gateway_.portfolio().holding(symbol);
    pending.holding_before = gateway_.portfolio().holding(symbol);
    if (open.direction != 0) {
        if (direction == open.direction) {
            ++open.days; // hold: the legs keep resting
            return;
        }
        // out at the open: the legs go, a market-on-open order flattens the position
        gateway_.cancel(symbol, open.stop_id);
        gateway_.cancel(symbol, open.target_id);
        Quantity exposure = pending.holding_before - open.base;
        if (exposure != 0) {
            pending.close_id = place(make_request(exposure > 0 ? Side::SELL : Side::BUY, OrderKind::MARKET, symbol, std::abs(exposure)), 1).order_id;
        }
        pending.close_reason = direction == 0 ? "close" : "flip";
    }
    Price price = gateway_.quote(symbol).last; // the reference the day opens from (yesterday's close, moved by the night's news)
    double sl_dist = action.sl_atr * action.atr;
    if (direction == 0 || price <= 0.0 || sl_dist <= 0.0 || action.tp_r <= 0.0) {
        return;
    }
    auto quantity = static_cast<Quantity>(std::floor(std::min(risk_fraction_ * equity_ / sl_dist, max_notional_ * equity_ / price)));
    if (quantity < 1) {
        return;
    }
    // market-on-open: it joins the opening auction (at the 2% auction collar) and fills at the open, or expires
    SubmitResult entry = place(make_request(direction == 1 ? Side::BUY : Side::SELL, OrderKind::MARKET, symbol, quantity), 1);
    if (entry.accepted) {
        pending.entry_id = entry.order_id;
        pending.direction = direction;
        pending.sl_dist = sl_dist;
        pending.tp_r = action.tp_r;
        pending.sl_atr = action.sl_atr;
    }
}

void RLBot::settle_auction(int day) {
    Portfolio portfolio = gateway_.portfolio();
    for (auto& [symbol, pending] : pending_) {
        Quantity holding = portfolio.holding(symbol);
        Price open_price = gateway_.quote(symbol).last; // only if a fill isn't known (it is, in-process)
        Open& open = open_[symbol];
        if (!pending.close_reason.empty()) {
            MarketGateway::Fill out = gateway_.filled(pending.close_id);
            record(symbol, open, out.quantity > 0 ? out.average() : open_price, pending.close_reason, day);
            open = Open{};
            Quantity left = holding - pending.base; // what the auction didn't take of a plain close
            if (pending.direction == 0 && left != 0) {
                // the auction didn't take all of it: the rest at market now
                place(make_request(left > 0 ? Side::SELL : Side::BUY, OrderKind::MARKET, symbol, std::abs(left)), 1);
            }
        }
        if (pending.direction == 0) {
            continue;
        }
        Quantity filled = (holding - pending.base) * pending.direction;
        if (filled <= 0) {
            continue; // the auction couldn't fill it
        }
        MarketGateway::Fill in = gateway_.filled(pending.entry_id);
        Price price = in.quantity > 0 ? in.average() : open_price;
        open.direction = pending.direction;
        open.quantity = filled;
        open.base = pending.base;
        open.entry = price;
        open.stop = price - open.direction * pending.sl_dist;
        open.target = price + open.direction * pending.tp_r * pending.sl_dist;
        open.sl_atr = pending.sl_atr;
        open.tp_r = pending.tp_r;
        open.days = 0;
        open.entry_day = day;
        place_legs(symbol, open, day);
    }
    pending_.clear();
}

void RLBot::place_legs(const Symbol& symbol, Open& open, int day) {
    Side exit_side = open.direction == 1 ? Side::SELL : Side::BUY;
    // the stop-loss: released when the price reaches the stop, as a sell (buy) limit 1% through it (watch() exits at market
    // if a gap goes past that limit); the take-profit: a limit at the target. Both GTC,
    // straight through the gateway (not place(): they must not be retracted as stale, they live until one of them fills)
    OrderRequest stop = make_request(exit_side, OrderKind::STOP, symbol, open.quantity, std::max(0.01, open.stop * (1.0 - open.direction * kStopLimitThrough)));
    stop.trigger_upper = std::max(0.01, open.stop);
    stop.time_in_force = TimeInForce::GTC;
    OrderRequest target = make_request(exit_side, OrderKind::LIMIT, symbol, open.quantity, std::max(0.01, open.target));
    target.time_in_force = TimeInForce::GTC;
    SubmitResult stop_result = gateway_.submit(stop);
    SubmitResult target_result = gateway_.submit(target);
    open.stop_id = stop_result.accepted ? stop_result.order_id : 0;
    open.target_id = target_result.accepted ? target_result.order_id : 0;
    legs_rejected_ += (stop_result.accepted ? 0 : 1) + (target_result.accepted ? 0 : 1);
    ++entries_;
    if (target_result.accepted && target_result.filled_quantity >= open.quantity) {
        // the target was already through the market (a fast move since the open): filled at once
        gateway_.cancel(symbol, open.stop_id);
        record(symbol, open, average_fill(target_result), "TP", day);
        open = Open{};
    }
}

void RLBot::close_at_market(const Symbol& symbol, Open& open, const std::string& reason, int day) {
    gateway_.cancel(symbol, open.stop_id);
    gateway_.cancel(symbol, open.target_id);
    Quantity exposure = gateway_.portfolio().holding(symbol) - open.base;
    Price exit = gateway_.quote(symbol).last;
    if (exposure != 0) {
        SubmitResult result = place(make_request(exposure > 0 ? Side::SELL : Side::BUY, OrderKind::MARKET, symbol, std::abs(exposure)), 1);
        if (result.filled_quantity > 0) {
            exit = average_fill(result);
        }
    }
    record(symbol, open, exit, reason, day);
    open = Open{};
}

void RLBot::record(const Symbol& symbol, const Open& open, Price exit, const std::string& reason, int day) {
    RLTrade trade;
    trade.symbol = symbol;
    trade.direction = open.direction;
    trade.entry_day = open.entry_day;
    trade.exit_day = day;
    trade.quantity = open.quantity;
    trade.entry = open.entry;
    trade.exit = exit;
    trade.stop = open.stop;
    trade.target = open.target;
    trade.sl_atr = open.sl_atr;
    trade.tp_r = open.tp_r;
    trade.reason = reason;
    trades_.push_back(trade);
}

} // namespace sim
