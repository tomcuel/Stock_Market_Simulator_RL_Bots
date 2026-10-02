#include "net/persistence.hpp"

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "logger.hpp"

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

    out.close();
    if (!out) {
        LOG_ERROR("persistence: error while writing ", tmp_path);
        std::remove(tmp_path.c_str());
        return false;
    }

    // atomic on POSIX filesystems: readers either see the old file or the fully-written new one, never a half-written one
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        LOG_ERROR("persistence: failed to rename ", tmp_path, " to ", path);
        std::remove(tmp_path.c_str());
        return false;
    }

    LOG_INFO("persistence: snapshot saved to ", path);
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

    LOG_INFO("persistence: restored ", accounts_loaded, " account(s), ", pending_cash.size(),
             " portfolio(s), ", pending_prices.size(), " symbol price(s) from ", path);
    return true;
}

} // namespace sim::net
