#include "mqtt_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "log.h"

namespace aid {

namespace {

void putU16(std::vector<uint8_t>& v, uint16_t n) {
    v.push_back((uint8_t)(n >> 8));
    v.push_back((uint8_t)n);
}
void putStr(std::vector<uint8_t>& v, const std::string& s) {
    putU16(v, (uint16_t)s.size());
    v.insert(v.end(), s.begin(), s.end());
}
int64_t nowMs() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

}  // namespace

std::vector<uint8_t> mqttEncodeRemainingLength(uint32_t n) {
    std::vector<uint8_t> v;
    do {
        uint8_t b = n % 128;
        n /= 128;
        if (n > 0) b |= 0x80;
        v.push_back(b);
    } while (n > 0);
    return v;
}

std::vector<uint8_t> mqttEncodeConnect(const MqttOptions& o) {
    std::vector<uint8_t> body;
    putStr(body, "MQTT");
    body.push_back(4);  // protocol level 3.1.1
    uint8_t flags = 0x02;  // clean session
    const bool will = !o.willTopic.empty();
    if (will) {
        flags |= 0x04;
        if (o.willRetain) flags |= 0x20;
    }
    if (!o.user.empty()) {
        flags |= 0x80;
        if (!o.password.empty()) flags |= 0x40;
    }
    body.push_back(flags);
    putU16(body, (uint16_t)o.keepAliveS);
    putStr(body, o.clientId);
    if (will) { putStr(body, o.willTopic); putStr(body, o.willPayload); }
    if (!o.user.empty()) {
        putStr(body, o.user);
        if (!o.password.empty()) putStr(body, o.password);
    }
    std::vector<uint8_t> pkt;
    pkt.push_back(0x10);
    const std::vector<uint8_t> rl = mqttEncodeRemainingLength((uint32_t)body.size());
    pkt.insert(pkt.end(), rl.begin(), rl.end());
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

std::vector<uint8_t> mqttEncodePublish(const std::string& topic, const std::string& payload, bool retain) {
    std::vector<uint8_t> body;
    putStr(body, topic);
    body.insert(body.end(), payload.begin(), payload.end());
    std::vector<uint8_t> pkt;
    pkt.push_back((uint8_t)(0x30 | (retain ? 1 : 0)));  // QoS 0
    const std::vector<uint8_t> rl = mqttEncodeRemainingLength((uint32_t)body.size());
    pkt.insert(pkt.end(), rl.begin(), rl.end());
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

std::vector<uint8_t> mqttEncodePingReq() { return {0xC0, 0x00}; }
std::vector<uint8_t> mqttEncodeDisconnect() { return {0xE0, 0x00}; }

struct MqttClient::Impl {
    MqttOptions opts;
    std::atomic<bool> quit{false};
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    struct Msg { std::string topic, payload; bool retain; };
    std::deque<Msg> queue;
    std::string lastError;
    int fd = -1;

    void setError(const std::string& e) {
        std::lock_guard<std::mutex> lock(mutex);
        lastError = e;
    }

    int connectTcp() {
        struct addrinfo hints, *res = nullptr;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        const std::string port = std::to_string(opts.port);
        if (getaddrinfo(opts.host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
            setError("cannot resolve " + opts.host);
            return -1;
        }
        const int s = socket(res->ai_family, res->ai_socktype, 0);
        if (s < 0) { freeaddrinfo(res); setError("socket failed"); return -1; }
        fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
        int r = connect(s, res->ai_addr, res->ai_addrlen);
        freeaddrinfo(res);
        if (r != 0 && errno != EINPROGRESS) { close(s); setError(std::string("connect: ") + strerror(errno)); return -1; }
        struct pollfd p;
        p.fd = s;
        p.events = POLLOUT;
        p.revents = 0;
        if (poll(&p, 1, 5000) <= 0) { close(s); setError("connect timed out"); return -1; }
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &sl);
        if (soerr != 0) { close(s); setError(std::string("connect: ") + strerror(soerr)); return -1; }
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        return s;
    }

    bool sendAll(const std::vector<uint8_t>& d) {
        size_t off = 0;
        const int64_t deadline = nowMs() + 5000;
        while (off < d.size()) {
            const ssize_t n = send(fd, d.data() + off, d.size() - off, MSG_NOSIGNAL);
            if (n > 0) { off += (size_t)n; continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                struct pollfd p;
                p.fd = fd;
                p.events = POLLOUT;
                p.revents = 0;
                if (poll(&p, 1, 500) < 0 && errno != EINTR) return false;
                if (nowMs() > deadline) return false;
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        return true;
    }

    // Reads whatever is available; returns false if the peer closed or errored.
    bool drainInput(std::vector<uint8_t>* acc) {
        uint8_t buf[256];
        for (;;) {
            const ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n > 0) { acc->insert(acc->end(), buf, buf + n); if (acc->size() > 4096) acc->clear(); continue; }
            if (n == 0) return false;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            if (errno == EINTR) continue;
            return false;
        }
    }

    bool waitConnack() {
        std::vector<uint8_t> in;
        const int64_t deadline = nowMs() + 5000;
        while (nowMs() < deadline && !quit) {
            struct pollfd p;
            p.fd = fd;
            p.events = POLLIN;
            p.revents = 0;
            poll(&p, 1, 200);
            // A broker that refuses the login sends CONNACK and closes at once: read the reply
            // first, then react to the close.
            const bool alive = drainInput(&in);
            if (in.size() >= 4) {
                if (in[0] == 0x20 && in[1] == 2) {
                    if (in[3] == 0) return true;
                    static const char* why[] = {"", "unacceptable protocol", "identifier rejected", "server unavailable",
                                                "bad user name or password", "not authorized"};
                    setError(std::string("broker refused the connection: ") + (in[3] <= 5 ? why[in[3]] : "unknown"));
                    return false;
                }
                setError("unexpected reply to CONNECT");
                return false;
            }
            if (!alive) { setError("broker closed the connection"); return false; }
        }
        setError("no CONNACK from the broker");
        return false;
    }

    void session(MqttClient* self) {
        std::vector<uint8_t> in;
        int64_t lastSend = nowMs();
        while (!quit) {
            // flush queued publishes
            for (;;) {
                Msg m;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (queue.empty()) break;
                    m = queue.front();
                    queue.pop_front();
                }
                if (!sendAll(mqttEncodePublish(m.topic, m.payload, m.retain))) { setError("send failed"); return; }
                lastSend = nowMs();
            }
            if (nowMs() - lastSend >= (int64_t)opts.keepAliveS * 500) {
                if (!sendAll(mqttEncodePingReq())) { setError("ping failed"); return; }
                lastSend = nowMs();
            }
            struct pollfd p;
            p.fd = fd;
            p.events = POLLIN;
            p.revents = 0;
            poll(&p, 1, 200);
            if (p.revents & (POLLIN | POLLERR | POLLHUP)) {
                if (!drainInput(&in)) { setError("connection lost"); return; }
                in.clear();  // we never subscribe: PINGRESP and friends are ignored
            }
        }
        (void)self;
    }

    void run(MqttClient* self) {
        int backoff = opts.reconnectMinMs;
        while (!quit) {
            fd = connectTcp();
            bool ok = fd >= 0;
            if (ok) {
                ok = sendAll(mqttEncodeConnect(opts)) && waitConnack();
                if (!ok && lastErrorEmpty()) setError("CONNECT failed");
            }
            if (ok) {
                backoff = opts.reconnectMinMs;
                setError("");
                self->connected_ = true;
                logInfo("MQTT connected to " + opts.host);
                if (self->onConnect_) self->onConnect_();
                session(self);
                self->connected_ = false;
                if (quit) sendAll(mqttEncodeDisconnect());
                logWarn("MQTT disconnected: " + getError());
            }
            if (fd >= 0) { close(fd); fd = -1; }
            if (quit) break;
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait_for(lock, std::chrono::milliseconds(backoff), [this] { return quit.load(); });
            backoff = std::min(backoff * 2, opts.reconnectMaxMs);
        }
    }

    bool lastErrorEmpty() {
        std::lock_guard<std::mutex> lock(mutex);
        return lastError.empty();
    }
    std::string getError() {
        std::lock_guard<std::mutex> lock(mutex);
        return lastError;
    }
};

MqttClient::MqttClient() : impl_(new Impl) {}
MqttClient::~MqttClient() { stop(); }

std::string MqttClient::lastError() const { return impl_->getError(); }

bool MqttClient::start(const MqttOptions& opts, std::string* err) {
    if (impl_->thread.joinable()) { if (err) *err = "already started"; return false; }
    if (opts.host.empty()) { if (err) *err = "mqtt host is empty"; return false; }
    if (opts.clientId.empty() || opts.keepAliveS < 5 || opts.keepAliveS > 600) {
        if (err) *err = "invalid client id or keep-alive";
        return false;
    }
    impl_->opts = opts;
    impl_->quit = false;
    impl_->thread = std::thread([this] { impl_->run(this); });
    return true;
}

void MqttClient::stop() {
    if (!impl_->thread.joinable()) return;
    impl_->quit = true;
    impl_->cv.notify_all();
    impl_->thread.join();
    connected_ = false;
}

bool MqttClient::publish(const std::string& topic, const std::string& payload, bool retain) {
    if (topic.empty() || topic.size() > 512 || payload.size() > 65536) return false;
    if (topic.find('#') != std::string::npos || topic.find('+') != std::string::npos) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->queue.size() >= 64) return false;
    impl_->queue.push_back({topic, payload, retain});
    return true;
}

}  // namespace aid
