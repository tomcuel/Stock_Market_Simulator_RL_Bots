#include <sys/socket.h>
#include <unistd.h>

#include <thread>

#include "net/protocol.hpp"
#include "nrt_framework.hpp"

using namespace sim::net;

namespace {

// a connected pair of local sockets, closed automatically: lets tests exercise real send()/recv() without needing an actual TCP listener
struct SocketPair {
    int a{-1};
    int b{-1};

    SocketPair() {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) {
            a = fds[0];
            b = fds[1];
        }
    }
    ~SocketPair() {
        if (a >= 0) ::close(a);
        if (b >= 0) ::close(b);
    }
    SocketPair(const SocketPair&) = delete;
};

} // namespace

TEST_CASE(framed_round_trip_small_payload) {
    SocketPair sp;
    set_recv_timeout(sp.b, 5);

    std::thread sender([&] { send_framed(sp.a, "hello world"); });
    auto received = recv_framed(sp.b);
    sender.join();

    CHECK(received.has_value());
    CHECK_EQ(*received, std::string("hello world"));
}

TEST_CASE(framed_round_trip_empty_payload) {
    SocketPair sp;
    set_recv_timeout(sp.b, 5);

    std::thread sender([&] { send_framed(sp.a, ""); });
    auto received = recv_framed(sp.b);
    sender.join();

    CHECK(received.has_value());
    CHECK(received->empty());
}

TEST_CASE(framed_round_trip_large_payload_spans_multiple_recv_calls) {
    SocketPair sp;
    set_recv_timeout(sp.b, 5);
    std::string large_payload(500000, 'x');

    std::thread sender([&] { send_framed(sp.a, large_payload); });
    auto received = recv_framed(sp.b);
    sender.join();

    CHECK(received.has_value());
    CHECK_EQ(received->size(), large_payload.size());
    CHECK(*received == large_payload);
}

TEST_CASE(framed_multiple_messages_on_one_connection_stay_in_order) {
    SocketPair sp;
    set_recv_timeout(sp.b, 5);

    std::thread sender([&] {
        send_framed(sp.a, "first");
        send_framed(sp.a, "second");
        send_framed(sp.a, "third");
    });

    auto m1 = recv_framed(sp.b);
    auto m2 = recv_framed(sp.b);
    auto m3 = recv_framed(sp.b);
    sender.join();

    CHECK(m1.has_value() && *m1 == "first");
    CHECK(m2.has_value() && *m2 == "second");
    CHECK(m3.has_value() && *m3 == "third");
}

TEST_CASE(oversized_length_header_is_rejected_not_allocated) {
    SocketPair sp;
    set_recv_timeout(sp.b, 2);

    unsigned char bogus_header[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // huge claimed length
    ::send(sp.a, bogus_header, sizeof(bogus_header), 0);

    auto received = recv_framed(sp.b);
    CHECK(!received.has_value());
}

TEST_CASE(disconnect_mid_payload_is_reported_not_hung_or_crashed) {
    SocketPair sp;
    set_recv_timeout(sp.b, 2);

    unsigned char header[8] = {0, 0, 0, 0, 0, 0, 0, 100}; // claims 100 bytes
    ::send(sp.a, header, sizeof(header), 0);
    ::send(sp.a, "short", 5, 0); // only sends 5 of the promised 100
    ::close(sp.a);
    sp.a = -1; // already closed, don't close it again in the destructor

    auto received = recv_framed(sp.b);
    CHECK(!received.has_value());
}

TEST_CASE(recv_times_out_on_a_silent_peer_instead_of_blocking_forever) {
    SocketPair sp;
    set_recv_timeout(sp.b, 1); // 1 second, so the test doesn't take long

    auto start = std::chrono::steady_clock::now();
    auto received = recv_framed(sp.b);
    auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(!received.has_value());
    CHECK(elapsed < std::chrono::seconds(5)); // generous upper bound, just proving it didn't hang
}
