#include "net/auth.hpp"

#include <random>
#include <sstream>

#include "net/sha256.hpp"

namespace sim::net {

std::string ClientDirectory::generate_salt() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<std::uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << dist(rng) << dist(rng);
    return oss.str();
}

std::string ClientDirectory::generate_token() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<std::uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << dist(rng) << dist(rng) << dist(rng) << dist(rng);
    return oss.str();
}

std::string ClientDirectory::hash_password(const std::string& salt_hex, const std::string& password) {
    return to_hex(Sha256::hash(salt_hex + password));
}

ClientDirectory::RegisterOutcome ClientDirectory::register_client(const std::string& username, const std::string& password, ClientId& out_client_id, std::string& out_token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (accounts_.count(username) > 0) {
        return RegisterOutcome::USERNAME_TAKEN;
    }

    Account account;
    account.client_id = next_client_id_++;
    account.salt_hex = generate_salt();
    account.password_hash_hex = hash_password(account.salt_hex, password);

    out_client_id = account.client_id;
    out_token = generate_token();
    tokens_.emplace(out_token, account.client_id);
    accounts_.emplace(username, std::move(account));
    return RegisterOutcome::OK;
}

ClientDirectory::AuthOutcome ClientDirectory::authenticate(const std::string& username, const std::string& password, ClientId& out_client_id, std::string& out_token) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = accounts_.find(username);
    if (it == accounts_.end()) {
        return AuthOutcome::UNKNOWN_USERNAME;
    }

    const Account& account = it->second;
    if (hash_password(account.salt_hex, password) != account.password_hash_hex) {
        return AuthOutcome::WRONG_PASSWORD;
    }

    out_client_id = account.client_id;
    out_token = generate_token();
    tokens_.emplace(out_token, account.client_id);
    return AuthOutcome::OK;
}

std::optional<ClientId> ClientDirectory::resolve_token(const std::string& token) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tokens_.find(token);
    if (it == tokens_.end()) return std::nullopt;
    return it->second;
}

std::size_t ClientDirectory::client_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return accounts_.size();
}

std::vector<AccountRecord> ClientDirectory::export_accounts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AccountRecord> records;
    records.reserve(accounts_.size());
    for (const auto& [username, account] : accounts_) {
        records.push_back(AccountRecord{username, account.client_id, account.salt_hex, account.password_hash_hex});
    }
    return records;
}

void ClientDirectory::import_account(const AccountRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    Account account;
    account.client_id = record.client_id;
    account.salt_hex = record.salt_hex;
    account.password_hash_hex = record.password_hash_hex;
    accounts_[record.username] = std::move(account);
    if (record.client_id >= next_client_id_) {
        next_client_id_ = record.client_id + 1;
    }
}

} // namespace sim::net
