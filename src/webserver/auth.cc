#include "auth.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "json_util.h"

namespace aid {

namespace {

const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256 {
    uint32_t h[8];
    uint8_t buf[64];
    size_t buflen;
    uint64_t total;

    Sha256() { reset(); }
    void reset() {
        static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        memcpy(h, init, sizeof(h));
        buflen = 0;
        total = 0;
    }
    void block(const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
                   ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const uint8_t* data, size_t len) {
        total += len;
        while (len > 0) {
            const size_t n = std::min(len, 64 - buflen);
            memcpy(buf + buflen, data, n);
            buflen += n; data += n; len -= n;
            if (buflen == 64) { block(buf); buflen = 0; }
        }
    }
    void finish(uint8_t out[32]) {
        const uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (buflen != 56) update(&zero, 1);
        uint8_t lenbe[8];
        for (int i = 0; i < 8; ++i) lenbe[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(lenbe, 8);
        for (int i = 0; i < 8; ++i) {
            out[i * 4] = (uint8_t)(h[i] >> 24);
            out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
            out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
            out[i * 4 + 3] = (uint8_t)h[i];
        }
    }
};

bool ctEqual(const std::string& a, const std::string& b) {
    unsigned char diff = (unsigned char)(a.size() != b.size());
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

}  // namespace

std::vector<uint8_t> sha256(const uint8_t* data, size_t len) {
    Sha256 s;
    s.update(data, len);
    std::vector<uint8_t> out(32);
    s.finish(out.data());
    return out;
}

std::string toHex(const std::vector<uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string o;
    o.reserve(v.size() * 2);
    for (uint8_t b : v) { o += d[b >> 4]; o += d[b & 15]; }
    return o;
}

bool fromHex(const std::string& s, std::vector<uint8_t>* out) {
    if (s.size() % 2) return false;
    out->clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        int v[2];
        for (int j = 0; j < 2; ++j) {
            const char c = s[i + j];
            if (c >= '0' && c <= '9') v[j] = c - '0';
            else if (c >= 'a' && c <= 'f') v[j] = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v[j] = c - 'A' + 10;
            else return false;
        }
        out->push_back((uint8_t)(v[0] * 16 + v[1]));
    }
    return true;
}

std::vector<uint8_t> hmacSha256(const std::vector<uint8_t>& key, const uint8_t* msg, size_t len) {
    std::vector<uint8_t> k = key;
    if (k.size() > 64) k = sha256(k.data(), k.size());
    k.resize(64, 0);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    Sha256 in;
    in.update(ipad, 64);
    in.update(msg, len);
    uint8_t ih[32];
    in.finish(ih);
    Sha256 out;
    out.update(opad, 64);
    out.update(ih, 32);
    std::vector<uint8_t> r(32);
    out.finish(r.data());
    return r;
}

std::vector<uint8_t> pbkdf2Sha256(const std::string& password, const std::vector<uint8_t>& salt,
                                  int iterations, size_t dkLen) {
    const std::vector<uint8_t> key(password.begin(), password.end());
    std::vector<uint8_t> out;
    for (uint32_t block = 1; out.size() < dkLen; ++block) {
        std::vector<uint8_t> s = salt;
        s.push_back((uint8_t)(block >> 24)); s.push_back((uint8_t)(block >> 16));
        s.push_back((uint8_t)(block >> 8));  s.push_back((uint8_t)block);
        std::vector<uint8_t> u = hmacSha256(key, s.data(), s.size());
        std::vector<uint8_t> t = u;
        for (int i = 1; i < iterations; ++i) {
            u = hmacSha256(key, u.data(), u.size());
            for (size_t j = 0; j < t.size(); ++j) t[j] ^= u[j];
        }
        out.insert(out.end(), t.begin(), t.end());
    }
    out.resize(dkLen);
    return out;
}

std::string randomHex(size_t bytes) {
    std::vector<uint8_t> v(bytes);
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return "";
    const bool ok = fread(v.data(), 1, bytes, f) == bytes;
    fclose(f);
    return ok ? toHex(v) : "";
}

std::string hashPassword(const std::string& password, int iterations) {
    std::vector<uint8_t> salt;
    if (!fromHex(randomHex(16), &salt) || salt.size() != 16) return "";
    const std::vector<uint8_t> h = pbkdf2Sha256(password, salt, iterations, 32);
    return "pbkdf2-sha256$" + std::to_string(iterations) + "$" + toHex(salt) + "$" + toHex(h);
}

bool verifyPassword(const std::string& password, const std::string& stored) {
    const std::string prefix = "pbkdf2-sha256$";
    if (stored.compare(0, prefix.size(), prefix) != 0) return false;
    const size_t a = stored.find('$', prefix.size());
    if (a == std::string::npos) return false;
    const size_t b = stored.find('$', a + 1);
    if (b == std::string::npos) return false;
    const int iters = atoi(stored.substr(prefix.size(), a - prefix.size()).c_str());
    if (iters < 1 || iters > 2000000) return false;
    std::vector<uint8_t> salt, want;
    if (!fromHex(stored.substr(a + 1, b - a - 1), &salt) || !fromHex(stored.substr(b + 1), &want)) return false;
    if (want.size() != 32 || salt.empty()) return false;
    const std::vector<uint8_t> got = pbkdf2Sha256(password, salt, iters, 32);
    return ctEqual(toHex(got), toHex(want));
}

// ------------------------------------------------------------------------------------------
AuthManager::AuthManager(const std::string& authFile, int idleMinutes, int maxHours)
    : file_(authFile), idleMs_((int64_t)idleMinutes * 60000), maxMs_((int64_t)maxHours * 3600000) {}

bool AuthManager::load(std::string* err) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream f(file_.c_str());
    if (!f) {
        if (err) *err = "no auth file";
        return false;
    }
    std::ostringstream all;
    all << f.rdbuf();
    std::string u, h;
    if (!jsonGetFlat(all.str(), "user", &u) || !jsonGetFlat(all.str(), "hash", &h) || u.empty() || h.empty()) {
        if (err) *err = "auth file is invalid";
        return false;
    }
    user_ = u;
    hash_ = h;
    return true;
}

bool AuthManager::hasPassword() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !hash_.empty();
}

std::string AuthManager::username() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return user_;
}

bool AuthManager::setPassword(const std::string& user, const std::string& password, std::string* err) {
    if (user.empty() || user.size() > 32) { if (err) *err = "user name must be 1..32 characters"; return false; }
    for (char c : user)
        if (c <= ' ' || c > '~' || c == '"' || c == '\\') { if (err) *err = "user name has invalid characters"; return false; }
    if (password.size() < 8 || password.size() > 128) { if (err) *err = "password must be 8..128 characters"; return false; }
    for (char c : password)
        if (c < ' ' || c > '~') { if (err) *err = "password must be printable ASCII"; return false; }
    const std::string hash = hashPassword(password);
    if (hash.empty()) { if (err) *err = "no random source available"; return false; }

    const std::string tmp = file_ + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) { if (err) *err = "cannot write " + tmp; return false; }
    chmod(tmp.c_str(), 0600);
    const std::string body = "{\"user\":\"" + jsonEscape(user) + "\",\"hash\":\"" + hash + "\"}\n";
    const bool ok = fwrite(body.data(), 1, body.size(), f) == body.size();
    const bool closed = fclose(f) == 0;
    if (!ok || !closed || rename(tmp.c_str(), file_.c_str()) != 0) {
        unlink(tmp.c_str());
        if (err) *err = "could not store the password file";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    user_ = user;
    hash_ = hash;
    sessions_.clear();  // a password change ends all sessions
    return true;
}

void AuthManager::expire(int64_t nowMs) {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (nowMs - it->second.lastMs > idleMs_ || nowMs - it->second.createdMs > maxMs_) it = sessions_.erase(it);
        else ++it;
    }
    for (auto it = attempts_.begin(); it != attempts_.end();) {
        if (it->second.lockedUntilMs <= nowMs && nowMs - it->second.windowStartMs > 300000) it = attempts_.erase(it);
        else ++it;
    }
}

std::string AuthManager::login(const std::string& user, const std::string& password,
                               const std::string& clientKey, int64_t nowMs, std::string* err,
                               int* retryAfterS) {
    std::string hash, expectedUser;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        expire(nowMs);
        if (hash_.empty()) { if (err) *err = "no password is set"; return ""; }
        Attempts& at = attempts_[clientKey];
        if (at.lockedUntilMs > nowMs) {
            if (retryAfterS) *retryAfterS = (int)((at.lockedUntilMs - nowMs + 999) / 1000);
            if (err) *err = "too many attempts";
            return "";
        }
        if (attempts_.size() > 64) attempts_.erase(attempts_.begin());
        hash = hash_;
        expectedUser = user_;
    }
    // The hash is computed outside the lock (it takes a moment on this CPU).
    const bool passOk = verifyPassword(password, hash);
    const bool userOk = ctEqual(user, expectedUser);
    std::lock_guard<std::mutex> lock(mutex_);
    Attempts& at = attempts_[clientKey];
    if (!(passOk && userOk)) {
        if (nowMs - at.windowStartMs > 300000) { at.failures = 0; at.windowStartMs = nowMs; }
        if (at.failures == 0) at.windowStartMs = nowMs;
        if (++at.failures >= 5) {
            at.lockedUntilMs = nowMs + 60000;
            at.failures = 0;
            if (retryAfterS) *retryAfterS = 60;
        }
        if (err) *err = "wrong user name or password";
        return "";
    }
    at.failures = 0;
    const std::string token = randomHex(32);
    if (token.empty()) { if (err) *err = "no random source available"; return ""; }
    if (sessions_.size() >= 16) {
        auto oldest = sessions_.begin();
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it)
            if (it->second.lastMs < oldest->second.lastMs) oldest = it;
        sessions_.erase(oldest);
    }
    sessions_[token] = Session{nowMs, nowMs};
    return token;
}

bool AuthManager::validate(const std::string& token, int64_t nowMs) {
    if (token.size() != 64) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    expire(nowMs);
    auto it = sessions_.find(token);
    if (it == sessions_.end()) return false;
    it->second.lastMs = nowMs;
    return true;
}

void AuthManager::logout(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(token);
}

bool AuthManager::verifyCurrent(const std::string& password) const {
    std::string hash;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hash = hash_;
    }
    return !hash.empty() && verifyPassword(password, hash);
}

size_t AuthManager::sessionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

}  // namespace aid
