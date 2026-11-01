#include "net/client_connection.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net/protocol.hpp"

namespace sim::net {

ClientConnection::~ClientConnection() {
    disconnect();
}

bool ClientConnection::connect(const std::string& host, int port) {
    socket_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd_ < 0) {
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) <= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    if (::connect(socket_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    set_recv_timeout(socket_fd_, kRecvTimeoutSeconds);
    disable_sigpipe(socket_fd_);
    return true;
}

void ClientConnection::disconnect() {
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
}

std::optional<std::string> ClientConnection::send_command(const std::string& command) {
    if (socket_fd_ < 0) {
        return std::nullopt;
    }
    if (!send_framed(socket_fd_, command)) {
        return std::nullopt;
    }
    return recv_framed(socket_fd_);
}

} // namespace sim::net
