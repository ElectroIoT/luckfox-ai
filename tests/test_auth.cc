// PC-side tests for the crypto primitives (against Python hashlib/hmac reference values) and
// the login / session / lockout logic. Run: bash scripts/test.sh
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "auth.h"

static int g_fail = 0, g_run = 0;
#define CHECK(cond)                                                   \
    do {                                                              \
        ++g_run;                                                      \
        if (!(cond)) {                                                \
            ++g_fail;                                                 \
            printf("FAIL line %d: %s\n", __LINE__, #cond);            \
        }                                                             \
    } while (0)

static std::string sha(const std::string& s) {
    return aid::toHex(aid::sha256((const uint8_t*)s.data(), s.size()));
}
static std::vector<uint8_t> bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

static void testPrimitives() {
    // Reference values computed with Python hashlib / hmac.
    CHECK(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(sha(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    const std::string fox = "The quick brown fox jumps over the lazy dog";
    CHECK(aid::toHex(aid::hmacSha256(bytes("key"), (const uint8_t*)fox.data(), fox.size())) ==
          "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
    CHECK(aid::toHex(aid::hmacSha256(bytes(std::string(100, 'k')), (const uint8_t*)"msg", 3)) ==
          "bd56a1782c2830e8abc6ed866a57a1230661e650b84c62f7ee3accc5fa5af491");   // key longer than a block

    CHECK(aid::toHex(aid::pbkdf2Sha256("password", bytes("salt"), 1, 32)) ==
          "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    CHECK(aid::toHex(aid::pbkdf2Sha256("password", bytes("salt"), 2, 32)) ==
          "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    CHECK(aid::toHex(aid::pbkdf2Sha256("password", bytes("salt"), 1000, 32)) ==
          "632c2812e46d4604102ba7618e9d6d7d2f8128f6266b4a03264d2a0460b7dcb3");
    CHECK(aid::toHex(aid::pbkdf2Sha256("passwordPASSWORDpassword",
                                       bytes("saltSALTsaltSALTsaltSALTsaltSALTsalt"), 4, 40)) ==
          "420789272fa094b8d87841e9e36ad173e3fbb3855ef99e286c3b22efd72401e1f048e23f3c28d5e4");  // two blocks

    std::vector<uint8_t> v;
    CHECK(aid::fromHex("00ff10Ab", &v) && v.size() == 4 && v[1] == 0xff && v[3] == 0xab);
    CHECK(!aid::fromHex("abc", &v));
    CHECK(!aid::fromHex("zz", &v));
    CHECK(aid::randomHex(16).size() == 32);
    CHECK(aid::randomHex(16) != aid::randomHex(16));

    const std::string h = aid::hashPassword("correct horse");
    CHECK(h.compare(0, 20, "pbkdf2-sha256$60000$") == 0);
    CHECK(aid::verifyPassword("correct horse", h));
    CHECK(!aid::verifyPassword("correct horsf", h));
    CHECK(!aid::verifyPassword("", h));
    CHECK(aid::hashPassword("correct horse") != h);                 // random salt
    CHECK(!aid::verifyPassword("x", "garbage"));
    CHECK(!aid::verifyPassword("x", "pbkdf2-sha256$0$00$00"));
    CHECK(!aid::verifyPassword("x", "pbkdf2-sha256$5$zz$00"));
}

static void testAuth() {
    char tmpl[] = "/tmp/aidauthXXXXXX";
    CHECK(mkdtemp(tmpl) != nullptr);
    const std::string file = std::string(tmpl) + "/auth.json";
    std::string err;
    int retry = 0;
    int64_t t = 1000000;

    aid::AuthManager am(file, 15, 8);
    CHECK(!am.load(&err));                                          // no file yet
    CHECK(!am.hasPassword());
    CHECK(am.login("admin", "whatever1", "1.2.3.4", t, &err, &retry).empty());   // nothing set
    CHECK(!am.setPassword("admin", "short", &err));                 // policy
    CHECK(!am.setPassword("admin", std::string(200, 'x'), &err));
    CHECK(!am.setPassword("ad min", "longenough1", &err));
    CHECK(!am.setPassword("", "longenough1", &err));
    CHECK(am.setPassword("admin", "s3cret-pass", &err));
    CHECK(am.hasPassword() && am.username() == "admin");
    struct stat st;
    CHECK(stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);

    aid::AuthManager am2(file, 15, 8);                              // survives a restart
    CHECK(am2.load(&err) && am2.hasPassword());

    std::string tok = am2.login("admin", "s3cret-pass", "1.2.3.4", t, &err, &retry);
    CHECK(tok.size() == 64);
    CHECK(am2.validate(tok, t + 1000));
    CHECK(!am2.validate("0123456789012345678901234567890123456789012345678901234567890123", t));
    CHECK(!am2.validate("short", t));
    CHECK(am2.login("admin", "wrong-pass", "5.5.5.5", t, &err, &retry).empty());
    CHECK(am2.login("root", "s3cret-pass", "5.5.5.5", t, &err, &retry).empty());   // wrong user

    // idle timeout: refreshed by use, expires after 15 min of silence
    CHECK(am2.validate(tok, t + 10 * 60000));
    CHECK(am2.validate(tok, t + 20 * 60000));                       // 10 min after last use
    CHECK(!am2.validate(tok, t + 40 * 60000));                      // 20 min after last use
    // absolute lifetime of 8 h even with constant use
    tok = am2.login("admin", "s3cret-pass", "1.2.3.4", t, &err, &retry);
    int64_t u = t;
    bool alive = true;
    for (int i = 0; i < 40 && alive; ++i) { u += 14 * 60000; alive = am2.validate(tok, u); }
    CHECK(!alive);
    CHECK(u - t <= 8 * 3600000LL + 14 * 60000);

    // logout
    tok = am2.login("admin", "s3cret-pass", "1.2.3.4", t, &err, &retry);
    am2.logout(tok);
    CHECK(!am2.validate(tok, t));

    // lockout after 5 failures per client, even for the right password; other clients unaffected
    for (int i = 0; i < 5; ++i) am2.login("admin", "bad-pass-" + std::to_string(i), "9.9.9.9", t, &err, &retry);
    CHECK(retry == 60);
    CHECK(am2.login("admin", "s3cret-pass", "9.9.9.9", t + 1000, &err, &retry).empty());
    CHECK(retry > 0 && retry <= 60);
    CHECK(am2.login("admin", "s3cret-pass", "8.8.8.8", t + 1000, &err, &retry).size() == 64);
    CHECK(am2.login("admin", "s3cret-pass", "9.9.9.9", t + 61000, &err, &retry).size() == 64);   // unlocked

    // changing the password ends every session
    tok = am2.login("admin", "s3cret-pass", "1.2.3.4", t + 70000, &err, &retry);
    CHECK(am2.validate(tok, t + 70001));
    CHECK(am2.setPassword("admin", "another-pass-9", &err));
    CHECK(!am2.validate(tok, t + 70002));
    CHECK(am2.login("admin", "s3cret-pass", "1.2.3.4", t + 80000, &err, &retry).empty());
    CHECK(am2.login("admin", "another-pass-9", "1.2.3.4", t + 80000, &err, &retry).size() == 64);

    std::string cmd = std::string("rm -rf ") + tmpl;
    CHECK(system(cmd.c_str()) == 0);
}

int main() {
    testPrimitives();
    testAuth();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
