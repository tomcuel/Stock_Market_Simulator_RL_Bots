#include "policy_client.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace sim {

namespace {

// raw text of `key=` in an answer, empty if absent
std::string text_field(const std::string& text, const std::string& key) {
    std::string needle = " " + key + "=";
    auto pos = text.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    auto end = text.find(' ', pos);
    return text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

std::optional<double> number_field(const std::string& text, const std::string& key) {
    std::string value = text_field(text, key);
    if (value.empty()) return std::nullopt;
    try {
        return std::stod(value);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

bool PolicyClient::connect(const std::string& host, int port) {
    std::lock_guard<std::mutex> lock(mutex_);
    return connection_.connect(host, port);
}

std::optional<std::string> PolicyClient::ask(const std::string& command) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connection_.is_connected()) return std::nullopt;
    return connection_.send_command(command);
}

std::vector<std::string> PolicyClient::models() {
    std::vector<std::string> result;
    auto response = ask("MODELS"); // "OK MODELS rl_f4 rl_f5"
    if (!response || response->rfind("OK MODELS", 0) != 0) return result;
    std::istringstream iss(response->substr(9));
    std::string name;
    while (iss >> name) result.push_back(name);
    return result;
}

std::optional<PolicyInfo> PolicyClient::info(const std::string& model) {
    auto response = ask("INFO " + model); // "OK INFO model=rl_f5 agent=ppo lookback=21 train_seeds=301,302"
    if (!response || response->rfind("OK INFO", 0) != 0) return std::nullopt;
    PolicyInfo info;
    info.name = model;
    info.agent = text_field(*response, "agent");
    info.lookback = static_cast<int>(number_field(*response, "lookback").value_or(0));
    std::istringstream seeds(text_field(*response, "train_seeds")); // comma separated
    for (std::string seed; std::getline(seeds, seed, ',');) {
        try {
            info.train_seeds.push_back(std::stoll(seed));
        } catch (...) {
        }
    }
    if (info.lookback <= 0 || info.agent.empty()) return std::nullopt;
    return info;
}

bool PolicyClient::observe(const Symbol& symbol, int day, const ObservedBar& bar) {
    std::ostringstream oss;
    oss << std::setprecision(12) << "OBSERVE " << symbol << ' ' << day << ' ' << bar.close << ' ' << bar.bid << ' ' << bar.ask << ' '
        << bar.bid_depth << ' ' << bar.ask_depth << ' ' << bar.volume << ' ' << bar.open << ' ' << bar.high << ' ' << bar.low << ' ' << bar.trades;
    auto response = ask(oss.str());
    return response && response->rfind("OK OBSERVED", 0) == 0;
}

PolicyAction PolicyClient::parse_action(const std::string& response) {
    PolicyAction a;
    if (response.rfind("ERR NOT_ENOUGH_DATA", 0) == 0) {
        a.not_ready = true;
        a.error = response;
        return a;
    }
    if (response.rfind("OK ACTION", 0) != 0) {
        a.error = response;
        return a;
    }
    a.model = text_field(response, "model");
    auto position = number_field(response, "position");
    a.position = std::clamp(static_cast<int>(position.value_or(0.0)), -1, 1);
    a.sl_atr = number_field(response, "sl_atr").value_or(0.0);
    a.tp_r = number_field(response, "tp_r").value_or(0.0);
    a.atr = number_field(response, "atr").value_or(0.0);
    a.ok = position.has_value();
    if (!a.ok) a.error = "unrecognized action: " + response;
    return a;
}

PolicyAction PolicyClient::act(const std::string& model, const Symbol& symbol, const std::string& state) {
    auto started = std::chrono::steady_clock::now();
    auto response = ask("ACT " + model + " " + symbol + (state.empty() ? "" : " " + state));
    double latency = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    PolicyAction a;
    if (!response) {
        a.error = "policy server unreachable";
    } else {
        a = parse_action(*response);
    }
    a.latency_ms = latency;
    return a;
}

} // namespace sim
