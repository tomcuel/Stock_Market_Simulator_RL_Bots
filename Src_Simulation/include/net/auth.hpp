//=======================================================================
// Client registration, authentication, and session tokens:
// Passwords are never stored (or logged, or sent back) in a recoverable form: 
// - each account stores a random salt and SHA-256(salt + password) 
// - authentication re-hashes the supplied password with the stored salt and compares digests
// (AES-256 encryption of the password, which is reversible given the key): a one-way salted hash means even a full server-database compromise doesn't hand over the original passwords, 
// (which is the standard practice for credential storage (reversible encryption is for data you need back, you don't need the original password back))
//
// Session tokens let a client reconnect (RESUME <token>) without re-sending its password over the wire every time a fresh random token is issued on every successful REGISTER/LOGIN and stays valid until the client (or an administrator) invalidates it, independent of any one TCP connection's lifetime
//=======================================================================
#ifndef NET_AUTH_HPP
#define NET_AUTH_HPP

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "types.hpp"

namespace sim::net {

// A persistable, non-secret-adjacent view of one account: the salt and hash are already one-way,
// so persisting them is exactly as safe as persisting nothing at all about the password itself
struct AccountRecord {
    std::string username;
    ClientId client_id;
    std::string salt_hex;
    std::string password_hash_hex;
};

class ClientDirectory {
public:
    enum class RegisterOutcome { OK, USERNAME_TAKEN };
    enum class AuthOutcome { OK, UNKNOWN_USERNAME, WRONG_PASSWORD };

    // Creates a new account and issues it a fresh session token. Thread-safe
    RegisterOutcome register_client(const std::string& username, const std::string& password, ClientId& out_client_id, std::string& out_token);

    // Verifies credentials for an existing account and issues it a fresh session token (Thread-safe)
    AuthOutcome authenticate(const std::string& username, const std::string& password, ClientId& out_client_id, std::string& out_token);

    // Resolves a previously issued session token back to its client id, without needing the password again 
    // Returns std::nullopt if the token is unknown/was never issued
    std::optional<ClientId> resolve_token(const std::string& token) const;

    std::size_t client_count() const;

    // loading an already-hashed record, not registering a new password: next_client_id_ is advanced past the highest imported id so freshly registered accounts never collide with a restored one
    std::vector<AccountRecord> export_accounts() const;
    void import_account(const AccountRecord& record);

private:
    struct Account {
        ClientId client_id;
        std::string salt_hex;
        std::string password_hash_hex; // SHA-256(salt_hex + password), hex-encoded
    };

    static std::string generate_salt();
    static std::string generate_token();
    static std::string hash_password(const std::string& salt_hex, const std::string& password);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Account> accounts_; // keyed by username
    std::unordered_map<std::string, ClientId> tokens_;  // keyed by session token
    ClientId next_client_id_{1};
};

} // namespace sim::net

#endif // NET_AUTH_HPP