#include "net/protocol.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace sim::net {

namespace {

// reads exactly `count` bytes into `buffer` (already sized), looping over recv() as needed
// Returns false on error, timeout, or a peer closing the connection before `count` bytes arrived
bool recv_exact(int socket_fd, char* buffer, std::size_t count) {
    std::size_t received = 0;
    while (received < count) {
        ssize_t n = ::recv(socket_fd, buffer + received, count - received, 0);
        if (n == 0) {
            return false; // peer closed the connection
        }
        if (n < 0) {
            return false; // error or timeout (EAGAIN/EWOULDBLOCK from SO_RCVTIMEO)
        }
        received += static_cast<std::size_t>(n);
    }
    return true;
}

// Writing to a socket whose peer has already gone away raises SIGPIPE, and SIGPIPE's default action terminates the whole process, silently
// For the server that meant one client disappearing mid-reply (a crash, Ctrl-C, a dropped connection) killed every other client's session and lost the end-of-run report found when a launcher stopped its bots before the server (exit status 141 = 128 + SIGPIPE)
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

bool send_exact(int socket_fd, const char* buffer, std::size_t count) {
    std::size_t sent = 0;
    while (sent < count) {
        ssize_t n = ::send(socket_fd, buffer + sent, count - sent, kSendFlags);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace

void disable_sigpipe(int socket_fd) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    ::setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)socket_fd; // Linux: handled per send() with MSG_NOSIGNAL
#endif
}

void set_recv_timeout(int socket_fd, int timeout_seconds) {
    struct timeval tv{};
    tv.tv_sec = timeout_seconds;
    tv.tv_usec = 0;
    ::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

bool send_framed(int socket_fd, const std::string& payload) {
    std::uint64_t length = payload.size();
    // encode as 8 raw bytes, most-significant byte first (network/big-endian byte order), written out by hand rather than composed from htonl() on two 32-bit halves
    unsigned char header[8];
    for (int i = 0; i < 8; ++i) {
        header[i] = static_cast<unsigned char>((length >> (8 * (7 - i))) & 0xFFu);
    }

    if (!send_exact(socket_fd, reinterpret_cast<const char*>(header), sizeof(header))) {
        return false;
    }
    if (!payload.empty() && !send_exact(socket_fd, payload.data(), payload.size())) {
        return false;
    }
    return true;
}

std::optional<std::string> recv_framed(int socket_fd) {
    unsigned char header[8];
    if (!recv_exact(socket_fd, reinterpret_cast<char*>(header), sizeof(header))) {
        return std::nullopt;
    }
    std::uint64_t length = 0;
    for (int i = 0; i < 8; ++i) {
        length = (length << 8) | header[i];
    }

    if (length > kMaxMessageBytes) {
        return std::nullopt; // protocol violation: refuse to allocate for an oversized claim
    }

    std::string payload(length, '\0');
    if (length > 0 && !recv_exact(socket_fd, payload.data(), length)) {
        return std::nullopt;
    }
    return payload;
}

} // namespace sim::net
