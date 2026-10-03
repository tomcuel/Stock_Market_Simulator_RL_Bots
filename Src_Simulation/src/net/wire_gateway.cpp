#include "net/wire_gateway.hpp"

#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>

namespace sim::net {

namespace {

// value of `key=` in a response line like "OK MARKET AAPL last=150.2 bid=149.9", if present
std::optional<std::string> field(const std::string& response, const std::string& key) {
    std::string needle = " " + key + "=";
    auto pos = response.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos += needle.size();
    auto end = response.find(' ', pos);
    return response.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

double number(const std::string& response, const std::string& key, double fallback = 0.0) {
    auto value = field(response, key);
    if (!value) {
        return fallback;
    }
    try {
        return std::stod(*value);
    }
    catch (...){
        return fallback;
    }
}

} // namespace

std::string WireGateway::send(const std::string& command) {
    last_command_ = command;
    auto response = connection_.send_command(command);
    if (!response){
        connected_ = false;
        last_response_ = "ERR DISCONNECTED";
    }
    else {
        last_response_ = *response;
    }
    if (echo_ && (command.rfind("ORDER", 0) == 0 || command.rfind("CANCEL", 0) == 0)){
        *echo_ << command << "  ->  " << last_response_ << '\n'; // trading actions only, not the market queries
    }
    return last_response_;
}

std::vector<Symbol> WireGateway::symbols() {
    std::string response = send("SYMBOLS"); // "OK SYMBOLS AAPL MSFT ..."
    std::vector<Symbol> result;
    if (response.rfind("OK SYMBOLS", 0) != 0) {
        return result;
    }
    std::istringstream iss(response.substr(10));
    std::string symbol;
    while (iss >> symbol) {
        result.push_back(symbol);
    }
    return result;
}

Quote WireGateway::quote(const Symbol& symbol) {
    std::string response = send("MARKET " + symbol);
    Quote quote;
    if (response.rfind("OK", 0) != 0) {
        return quote;
    }
    quote.last = number(response, "last");
    quote.best_bid = number(response, "bid");
    quote.best_ask = number(response, "ask");
    return quote;
}

Portfolio WireGateway::portfolio() {
    // "OK PORTFOLIO cash=... reserved_cash=... available_cash=... AAPL=50 MSFT=12"
    std::string response = send("PORTFOLIO");
    Portfolio portfolio;
    if (response.rfind("OK PORTFOLIO", 0) != 0) {
        return portfolio;
    }
    std::istringstream iss(response.substr(12));
    std::string token;
    while (iss >> token){
        auto eq = token.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = token.substr(0, eq);
        std::string value = token.substr(eq + 1);
        try {
            if (key == "cash") {
                portfolio.cash = std::stod(value);
            }
            else if (key == "reserved_cash") {
                portfolio.reserved_cash = std::stod(value);
            }
            else if (key == "available_cash") {
                continue; // derived: cash - reserved_cash
            }
            else {
                portfolio.holdings[key] = std::stoll(value);
            }
        }
        catch (...){
            // an unexpected token is ignored rather than breaking the whole portfolio
        }
    }
    return portfolio;
}

std::string WireGateway::order_command(const OrderRequest& request) {
    std::ostringstream oss;
    oss << std::setprecision(15) << "ORDER " << to_string(request.side) << ' ' << request.symbol << ' ' << request.quantity << ' '
        << to_string(request.kind);
    if (request.kind != OrderKind::MARKET) {
        oss << " PRICE=" << request.price;
    }
    if (request.trigger_lower) {
        oss << " TRIGGER_LOWER=" << *request.trigger_lower;
    }
    if (request.trigger_upper) {
        oss << " TRIGGER_UPPER=" << *request.trigger_upper;
    }
    if (request.expires_in) {
        oss << " EXPIRES=" << request.expires_in->count();
    }
    if (request.time_in_force) {
        oss << " TIF=" << to_string(*request.time_in_force);
    }
    if (request.not_before_in) {
        oss << " NOT_BEFORE=" << request.not_before_in->count();
    }
    return oss.str();
}

SubmitResult WireGateway::submit(OrderRequest request) {
    request.client = client_;
    // "OK ORDER order_id=12 status=FILLED filled=5 trades=1", "status=RESTING", "status=QUEUED", "status=NO_LIQUIDITY", or "ERR ORDER order_id=12 reason=insufficient cash"
    std::string response = send(order_command(request));
    SubmitResult result;
    result.order_id = static_cast<OrderId>(number(response, "order_id"));
    if (response.rfind("OK ORDER", 0) == 0){
        result.accepted = true;
        auto status = field(response, "status");
        result.queued = status && *status == "QUEUED";
        result.filled_quantity = static_cast<Quantity>(number(response, "filled"));
    }
    else {
        auto reason_pos = response.find("reason=");
        result.reject_reason = (reason_pos == std::string::npos) ? response : response.substr(reason_pos + 7);
    }
    return result;
}

bool WireGateway::cancel(const Symbol& symbol, OrderId order_id) {
    return send("CANCEL " + std::to_string(order_id) + " " + symbol) == "OK CANCELLED";
}

} // namespace sim::net
