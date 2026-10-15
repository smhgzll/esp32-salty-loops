// SPDX-License-Identifier: GPL-3.0-or-later
// HTTP + WebSocket server (esp_http_server, core 0).
//
// Security model for a local device (docs/SECURITY.md):
//  - browsers attach an Origin header to cross-site requests; every WebSocket upgrade and every
//    POST whose Origin does not match the Host is refused, so a web page elsewhere cannot drive the
//    device through the user's browser;
//  - privileged endpoints (firmware update, factory reset, project restore) need a bearer token
//    from POST /api/login with the admin password (CONFIG_DAW_ADMIN_PASSWORD / settings);
//  - request sizes and the number of WebSocket clients are bounded; slow clients lose meter frames
//    first and are resynchronised instead of buffering without limit.
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "web_server.hpp"

extern "C" {
struct daw_web_asset {
    const char *path, *mime;
    uint32_t offset, size;
    uint8_t gzip;
};
extern const struct daw_web_asset daw_web_assets[];
extern const uint32_t daw_web_asset_count;
extern const uint8_t _binary_web_bundle_bin_start[];
}

namespace daw::web {

static const char *TAG = "web";
static httpd_handle_t s_server;
static Callbacks s_cb;
static std::string s_admin;

// ---- WebSocket clients and their send queues ------------------------------------------------

constexpr size_t kLiveLimit = 24 * 1024;    // bytes waiting before meter / status frames are dropped
constexpr size_t kStateLimit = 192 * 1024;  // beyond this a State frame is dropped -> resync
// A State frame larger than this (the welcome of a large project, e.g. a MIDI import of 5,000
// notes: 220+ KB) is queued alone, outside the limits above: at most one per client, once at
// most kLiveLimit of other frames wait. Dropping it would resync the client with another forever.
constexpr size_t kAlone = kStateLimit - kLiveLimit;

// Each client has a FIFO of outgoing frames. At most one httpd work item per client drains it, so
// a burst of messages costs one control-socket message instead of one each (the httpd control
// queue is small). If scheduling fails, the control task's kick() retries.
struct WsClient {
    int fd = -1;
    std::deque<std::string> q;
    size_t pending = 0;  // bytes queued
    size_t big = 0;      // of which one frame larger than kAlone
    bool scheduled = false;
    bool resync = false;
    bool receivingText = false;
    std::string incoming;
};
static WsClient s_clients[CONFIG_DAW_MAX_CLIENTS];
static std::mutex s_lock;  // guards s_clients
static std::atomic<uint32_t> s_dropped{0};

static WsClient *findClient(int fd)
{
    for (auto &c : s_clients)
        if (c.fd == fd)
            return &c;
    return nullptr;
}

uint32_t clientCount()
{
    std::lock_guard<std::mutex> g(s_lock);
    uint32_t n = 0;
    for (auto &c : s_clients)
        n += c.fd >= 0 ? 1 : 0;
    return n;
}

uint32_t droppedFrames() { return s_dropped.load(); }

int takeResync()
{
    std::lock_guard<std::mutex> g(s_lock);
    for (auto &c : s_clients)
        if (c.fd >= 0 && c.resync && c.pending < kLiveLimit) {
            c.resync = false;
            return c.fd;
        }
    return -1;
}

// Complete writes. httpd_ws_send_frame_async() ignores partial send() results (a send timeout on
// a slow reader), which truncates a frame and corrupts the stream; this loop does not.
static bool sendAll(int fd, const char *p, size_t n)
{
    int stalls = 0;
    while (n) {
        const int r = httpd_socket_send(s_server, fd, p, n, 0);
        if (r > 0) {
            p += r;
            n -= (size_t)r;
            stalls = 0;
            continue;
        }
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++stalls < 4)  // up to 4 x send_wait_timeout (3 s)
            continue;
        return false;
    }
    return true;
}

static bool sendText(int fd, const std::string &payload)
{
    uint8_t hdr[10];
    size_t h = 0;
    hdr[h++] = 0x81;  // FIN + text; server frames are not masked
    const size_t n = payload.size();
    if (n < 126)
        hdr[h++] = (uint8_t)n;
    else if (n < 65536) {
        hdr[h++] = 126;
        hdr[h++] = (uint8_t)(n >> 8);
        hdr[h++] = (uint8_t)n;
    } else {
        hdr[h++] = 127;
        for (int i = 7; i >= 0; i--)
            hdr[h++] = (uint8_t)((uint64_t)n >> (8 * i));
    }
    return sendAll(fd, reinterpret_cast<const char *>(hdr), h) && sendAll(fd, payload.data(), n);
}

static void flushWork(void *arg)
{
    const int fd = (int)(intptr_t)arg;
    for (;;) {
        std::string frame;
        {
            std::lock_guard<std::mutex> g(s_lock);
            WsClient *c = findClient(fd);
            if (!c)
                return;
            if (c->q.empty()) {
                c->scheduled = false;
                return;
            }
            frame = std::move(c->q.front());
            c->q.pop_front();
            c->pending -= frame.size();
            if (frame.size() > kAlone)
                c->big = 0;
        }
        if (httpd_ws_get_fd_info(s_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET || !sendText(fd, frame)) {
            ESP_LOGW(TAG, "send to %d failed: closing", fd);
            std::lock_guard<std::mutex> g(s_lock);
            if (WsClient *c = findClient(fd)) {
                c->q.clear();
                c->pending = 0;
                c->big = 0;
                c->scheduled = false;
            }
            httpd_sess_trigger_close(s_server, fd);
            return;
        }
    }
}

// schedule the drain of every client that has frames waiting (from send() and the control task)
void kick()
{
    int todo[CONFIG_DAW_MAX_CLIENTS];
    int n = 0;
    {
        std::lock_guard<std::mutex> g(s_lock);
        for (auto &c : s_clients)
            if (c.fd >= 0 && !c.q.empty() && !c.scheduled) {
                c.scheduled = true;
                todo[n++] = c.fd;
            }
    }
    for (int i = 0; i < n; i++)
        if (httpd_queue_work(s_server, flushWork, (void *)(intptr_t)todo[i]) != ESP_OK) {
            std::lock_guard<std::mutex> g(s_lock);
            if (WsClient *c = findClient(todo[i]))
                c->scheduled = false;  // retried at the next kick
        }
}

void send(int client, const std::string &text, MsgClass cls)
{
    if (!s_server)
        return;
    {
        std::lock_guard<std::mutex> g(s_lock);
        for (auto &c : s_clients) {
            if (c.fd < 0 || (client >= 0 && c.fd != client))
                continue;
            const size_t regular = c.pending - c.big;
            const bool alone = cls == MsgClass::State && text.size() > kAlone;
            const bool full = alone ? c.big > 0 || regular > kLiveLimit
                                    : (cls == MsgClass::Live && regular > kLiveLimit) || regular + text.size() > kStateLimit;
            if (full) {
                s_dropped.fetch_add(1);
                if (cls == MsgClass::State)
                    c.resync = true;  // it missed state: a full snapshot once it drained
                continue;
            }
            c.q.push_back(text);
            c.pending += text.size();
            if (alone)
                c.big = text.size();
        }
    }
    kick();
}

static void onClose(httpd_handle_t, int fd)
{
    bool was = false;
    {
        std::lock_guard<std::mutex> g(s_lock);
        if (WsClient *c = findClient(fd)) {
            c->fd = -1;
            c->q.clear();
            c->pending = 0;
            c->big = 0;
            c->scheduled = false;
            c->resync = false;
            c->receivingText = false;
            c->incoming.clear();
            was = true;
        }
    }
    if (was) {
        ESP_LOGI(TAG, "client %d closed (%u connected)", fd, (unsigned)clientCount());
        if (s_cb.onClose)
            s_cb.onClose(fd);
    }
    close(fd);
}

// ---- request helpers ----------------------------------------------------------------------

static std::string header(httpd_req_t *req, const char *name)
{
    const size_t n = httpd_req_get_hdr_value_len(req, name);
    if (!n || n > 512)
        return "";
    std::string v(n + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, name, v.data(), v.size()) != ESP_OK)
        return "";
    v.resize(n);
    return v;
}

// same-origin check: no Origin (not a browser page) or Origin == http://<Host>
static bool sameOrigin(httpd_req_t *req)
{
    const std::string origin = header(req, "Origin");
    if (origin.empty())
        return true;
    const std::string host = header(req, "Host");
    return !host.empty() && (origin == "http://" + host || origin == "https://" + host);
}

struct Token {
    char value[33];
    int64_t expires;
};
static Token s_tokens[4];

static bool authorized(httpd_req_t *req)
{
    const std::string a = header(req, "Authorization");
    if (a.size() != 7 + 32 || a.compare(0, 7, "Bearer ") != 0)
        return false;
    const int64_t now = esp_timer_get_time();
    for (auto &t : s_tokens) {
        if (t.expires <= now)
            continue;
        uint8_t diff = 0;  // constant time
        for (int i = 0; i < 32; i++)
            diff |= (uint8_t)(t.value[i] ^ a[7 + (size_t)i]);
        if (!diff)
            return true;
    }
    return false;
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *type, const std::string &body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.data(), (ssize_t)body.size());
}

static esp_err_t replyError(httpd_req_t *req, const char *status, const char *msg)
{
    return reply(req, status, "application/json", std::string("{\"error\":\"") + msg + "\"}");
}

// exactly n bytes of the request body (a sender silent for a minute has gone)
static bool recvAll(httpd_req_t *req, char *dst, size_t n)
{
    size_t got = 0;
    int stalls = 0;
    while (got < n) {
        const int r = httpd_req_recv(req, dst + got, n - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++stalls < 6)  // 6 x recv_wait_timeout (10 s)
            continue;
        if (r <= 0)
            return false;
        stalls = 0;
        got += (size_t)r;
    }
    return true;
}

static bool readBody(httpd_req_t *req, std::string &out, size_t max)
{
    if (req->content_len > max)
        return false;
    out.resize(req->content_len);
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, out.data() + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (r <= 0)
            return false;
        got += (size_t)r;
    }
    return true;
}

// ---- handlers -----------------------------------------------------------------------------

// before the upgrade: refuse foreign origins and connections beyond the client limit
static esp_err_t onWsPreHandshake(httpd_req_t *req)
{
    if (!sameOrigin(req)) {
        ESP_LOGW(TAG, "WebSocket from a foreign origin refused");
        return ESP_FAIL;
    }
    if (clientCount() >= CONFIG_DAW_MAX_CLIENTS) {
        ESP_LOGW(TAG, "too many WebSocket clients");
        return ESP_FAIL;
    }
    return ESP_OK;
}

// after the upgrade (ESP-IDF 5.5 does not call the URI handler for the handshake itself)
static esp_err_t onWsPostHandshake(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    bool added = false;
    {
        std::lock_guard<std::mutex> g(s_lock);
        for (auto &c : s_clients)
            if (c.fd < 0) {
                c.fd = fd;
                c.q.clear();
                c.pending = 0;
                c.big = 0;
                c.scheduled = false;
                c.resync = false;
                c.receivingText = false;
                c.incoming.clear();
                added = true;
                break;
            }
    }
    if (!added) {
        ESP_LOGW(TAG, "client %d refused: all %d slots in use", fd, CONFIG_DAW_MAX_CLIENTS);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "client %d connected (%u connected)", fd, (unsigned)clientCount());
    if (s_cb.onOpen)
        s_cb.onOpen(fd);
    return ESP_OK;
}

static esp_err_t onWs(httpd_req_t *req)
{
    httpd_ws_frame_t f = {};
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK)
        return ESP_FAIL;
    if (f.type != HTTPD_WS_TYPE_TEXT && f.type != HTTPD_WS_TYPE_CONTINUE)
        return ESP_FAIL;  // control frames are handled by httpd; binary is not protocol 1
    if (f.len > kMaxMessageBytes)
        return ESP_FAIL;
    char *buf = static_cast<char *>(malloc(f.len + 1));
    if (!buf)
        return ESP_FAIL;
    f.payload = reinterpret_cast<uint8_t *>(buf);
    if (httpd_ws_recv_frame(req, &f, f.len) != ESP_OK) {
        free(buf);
        return ESP_FAIL;
    }
    buf[f.len] = 0;
    const int fd = httpd_req_to_sockfd(req);
    std::string joined;
    {
        std::lock_guard<std::mutex> g(s_lock);
        WsClient *c = findClient(fd);
        if (!c || (f.type == HTTPD_WS_TYPE_CONTINUE ? !c->receivingText : c->receivingText)) {
            free(buf); return ESP_FAIL;
        }
        if (f.type == HTTPD_WS_TYPE_CONTINUE || !f.final) {
            if (f.len > kMaxMessageBytes - c->incoming.size()) { free(buf); return ESP_FAIL; }
            c->incoming.append(buf, f.len);
            c->receivingText = !f.final;
            free(buf);
            if (!f.final) return ESP_OK;
            joined = std::move(c->incoming);
            c->incoming.clear();
            f.len = joined.size();
            buf = static_cast<char *>(malloc(f.len + 1));
            if (!buf) return ESP_FAIL;
            memcpy(buf, joined.data(), f.len); buf[f.len] = 0;
        }
    }
    if (!s_cb.onText || !s_cb.onText(fd, buf, f.len)) {
        // the control task's queue is full: tell the client (it retries)
        // (queued like any other frame, so it cannot interleave with a frame being written)
        send(httpd_req_to_sockfd(req), "{\"t\":\"err\",\"code\":\"busy\",\"msg\":\"device busy, retry\"}", MsgClass::State);
    }
    return ESP_OK;
}

static esp_err_t onAsset(httpd_req_t *req)
{
    std::string path(req->uri);
    const size_t q = path.find('?');
    if (q != std::string::npos)
        path.resize(q);
    if (path == "/")
        path = "/index.html";
    const daw_web_asset *a = nullptr, *index = nullptr;
    for (uint32_t i = 0; i < daw_web_asset_count; i++) {
        if (path == daw_web_assets[i].path)
            a = &daw_web_assets[i];
        if (!strcmp(daw_web_assets[i].path, "/index.html"))
            index = &daw_web_assets[i];
    }
    if (!a) {
        if (path.compare(0, 5, "/api/") == 0 || path.find('.') != std::string::npos)
            return replyError(req, "404 Not Found", "not found");
        a = index;  // client-side routes
    }
    if (!a)
        return replyError(req, "404 Not Found", "no web application embedded");
    httpd_resp_set_type(req, a->mime);
    if (a->gzip)
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    // hashed bundle files never change; the page itself is always revalidated
    httpd_resp_set_hdr(req, "Cache-Control", strstr(a->path, "/assets/") ? "public, max-age=31536000, immutable" : "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    if (a == index)
        httpd_resp_set_hdr(req, "Content-Security-Policy",
                           "default-src 'self'; connect-src 'self' ws: wss:; img-src 'self' data:; style-src 'self' 'unsafe-inline'");
    return httpd_resp_send(req, reinterpret_cast<const char *>(_binary_web_bundle_bin_start + a->offset), a->size);
}

static esp_err_t onStatus(httpd_req_t *req)
{
    return reply(req, "200 OK", "application/json", s_cb.status ? s_cb.status() : "{}");
}

static esp_err_t onLogin(httpd_req_t *req)
{
    if (!sameOrigin(req))
        return replyError(req, "403 Forbidden", "cross-origin request");
    std::string body;
    if (!readBody(req, body, 512))
        return replyError(req, "413 Payload Too Large", "too large");
    if (s_admin.empty())
        return replyError(req, "403 Forbidden", "privileged operations are disabled (no admin password set)");
    // {"password":"..."}: a tiny extraction, the body is bounded
    const size_t k = body.find("\"password\"");
    const size_t q1 = k == std::string::npos ? k : body.find('"', body.find(':', k) + 1);
    const size_t q2 = q1 == std::string::npos ? q1 : body.find('"', q1 + 1);
    const std::string pw = q2 == std::string::npos ? "" : body.substr(q1 + 1, q2 - q1 - 1);
    uint8_t diff = pw.size() != s_admin.size();
    for (size_t i = 0; i < s_admin.size(); i++)
        diff |= (uint8_t)(s_admin[i] ^ (i < pw.size() ? pw[i] : 0));
    if (diff) {
        vTaskDelay(pdMS_TO_TICKS(1000));  // slows guessing down
        return replyError(req, "401 Unauthorized", "wrong password");
    }
    Token *slot = &s_tokens[0];
    for (auto &t : s_tokens)
        if (t.expires < slot->expires)
            slot = &t;
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof rnd);
    for (int i = 0; i < 16; i++)
        snprintf(slot->value + 2 * i, 3, "%02x", rnd[i]);
    slot->expires = esp_timer_get_time() + 3600ll * 1000000ll;
    return reply(req, "200 OK", "application/json", std::string("{\"token\":\"") + slot->value + "\",\"expiresIn\":3600}");
}

static esp_err_t onProjectGet(httpd_req_t *req)
{
    std::string out;
    if (!s_cb.exportProject || !s_cb.exportProject(out))
        return replyError(req, "503 Service Unavailable", "busy");
    if (out == "null")
        return replyError(req, "507 Insufficient Storage", "out of memory writing the project");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"project.esp32daw.json\"");
    return reply(req, "200 OK", "application/json", out);
}

static esp_err_t onProjectPost(httpd_req_t *req)
{
    if (!sameOrigin(req))
        return replyError(req, "403 Forbidden", "cross-origin request");
    if (!authorized(req))
        return replyError(req, "401 Unauthorized", "log in first");
    std::string body, err;
    if (!readBody(req, body, 512 * 1024))
        return replyError(req, "413 Payload Too Large", "project too large");
    if (!s_cb.importProject || !s_cb.importProject(body, err))
        return replyError(req, "400 Bad Request", err.empty() ? "import failed" : err.c_str());
    return reply(req, "200 OK", "application/json", "{\"ok\":true}");
}

static void restartSoon(void *)
{
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

static esp_err_t onReboot(httpd_req_t *req)
{
    if (!sameOrigin(req))
        return replyError(req, "403 Forbidden", "cross-origin request");
    if (!authorized(req))
        return replyError(req, "401 Unauthorized", "log in first");
    if (s_cb.beforeFlashWork)
        s_cb.beforeFlashWork();
    reply(req, "200 OK", "application/json", "{\"ok\":true}");
    xTaskCreate(restartSoon, "restart", 2048, nullptr, 5, nullptr);
    return ESP_OK;
}

static esp_err_t onFactoryReset(httpd_req_t *req)
{
    if (!sameOrigin(req))
        return replyError(req, "403 Forbidden", "cross-origin request");
    if (!authorized(req))
        return replyError(req, "401 Unauthorized", "log in first");
    if (s_cb.beforeFlashWork)
        s_cb.beforeFlashWork();
    if (s_cb.factoryReset)
        s_cb.factoryReset();
    reply(req, "200 OK", "application/json", "{\"ok\":true}");
    xTaskCreate(restartSoon, "restart", 2048, nullptr, 5, nullptr);
    return ESP_OK;
}

static esp_err_t onTestFailHealth(httpd_req_t *req)
{
    if (!sameOrigin(req))
        return replyError(req, "403 Forbidden", "cross-origin request");
    if (!authorized(req))
        return replyError(req, "401 Unauthorized", "log in first");
    s_cb.testFailNextHealth();
    return reply(req, "200 OK", "application/json", "{\"ok\":true}");
}

static esp_err_t onSamplesGet(httpd_req_t *req)
{
    return reply(req, "200 OK", "application/json", s_cb.samplesJson ? s_cb.samplesJson() : "{\"list\":[]}");
}

static std::string jsonError(const std::string &msg)
{
    std::string o = "{\"error\":\"";
    for (char ch : msg)
        o += ch == '"' || ch == '\\' ? '_' : ch;
    return o + "\"}";
}

// the file goes to PSRAM first (std::string uses the PSRAM-first allocator), then the control task
// validates it and writes it to the "samples" partition (seconds for MBs: audio is stopped anyway)
static esp_err_t onSamplesPost(httpd_req_t *req)
{
    if (!sameOrigin(req))
        return replyError(req, "403 Forbidden", "cross-origin request");
    char query[160] = "", raw[128] = "";
    httpd_req_get_url_query_str(req, query, sizeof query);
    if (httpd_query_key_value(query, "name", raw, sizeof raw) != ESP_OK)
        return replyError(req, "400 Bad Request", "name missing");
    char reclaim[16] = "";
    const bool reclaimModules = httpd_query_key_value(query, "reclaim", reclaim, sizeof reclaim) == ESP_OK && !strcmp(reclaim, "modules");
    // %xx and + decoding of the name
    std::string name;
    for (size_t i = 0; raw[i]; i++) {
        if (raw[i] == '+')
            name += ' ';
        else if (raw[i] == '%' && raw[i + 1] && raw[i + 2]) {
            const char hex[3] = {raw[i + 1], raw[i + 2], 0};
            name += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else
            name += raw[i];
    }
    std::string err;
    const size_t len = req->content_len;
    if (len > 64u << 20)
        return replyError(req, "413 Payload Too Large", "file too large for the sample library");
    // the first bytes say what it is: a file larger than 1 MB that is not a WAV (a SoundFont, a
    // tracker module) goes to the library in pieces as it arrives (a GM font of 6 MB+ would not
    // fit in free PSRAM next to a song); sampleBegin refuses what the library does not take.
    // WAV files are decoded whole to check them.
    std::string head(std::min<size_t>(len, 4096), '\0');
    if (!recvAll(req, head.data(), head.size()))
        return replyError(req, "400 Bad Request", "upload interrupted");
    const bool wav = head.size() >= 12 && !memcmp(head.data(), "RIFF", 4) && !memcmp(head.data() + 8, "WAVE", 4);
    if (!wav && len > (1u << 20) && s_cb.sampleBegin) {
        if (!s_cb.sampleBegin(name, len, head, err, reclaimModules))
            return reply(req, "400 Bad Request", "application/json", jsonError(err.empty() ? "import failed" : err));
        std::string piece(32 * 1024, '\0');
        for (size_t got = head.size(); got < len;) {
            const size_t n = std::min(piece.size(), len - got);
            if (!recvAll(req, piece.data(), n)) {
                s_cb.sampleAbort();
                return replyError(req, "400 Bad Request", "upload interrupted");
            }
            piece.resize(n);
            if (!s_cb.sampleAppend(piece, err))
                return reply(req, "400 Bad Request", "application/json", jsonError(err.empty() ? "import failed" : err));
            piece.resize(32 * 1024);
            got += n;
        }
        if (!s_cb.sampleFinish(err))
            return reply(req, "400 Bad Request", "application/json", jsonError(err.empty() ? "import failed" : err));
        return reply(req, "200 OK", "application/json", s_cb.samplesJson ? s_cb.samplesJson() : "{}");
    }
    // anything else whole, in PSRAM: refuse instead of running out (an allocation failure is fatal)
    if (len + (256u << 10) > heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM))
        return replyError(req, "507 Insufficient Storage", "not enough free memory to receive this file now");
    std::string body = head;
    body.resize(len);
    if (!recvAll(req, body.data() + head.size(), len - head.size()))
        return replyError(req, "400 Bad Request", "upload interrupted");
    if (!s_cb.importSample || !s_cb.importSample(name, body, err, reclaimModules))
        return reply(req, "400 Bad Request", "application/json", jsonError(err.empty() ? "import failed" : err));
    return reply(req, "200 OK", "application/json", s_cb.samplesJson ? s_cb.samplesJson() : "{}");
}

static esp_err_t onSettingsGet(httpd_req_t *req)
{
    if (!authorized(req))
        return replyError(req, "401 Unauthorized", "log in first");
    return reply(req, "200 OK", "application/json", s_cb.settingsJson ? s_cb.settingsJson() : "{}");
}

static esp_err_t onSettingsPost(httpd_req_t *req)
{
    if (!sameOrigin(req))
        return replyError(req, "403 Forbidden", "cross-origin request");
    if (!authorized(req))
        return replyError(req, "401 Unauthorized", "log in first");
    std::string body, err;
    if (!readBody(req, body, 2048))
        return replyError(req, "413 Payload Too Large", "too large");
    if (!s_cb.saveSettings || !s_cb.saveSettings(body, err))
        return reply(req, "400 Bad Request", "application/json", jsonError(err.empty() ? "not saved" : err));
    return reply(req, "200 OK", "application/json", "{\"ok\":true,\"restart\":true}");
}

void setAdminPassword(const std::string &pw)
{
    s_admin = pw;
    for (auto &t : s_tokens)
        t.expires = 0;
}

esp_err_t otaHandler(httpd_req_t *req);  // ota.cpp

static esp_err_t onOta(httpd_req_t *req)
{
    if (!sameOrigin(req))
        return replyError(req, "403 Forbidden", "cross-origin request");
    if (!authorized(req))
        return replyError(req, "401 Unauthorized", "log in first");
    if (s_cb.beforeFlashWork)
        s_cb.beforeFlashWork();
    return otaHandler(req);
}

esp_err_t start(const Callbacks &cb, uint16_t port, const std::string &adminPassword)
{
    s_cb = cb;
    s_admin = adminPassword;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = port;
    cfg.core_id = 0;
    cfg.task_priority = 5;
    cfg.stack_size = 8192;
    cfg.max_open_sockets = CONFIG_DAW_MAX_CLIENTS + 3;
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.close_fn = onClose;
    cfg.send_wait_timeout = 3;
    cfg.recv_wait_timeout = 10;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK)
        return err;
    const httpd_uri_t uris[] = {
        {.uri = "/ws", .method = HTTP_GET, .handler = onWs, .user_ctx = nullptr, .is_websocket = true,
         .handle_ws_control_frames = false, .supported_subprotocol = nullptr, .ws_pre_handshake_cb = onWsPreHandshake,
         .ws_post_handshake_cb = onWsPostHandshake},
        {.uri = "/api/status", .method = HTTP_GET, .handler = onStatus, .user_ctx = nullptr},
        {.uri = "/api/login", .method = HTTP_POST, .handler = onLogin, .user_ctx = nullptr},
        {.uri = "/api/project", .method = HTTP_GET, .handler = onProjectGet, .user_ctx = nullptr},
        {.uri = "/api/project", .method = HTTP_POST, .handler = onProjectPost, .user_ctx = nullptr},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = onOta, .user_ctx = nullptr},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = onReboot, .user_ctx = nullptr},
        {.uri = "/api/factory-reset", .method = HTTP_POST, .handler = onFactoryReset, .user_ctx = nullptr},
        {.uri = "/api/samples", .method = HTTP_GET, .handler = onSamplesGet, .user_ctx = nullptr},
        {.uri = "/api/samples", .method = HTTP_POST, .handler = onSamplesPost, .user_ctx = nullptr},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = onSettingsGet, .user_ctx = nullptr},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = onSettingsPost, .user_ctx = nullptr},
        {.uri = "/*", .method = HTTP_GET, .handler = onAsset, .user_ctx = nullptr},
    };
    for (const auto &u : uris)
        if (httpd_register_uri_handler(s_server, &u) != ESP_OK)
            ESP_LOGE(TAG, "cannot register %s", u.uri);
    if (s_cb.testFailNextHealth) {
        const httpd_uri_t t = {.uri = "/api/test/fail-next-health", .method = HTTP_POST, .handler = onTestFailHealth, .user_ctx = nullptr};
        httpd_register_uri_handler(s_server, &t);
    }
    ESP_LOGI(TAG, "HTTP + WebSocket server on port %u (%u assets embedded, privileged operations %s)", port,
             (unsigned)daw_web_asset_count, s_admin.empty() ? "DISABLED (no admin password)" : "need a login");
    return ESP_OK;
}

}  // namespace daw::web
