// SPDX-License-Identifier: GPL-3.0-or-later
// A small single-threaded HTTP/1.1 + WebSocket (RFC 6455) server for the Linux host build:
// poll() loop, bounded buffers, the same endpoints and security rules as the firmware
// (components/daw_webserver). Runs on the control thread together with the DAW session.
#pragma once

#include <stdint.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "daw/session.hpp"

namespace host {

struct HttpRequest {
    std::string method, path, query;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;
    std::string header(const std::string &k) const
    {
        auto it = headers.find(k);
        return it == headers.end() ? "" : it->second;
    }
};

struct HttpResponse {
    int status = 200;
    std::string type = "application/json", body;
    std::vector<std::pair<std::string, std::string>> headers;
};

class HttpServer {
public:
    struct Handlers {
        std::function<void(int)> wsOpen, wsClose;
        std::function<void(int, const std::string &)> wsText;
        std::function<HttpResponse(const HttpRequest &)> http;
    };
    bool listen(const std::string &addr, uint16_t port, std::string *err);
    void setHandlers(Handlers h) { h_ = std::move(h); }
    // one poll round (timeoutMs), then returns
    void poll(int timeoutMs);
    void send(int client, const std::string &text, daw::MsgClass cls);  // client -1 = all WS clients
    int takeResync();
    size_t wsClients() const;
    uint32_t dropped() const { return dropped_; }
    static bool sameOrigin(const HttpRequest &r);
    static std::string urlDecode(const std::string &s);  // %xx and +

private:
    struct Conn {
        int fd;
        bool ws = false;
        std::string in, out;
        std::string frag;  // fragmented message being reassembled
        bool closing = false, resync = false;
        uint64_t lastActive = 0;
    };
    void accept();
    bool readConn(Conn &c);
    bool handleHttp(Conn &c);
    bool handleWs(Conn &c);
    void queueFrame(Conn &c, uint8_t opcode, const std::string &payload);
    void flush(Conn &c);
    void close(Conn &c);

    int listen_ = -1;
    std::map<int, Conn> conns_;
    Handlers h_;
    uint32_t dropped_ = 0;
};

std::string sha1Base64(const std::string &in);

}  // namespace host
