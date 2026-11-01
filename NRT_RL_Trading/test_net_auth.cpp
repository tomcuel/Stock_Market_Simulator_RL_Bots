#include <algorithm>
#include <thread>
#include <vector>

#include "net/auth.hpp"
#include "net/sha256.hpp"
#include "nrt_framework.hpp"

using namespace sim;
using namespace sim::net;

TEST_CASE(sha256_matches_known_test_vectors) {
    CHECK_EQ(to_hex(Sha256::hash("")), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(to_hex(Sha256::hash("abc")), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(to_hex(Sha256::hash("The quick brown fox jumps over the lazy dog")), std::string("d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"));
}

TEST_CASE(sha256_is_deterministic_and_input_sensitive) {
    auto h1 = to_hex(Sha256::hash("password123"));
    auto h2 = to_hex(Sha256::hash("password123"));
    auto h3 = to_hex(Sha256::hash("password124"));
    CHECK_EQ(h1, h2);       // same input -> same digest every time
    CHECK(h1 != h3);        // a one-character change -> a completely different digest
}

TEST_CASE(register_and_login_with_correct_password_succeeds) {
    ClientDirectory directory;
    ClientId id;
    std::string token;
    CHECK(directory.register_client("alice", "secret", id, token) == ClientDirectory::RegisterOutcome::OK);
    CHECK(!token.empty());

    ClientId login_id;
    std::string login_token;
    CHECK(directory.authenticate("alice", "secret", login_id, login_token) == ClientDirectory::AuthOutcome::OK);
    CHECK_EQ(login_id, id);
    CHECK(login_token != token); // a fresh token is issued on every successful login
}

TEST_CASE(login_with_wrong_password_is_rejected) {
    ClientDirectory directory;
    ClientId id;
    std::string token;
    directory.register_client("alice", "secret", id, token);

    ClientId out_id;
    std::string out_token;
    CHECK(directory.authenticate("alice", "wrong", out_id, out_token) == ClientDirectory::AuthOutcome::WRONG_PASSWORD);
}

TEST_CASE(login_with_unknown_username_is_rejected) {
    ClientDirectory directory;
    ClientId out_id;
    std::string out_token;
    CHECK(directory.authenticate("nobody", "whatever", out_id, out_token) == ClientDirectory::AuthOutcome::UNKNOWN_USERNAME);
}

TEST_CASE(duplicate_username_registration_is_rejected) {
    ClientDirectory directory;
    ClientId id1, id2;
    std::string token1, token2;
    CHECK(directory.register_client("alice", "secret1", id1, token1) == ClientDirectory::RegisterOutcome::OK);
    CHECK(directory.register_client("alice", "secret2", id2, token2) == ClientDirectory::RegisterOutcome::USERNAME_TAKEN);
}

TEST_CASE(resume_token_resolves_to_the_right_client) {
    ClientDirectory directory;
    ClientId id;
    std::string token;
    directory.register_client("alice", "secret", id, token);

    auto resolved = directory.resolve_token(token);
    CHECK(resolved.has_value());
    CHECK_EQ(*resolved, id);

    auto unresolved = directory.resolve_token("not-a-real-token");
    CHECK(!unresolved.has_value());
}

TEST_CASE(export_and_import_account_round_trips_correctly) {
    ClientDirectory source;
    ClientId id;
    std::string token;
    source.register_client("alice", "secret", id, token);

    auto exported = source.export_accounts();
    CHECK_EQ(exported.size(), std::size_t{1});

    ClientDirectory restored;
    for (const auto& record : exported) {
        restored.import_account(record);
    }

    // the restored directory must authenticate with the original password (proving the salt+hash round-tripped correctly) and must not have re-hashed anything (import is not registration)
    ClientId login_id;
    std::string login_token;
    CHECK(restored.authenticate("alice", "secret", login_id, login_token) == ClientDirectory::AuthOutcome::OK);
    CHECK_EQ(login_id, id);
}

TEST_CASE(imported_account_advances_next_client_id_past_the_imported_one) {
    ClientDirectory directory;
    sim::net::AccountRecord record{"restored_user", /*client_id=*/500, "somesalt", "somehash"};
    directory.import_account(record);

    // a freshly registered account must get an id strictly greater than any imported one, so a restart-then-register never collides with a restored client's id
    ClientId new_id;
    std::string token;
    directory.register_client("newuser", "pw", new_id, token);
    CHECK(new_id > 500);
}

// The most important test in this file: many threads racing to register the SAME username must result in exactly one success and every other attempt correctly rejected
// never two different callers both believing they own "alice", and never a crash/corruption of the accounts map itself
// This directly exercises ClientDirectory's mutex under real contention
TEST_CASE(concurrent_registration_of_the_same_username_has_exactly_one_winner) {
    ClientDirectory directory;
    const int num_threads = 32;
    std::vector<std::thread> threads;
    std::atomic<int> successes{0};
    std::vector<ClientId> winning_ids(num_threads, 0);

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, i] {
            ClientId id;
            std::string token;
            auto outcome = directory.register_client("contested_name", "pw" + std::to_string(i), id, token);
            if (outcome == ClientDirectory::RegisterOutcome::OK) {
                successes.fetch_add(1);
                winning_ids[i] = id;
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    CHECK_EQ(successes.load(), 1);
    CHECK_EQ(directory.client_count(), std::size_t{1});
}

// Many threads registering different usernames concurrently must all succeed, each with a unique client id: proving the mutex serializes id assignment correctly without ever handing out a duplicate under contention
TEST_CASE(concurrent_registration_of_distinct_usernames_all_succeed_with_unique_ids) {
    ClientDirectory directory;
    const int num_threads = 50;
    std::vector<std::thread> threads;
    std::vector<ClientId> ids(num_threads, 0);
    std::vector<bool> ok(num_threads, false);

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, i] {
            ClientId id;
            std::string token;
            auto outcome = directory.register_client("user_" + std::to_string(i), "pw", id, token);
            ok[i] = (outcome == ClientDirectory::RegisterOutcome::OK);
            ids[i] = id;
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    for (bool success : ok) {
        CHECK(success);
    }

    std::sort(ids.begin(), ids.end());
    auto duplicate = std::adjacent_find(ids.begin(), ids.end());
    CHECK(duplicate == ids.end()); // no two threads were ever handed the same client id

    CHECK_EQ(directory.client_count(), std::size_t{num_threads});
}

// Concurrent authenticate() calls against the same already-registered account (many simultaneous logins, e.g. several bot processes resuming at once) must all succeed and each get a distinct token, with no lost or corrupted token entries
TEST_CASE(concurrent_login_to_the_same_account_all_succeed_with_distinct_tokens) {
    ClientDirectory directory;
    ClientId original_id;
    std::string original_token;
    directory.register_client("alice", "secret", original_id, original_token);

    const int num_threads = 20;
    std::vector<std::thread> threads;
    std::vector<std::string> tokens(num_threads);
    std::vector<bool> ok(num_threads, false);

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, i] {
            ClientId id;
            std::string token;
            auto outcome = directory.authenticate("alice", "secret", id, token);
            ok[i] = (outcome == ClientDirectory::AuthOutcome::OK) && (id == original_id);
            tokens[i] = token;
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    for (bool success : ok) {
        CHECK(success);
    }

    std::sort(tokens.begin(), tokens.end());
    auto duplicate = std::adjacent_find(tokens.begin(), tokens.end());
    CHECK(duplicate == tokens.end()); // every concurrent login got its own distinct token

    // every one of those tokens must resolve back to the same client id
    for (const auto& token : tokens) {
        auto resolved = directory.resolve_token(token);
        CHECK(resolved.has_value());
        CHECK_EQ(*resolved, original_id);
    }
}
