//=======================================================================
// A thin client-side connection wrapper: connect, send one command, get one response
// Used identically by sim_client_main.cpp's interactive/piped mode and its --bot mode, 
// so a bot's every action is provably going through the exact same code path (and therefore the exact same wire protocol) a human typing commands would use
// there is no separate in-process shortcut a bot could take instead
//=======================================================================
#ifndef NET_CLIENT_CONNECTION_HPP
#define NET_CLIENT_CONNECTION_HPP

#include <optional>
#include <string>

namespace sim::net {

class ClientConnection {
public:
    ~ClientConnection();

    bool connect(const std::string& host, int port);
    void disconnect();
    bool is_connected() const { return socket_fd_ >= 0; }

    // Sends `command` and waits for the single-line response. Returns std::nullopt if the connection drops or times out.
    std::optional<std::string> send_command(const std::string& command);

private:
    int socket_fd_{-1};
};

} // namespace sim::net

#endif // NET_CLIENT_CONNECTION_HPP