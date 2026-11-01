#include <cstdio>
#include <random>

#include "matching_engine.hpp"
#include "net/auth.hpp"
#include "net/persistence.hpp"
#include "notification.hpp"
#include "nrt_framework.hpp"
#include "test_helpers.hpp"

using namespace sim;
using namespace sim::net;

namespace {

// unique-per-test-run temp path, so parallel/repeated test runs never collide on the same file
std::string temp_snapshot_path(const std::string& tag) {
    return (nrt::fresh_output_dir("persistence_" + tag) / "snapshot.txt").string(); // under NRT/output/
}

} // namespace

TEST_CASE(load_of_a_missing_snapshot_file_is_not_an_error) {
    ClientDirectory directory;
    NotificationBus bus;
    MatchingEngine engine(bus);
    bool ok = PersistenceStore::load((nrt::fresh_output_dir("persistence_missing") / "does_not_exist.txt").string(), directory, engine);
    CHECK(!ok); // reports false (nothing to load) ...
    CHECK_EQ(directory.client_count(), std::size_t{0}); // ... but leaves everything in a clean, usable state
}

TEST_CASE(save_then_load_round_trips_accounts_portfolios_and_prices) {
    std::string path = temp_snapshot_path("roundtrip");

    {
        ClientDirectory directory;
        NotificationBus bus;
        MatchingEngine engine(bus);

        ClientId alice_id, bob_id;
        std::string token;
        directory.register_client("alice", "alice_password", alice_id, token);
        directory.register_client("bob", "bob_password", bob_id, token);

        engine.register_symbol("AAPL", 150.0);
        engine.register_symbol("MSFT", 300.0);
        engine.ensure_client(alice_id, 12345.67);
        engine.grant_initial_holdings(alice_id, "AAPL", 10);
        engine.grant_initial_holdings(alice_id, "MSFT", 3);
        engine.ensure_client(bob_id, 500.0);

        CHECK(PersistenceStore::save(path, directory, engine));
    }

    // fresh, unrelated directory/engine -- everything below must come purely from the file
    ClientDirectory restored_directory;
    NotificationBus bus;
    MatchingEngine restored_engine(bus);
    CHECK(PersistenceStore::load(path, restored_directory, restored_engine));

    CHECK_EQ(restored_directory.client_count(), std::size_t{2});

    ClientId alice_login_id;
    std::string alice_token;
    CHECK(restored_directory.authenticate("alice", "alice_password", alice_login_id, alice_token) == ClientDirectory::AuthOutcome::OK);

    Portfolio alice_portfolio = restored_engine.portfolio_snapshot(alice_login_id);
    CHECK_NEAR(alice_portfolio.cash, 12345.67, 1e-6);
    CHECK_EQ(alice_portfolio.holding("AAPL"), Quantity{10});
    CHECK_EQ(alice_portfolio.holding("MSFT"), Quantity{3});

    CHECK_EQ(restored_engine.last_price("AAPL"), 150.0);
    CHECK_EQ(restored_engine.last_price("MSFT"), 300.0);

    std::remove(path.c_str());
}

TEST_CASE(restored_account_rejects_the_wrong_password) {
    std::string path = temp_snapshot_path("wrongpw");

    {
        ClientDirectory directory;
        NotificationBus bus;
        MatchingEngine engine(bus);
        ClientId id;
        std::string token;
        directory.register_client("alice", "correct_password", id, token);
        engine.ensure_client(id, 1000.0);
        PersistenceStore::save(path, directory, engine);
    }

    ClientDirectory restored_directory;
    NotificationBus bus;
    MatchingEngine restored_engine(bus);
    PersistenceStore::load(path, restored_directory, restored_engine);

    ClientId out_id;
    std::string out_token;
    CHECK(restored_directory.authenticate("alice", "wrong_password", out_id, out_token) == ClientDirectory::AuthOutcome::WRONG_PASSWORD);

    std::remove(path.c_str());
}

TEST_CASE(save_is_safe_to_call_while_engine_state_keeps_changing) {
    // not a strict concurrency proof, but exercises save() reading a live, still-mutating engine through its already-thread-safe accessors (all_portfolios/all_last_prices/export_accounts)
    // without crashing or corrupting the snapshot file -- the actual thread-safety of each of those accessors is covered by the dedicated mutex tests elsewhere in this suite
    std::string path = temp_snapshot_path("concurrent_save");

    ClientDirectory directory;
    NotificationBus bus;
    MatchingEngine engine(bus);
    engine.register_symbol("AAPL", 100.0);
    ClientId id;
    std::string token;
    directory.register_client("alice", "pw", id, token);
    engine.ensure_client(id, 100000.0);
    engine.grant_initial_holdings(id, "AAPL", 100);

    std::atomic<bool> stop{false};
    std::thread mutator([&] {
        while (!stop.load()) {
            OrderRequest request;
            request.client = id;
            request.side = Side::BUY;
            request.kind = OrderKind::MARKET;
            request.symbol = "AAPL";
            request.quantity = 1;
            engine.submit_order(request);
        }
    });

    for (int i = 0; i < 5; ++i) {
        CHECK(PersistenceStore::save(path, directory, engine));
    }

    stop.store(true);
    mutator.join();
    std::remove(path.c_str());
}
