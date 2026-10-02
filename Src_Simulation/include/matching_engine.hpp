//=======================================================================
// Ties together per-symbol order books, client portfolios, the waiting-order registry, and the notification bus into one engine
// Single entry point bots / the socket server / tests submit orders through whether running in-process (Simulation, NRT tests) or driven remotely over the wire (in server files handling socket connections)
//=======================================================================
#pragma once

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "metrics.hpp"
#include "notification.hpp"
#include "order_book.hpp"
#include "order_registry.hpp"
#include "portfolio.hpp"
#include "types.hpp"

namespace sim {

class MatchingEngine {
public:
    explicit MatchingEngine(NotificationBus& bus) : bus_(bus) {}

    // Registers a tradable symbol with a reference starting price (used as the "last price" until the first real trade happens)
    void register_symbol(const Symbol& symbol, Price initial_price);

    // Ensures a client has a portfolio, creating one with `initial_cash` if it doesn't exist yet
    void ensure_client(ClientId client, double initial_cash);

    // Credits `quantity` shares of `symbol` directly into a client's portfolio, bypassing the order book. 
    // This is an administrative allocation (e.g. an IPO grant or simulation seed inventory), not a trade: it does not go through risk checks, matching, or produce a Trade event
    // (mostly used to run test and allocate initial holdings to bots without needing to submit and match a real order first in the simulation)
    void grant_initial_holdings(ClientId client, const Symbol& symbol, Quantity quantity);

    // Submits an order for matching
    // Performs a pre-trade risk check (cash for buys, holdings for sells) before it ever touches the book or the waiting registry. 
    // If the order's release band is already satisfied (true immediately for MARKET/plain LIMIT orders, for STOP/LIMIT_STOP, only once the reference price crosses into the requested band) 
    // and its `not_before` "start date" has passed, it is matched right away
    //Otherwise it is placed in the waiting registry (`SubmitResult::queued == true`) until try_release_waiting_orders() picks it up
    //Publishes a TradeEvent/OrderAcceptedEvent/OrderQueuedEvent/OrderRejectedEvent to the NotificationBus either way: callers never block on downstream consumers of that event
    SubmitResult submit_order(const OrderRequest& request);

    // Scans the waiting registry and, for every order whose  release band/start date is now satisfied, submits it into the live book: expired orders are dropped and reported
    // Called periodically by a background watcher thread, and opportunistically right after a trade updates a symbol's last price (since that's exactly when a STOP/LIMIT_STOP order's band is most likely to have just become satisfied)
    // (mirroring how Src_SQL's trigger/expiration watcher thread works without the SQL/socket coupling)
    struct ReleaseSummary {
        std::vector<SubmitResult> released;
        std::vector<Order> expired;
    };
    // - full_scan=false (the default, and what the hot path should use) only rescans symbols that had a trade since the last call 
    // - full_scan=true walks every symbol's waiting orders regardless of recent activity and must be called periodically (even if rarely) 
    // so an order waiting purely on a `not_before` start date (with no price-driven trigger to ever mark its symbol dirty) and expired orders on otherwise-quiet symbols are still eventually picked up
    ReleaseSummary try_release_waiting_orders(bool full_scan = false);

    bool cancel_order(const Symbol& symbol, Side side, Price price, OrderId order_id);
    // Cancels a resting order by id alone (no need to already know its side/price)
    bool cancel_order(const Symbol& symbol, OrderId order_id);
    bool cancel_waiting_order(OrderId order_id);

    // Last traded price for a symbol (falls back to the registered reference price if there is no trade yet) (0.0 for an unknown symbol)
    Price last_price(const Symbol& symbol) const;

    std::unordered_map<Symbol, Price> all_last_prices() const;

    BookSnapshot snapshot(const Symbol& symbol, std::size_t depth = 5) const;

    Portfolio portfolio_snapshot(ClientId client) const;

    // Every client's portfolio, keyed by client id. Used by the persistence layer (net/persistence) to snapshot state without needing to already know every client id in advance
    std::unordered_map<ClientId, Portfolio> all_portfolios() const;

    std::vector<Symbol> symbols() const;

    std::size_t waiting_order_count() const { return registry_.size(); }

    // Every individual order resting in `symbol`'s book (empty for an unknown symbol, never throws) and every order still waiting in the STOP/LIMIT_STOP/start-date registry
    // Read-only views used by the end-of-run market report
    std::vector<Order> resting_orders(const Symbol& symbol) const;
    std::vector<Order> waiting_orders() const { return registry_.all_waiting(); }

private:
    OrderBook& book_for(const Symbol& symbol);
    const OrderBook& book_for(const Symbol& symbol) const;
    bool symbol_exists(const Symbol& symbol) const;

    // pre-trade risk check: buys need enough cash (using a conservative reference price for MARKET orders, since the real fill price isn't known yet), sells need enough held shares
    bool passes_risk_check(ClientId client, Side side, OrderKind kind, const Symbol& symbol, Quantity quantity, Price price) const;

    // builds the release band (`release_lower`/`release_upper`) and absolute `not_before`/`expires_at` timestamps for a fresh order from its request, per OrderKind's semantics
    Order build_order(const OrderRequest& request, OrderId id, TimePoint now) const;

    // matches an already-validated, already-released order against the book and settles any trades: shared by submit_order() (immediate release) and try_release_waiting_orders() (delayed release)
    SubmitResult match_and_settle(Order order);

    void settle(const Trade& trade);
    void mark_dirty(const Symbol& symbol);
    std::vector<Symbol> drain_dirty_symbols();

    NotificationBus& bus_;
    MetricsRegistry& metrics_ = MetricsRegistry::instance();

    mutable std::shared_mutex books_mutex_;
    std::unordered_map<Symbol, std::unique_ptr<OrderBook>> books_;

    mutable std::shared_mutex prices_mutex_;
    std::unordered_map<Symbol, Price> last_prices_;

    mutable std::mutex portfolios_mutex_;
    std::unordered_map<ClientId, Portfolio> portfolios_;

    OrderRegistry registry_; // orders waiting for their release band / start date (own mutex)

    // symbols that traded since the last dirty-only release scan: its own small mutex, 
    // separate from prices_mutex_/books_mutex_, so marking a symbol dirty on the settlement hot path never contends with an unrelated symbol's price read
    mutable std::mutex dirty_mutex_;
    std::unordered_set<Symbol> dirty_symbols_;

    std::atomic<OrderId> next_order_id_{1};

    static constexpr double kMarketOrderCashSafetyFactor = 1.10; // buffer above reference price for MARKET buys
};

} // namespace sim
