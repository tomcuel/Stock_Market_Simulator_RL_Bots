//=======================================================================
// MarketGateway over TCP: lets any strategy from bot.hpp trade on a sim_server.x through the text protocol, exactly like a human typing commands into sim_client.x
//   symbols()   -> SYMBOLS
//   quote()     -> MARKET <symbol>          (last, bid, ask)
//   portfolio() -> PORTFOLIO                (cash, reserved_cash, holdings)
//   submit()    -> ORDER <side> <symbol> <qty> <kind> [PRICE=..] [TRIGGER_LOWER=..] ...
//   cancel()    -> CANCEL <order_id> <symbol>
// The client must already be authenticated on `connection` (REGISTER, LOGIN or RESUME)
//=======================================================================
#ifndef NET_WIRE_GATEWAY_HPP
#define NET_WIRE_GATEWAY_HPP

#include <ostream>
#include <string>

#include "gateway.hpp"
#include "net/client_connection.hpp"

namespace sim::net {

class WireGateway : public MarketGateway {
public:
    WireGateway(ClientConnection& connection, ClientId client) : connection_(connection), client_(client) {}

    ClientId client() const override {return client_;}
    std::vector<Symbol> symbols() override;
    Quote quote(const Symbol& symbol) override;
    Portfolio portfolio() override;
    SubmitResult submit(OrderRequest request) override;
    bool cancel(const Symbol& symbol, OrderId order_id) override;

    // the command line submit() sends for `request` (exposed for tests and logging)
    static std::string order_command(const OrderRequest& request);
    // the last command/response pair
    const std::string& last_command() const {return last_command_;}
    const std::string& last_response() const {return last_response_;}
    // false once the connection dropped: bots stop their loop on it
    bool connected() const {return connected_;}
    // prints every ORDER / CANCEL sent and its answer to `out` (nullptr = silent)
    void set_echo(std::ostream* out) {echo_ = out;}

private:
    std::string send(const std::string& command);

    ClientConnection& connection_;
    ClientId client_;
    std::string last_command_;
    std::string last_response_;
    bool connected_{true};
    std::ostream* echo_{nullptr};
};

} // namespace sim::net

#endif // NET_WIRE_GATEWAY_HPP
