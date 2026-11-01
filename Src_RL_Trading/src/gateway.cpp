#include "gateway.hpp"

#include "matching_engine.hpp"
#include "notification.hpp"

namespace sim {

std::vector<Symbol> InProcessGateway::symbols() {
    return engine_.symbols();
}

Quote InProcessGateway::quote(const Symbol& symbol)
{
    Quote quote;
    quote.last = engine_.last_price(symbol);
    auto snap = engine_.snapshot(symbol, 1);
    if (!snap.bids.empty()) quote.best_bid = snap.bids.front().price;
    if (!snap.asks.empty()) quote.best_ask = snap.asks.front().price;
    return quote;
}

Portfolio InProcessGateway::portfolio() {
    return engine_.portfolio_snapshot(client_);
}

SubmitResult InProcessGateway::submit(OrderRequest request) {
    request.client = client_;
    return engine_.submit_order(request);
}

InProcessGateway::~InProcessGateway() {
    if (tracking_) {
        engine_.bus().unsubscribe(subscription_);
    }
}

void InProcessGateway::track_fills() {
    if (tracking_) {
        return;
    }
    tracking_ = true;
    subscription_ = engine_.bus().subscribe([this](const Event& event) {
        const auto* trade = std::get_if<TradeEvent>(&event);
        if (!trade || (trade->trade.buyer != client_ && trade->trade.seller != client_)) {
            return;
        }
        std::lock_guard<std::mutex> lock(fills_mutex_);
        for (OrderId id : {trade->trade.buyer == client_ ? trade->trade.buy_order_id : OrderId{0}, trade->trade.seller == client_ ? trade->trade.sell_order_id : OrderId{0}}) {
            if (id != 0) {
                Fill& fill = fills_[id];
                fill.quantity += trade->trade.quantity;
                fill.notional += trade->trade.price * static_cast<double>(trade->trade.quantity);
            }
        }
    });
}

MarketGateway::Fill InProcessGateway::filled(OrderId order_id) {
    std::lock_guard<std::mutex> lock(fills_mutex_);
    auto it = fills_.find(order_id);
    return it == fills_.end() ? Fill{} : it->second;
}

bool InProcessGateway::cancel(const Symbol& symbol, OrderId order_id) {
    // resting in the book first (the common case), then still waiting for a STOP/LIMIT_STOP trigger, and always only this client's own orders
    return engine_.cancel_order(symbol, order_id, client_) || engine_.cancel_waiting_order(order_id, client_);
}

} // namespace sim
