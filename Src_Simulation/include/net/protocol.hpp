//=======================================================================
// Length-prefixed ("framed") socket I/O: applies the lesson from Test_Functionnalities/Sockets/Socket_Size directly:
// a naive delimiter-terminated read (scan for a marker string) either times out or misreads on large/odd payloads, and offers no defense against a peer that never sends the delimiter
// Every message here is instead sent as an 8-byte big-endian length header followed by exactly that many payload bytes, so the receiver always knows exactly how much to read and can bound its allocation up front
//
// This is transport plumbing only: it has no idea what a "command" or "order" is, and is used identically by the server and every client (interactive, scripted, or bot)
//=======================================================================
#ifndef NET_PROTOCOL_HPP
#define NET_PROTOCOL_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace sim::net {

// Hard cap on a single message's payload size. Bounds the allocation recv_framed() will ever make for a single message, regardless of what a header claims
// a peer sending a bogus multi-gigabyte length prefix gets rejected outright instead of causing an out-of-memory crash
constexpr std::uint64_t kMaxMessageBytes = 1u << 20; // 1 MiB: comfortably more than any command/response needs

// How long a single recv() call is allowed to block before giving up
// Prevents a slow/stalled peer from wedging a server thread (or a client) forever
constexpr int kRecvTimeoutSeconds = 30;

// Applies SO_RCVTIMEO to `socket_fd` so subsequent recv() calls on it respect kRecvTimeoutSeconds instead of blocking indefinitely
void set_recv_timeout(int socket_fd, int timeout_seconds = kRecvTimeoutSeconds);

// Makes writes to this socket fail with EPIPE instead of raising SIGPIPE (which would terminate the whole process) when the peer has disconnected
void disable_sigpipe(int socket_fd);

// Writes `payload` as one framed message
// Returns false on any socket error (including a partial write it couldn't recover from): callers should treat that as "the connection is dead"
bool send_framed(int socket_fd, const std::string& payload);

// Reads one framed message. Returns std::nullopt if the connection closed, timed out, or sent a header claiming more than kMaxMessageBytes (treated as a protocol violation, not silently truncated)
// Loops internally to handle partial reads: TCP makes no promise that one send() on the other end arrives as one recv() on this end
std::optional<std::string> recv_framed(int socket_fd);

} // namespace sim::net

#endif // NET_PROTOCOL_HPP