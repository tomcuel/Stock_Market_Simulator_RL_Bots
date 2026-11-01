#include "net/persistence.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "logger.hpp"
#include "paths.hpp"

namespace sim::net {

bool PersistenceStore::save(const std::string& path, const ClientDirectory& directory, const MatchingEngine& engine) {
    std::string tmp_path = path + ".tmp";
    std::ofstream out(tmp_path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        LOG_ERROR("persistence: failed to open ", tmp_path, " for writing");
        return false;
    }
    // std::ostream's default precision is only 6 significant digits: nowhere near enough to round-trip a double exactly (e.g. cash=12345.67 would be truncated to 12345.7 on save)
    // 17 significant decimal digits is the standard figure that's always sufficient to reproduce any IEEE-754 double exactly when parsed back
    out << std::setprecision(17);

    for (const auto& account : directory.export_accounts()) {
        out << "ACCOUNT " << account.username << " " << account.client_id << " " << account.salt_hex << " " << account.password_hash_hex << "\n";
    }

    for (const auto& [client_id, portfolio] : engine.all_portfolios()) {
        out << "CASH " << client_id << " " << portfolio.cash << "\n";
        for (const auto& [symbol, quantity] : portfolio.holdings) {
            if (quantity != 0) {
                out << "HOLDING " << client_id << " " << symbol << " " << quantity << "\n";
            }
        }
    }

    for (const auto& [symbol, price] : engine.all_last_prices()) {
        out << "PRICE " << symbol << " " << price << "\n";
    }
    // open orders that live on after the close (GTC, or with their own expiry date): where they were, and how much trading
    // time they have left, measured from the close when the market is closed (order dates run on a clock local to this
    // process, so the next session counts from its own opening)
    TimePoint now = engine.trading_clock();
    auto remaining_ms = [now](const std::optional<TimePoint>& at) -> long long {
        return at ? std::chrono::duration_cast<std::chrono::milliseconds>(*at - now).count() : -1;
    };
    for (const auto& open : engine.open_orders()) {
        const Order& o = open.order;
        out << "ORDER " << (open.waiting ? "WAIT" : "BOOK") << " " << o.id << " " << o.client << " " << o.symbol << " " << to_string(o.side) << " "
            << to_string(o.kind) << " " << to_string(o.time_in_force) << " " << o.quantity << " " << o.price << " " << o.release_lower << " "
            << o.release_upper << " " << remaining_ms(o.expires_at) << " " << std::max<long long>(0, remaining_ms(o.not_before)) << "\n";
    }

    out.close();
    if (!out) {
        LOG_ERROR("persistence: error while writing ", tmp_path);
        std::remove(tmp_path.c_str());
        return false;
    }

    // atomic on POSIX filesystems: readers either see the old file or the fully-written new one, never a half-written one
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        LOG_ERROR("persistence: failed to rename ", display_path(tmp_path), " to ", display_path(path));
        std::remove(tmp_path.c_str());
        return false;
    }

    LOG_INFO("persistence: snapshot saved to ", display_path(path));
    return true;
}

bool PersistenceStore::load(const std::string& path, ClientDirectory& directory, MatchingEngine& engine) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return false; // no snapshot yet: normal on first-ever startup, not an error
    }

    // ensure_client() must run before grant_initial_holdings()/CASH for the same client, and a client's exact starting cash (from the snapshot) must be set before any holdings are credited
    // so holdings/prices are buffered and applied in a second pass after every account and cash line has been processed, regardless of the order they appear in the file
    struct PendingHolding { ClientId client_id; Symbol symbol; Quantity quantity; };
    std::vector<PendingHolding> pending_holdings;
    std::unordered_map<Symbol, Price> pending_prices;
    std::unordered_map<ClientId, double> pending_cash;
    std::vector<std::string> pending_orders; // restored last: their reservations need the cash, holdings and prices first

    std::string line;
    std::size_t accounts_loaded = 0;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string tag;
        iss >> tag;

        if (tag == "ACCOUNT") {
            AccountRecord record;
            iss >> record.username >> record.client_id >> record.salt_hex >> record.password_hash_hex;
            directory.import_account(record);
            ++accounts_loaded;
        } 
        else if (tag == "CASH") {
            ClientId client_id;
            double cash;
            iss >> client_id >> cash;
            pending_cash[client_id] = cash;
        } 
        else if (tag == "HOLDING") {
            PendingHolding holding;
            iss >> holding.client_id >> holding.symbol >> holding.quantity;
            pending_holdings.push_back(holding);
        } 
        else if (tag == "PRICE") {
            Symbol symbol;
            Price price;
            iss >> symbol >> price;
            pending_prices[symbol] = price;
        }
        else if (tag == "ORDER") {
            pending_orders.push_back(line);
        }
        // unknown tags are ignored rather than rejecting the whole file, so a snapshot written by a newer version with extra record types still loads under an older binary
    }

    for (const auto& [client_id, cash] : pending_cash) {
        engine.ensure_client(client_id, cash);
    }
    for (const auto& holding : pending_holdings) {
        engine.ensure_client(holding.client_id, 0.0); // no-op if CASH already created it
        engine.grant_initial_holdings(holding.client_id, holding.symbol, holding.quantity);
    }
    for (const auto& [symbol, price] : pending_prices) {
        engine.register_symbol(symbol, price); // safe to call again for an already-registered symbol
    }

    // the orders carried over: their reservations are taken again (reservations are never saved), they don't match yet, the opening auction does (sim_server.x runs it before accepting connections)
    std::size_t restored = 0, refused = 0, expired = 0;
    TimePoint now = Clock::now();
    for (const auto& order_line : pending_orders) {
        std::istringstream fields(order_line);
        std::string tag, where, side, kind, tif, lower, upper;
        long long expires_ms = -1, not_before_ms = 0;
        Order order;
        fields >> tag >> where >> order.id >> order.client >> order.symbol >> side >> kind >> tif >> order.quantity >> order.price >> lower >> upper >> expires_ms >> not_before_ms;
        if (!fields) {
            ++refused;
            continue;
        }
        order.side = side == "SELL" ? Side::SELL : Side::BUY;
        order.kind = kind == "MARKET" ? OrderKind::MARKET : kind == "STOP" ? OrderKind::STOP : kind == "LIMIT_STOP" ? OrderKind::LIMIT_STOP : OrderKind::LIMIT;
        order.time_in_force = tif == "GTC" ? TimeInForce::GTC : TimeInForce::DAY;
        order.release_lower = std::stod(lower); // "inf" / "-inf" for an unbounded band
        order.release_upper = std::stod(upper);
        order.submitted_at = now; // the file order keeps the time priority
        if (expires_ms == 0 || expires_ms < -1) {
            ++expired; // its expiry date had already passed when the snapshot was written
            continue;
        }
        if (expires_ms > 0) {
            order.expires_at = now + std::chrono::milliseconds(expires_ms);
        }
        if (not_before_ms > 0) {
            order.not_before = now + std::chrono::milliseconds(not_before_ms);
        }
        if (engine.restore_order(MatchingEngine::OpenOrder{order, where == "WAIT"})) {
            ++restored;
        } 
        else {
            ++refused;
        }
    }
    if (!pending_orders.empty()) {
        LOG_INFO("persistence: restored ", restored, " open order(s) (", refused, " refused, ", expired, " past their expiry date)");
    }

    LOG_INFO("persistence: restored ", accounts_loaded, " account(s), ", pending_cash.size(), " portfolio(s), ", pending_prices.size(), " symbol price(s) from ", path);
    return true;
}

} // namespace sim::net
