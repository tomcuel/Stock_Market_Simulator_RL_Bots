#include "gateway.hpp"

#include "matching_engine.hpp"

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

bool InProcessGateway::cancel(const Symbol& symbol, OrderId order_id) {
    // resting in the book first (the common case), then still waiting for a STOP/LIMIT_STOP trigger, and always only this client's own orders
    return engine_.cancel_order(symbol, order_id, client_) || engine_.cancel_waiting_order(order_id, client_);
}

} // namespace sim
