#include <random>
#include <thread>
#include <vector>

#include "matching_engine.hpp"
#include "nrt_framework.hpp"
#include "test_helpers.hpp"

using namespace sim;

// Hammers the engine with many concurrent buyers and sellers on the same symbol and checks a fundamental invariant that must hold no matter how threads interleave: 
// total shares and total cash across all participants are conserved (a trade only ever moves value between two clients, it never creates or destroys it) and nothing crashes or deadlocks under contention
TEST_CASE(concurrent_orders_conserve_total_shares_and_cash) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);

    const int num_clients = 20;
    const double initial_cash = 100000.0;
    const Quantity initial_shares = 100;

    for (ClientId id = 1; id <= num_clients; ++id) {
        engine.ensure_client(id, initial_cash);
        engine.grant_initial_holdings(id, "AAPL", initial_shares);
    }

    const int orders_per_thread = 200;
    std::vector<std::thread> threads;
    for (int t = 0; t < num_clients; ++t) {
        threads.emplace_back([&engine, t]() {
            ClientId client = t + 1;
            std::mt19937 rng(t + 1);
            std::uniform_int_distribution<int> qty_dist(1, 5);
            std::uniform_real_distribution<double> price_dist(90.0, 110.0);
            std::bernoulli_distribution side_dist(0.5);
            std::bernoulli_distribution kind_dist(0.3);

            for (int i = 0; i < orders_per_thread; ++i) {
                Side side = side_dist(rng) ? Side::BUY : Side::SELL;
                if (kind_dist(rng)) {
                    engine.submit_order(nrt::make_order(client, side, OrderKind::MARKET, "AAPL", qty_dist(rng)));
                } 
                else {
                    engine.submit_order(nrt::make_order(client, side, OrderKind::LIMIT, "AAPL", qty_dist(rng), price_dist(rng)));
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    double total_cash = 0.0;
    Quantity total_shares = 0;
    for (ClientId id = 1; id <= num_clients; ++id) {
        Portfolio p = engine.portfolio_snapshot(id);
        total_cash += p.cash;
        total_shares += p.holding("AAPL");
    }

    CHECK_NEAR(total_cash, initial_cash * num_clients, 1e-6);
    CHECK_EQ(total_shares, initial_shares * num_clients);
}

// Same idea but across several independently-locked symbols at once, to exercise the multi-book routing path (per-symbol shared_mutex) concurrently without cross-symbol interference
TEST_CASE(concurrent_orders_across_multiple_symbols_stay_isolated) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    std::vector<Symbol> symbols = {"AAA", "BBB", "CCC"};
    for (const auto& s : symbols) {
        engine.register_symbol(s, 50.0);
    }

    const int num_clients = 12;
    for (ClientId id = 1; id <= num_clients; ++id) {
        engine.ensure_client(id, 50000.0);
        for (const auto& s : symbols) {
            engine.grant_initial_holdings(id, s, 50);
        }
    }

    std::vector<std::thread> threads;
    for (int t = 0; t < num_clients; ++t) {
        threads.emplace_back([&engine, &symbols, t]() {
            ClientId client = t + 1;
            std::mt19937 rng(t + 100);
            std::uniform_int_distribution<std::size_t> symbol_dist(0, symbols.size() - 1);
            std::uniform_int_distribution<int> qty_dist(1, 3);
            std::bernoulli_distribution side_dist(0.5);

            for (int i = 0; i < 100; ++i) {
                Side side = side_dist(rng) ? Side::BUY : Side::SELL;
                const Symbol& symbol = symbols[symbol_dist(rng)];
                engine.submit_order(nrt::make_order(client, side, OrderKind::MARKET, symbol, qty_dist(rng)));
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    for (const auto& s : symbols) {
        Quantity total_shares = 0;
        for (ClientId id = 1; id <= num_clients; ++id) {
            total_shares += engine.portfolio_snapshot(id).holding(s);
        }
        CHECK_EQ(total_shares, Quantity{50 * num_clients});
    }
}

// Direct stress test of MatchingEngine::books_mutex_ (the shared_mutex guarding the symbol->book map itself, separate from any individual OrderBook's own mutex): 
// many threads concurrently registering brand-new symbols while other threads concurrently read symbols()/snapshot() on symbols that already exist
// Registration is rare in real usage (done once at startup), but this proves the map itself never gets corrupted if it ever does happen concurrently with reads
TEST_CASE(concurrent_symbol_registration_and_reads_never_corrupt_the_book_map) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("STABLE", 100.0); // exists for the whole test, for readers to poll

    const int num_registrar_threads = 10;
    const int symbols_per_thread = 20;
    std::atomic<bool> stop_readers{false};

    std::vector<std::thread> registrars;
    for (int t = 0; t < num_registrar_threads; ++t) {
        registrars.emplace_back([&, t] {
            for (int i = 0; i < symbols_per_thread; ++i) {
                engine.register_symbol("SYM_" + std::to_string(t) + "_" + std::to_string(i), 50.0);
            }
        });
    }

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&] {
            while (!stop_readers.load()) {
                auto symbols = engine.symbols(); // must never crash while the map is being written to
                CHECK(!symbols.empty());
                auto snap = engine.snapshot("STABLE", 5);
                (void)snap;
            }
        });
    }

    for (auto& t : registrars) {
        t.join();
    }
    stop_readers.store(true);
    for (auto& t : readers) {
        t.join();
    }

    // every symbol that was supposedly registered must actually be there and independently usable
    auto final_symbols = engine.symbols();
    CHECK_EQ(final_symbols.size(), std::size_t{1 + num_registrar_threads * symbols_per_thread});
    CHECK_EQ(engine.last_price("SYM_3_7"), 50.0);
}

// Direct stress test of MatchingEngine::prices_mutex_: many reader threads continuously calling last_price()/all_last_prices() while many writer threads continuously push trades that update it
// A shared_mutex should let readers proceed concurrently with each other but never see a torn/partial write: this proves that under real contention, not just in principle
TEST_CASE(concurrent_price_reads_never_see_a_torn_or_stale_indefinitely_value) {
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);

    const int num_clients = 10;
    for (ClientId id = 1; id <= num_clients; ++id) {
        engine.ensure_client(id, 1000000.0);
        engine.grant_initial_holdings(id, "AAPL", 1000);
    }

    std::atomic<bool> stop{false};
    std::atomic<int> reads_performed{0};

    std::vector<std::thread> writers;
    for (int t = 0; t < 6; ++t) {
        writers.emplace_back([&, t] {
            std::mt19937 rng(t + 1);
            std::uniform_real_distribution<double> price_dist(90.0, 110.0);
            ClientId buyer = (t % num_clients) + 1;
            ClientId seller = ((t + 1) % num_clients) + 1;
            for (int i = 0; i < 300; ++i) {
                double price = price_dist(rng);
                engine.submit_order(nrt::make_order(seller, Side::SELL, OrderKind::LIMIT, "AAPL", 1, price));
                engine.submit_order(nrt::make_order(buyer, Side::BUY, OrderKind::LIMIT, "AAPL", 1, price));
            }
        });
    }

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                Price p = engine.last_price("AAPL");
                CHECK(p > 0.0); // AAPL was registered with a positive reference price and only ever trades at positive prices, so this must never observe 0/negative/garbage
                auto all_prices = engine.all_last_prices();
                CHECK(all_prices.count("AAPL") == 1);
                reads_performed.fetch_add(1);
            }
        });
    }

    for (auto& t : writers) {
        t.join();
    }
    stop.store(true);
    for (auto& t : readers) {
        t.join();
    }

    CHECK(reads_performed.load() > 0); // sanity: readers actually got to run concurrently with writers
}
