// SPDX-License-Identifier: GPL-3.0-or-later
#include "http_server.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

namespace host {

namespace {

constexpr size_t kMaxRequest = 24 * 1024 * 1024;  // a firmware image, a project, a sample (library: up to 22 MB)
constexpr size_t kMaxHeader = 16 * 1024;
constexpr size_t kLiveLimit = 256 * 1024;
constexpr size_t kStateLimit = 8 * 1024 * 1024;

uint64_t nowMs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// SHA-1 (FIPS 180-1), only for the WebSocket handshake
void sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::vector<uint8_t> m(data, data + len);
    const uint64_t bits = (uint64_t)len * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56)
        m.push_back(0);
    for (int i = 7; i >= 0; i--)
        m.push_back((uint8_t)(bits >> (8 * i)));
    auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)m[off + 4 * i] << 24 | (uint32_t)m[off + 4 * i + 1] << 16 | (uint32_t)m[off + 4 * i + 2] << 8 | m[off + 4 * i + 3];
        for (int i = 16; i < 80; i++)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 4; k++)
            out[4 * i + k] = (uint8_t)(h[i] >> (24 - 8 * k));
}

std::string base64(const uint8_t *d, size_t n)
{
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += t[(v >> 18) & 63];
        o += t[(v >> 12) & 63];
        o += i + 1 < n ? t[(v >> 6) & 63] : '=';
        o += i + 2 < n ? t[v & 63] : '=';
    }
    return o;
}

const char *statusText(int s)
{
    switch (s) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

}  // namespace

std::string sha1Base64(const std::string &in)
{
    uint8_t h[20];
    sha1(reinterpret_cast<const uint8_t *>(in.data()), in.size(), h);
    return base64(h, 20);
}

bool HttpServer::sameOrigin(const HttpRequest &r)
{
    const std::string o = r.header("origin");
    if (o.empty())
        return true;
    const std::string host = r.header("host");
    return !host.empty() && (o == "http://" + host || o == "https://" + host);
}

bool HttpServer::listen(const std::string &addr, uint16_t port, std::string *err)
{
    listen_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) != 1 || bind(listen_, (sockaddr *)&sa, sizeof sa) != 0 ||
        ::listen(listen_, 16) != 0) {
        if (err)
            *err = std::string("cannot listen on ") + addr + ":" + std::to_string(port) + ": " + strerror(errno);
        ::close(listen_);
        listen_ = -1;
        return false;
    }
    return true;
}

void HttpServer::accept()
{
    for (;;) {
        int fd = accept4(listen_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0)
            return;
        if (conns_.size() >= 32) {
            ::close(fd);
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        Conn c;
        c.fd = fd;
        c.lastActive = nowMs();
        conns_[fd] = c;
    }
}

void HttpServer::close(Conn &c)
{
    if (c.ws && h_.wsClose)
        h_.wsClose(c.fd);
    ::close(c.fd);
    c.fd = -1;
}

bool HttpServer::readConn(Conn &c)
{
    char buf[65536];
    for (;;) {
        ssize_t n = recv(c.fd, buf, sizeof buf, 0);
        if (n > 0) {
            c.in.append(buf, (size_t)n);
            c.lastActive = nowMs();
            if (c.in.size() > kMaxRequest + kMaxHeader)
                return false;
            continue;
        }
        if (n == 0)
            return false;
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
}

bool HttpServer::handleHttp(Conn &c)
{
    for (;;) {
        const size_t he = c.in.find("\r\n\r\n");
        if (he == std::string::npos)
            return c.in.size() <= kMaxHeader;
        HttpRequest r;
        std::string head = c.in.substr(0, he);
        size_t le = head.find("\r\n");
        std::string line = head.substr(0, le);
        size_t s1 = line.find(' '), s2 = line.rfind(' ');
        if (s1 == std::string::npos || s2 == s1)
            return false;
        r.method = line.substr(0, s1);
        std::string target = line.substr(s1 + 1, s2 - s1 - 1);
        const size_t q = target.find('?');
        r.path = target.substr(0, q);
        if (q != std::string::npos)
            r.query = target.substr(q + 1);
        size_t pos = le == std::string::npos ? head.size() : le + 2;
        while (pos < head.size()) {
            size_t e = head.find("\r\n", pos);
            if (e == std::string::npos)
                e = head.size();
            std::string h = head.substr(pos, e - pos);
            const size_t colon = h.find(':');
            if (colon != std::string::npos) {
                std::string k = h.substr(0, colon), v = h.substr(colon + 1);
                std::transform(k.begin(), k.end(), k.begin(), ::tolower);
                v.erase(0, v.find_first_not_of(" \t"));
                r.headers[k] = v;
            }
            pos = e + 2;
        }
        const size_t len = (size_t)atoll(r.header("content-length").c_str());
        if (len > kMaxRequest) {
            c.out += "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            c.closing = true;
            return true;
        }
        if (c.in.size() < he + 4 + len)
            return true;  // body still arriving
        r.body = c.in.substr(he + 4, len);
        c.in.erase(0, he + 4 + len);

        // WebSocket upgrade
        std::string up = r.header("upgrade");
        std::transform(up.begin(), up.end(), up.begin(), ::tolower);
        if (r.path == "/ws" && up == "websocket") {
            const std::string key = r.header("sec-websocket-key");
            if (key.empty() || !sameOrigin(r) || wsClients() >= 6) {
                c.out += "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                c.closing = true;
                return true;
            }
            c.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                     sha1Base64(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11") + "\r\n\r\n";
            c.ws = true;
            if (h_.wsOpen)
                h_.wsOpen(c.fd);
            return handleWs(c);
        }
        HttpResponse resp = h_.http ? h_.http(r) : HttpResponse{404, "text/plain", "not found", {}};
        std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + statusText(resp.status) + "\r\nContent-Type: " + resp.type +
                          "\r\nContent-Length: " + std::to_string(resp.body.size()) + "\r\n";
        for (auto &h : resp.headers)
            out += h.first + ": " + h.second + "\r\n";
        out += "\r\n";
        c.out += out;
        c.out += resp.body;
    }
}

bool HttpServer::handleWs(Conn &c)
{
    for (;;) {
        if (c.in.size() < 2)
            return true;
        const uint8_t b0 = (uint8_t)c.in[0], b1 = (uint8_t)c.in[1];
        const bool fin = b0 & 0x80;
        const uint8_t op = b0 & 0x0F;
        if (!(b1 & 0x80))
            return false;  // client frames must be masked
        uint64_t len = b1 & 0x7F;
        size_t hdr = 2;
        if (len == 126) {
            if (c.in.size() < 4)
                return true;
            len = (uint64_t)(uint8_t)c.in[2] << 8 | (uint8_t)c.in[3];
            hdr = 4;
        } else if (len == 127) {
            if (c.in.size() < 10)
                return true;
            len = 0;
            for (int i = 0; i < 8; i++)
                len = len << 8 | (uint8_t)c.in[2 + (size_t)i];
            hdr = 10;
        }
        if (len > daw::kMaxMessageBytes + 1024)
            return false;
        if (c.in.size() < hdr + 4 + len)
            return true;
        uint8_t mask[4];
        memcpy(mask, c.in.data() + hdr, 4);
        std::string payload = c.in.substr(hdr + 4, (size_t)len);
        for (size_t i = 0; i < payload.size(); i++)
            payload[i] ^= (char)mask[i & 3];
        c.in.erase(0, hdr + 4 + (size_t)len);
        switch (op) {
        case 0x0:  // continuation
            c.frag += payload;
            if (c.frag.size() > daw::kMaxMessageBytes)
                return false;
            if (fin) {
                if (h_.wsText)
                    h_.wsText(c.fd, c.frag);
                c.frag.clear();
            }
            break;
        case 0x1:
            if (fin) {
                if (h_.wsText)
                    h_.wsText(c.fd, payload);
            } else
                c.frag = payload;
            break;
        case 0x8:
            queueFrame(c, 0x8, payload.substr(0, 2));
            c.closing = true;
            return true;
        case 0x9: queueFrame(c, 0xA, payload); break;
        default: break;  // pong, binary: ignored
        }
    }
}

void HttpServer::queueFrame(Conn &c, uint8_t opcode, const std::string &p)
{
    std::string f;
    f += (char)(0x80 | opcode);
    if (p.size() < 126)
        f += (char)p.size();
    else if (p.size() < 65536) {
        f += (char)126;
        f += (char)(p.size() >> 8);
        f += (char)(p.size() & 0xFF);
    } else {
        f += (char)127;
        for (int i = 7; i >= 0; i--)
            f += (char)((uint64_t)p.size() >> (8 * i));
    }
    c.out += f;
    c.out += p;
}

void HttpServer::send(int client, const std::string &text, daw::MsgClass cls)
{
    for (auto &kv : conns_) {
        Conn &c = kv.second;
        if (!c.ws || c.fd < 0 || (client >= 0 && c.fd != client))
            continue;
        if ((cls == daw::MsgClass::Live && c.out.size() > kLiveLimit) || c.out.size() + text.size() > kStateLimit) {
            dropped_++;
            if (cls == daw::MsgClass::State)
                c.resync = true;
            continue;
        }
        queueFrame(c, 0x1, text);
        flush(c);
    }
}

int HttpServer::takeResync()
{
    for (auto &kv : conns_)
        if (kv.second.ws && kv.second.resync && kv.second.out.size() < kLiveLimit) {
            kv.second.resync = false;
            return kv.first;
        }
    return -1;
}

size_t HttpServer::wsClients() const
{
    size_t n = 0;
    for (auto &kv : conns_)
        n += kv.second.ws && kv.second.fd >= 0;
    return n;
}

void HttpServer::flush(Conn &c)
{
    while (!c.out.empty()) {
        ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
        if (n > 0)
            c.out.erase(0, (size_t)n);
        else
            break;
    }
}

void HttpServer::poll(int timeoutMs)
{
    std::vector<pollfd> fds;
    fds.push_back({listen_, POLLIN, 0});
    for (auto &kv : conns_)
        fds.push_back({kv.first, (short)(POLLIN | (kv.second.out.empty() ? 0 : POLLOUT)), 0});
    ::poll(fds.data(), fds.size(), timeoutMs);
    if (fds[0].revents & POLLIN)
        accept();
    for (size_t i = 1; i < fds.size(); i++) {
        auto it = conns_.find(fds[i].fd);
        if (it == conns_.end())
            continue;
        Conn &c = it->second;
        bool ok = true;
        if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
            ok = readConn(c);
            if (ok)
                ok = c.ws ? handleWs(c) : handleHttp(c);
        }
        if (c.fd >= 0)
            flush(c);
        // idle HTTP connections close after 30 s; WebSockets live as long as the peer (the time is
        // read after readConn: a time from before it is older than lastActive, and the difference
        // would wrap around, closing a connection that is still receiving a large body)
        if (!c.ws && nowMs() - c.lastActive > 30000)
            ok = false;
        if (!ok || (c.closing && c.out.empty())) {
            close(c);
            conns_.erase(it);
        }
    }
}

std::string HttpServer::urlDecode(const std::string &s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+')
            o += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else
            o += s[i];
    }
    return o;
}

}  // namespace host
