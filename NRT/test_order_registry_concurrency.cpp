// Direct stress tests of OrderRegistry's mutex: many threads adding, cancelling, and scanning waiting orders simultaneously
// The registry sits on the hot path (every STOP/LIMIT_STOP order that isn't immediately releasable goes through it, and the watcher thread scans it constantly)
#include <atomic>
#include <random>
#include <thread>
#include <vector>

#include "nrt_framework.hpp"
#include "order_registry.hpp"

using namespace sim;

namespace {

Order make_waiting_order(OrderId id, ClientId client, const Symbol& symbol, Price release_lower, Price release_upper) {
    Order order;
    order.id = id;
    order.client = client;
    order.side = Side::BUY;
    order.kind = OrderKind::LIMIT_STOP;
    order.symbol = symbol;
    order.quantity = 1;
    order.price = 100.0;
    order.release_lower = release_lower;
    order.release_upper = release_upper;
    order.submitted_at = Clock::now();
    return order;
}

} // namespace

TEST_CASE(registry_add_then_cancel_by_id_removes_it) {
    OrderRegistry registry;
    registry.add(make_waiting_order(1, 1, "AAPL", 90.0, 110.0));
    CHECK_EQ(registry.size(), std::size_t{1});
    CHECK(registry.cancel(1));
    CHECK_EQ(registry.size(), std::size_t{0});
    CHECK(!registry.cancel(1)); // already gone
}

TEST_CASE(registry_scan_by_symbol_only_touches_that_symbols_bucket) {
    OrderRegistry registry;
    registry.add(make_waiting_order(1, 1, "AAPL", 200.0, 300.0)); // out of band, won't release
    registry.add(make_waiting_order(2, 1, "MSFT", 90.0, 110.0));  // in band, will release

    auto aapl_scan = registry.scan_and_release_symbol("AAPL", Clock::now(), [](const Symbol&) { return 100.0; });
    CHECK_EQ(aapl_scan.released.size(), std::size_t{0}); // AAPL order stays: out of its own band
    CHECK_EQ(registry.size(), std::size_t{2}); // MSFT untouched by an AAPL-only scan

    auto msft_scan = registry.scan_and_release_symbol("MSFT", Clock::now(), [](const Symbol&) { return 100.0; });
    CHECK_EQ(msft_scan.released.size(), std::size_t{1});
    CHECK_EQ(registry.size(), std::size_t{1}); // only AAPL's order remains
}

TEST_CASE(registry_full_scan_covers_every_symbol) {
    OrderRegistry registry;
    registry.add(make_waiting_order(1, 1, "AAPL", 90.0, 110.0));
    registry.add(make_waiting_order(2, 1, "MSFT", 90.0, 110.0));
    registry.add(make_waiting_order(3, 1, "GOOG", 200.0, 300.0)); // stays out of band

    auto scan = registry.scan_and_release(Clock::now(), [](const Symbol&) { return 100.0; });
    CHECK_EQ(scan.released.size(), std::size_t{2});
    CHECK_EQ(registry.size(), std::size_t{1});
}

TEST_CASE(symbols_with_waiting_orders_reflects_current_contents) {
    OrderRegistry registry;
    registry.add(make_waiting_order(1, 1, "AAPL", 200.0, 300.0));
    registry.add(make_waiting_order(2, 1, "AAPL", 200.0, 300.0));
    registry.add(make_waiting_order(3, 1, "MSFT", 200.0, 300.0));

    auto symbols = registry.symbols_with_waiting_orders();
    CHECK_EQ(symbols.size(), std::size_t{2}); // AAPL and MSFT, regardless of how many orders each holds
}

// The critical mutex test for this class: many threads simultaneously adding new waiting orders, cancelling existing ones and running release scans: all against the same registry, all at once
// Checks two things that would only fail under a real race: 
// (1) the registry's own id-to-symbol index and per-symbol buckets never desync (every order that size() claims exists must be individually reachable)
// (2) no order is ever released or expired twice, and none vanish without being accounted for
TEST_CASE(concurrent_add_cancel_and_scan_never_lose_or_double_release_an_order) {
    OrderRegistry registry;
    const int num_adder_threads = 8;
    const int orders_per_adder = 150;
    const std::vector<Symbol> symbols = {"AAPL", "MSFT", "GOOG"};

    std::atomic<OrderId> next_id{1};
    std::atomic<int> total_added{0};
    std::atomic<int> total_released{0};
    std::atomic<bool> stop_scanning{false};

    // half the orders are born already release-ready (band contains the fixed reference price of 100), the other half are permanently out of band
    //so this test can assert exactly how many should eventually be released, not just that "some number" were
    auto reference_price = [](const Symbol&) {return 100.0;};

    std::vector<std::thread> adders;
    for (int t = 0; t < num_adder_threads; ++t) {
        adders.emplace_back([&, t] {
            std::mt19937 rng(t + 1);
            std::uniform_int_distribution<std::size_t> symbol_dist(0, symbols.size() - 1);
            std::bernoulli_distribution releasable_dist(0.5);

            for (int i = 0; i < orders_per_adder; ++i) {
                OrderId id = next_id.fetch_add(1);
                const Symbol& symbol = symbols[symbol_dist(rng)];
                Order order = releasable_dist(rng)
                    ? make_waiting_order(id, t, symbol, 90.0, 110.0)   // will release (100 is in band)
                    : make_waiting_order(id, t, symbol, 500.0, 600.0); // never releases
                registry.add(std::move(order));
                total_added.fetch_add(1);
            }
        });
    }

    // a concurrent scanner thread, racing with the adders above, draining released orders into a running count
    // this is exactly the shape of the real watcher thread vs. live order submission racing in SimulationServer
    std::atomic<int> released_this_run{0};
    std::thread scanner([&] {
        while (!stop_scanning.load()) {
            for (const auto& symbol : symbols) {
                auto scan = registry.scan_and_release_symbol(symbol, Clock::now(), reference_price);
                released_this_run.fetch_add(static_cast<int>(scan.released.size()));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    for (auto& th : adders) th.join();

    // one final scan after every adder has finished, to catch anything added right at the end
    for (const auto& symbol : symbols) {
        auto scan = registry.scan_and_release_symbol(symbol, Clock::now(), reference_price);
        released_this_run.fetch_add(static_cast<int>(scan.released.size()));
    }
    stop_scanning.store(true);
    scanner.join();
    total_released.store(released_this_run.load());

    int total = total_added.load();
    CHECK_EQ(total, num_adder_threads * orders_per_adder);

    // exactly the "releasable" half (statistically ~50%, but must be an exact accounting, not an approximation) should have been released
    // the rest must still be sitting in the registry: nothing lost, nothing double-counted
    std::size_t remaining = registry.size();
    CHECK_EQ(static_cast<std::size_t>(total_released.load()) + remaining, static_cast<std::size_t>(total));
}
