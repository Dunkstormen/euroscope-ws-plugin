#include "WsServer.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static const socket_t kInvalidSocket = INVALID_SOCKET;
static void CloseSock(socket_t s) { closesocket(s); }
static bool SetNonBlocking(socket_t s) { u_long on = 1; return ioctlsocket(s, FIONBIO, &on) == 0; }
static bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#  define SEND_FLAGS 0
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
static const socket_t kInvalidSocket = -1;
static void CloseSock(socket_t s) { close(s); }
static bool SetNonBlocking(socket_t s) { return fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) == 0; }
static bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
#  define SEND_FLAGS MSG_NOSIGNAL
#endif

namespace {

// A client whose unsent backlog grows past this is too slow and gets dropped.
const size_t kMaxOutBuffer = 16 * 1024 * 1024;
// Max size of an incoming HTTP handshake or client frame we accept.
const size_t kMaxInBuffer = 1024 * 1024;

// ---------------------------------------------------------------- SHA-1 ----
// Only used for the handshake's Sec-WebSocket-Accept header.
struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

    static uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

    void Block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                   (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
            uint32_t t = Rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = Rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    static std::string Digest(const std::string& msg) {
        Sha1 s;
        std::string data = msg;
        uint64_t bitLen = uint64_t(msg.size()) * 8;
        data.push_back('\x80');  // padding marker byte; '\x80' avoids MSVC C4310
        while (data.size() % 64 != 56) data.push_back('\0');
        for (int i = 7; i >= 0; --i) data.push_back(char((bitLen >> (i * 8)) & 0xFF));
        for (size_t i = 0; i < data.size(); i += 64) s.Block(reinterpret_cast<const uint8_t*>(data.data() + i));
        std::string out(20, '\0');
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 4; ++j) out[i * 4 + j] = char((s.h[i] >> (24 - j * 8)) & 0xFF);
        return out;
    }
};

std::string Base64(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t v = uint8_t(in[i]) << 16;
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
    }
    return out;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// Build a single unmasked server->client frame.
std::string MakeFrame(uint8_t opcode, const std::string& payload) {
    std::string f;
    f.push_back(char(0x80 | opcode));  // FIN + opcode
    size_t n = payload.size();
    if (n < 126) {
        f.push_back(char(n));
    } else if (n <= 0xFFFF) {
        f.push_back(char(126));
        f.push_back(char((n >> 8) & 0xFF));
        f.push_back(char(n & 0xFF));
    } else {
        f.push_back(char(127));
        for (int i = 7; i >= 0; --i) f.push_back(char((uint64_t(n) >> (i * 8)) & 0xFF));
    }
    f += payload;
    return f;
}

struct Client {
    WsServer::ClientId id = 0;
    socket_t sock = kInvalidSocket;
    bool open = false;          // handshake done
    bool closing = false;       // flush outBuf, then close
    std::string inBuf;
    std::string outBuf;
    std::string fragment;       // reassembly of fragmented text messages
};

// Returns false if the handshake is malformed. Appends the HTTP response to c.outBuf.
bool HandleHandshake(Client& c) {
    size_t end = c.inBuf.find("\r\n\r\n");
    if (end == std::string::npos) return c.inBuf.size() < kMaxInBuffer;  // wait for more

    std::string request = c.inBuf.substr(0, end);
    c.inBuf.erase(0, end + 4);

    std::string key;
    bool upgrade = false;
    size_t pos = request.find("\r\n");
    while (pos != std::string::npos) {
        size_t next = request.find("\r\n", pos + 2);
        std::string line = request.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = Lower(Trim(line.substr(0, colon)));
            std::string value = Trim(line.substr(colon + 1));
            if (name == "sec-websocket-key") key = value;
            if (name == "upgrade" && Lower(value) == "websocket") upgrade = true;
        }
        pos = next;
    }

    if (key.empty() || !upgrade) {
        const char* body = "This is a WebSocket endpoint. Connect with a WebSocket client.\n";
        c.outBuf += "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: " +
                    std::to_string(strlen(body)) + "\r\n\r\n" + body;
        c.closing = true;
        return true;
    }

    std::string accept = Base64(Sha1::Digest(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
    c.outBuf += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    c.open = true;
    return true;
}

// Parse complete client frames from c.inBuf. Returns false on protocol error.
bool HandleFrames(Client& c, std::vector<std::string>& textMessages) {
    for (;;) {
        const std::string& b = c.inBuf;
        if (b.size() < 2) return true;
        uint8_t b0 = uint8_t(b[0]), b1 = uint8_t(b[1]);
        bool fin = (b0 & 0x80) != 0;
        uint8_t opcode = b0 & 0x0F;
        bool masked = (b1 & 0x80) != 0;
        uint64_t len = b1 & 0x7F;
        size_t hdr = 2;
        if (len == 126) {
            if (b.size() < 4) return true;
            len = (uint64_t(uint8_t(b[2])) << 8) | uint8_t(b[3]);
            hdr = 4;
        } else if (len == 127) {
            if (b.size() < 10) return true;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | uint8_t(b[2 + i]);
            hdr = 10;
        }
        if (!masked) return false;  // RFC 6455: clients must mask
        if (len > kMaxInBuffer) return false;
        if (b.size() < hdr + 4 + len) return true;  // wait for the rest

        const uint8_t* mask = reinterpret_cast<const uint8_t*>(b.data() + hdr);
        std::string payload(size_t(len), '\0');
        for (size_t i = 0; i < len; ++i) payload[i] = char(uint8_t(b[hdr + 4 + i]) ^ mask[i % 4]);
        c.inBuf.erase(0, hdr + 4 + size_t(len));

        switch (opcode) {
            case 0x0:  // continuation
            case 0x1:  // text
                c.fragment += payload;
                if (c.fragment.size() > kMaxInBuffer) return false;
                if (fin) { textMessages.push_back(std::move(c.fragment)); c.fragment.clear(); }
                break;
            case 0x2:  // binary: ignored
                break;
            case 0x8:  // close: echo and hang up
                c.outBuf += MakeFrame(0x8, payload.substr(0, 2));
                c.closing = true;
                return true;
            case 0x9:  // ping
                c.outBuf += MakeFrame(0xA, payload);
                break;
            case 0xA:  // pong
                break;
            default:
                return false;
        }
    }
}

}  // namespace

WsServer::~WsServer() { Stop(); }

bool WsServer::Start(const std::string& bindAddress, uint16_t port, std::string* error) {
    if (running_) return true;
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        if (listenSocket_ != -1) { CloseSock(socket_t(listenSocket_)); listenSocket_ = -1; }
        return false;
    };

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return fail("WSAStartup failed");
#endif

    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidSocket) return fail("socket() failed");
    listenSocket_ = intptr_t(s);

#ifndef _WIN32
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) return fail("invalid bind address " + bindAddress);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        return fail("could not bind " + bindAddress + ":" + std::to_string(port) + " (port in use?)");
    if (listen(s, 16) != 0) return fail("listen() failed");
    SetNonBlocking(s);

    port_ = port;
    running_ = true;
    thread_ = std::thread(&WsServer::Run, this);
    return true;
}

void WsServer::Stop() {
    if (!running_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
    CloseSock(socket_t(listenSocket_));
    listenSocket_ = -1;
    clientCount_ = 0;
#ifdef _WIN32
    WSACleanup();
#endif
}

void WsServer::Publish(const std::string& topic, const std::string& text, bool retain) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back({topic, 0, text});
    if (!retain) return;
    for (auto& r : retained_)
        if (r.first == topic) { r.second = text; return; }
    retained_.emplace_back(topic, text);
}

void WsServer::SendTo(ClientId client, const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back({std::string(), client, text});
}

void WsServer::Subscribe(ClientId client, const std::set<std::string>& topics) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(client);
    if (it == subscriptions_.end()) return;  // gone
    for (const auto& r : retained_)
        if (topics.count(r.first) && !it->second.count(r.first)) pending_.push_back({std::string(), client, r.second});
    it->second = topics;
}

void WsServer::SetDefaultTopics(const std::set<std::string>& topics) {
    std::lock_guard<std::mutex> lock(mutex_);
    defaultTopics_ = topics;
}

bool WsServer::HasSubscribers(const std::string& topic) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& s : subscriptions_)
        if (s.second.count(topic)) return true;
    return false;
}

void WsServer::ClearRetained() {
    std::lock_guard<std::mutex> lock(mutex_);
    retained_.clear();
}

void WsServer::SetMessageHandler(MessageHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    handler_ = std::move(handler);
}

void WsServer::SetCloseHandler(CloseHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    closeHandler_ = std::move(handler);
}

void WsServer::Run() {
    socket_t listener = socket_t(listenSocket_);
    std::vector<Client> clients;
    char buf[16384];

    while (running_) {
        // 1. Pick up messages queued by the plugin thread.
        std::vector<Outgoing> outgoing;
        std::map<ClientId, std::set<std::string>> subs;
        MessageHandler handler;
        CloseHandler closeHandler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            outgoing.swap(pending_);
            if (!outgoing.empty()) subs = subscriptions_;
            handler = handler_;
            closeHandler = closeHandler_;
        }
        for (const Outgoing& msg : outgoing) {
            std::string frame = MakeFrame(0x1, msg.text);
            for (Client& c : clients) {
                if (!c.open || c.closing) continue;
                if (msg.topic.empty() ? c.id == msg.client : subs[c.id].count(msg.topic) > 0) c.outBuf += frame;
            }
        }

        // 2. Wait for socket activity (short timeout so new broadcasts go out promptly).
        fd_set readSet, writeSet;
        FD_ZERO(&readSet);
        FD_ZERO(&writeSet);
        FD_SET(listener, &readSet);
        socket_t maxFd = listener;
        for (Client& c : clients) {
            FD_SET(c.sock, &readSet);
            if (!c.outBuf.empty()) FD_SET(c.sock, &writeSet);
            maxFd = std::max(maxFd, c.sock);
        }
        timeval tv{0, 20 * 1000};
        int ready = select(int(maxFd + 1), &readSet, &writeSet, nullptr, &tv);
        if (ready < 0) continue;

        // 3. Accept new connections (FD_SETSIZE limits us to ~60 clients on Windows; plenty here).
        if (FD_ISSET(listener, &readSet)) {
            for (;;) {
                socket_t ns = accept(listener, nullptr, nullptr);
                if (ns == kInvalidSocket) break;
                if (clients.size() + 1 >= FD_SETSIZE) { CloseSock(ns); continue; }
                SetNonBlocking(ns);
                int one = 1;
                setsockopt(ns, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
                Client c;
                c.id = nextClientId_++;  // never reused, even across restarts
                c.sock = ns;
                clients.push_back(std::move(c));
            }
        }

        // 4. Read / write each client.
        for (Client& c : clients) {
            bool dead = false;

            if (FD_ISSET(c.sock, &readSet)) {
                int n = int(recv(c.sock, buf, sizeof(buf), 0));
                if (n > 0) {
                    c.inBuf.append(buf, size_t(n));
                    if (!c.open) {
                        bool wasOpen = c.open;
                        if (!HandleHandshake(c)) dead = true;
                        // Subscribe a new client and give it the latest retained messages straight away.
                        if (!wasOpen && c.open) {
                            std::lock_guard<std::mutex> lock(mutex_);
                            subscriptions_[c.id] = defaultTopics_;
                            for (const auto& r : retained_)
                                if (defaultTopics_.count(r.first)) c.outBuf += MakeFrame(0x1, r.second);
                        }
                    }
                    if (!dead && c.open && !c.closing) {
                        std::vector<std::string> msgs;
                        if (!HandleFrames(c, msgs)) dead = true;
                        if (handler)
                            for (const std::string& m : msgs) handler(c.id, m);
                    }
                    if (c.inBuf.size() > kMaxInBuffer) dead = true;
                } else if (n == 0 || !WouldBlock()) {
                    dead = true;
                }
            }

            if (!dead && !c.outBuf.empty()) {
                int n = int(send(c.sock, c.outBuf.data(), int(std::min<size_t>(c.outBuf.size(), 1 << 20)), SEND_FLAGS));
                if (n > 0) c.outBuf.erase(0, size_t(n));
                else if (n < 0 && !WouldBlock()) dead = true;
            }

            if (c.outBuf.size() > kMaxOutBuffer) dead = true;  // client can't keep up
            if (c.closing && c.outBuf.empty()) dead = true;

            if (dead) {
                CloseSock(c.sock);
                c.sock = kInvalidSocket;
                if (c.open) {
                    { std::lock_guard<std::mutex> lock(mutex_); subscriptions_.erase(c.id); }
                    if (closeHandler) closeHandler(c.id);
                }
            }
        }
        clients.erase(std::remove_if(clients.begin(), clients.end(),
                                     [](const Client& c) { return c.sock == kInvalidSocket; }),
                      clients.end());
        clientCount_ = size_t(std::count_if(clients.begin(), clients.end(), [](const Client& c) { return c.open; }));
    }

    CloseHandler closeHandler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closeHandler = closeHandler_;
        subscriptions_.clear();
        pending_.clear();
    }
    for (Client& c : clients) {
        if (c.open) {
            std::string bye = MakeFrame(0x8, std::string("\x03\xE9", 2));  // 1001 going away
            send(c.sock, bye.data(), int(bye.size()), SEND_FLAGS);
        }
        CloseSock(c.sock);
        // Let the owner drop per-client state (auth, filters) for clients closed by Stop().
        if (c.open && closeHandler) closeHandler(c.id);
    }
}
