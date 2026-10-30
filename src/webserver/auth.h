// Authentication for the dashboard (Phase 9): PBKDF2-HMAC-SHA256 password hashes, random
// session tokens with idle and absolute timeouts, and login rate limiting.
// SHA-256/HMAC/PBKDF2 are implemented here (no crypto library on the board) and checked against
// Python's hashlib in tests/test_auth.cc.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace aid {

std::vector<uint8_t> sha256(const uint8_t* data, size_t len);
std::string toHex(const std::vector<uint8_t>& v);
bool fromHex(const std::string& s, std::vector<uint8_t>* out);
std::vector<uint8_t> hmacSha256(const std::vector<uint8_t>& key, const uint8_t* msg, size_t len);
std::vector<uint8_t> pbkdf2Sha256(const std::string& password, const std::vector<uint8_t>& salt,
                                  int iterations, size_t dkLen);

// "pbkdf2-sha256$<iterations>$<salt hex>$<hash hex>"; empty string if no random source.
std::string hashPassword(const std::string& password, int iterations = 60000);
bool verifyPassword(const std::string& password, const std::string& stored);
// Random bytes from /dev/urandom as hex; empty string on failure.
std::string randomHex(size_t bytes);

class AuthManager {
public:
    AuthManager(const std::string& authFile, int idleMinutes, int maxHours);

    // Reads the auth file ({"user":"..","hash":".."}). Returns false if it does not exist.
    bool load(std::string* err);
    bool hasPassword() const;
    std::string username() const;

    // Policy: 8..128 printable characters. Writes the file atomically with mode 0600.
    bool setPassword(const std::string& user, const std::string& password, std::string* err);

    // Returns a session token, or "" on failure. *retryAfterS is set when the client is locked out.
    // clientKey identifies the caller for rate limiting (the peer IP).
    std::string login(const std::string& user, const std::string& password,
                      const std::string& clientKey, int64_t nowMs, std::string* err,
                      int* retryAfterS);
    bool validate(const std::string& token, int64_t nowMs);  // also refreshes the idle timer
    void logout(const std::string& token);
    size_t sessionCount() const;
    // True if `password` is the current password (for "change password"; no session, no rate limit).
    bool verifyCurrent(const std::string& password) const;

private:
    struct Session { int64_t createdMs, lastMs; };
    struct Attempts { int failures = 0; int64_t windowStartMs = 0; int64_t lockedUntilMs = 0; };

    void expire(int64_t nowMs);  // caller holds mutex_

    std::string file_;
    int64_t idleMs_, maxMs_;
    mutable std::mutex mutex_;
    std::string user_, hash_;
    std::map<std::string, Session> sessions_;
    std::map<std::string, Attempts> attempts_;
};

}  // namespace aid
