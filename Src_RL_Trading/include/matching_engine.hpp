//=======================================================================
// Ties together per-symbol order books, client portfolios, the waiting-order registry, and the notification bus into one engine
// Single entry point bots / the socket server / tests submit orders through whether running in-process (Simulation, NRT tests) or driven remotely over the wire (in server files handling socket connections)
//=======================================================================
#ifndef MATCHING_ENGINE_HPP
#define MATCHING_ENGINE_HPP

#include <memory>
#include <mutex>
#include <optional>
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
    void grant_initial_holdings(ClientId client, const Symbol& symbol, Quantity quantity);

    // Opens `client` with a balanced account worth `equity` at the current prices: 
    // `cash_fraction` of it in cash, the rest split equally by value across every registered symbol (shares = budget per symbol / price, rounded)
    // Every account then holds about the same value of each symbol whatever its price, instead of the same number of shares: 
    // 100 shares of every symbol made a bot 98% stock and 2% cash with index prices (100 x 42573 for ^DJI alone), 
    // so it could sell everything and buy almost nothing, and the market drifted down
    // A symbol whose single share costs more than twice its budget gets no shares: returns the account's starting stock value
    double fund_balanced_account(ClientId client, double equity, double cash_fraction);

    // Submits an order for matching
    // Performs a pre-trade risk check (cash for buys, holdings for sells) before it ever touches the book or the waiting registry. 
    // If the order's release band is already satisfied (true immediately for MARKET/plain LIMIT orders, for STOP/LIMIT_STOP, only once the reference price crosses into the requested band) 
    // and its `not_before` "start date" has passed, it is matched right away
    // Otherwise it is placed in the waiting registry (`SubmitResult::queued == true`) until try_release_waiting_orders() picks it up
    // Publishes a TradeEvent/OrderAcceptedEvent/OrderQueuedEvent/OrderRejectedEvent to the NotificationBus either way: callers never block on downstream consumers of that event
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

    bool cancel_order(const Symbol& symbol, Side side, Price price, OrderId order_id, std::optional<ClientId> owner = std::nullopt);
    // Cancels a resting order by id alone (no need to already know its side/price)
    bool cancel_order(const Symbol& symbol, OrderId order_id, std::optional<ClientId> owner = std::nullopt);
    bool cancel_waiting_order(OrderId order_id, std::optional<ClientId> owner = std::nullopt);

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

    // end of the trading day (used by sim_server.x when a session closes)
    // Pre-close: accepted orders rest without matching (the books may cross) and waiting orders are not released, so the fixing sees every order of the pre-close period at once
    void set_auction_mode(bool on) {auction_mode_.store(on);}
    // After the closing auction the market is closed: every new order is rejected ("market closed") until the process ends, so nothing trades between the close and the snapshot
    // A restarted server opens again the closing time is kept: order dates count trading time, so an order's clock stops at the close (the snapshot measures what it has left from the close, however long the server then takes to stop)
    void close_market() {
        closed_at_ticks_.store(Clock::now().time_since_epoch().count());
        market_closed_.store(true);
    }
    bool market_closed() const {return market_closed_.load();}
    // the close if the market is closed, otherwise now: the reference for an order's remaining time
    TimePoint trading_clock() const {
        return market_closed_.load() ? TimePoint(Clock::duration(closed_at_ticks_.load())) : Clock::now();
    }

    bool auction_mode() const {return auction_mode_.load();}
    struct ClosingPrice {
        Symbol symbol;
        Price price{0.0};    // the official close: the auction price, or the last traded price when nothing crossed
        Quantity volume{0};  // shares executed by the auction
        bool crossed{false};
    };
    // The fixing on every symbol (OrderBook::uncross): trades at one price per symbol, settled like any trade, 
    // and that price becomes the symbol's last price, so the snapshot saved next holds the official closing prices Leaves pre-close (auction mode off)
    std::vector<ClosingPrice> run_closing_auction();
    // The opening auction of a session that restored orders from a snapshot (they may cross each other), same rule
    std::vector<ClosingPrice> run_opening_auction() {return run_closing_auction();}
    // At the close: every DAY order (resting or waiting) expires and releases its reservation, and so does any MARKET order left (immediate or cancel, never carried) and any order already past its expiry date, GTC orders (and orders with their own expiry date still ahead) stay for the next session, returns how many expired
    std::size_t expire_day_orders();
    // every order, DAY or not (tests, and a full reset)
    std::size_t expire_all_orders();

    // carrying orders from one session to the next (net/persistence.cpp)
    struct OpenOrder {
        Order order;
        bool waiting{false}; // in the waiting registry (STOP / LIMIT_STOP / start date), otherwise resting in the book
    };
    // every open order: book orders in priority order (best price first, oldest first), then waiting orders
    std::vector<OpenOrder> open_orders() const;
    // puts a saved order back (its reservation is taken again, it does NOT match: the opening auction does), keeps its id,
    // false (and nothing changes) if the account can no longer afford it
    bool restore_order(const OpenOrder& open);

private:
    OrderBook& book_for(const Symbol& symbol);
    const OrderBook& book_for(const Symbol& symbol) const;
    bool symbol_exists(const Symbol& symbol) const;

    // Reservations: try_reserve() checks what the client still has available (cash or shares not already engaged by its other open orders) 
    // and sets the order's worst case aside, atomically under portfolios_mutex_, so two concurrent orders can't both pass the check on the same money
    // release_reservation() gives back what `quantity` units of an order had reserved (cancel, expiry, or the discarded remainder of a MARKET order)
    bool try_reserve(const Order& order);
    void release_reservation(const Order& order, Quantity quantity);

    // builds the release band (`release_lower`/`release_upper`) and absolute `not_before`/`expires_at` timestamps for a fresh order from its request, per OrderKind's semantics
    Order build_order(const OrderRequest& request, OrderId id, TimePoint now) const;

    // matches an already-validated, already-released order against the book and settles any trades: shared by submit_order() (immediate release) and try_release_waiting_orders() (delayed release)
    SubmitResult match_and_settle(Order order);

    // moves cash and shares for one trade and consumes the matching reservations
    // buyer_reserve_price is what the buy order reserved per share (its limit or collar), which can exceed the trade price
    void settle(const Trade& trade, Price buyer_reserve_price);
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
    std::atomic<bool> auction_mode_{false};
    std::atomic<bool> market_closed_{false};
    std::atomic<Clock::rep> closed_at_ticks_{0};
    std::size_t expire_orders(bool day_only);

    // MARKET orders get a price collar around the last price: a BUY never pays more than +10%, 
    // a SELL never sells for less than -10%, and whatever can't fill inside the collar is discarded (still IOC)
    // It bounds a market buy's worst-case cost, which is what makes it reservable
    static constexpr double kMarketCollar = 0.10;
};

} // namespace sim

#endif // MATCHING_ENGINE_HPP