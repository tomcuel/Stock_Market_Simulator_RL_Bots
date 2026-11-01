//=======================================================================
// Client of the Python policy server (RL_Agents/rl_agents/serve.py): the service in front of the trained RL agents
//
// Live data flow (see README.md, "How Python and C++ talk"):
//   at every close a MarketFeed pushes one daily bar per symbol:
//     OBSERVE <symbol> <day> <close> <bid> <ask> <bid_depth> <ask_depth> <volume> <open> <high> <low> <trades>
//   the server keeps a rolling stream per symbol, the agents' observations are built from it
//   in each pre-open an RLBot sends its open trade and gets the next one:
//     ACT <model> <symbol> dir= entry= sl= tp= days= tpr= -> OK ACTION model=rl symbol=AAPL position=d direction=d sl_atr=.. tp_r=.. atr=..
// Keeping the agents in Python means a trained agent becomes a bot without writing C++, and the exchange stays free of ML dependencies: the C++ side only speaks the framed text protocol it already uses for its own clients
//=======================================================================
#ifndef POLICY_CLIENT_HPP
#define POLICY_CLIENT_HPP

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "net/client_connection.hpp"
#include "types.hpp"

namespace sim {

struct PolicyInfo {
    std::string name;
    std::string agent;        // ppo (a trained agent) or rule (a baseline)
    int lookback{0};          // daily bars the agent needs before its first decision (its features' warm-up)
    std::vector<long long> train_seeds; // every simulation seed it learned from (train_seeds=301,302,...)
    bool trained_on_seed(long long seed) const {
        for (long long s : train_seeds) if (s == seed) return true;
        return false;
    }
};

// one day's bar of a symbol, as the policy server's stream holds it
struct ObservedBar {
    double close{0.0};
    double bid{0.0};          // the book when the pre-close started
    double ask{0.0};
    double bid_depth{0.0};    // quantity in the top 10 levels of each side
    double ask_depth{0.0};
    double volume{0.0};
    double open{0.0};
    double high{0.0};
    double low{0.0};
    double trades{0.0};
};

struct PolicyAction {
    bool ok{false};
    bool not_ready{false};    // the stream doesn't hold the agent's lookback yet: wait, not an error
    int position{0};          // -1 short, 0 flat, +1 long
    double sl_atr{0.0};       // the trade's stop distance in ATRs
    double tp_r{0.0};         // its target in multiples of the stop distance
    double atr{0.0};          // the ATR, in price units
    std::string model;
    double latency_ms{0.0};   // request to answer: how old the decision already is when the bot gets it
    std::string error;
};

class PolicyClient {
public:
    bool connect(const std::string& host, int port);
    bool connected() const { return connection_.is_connected(); }

    std::vector<std::string> models();                                          // MODELS
    std::optional<PolicyInfo> info(const std::string& model);                   // INFO <model>
    bool observe(const Symbol& symbol, int day, const ObservedBar& bar);       // OBSERVE
    // ACT <model> <symbol> key=value...: the bot's open trade (dir, entry, sl, tp, days, tpr) in, the next trade out
    PolicyAction act(const std::string& model, const Symbol& symbol, const std::string& state);

    // parses one ACT answer (public so it can be tested without a server)
    static PolicyAction parse_action(const std::string& response);

private:
    std::optional<std::string> ask(const std::string& command);

    net::ClientConnection connection_;
    std::mutex mutex_; // one request at a time on this connection
};

} // namespace sim

#endif // POLICY_CLIENT_HPP
