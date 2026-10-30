// PC-side tests: MQTT packet encoding (byte-exact against the 3.1.1 spec), the client against a
// fake broker on loopback, and the Home Assistant discovery messages.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ha_discovery.h"
#include "mqtt_client.h"

static int g_fail = 0, g_run = 0;
#define CHECK(cond)                                                   \
    do {                                                              \
        ++g_run;                                                      \
        if (!(cond)) {                                                \
            ++g_fail;                                                 \
            printf("FAIL line %d: %s\n", __LINE__, #cond);            \
        }                                                             \
    } while (0)

static std::vector<uint8_t> V(std::initializer_list<int> l) {
    std::vector<uint8_t> v;
    for (int x : l) v.push_back((uint8_t)x);
    return v;
}

static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

template <class F>
static bool waitFor(F f, int timeoutMs) {
    for (int i = 0; i < timeoutMs / 10; ++i) {
        if (f()) return true;
        sleepMs(10);
    }
    return f();
}

// ---------------------------------------------------------------- fake broker
struct FakeBroker {
    int listenFd = -1;
    int port = 0;
    std::thread th;
    std::atomic<bool> quit{false};
    std::mutex mu;
    std::vector<std::string> topics, payloads;
    std::vector<bool> retains;
    std::string connectClientId, connectUser, connectPass, willTopic, willPayload;
    uint8_t connectFlags = 0;
    int connections = 0;
    int refuseCode = 0;        // CONNACK return code for the next connections
    int dropAfterPublishes = 0;  // close the connection after this many PUBLISHes (0 = never)
    bool sawDisconnect = false;
    bool sawPing = false;

    bool start() {
        listenFd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (bind(listenFd, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(listenFd, 4) != 0) return false;
        socklen_t l = sizeof(a);
        getsockname(listenFd, (struct sockaddr*)&a, &l);
        port = ntohs(a.sin_port);
        th = std::thread([this] { loop(); });
        return true;
    }
    void stop() {
        quit = true;
        if (th.joinable()) th.join();
        close(listenFd);
    }
    static bool readExact(int fd, uint8_t* b, size_t n, int timeoutMs) {
        size_t off = 0;
        while (off < n) {
            struct pollfd p;
            p.fd = fd; p.events = POLLIN; p.revents = 0;
            if (poll(&p, 1, timeoutMs) <= 0) return false;
            const ssize_t r = recv(fd, b + off, n - off, 0);
            if (r <= 0) return false;
            off += (size_t)r;
        }
        return true;
    }
    static bool readPacket(int fd, uint8_t* first, std::vector<uint8_t>* body, int timeoutMs) {
        if (!readExact(fd, first, 1, timeoutMs)) return false;
        uint32_t len = 0, mult = 1;
        for (;;) {
            uint8_t b;
            if (!readExact(fd, &b, 1, timeoutMs)) return false;
            len += (b & 127) * mult;
            mult *= 128;
            if (!(b & 128)) break;
        }
        body->assign(len, 0);
        return len == 0 || readExact(fd, body->data(), len, timeoutMs);
    }
    void loop() {
        while (!quit) {
            struct pollfd p;
            p.fd = listenFd; p.events = POLLIN; p.revents = 0;
            if (poll(&p, 1, 50) <= 0) continue;
            const int c = accept(listenFd, nullptr, nullptr);
            if (c < 0) continue;
            serve(c);
            close(c);
        }
    }
    void serve(int c) {
        uint8_t first;
        std::vector<uint8_t> b;
        if (!readPacket(c, &first, &b, 2000) || first != 0x10 || b.size() < 12) return;
        size_t i = 0;
        auto str = [&](std::string* out) {
            const size_t n = ((size_t)b[i] << 8) | b[i + 1];
            i += 2;
            out->assign((const char*)&b[i], n);
            i += n;
        };
        std::string proto;
        str(&proto);
        const uint8_t level = b[i++];
        const uint8_t flags = b[i++];
        i += 2;  // keep alive
        {
            std::lock_guard<std::mutex> lock(mu);
            ++connections;
            connectFlags = flags;
            str(&connectClientId);
            if (flags & 0x04) { str(&willTopic); str(&willPayload); }
            if (flags & 0x80) str(&connectUser);
            if (flags & 0x40) str(&connectPass);
        }
        if (proto != "MQTT" || level != 4) return;
        int code;
        { std::lock_guard<std::mutex> lock(mu); code = refuseCode; }
        const uint8_t ack[4] = {0x20, 0x02, 0x00, (uint8_t)code};
        send(c, ack, 4, MSG_NOSIGNAL);
        if (code != 0) return;
        int publishes = 0;
        while (!quit) {
            if (!readPacket(c, &first, &b, 200)) {
                struct pollfd p;
                p.fd = c; p.events = POLLIN; p.revents = 0;
                if (poll(&p, 1, 0) > 0) {   // closed by the client
                    char x;
                    if (recv(c, &x, 1, MSG_PEEK) == 0) return;
                }
                continue;
            }
            if ((first & 0xF0) == 0x30) {
                const size_t n = ((size_t)b[0] << 8) | b[1];
                std::lock_guard<std::mutex> lock(mu);
                topics.push_back(std::string((const char*)&b[2], n));
                payloads.push_back(std::string((const char*)&b[2 + n], b.size() - 2 - n));
                retains.push_back(first & 1);
                if (dropAfterPublishes && ++publishes >= dropAfterPublishes) return;
            } else if (first == 0xC0) {
                const uint8_t pong[2] = {0xD0, 0x00};
                send(c, pong, 2, MSG_NOSIGNAL);
                std::lock_guard<std::mutex> lock(mu);
                sawPing = true;
            } else if (first == 0xE0) {
                std::lock_guard<std::mutex> lock(mu);
                sawDisconnect = true;
                return;
            }
        }
    }
    size_t published() { std::lock_guard<std::mutex> lock(mu); return topics.size(); }
    int conns() { std::lock_guard<std::mutex> lock(mu); return connections; }
};

static void testEncoding() {
    CHECK(aid::mqttEncodeRemainingLength(0) == V({0x00}));
    CHECK(aid::mqttEncodeRemainingLength(127) == V({0x7F}));
    CHECK(aid::mqttEncodeRemainingLength(128) == V({0x80, 0x01}));
    CHECK(aid::mqttEncodeRemainingLength(16383) == V({0xFF, 0x7F}));
    CHECK(aid::mqttEncodeRemainingLength(16384) == V({0x80, 0x80, 0x01}));

    aid::MqttOptions o;
    o.host = "x";
    o.clientId = "c1";
    o.keepAliveS = 30;
    CHECK(aid::mqttEncodeConnect(o) ==
          V({0x10, 0x0E, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x02, 0x00, 0x1E, 0x00, 0x02, 'c', '1'}));
    CHECK(aid::mqttEncodePublish("a/b", "hi", true) == V({0x31, 0x07, 0x00, 0x03, 'a', '/', 'b', 'h', 'i'}));
    CHECK(aid::mqttEncodePublish("a/b", "", false) == V({0x30, 0x05, 0x00, 0x03, 'a', '/', 'b'}));
    CHECK(aid::mqttEncodePingReq() == V({0xC0, 0x00}));
    CHECK(aid::mqttEncodeDisconnect() == V({0xE0, 0x00}));

    o.user = "u";
    o.password = "p";
    o.willTopic = "t";
    o.willPayload = "off";
    const std::vector<uint8_t> c = aid::mqttEncodeConnect(o);
    CHECK(c[9] == (0x02 | 0x04 | 0x20 | 0x80 | 0x40));   // clean, will, will retain, user, password
}

static void testClient() {
    FakeBroker br;
    CHECK(br.start());
    aid::MqttClient cl;
    std::atomic<int> connectCalls{0};
    cl.setOnConnect([&] { ++connectCalls; });

    aid::MqttOptions o;
    o.host = "127.0.0.1";
    o.port = br.port;
    o.user = "ha";
    o.password = "secret pass";
    o.clientId = "cam1";
    o.willTopic = "cam/status";
    o.willPayload = "offline";
    o.reconnectMinMs = 100;
    o.reconnectMaxMs = 400;
    std::string err;
    CHECK(!cl.publish("", "x", false));
    CHECK(!cl.publish("a/#", "x", false));
    CHECK(!cl.publish("a/+/b", "x", false));
    cl.publish("early/topic", "queued before connect", true);
    CHECK(cl.start(o, &err));
    CHECK(waitFor([&] { return cl.connected(); }, 3000));
    CHECK(connectCalls == 1);
    {
        std::lock_guard<std::mutex> lock(br.mu);
        CHECK(br.connectClientId == "cam1" && br.connectUser == "ha" && br.connectPass == "secret pass");
        CHECK(br.willTopic == "cam/status" && br.willPayload == "offline");
        CHECK((br.connectFlags & 0x20) && (br.connectFlags & 0x04));
    }
    cl.publish("cam/status", "online", true);
    cl.publish("cam/detection/person", "ON", false);
    CHECK(waitFor([&] { return br.published() == 3; }, 3000));
    {
        std::lock_guard<std::mutex> lock(br.mu);
        CHECK(br.topics[0] == "early/topic" && br.retains[0]);
        CHECK(br.topics[1] == "cam/status" && br.payloads[1] == "online" && br.retains[1]);
        CHECK(br.topics[2] == "cam/detection/person" && br.payloads[2] == "ON" && !br.retains[2]);
    }
    cl.stop();
    CHECK(!cl.connected());
    { std::lock_guard<std::mutex> lock(br.mu); CHECK(br.sawDisconnect); }
    br.stop();
}

static void testRefusalAndReconnect() {
    FakeBroker br;
    CHECK(br.start());
    br.refuseCode = 4;  // bad user name or password
    aid::MqttClient cl;
    int calls = 0;
    std::mutex cm;
    cl.setOnConnect([&] { std::lock_guard<std::mutex> l(cm); ++calls; });
    aid::MqttOptions o;
    o.host = "127.0.0.1";
    o.port = br.port;
    o.reconnectMinMs = 100;
    o.reconnectMaxMs = 200;
    std::string err;
    CHECK(cl.start(o, &err));
    CHECK(waitFor([&] { return br.conns() >= 2; }, 3000));           // it keeps retrying
    CHECK(!cl.connected());
    CHECK(cl.lastError().find("bad user name or password") != std::string::npos);
    { std::lock_guard<std::mutex> l(cm); CHECK(calls == 0); }

    { std::lock_guard<std::mutex> lock(br.mu); br.refuseCode = 0; br.dropAfterPublishes = 1; }
    CHECK(waitFor([&] { return cl.connected(); }, 3000));
    cl.publish("t/one", "1", false);                                  // broker drops the link after this
    CHECK(waitFor([&] { return br.published() == 1; }, 3000));
    const int before = br.conns();
    CHECK(waitFor([&] { return br.conns() > before && cl.connected(); }, 4000));   // reconnected on its own
    { std::lock_guard<std::mutex> l(cm); CHECK(calls >= 2); }
    cl.stop();
    br.stop();

    // queue is bounded while the broker is unreachable
    aid::MqttClient dead;
    aid::MqttOptions d;
    d.host = "127.0.0.1";
    d.port = 1;           // nothing listens here
    d.reconnectMinMs = 200;
    CHECK(dead.start(d, &err));
    int accepted = 0;
    for (int i = 0; i < 100; ++i) accepted += dead.publish("q/t", "x", false) ? 1 : 0;
    CHECK(accepted == 64);
    dead.stop();
    CHECK(!dead.start(aid::MqttOptions(), &err));                     // empty host refused
}

static bool balanced(const std::string& s) {
    int brace = 0, bracket = 0, quotes = 0;
    bool esc = false;
    for (char c : s) {
        if (esc) { esc = false; continue; }
        if (c == '\\') { esc = true; continue; }
        if (c == '"') ++quotes;
        else if (quotes % 2 == 0) {
            if (c == '{') ++brace; else if (c == '}') --brace;
            else if (c == '[') ++bracket; else if (c == ']') --bracket;
        }
    }
    return brace == 0 && bracket == 0 && quotes % 2 == 0;
}

static void testDiscovery() {
    aid::HaDevice d;
    d.prefix = "luckfox-ai";
    d.deviceName = "Front \"Door\" Cam";
    d.swVersion = "1.0";
    d.snapshotUrl = "http://192.168.1.160:8080/snapshot.jpg";
    CHECK(aid::haNodeId("Luckfox-AI") == "luckfox_ai");
    CHECK(aid::haStatusTopic(d) == "luckfox-ai/status");
    const std::vector<aid::HaMessage> m = aid::haDiscoveryMessages(d);
    CHECK(m.size() == 10);
    bool person = false;
    std::vector<std::string> ids;
    for (const aid::HaMessage& x : m) {
        CHECK(x.retain);
        CHECK(x.topic.compare(0, 14, "homeassistant/") == 0 && x.topic.size() > 7 &&
              x.topic.compare(x.topic.size() - 7, 7, "/config") == 0);
        CHECK(balanced(x.payload));
        CHECK(x.payload.find("\"availability_topic\":\"luckfox-ai/status\"") != std::string::npos);
        CHECK(x.payload.find("\"identifiers\":[\"luckfox_ai\"]") != std::string::npos);
        CHECK(x.payload.find("Front \\\"Door\\\" Cam") != std::string::npos);   // name is escaped
        const size_t p = x.payload.find("\"unique_id\":\"");
        CHECK(p != std::string::npos);
        ids.push_back(x.payload.substr(p, 40));
        if (x.topic == "homeassistant/binary_sensor/luckfox_ai/person/config") {
            person = true;
            CHECK(x.payload.find("\"state_topic\":\"luckfox-ai/detection/person\"") != std::string::npos);
            CHECK(x.payload.find("\"payload_on\":\"ON\"") != std::string::npos);
        }
    }
    CHECK(person);
    for (size_t i = 0; i < ids.size(); ++i)
        for (size_t j = i + 1; j < ids.size(); ++j) CHECK(ids[i] != ids[j]);       // unique ids
    d.snapshotUrl.clear();
    CHECK(aid::haDiscoveryMessages(d).size() == 9);
}

int main() {
    testEncoding();
    testClient();
    testRefusalAndReconnect();
    testDiscovery();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
