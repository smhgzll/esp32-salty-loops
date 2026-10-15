// SPDX-License-Identifier: GPL-3.0-or-later
// daw-host: the DAW on Linux. The same portable core as the firmware with a Linux platform layer:
// ALSA (or a clock-paced null) audio thread, an HTTP / WebSocket server for the browser DAW on the
// control thread, project slots in a file with flash semantics, offline WAV rendering.
//
//   daw-host [--port 8080] [--bind 127.0.0.1] [--web web/dist] [--data DIR] [--audio alsa|null]
//            [--cpu-guard 900] [--cpu-admit 750]   (x1000 of the audio deadline; 0 = off) [--test-hooks]
//            [--alsa-device default] [--admin-password PW]
//   daw-host --render out.wav [--project file.json] [--mode song|pattern] [--seconds 30]
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <thread>

#include "cJSON.h"
#include "daw/platform.hpp"
#include "daw/plugin_host.hpp"
#include "daw/project_json.hpp"
#include "daw/session.hpp"
#include "daw/settings_update.hpp"
#include "daw/store.hpp"
#include "http_server.hpp"
#include "render_worker.hpp"

#ifdef DAW_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

using namespace daw;

static std::atomic<bool> g_stop{false};
static void onSignal(int) { g_stop = true; }

static const char *arg(int argc, char **argv, const char *name, const char *def)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], name))
            return argv[i + 1];
    return def;
}
static bool flag(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], name))
            return true;
    return false;
}

// ---- project slots in a file, with NOR flash semantics (like the firmware's partition) -------

class FileFlash : public FlashDevice {
public:
    FileFlash(const std::string &path, uint32_t size) : path_(path), data_(size, 0xFF)
    {
        std::ifstream f(path, std::ios::binary);
        if (f)
            f.read(reinterpret_cast<char *>(data_.data()), size);
    }
    uint32_t size() const override { return (uint32_t)data_.size(); }
    bool read(uint32_t off, void *dst, uint32_t len) override
    {
        if ((uint64_t)off + len > data_.size())
            return false;
        memcpy(dst, data_.data() + off, len);
        return true;
    }
    bool write(uint32_t off, const void *src, uint32_t len) override
    {
        if ((uint64_t)off + len > data_.size())
            return false;
        const uint8_t *s = static_cast<const uint8_t *>(src);
        for (uint32_t i = 0; i < len; i++)
            data_[off + i] &= s[i];
        return persist();
    }
    bool erase(uint32_t off, uint32_t len) override
    {
        if (off % kSector || len % kSector || (uint64_t)off + len > data_.size())
            return false;
        memset(data_.data() + off, 0xFF, len);
        return persist();
    }

private:
    bool persist()
    {
        if (path_.empty()) return true;  // temporary resource library for offline rendering
        const std::string tmp = path_ + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char *>(data_.data()), (std::streamsize)data_.size());
            if (!f)
                return false;
        }
        return rename(tmp.c_str(), path_.c_str()) == 0;
    }
    std::string path_;
    std::vector<uint8_t> data_;
};

// ---- audio ----------------------------------------------------------------------------------

static void audioThread(Engine *eng, std::string backend, std::string device)
{
    const uint32_t sr = eng->config().sampleRate, n = eng->config().blockFrames;
    std::vector<float> L(n), R(n);
    std::vector<int32_t> pcm(2 * n);
    Dither dither;
#ifdef DAW_HAVE_ALSA
    snd_pcm_t *pcmh = nullptr;
    if (backend == "alsa") {
        int e = snd_pcm_open(&pcmh, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
        if (e == 0)
            e = snd_pcm_set_params(pcmh, SND_PCM_FORMAT_S32_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, sr, 1, 23220);  // ~23 ms like 4x256 DMA
        if (e < 0) {
            platform::log(platform::Log::Warn, "audio", "ALSA device '%s': %s; using the null backend", device.c_str(), snd_strerror(e));
            if (pcmh)
                snd_pcm_close(pcmh);
            pcmh = nullptr;
        } else
            platform::log(platform::Log::Info, "audio", "ALSA '%s': %u Hz, S32_LE stereo (24-bit data), block %u", device.c_str(), sr, n);
    }
#else
    (void)device;
    if (backend == "alsa")
        platform::log(platform::Log::Warn, "audio", "built without ALSA: null backend");
#endif
    auto next = std::chrono::steady_clock::now();
    const auto period = std::chrono::nanoseconds((uint64_t)n * 1000000000ull / sr);
    while (!g_stop) {
        eng->render(L.data(), R.data(), n);
#ifdef DAW_HAVE_ALSA
        if (pcmh) {
            toI2S32(L.data(), R.data(), pcm.data(), n, dither, true);
            snd_pcm_sframes_t w = snd_pcm_writei(pcmh, pcm.data(), n);
            if (w < 0)
                snd_pcm_recover(pcmh, (int)w, 1);
            continue;
        }
#endif
        next += period;
        std::this_thread::sleep_until(next);
    }
#ifdef DAW_HAVE_ALSA
    if (pcmh) {
        snd_pcm_drain(pcmh);
        snd_pcm_close(pcmh);
    }
#endif
}

// ---- offline render -------------------------------------------------------------------------

static int renderToWav(int argc, char **argv)
{
    const std::string out = arg(argc, argv, "--render", "out.wav");
    const double seconds = atof(arg(argc, argv, "--seconds", "16"));
    const std::string mode = arg(argc, argv, "--mode", "song");
    RenderWorker worker;
    Engine eng(Engine::Config{44100, 256, 0, flag(argc, argv, "--parallel-render") ? &worker.api : nullptr});
    FileFlash resourceFlash("", 4 * 1024 * 1024);
    SampleStore resourceStore(resourceFlash);
    Session::Config renderConfig;
    if (const char *dir = arg(argc, argv, "--resource-dir", nullptr)) {
        resourceStore.mount();
        std::error_code ec;
        std::filesystem::directory_iterator files(dir, ec);
        if (ec) { fprintf(stderr, "%s: %s\n", dir, ec.message().c_str()); return 1; }
        for (const auto &entry : files) {
            const auto ext = entry.path().extension().string();
            if (ext != ".wav" && ext != ".sf2") continue;
            std::ifstream f(entry.path(), std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
            std::string err;
            if (!resourceStore.put(entry.path().filename().string(), bytes.data(), bytes.size(), &err)) {
                fprintf(stderr, "%s: %s\n", entry.path().c_str(), err.c_str()); return 1;
            }
        }
        renderConfig.samples = &resourceStore;
    }
    Session s(eng, renderConfig);
    if (const char *pf = arg(argc, argv, "--project", nullptr)) {
        std::ifstream f(pf);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string text = ss.str();
        std::string err;
        if (!s.importProject(text.data(), text.size(), &err)) {
            fprintf(stderr, "%s: %s\n", pf, err.c_str());
            return 1;
        }
    } else
        s.start(defaultProject());
    eng.post(EngineCmd{EngineCmd::SetMode, (uint8_t)(mode == "song" ? 1 : 0)});
    eng.post(EngineCmd{EngineCmd::Play});
    FILE *f = fopen(out.c_str(), "wb");
    if (!f) {
        perror(out.c_str());
        return 1;
    }
    const uint32_t frames = (uint32_t)(seconds * 44100);
    auto put32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto put16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f);
    put32(36 + frames * 6);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(16);
    put16(1);
    put16(2);
    put32(44100);
    put32(44100 * 6);
    put16(6);
    put16(24);
    fwrite("data", 1, 4, f);
    put32(frames * 6);
    float L[256], R[256];
    double peak = 0;
    for (uint32_t done = 0; done < frames; done += 256) {
        eng.render(L, R, 256);
        const uint32_t n = std::min<uint32_t>(256, frames - done);
        for (uint32_t i = 0; i < n; i++)
            for (float x : {L[i], R[i]}) {
                const int32_t v = floatToS24(x);
                peak = std::max(peak, (double)fabsf(x));
                uint8_t b[3] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16)};
                fwrite(b, 1, 3, f);
            }
        if ((done / 256) % 32 == 0)
            s.tick(done / 44);
    }
    fclose(f);
    printf("rendered %.1f s (%s mode) to %s, peak %.3f\n", seconds, mode.c_str(), out.c_str(), peak);
    return 0;
}

// ---- server ---------------------------------------------------------------------------------

static std::string mimeOf(const std::string &p)
{
    static const std::pair<const char *, const char *> m[] = {{".html", "text/html; charset=utf-8"}, {".js", "text/javascript"},
                                                               {".css", "text/css"}, {".svg", "image/svg+xml"},
                                                               {".json", "application/json"}, {".png", "image/png"},
                                                               {".woff2", "font/woff2"}, {".ico", "image/x-icon"},
                                                               {".txt", "text/plain; charset=utf-8"}};
    for (auto &x : m)
        if (p.size() >= strlen(x.first) && p.compare(p.size() - strlen(x.first), std::string::npos, x.first) == 0)
            return x.second;
    return "application/octet-stream";
}

static bool readFile(const std::string &path, std::string &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

int main(int argc, char **argv)
{
    if (arg(argc, argv, "--render", nullptr))
        return renderToWav(argc, argv);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGPIPE, SIG_IGN);
    const uint16_t port = (uint16_t)atoi(arg(argc, argv, "--port", "8080"));
    const std::string bind = arg(argc, argv, "--bind", "127.0.0.1");
    const std::string web = arg(argc, argv, "--web", "web/dist");
    std::string data = arg(argc, argv, "--data", "");
    if (data.empty()) {
        const char *home = getenv("HOME");
        data = std::string(home ? home : ".") + "/.local/share/esp32-s3-daw";
    }
    mkdir((data.substr(0, data.rfind('/'))).c_str(), 0755);
    mkdir(data.c_str(), 0755);
    std::string admin = arg(argc, argv, "--admin-password", getenv("DAW_ADMIN_PASSWORD") ? getenv("DAW_ADMIN_PASSWORD") : "");
    if (admin.empty()) {
        uint8_t r[6];
        platform::randomBytes(r, sizeof r);
        char b[16];
        snprintf(b, sizeof b, "%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2], r[3], r[4], r[5]);
        admin = b;
        printf("admin password for this run: %s (--admin-password to choose one)\n", admin.c_str());
    }

    // the CPU guard and admission limits of the board (Linux timing is real, unlike QEMU's)
    Engine eng(Engine::Config{44100, 256, (uint32_t)atoi(arg(argc, argv, "--cpu-guard", "900"))});
    FileFlash flash(data + "/projects.flash", 2 * 1024 * 1024);
    ProjectStore store(flash, 8);
    // the size of the device's partition: 6 MB, 22 MB on 32 MB boards (--samples-mb 22)
    FileFlash sampleFlash(data + "/samples.flash", (uint32_t)atoi(arg(argc, argv, "--samples-mb", "6")) * 1024u * 1024u);
    SampleStore samples(sampleFlash);
    samples.mount();
    FileFlash presetFlash(data + "/presets.flash", 512 * 1024);
    SampleStore presetStore(presetFlash);
    PresetLibrary presets(presetStore);
    presets.mount();
    host::HttpServer server;
    Session::Config sc;
    sc.deviceName = "daw-host";
    sc.target = "host";
    sc.store = &store;
    sc.samples = &samples;
    sc.presets = &presets;
    sc.cpuAdmitPermille = (uint32_t)atoi(arg(argc, argv, "--cpu-admit", "750"));
    sc.unixTime = [] { return (uint32_t)time(nullptr); };
    sc.waitForAudio = [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); };
    sc.platformDiag = [&](cJSON *d) {
        cJSON_AddStringToObject(d, "audioBackend", arg(argc, argv, "--audio", "alsa"));
        cJSON_AddNumberToObject(d, "wsClients", (double)server.wsClients());
        cJSON_AddNumberToObject(d, "wsDropped", server.dropped());
    };
    Session session(eng, sc);
    session.setSender([&](int c, const std::string &t, MsgClass cls) { server.send(c, t, cls); });
    session.boot();

    std::thread audio(audioThread, &eng, std::string(arg(argc, argv, "--audio", "alsa")),
                      std::string(arg(argc, argv, "--alsa-device", "default")));

    struct Token {
        std::string v;
        uint64_t exp;
    };
    std::vector<Token> tokens;
    auto authorized = [&](const host::HttpRequest &r) {
        const std::string a = r.header("authorization");
        const uint64_t now = platform::micros() / 1000;
        for (auto &t : tokens)
            if (t.exp > now && a == "Bearer " + t.v)
                return true;
        return false;
    };
    auto json = [](int status, const std::string &body) { return host::HttpResponse{status, "application/json", body, {{"Cache-Control", "no-store"}}}; };

    const bool testHooks = flag(argc, argv, "--test-hooks");
    host::HttpServer::Handlers h;
    h.wsOpen = [&](int c) { session.clientConnected(c); };
    h.wsClose = [&](int c) { session.clientDisconnected(c); };
    h.wsText = [&](int c, const std::string &t) { session.handleText(c, t.data(), t.size()); };
    h.http = [&](const host::HttpRequest &r) -> host::HttpResponse {
        if (r.method == "POST" && !host::HttpServer::sameOrigin(r))
            return json(403, "{\"error\":\"cross-origin request\"}");
        if (r.path == "/api/status")
            return json(200, session.statusJson());
        if (r.path == "/api/login" && r.method == "POST") {
            cJSON *j = cJSON_ParseWithLength(r.body.data(), r.body.size());
            const cJSON *pw = cJSON_GetObjectItem(j, "password");
            const bool ok = cJSON_IsString(pw) && admin == pw->valuestring;
            cJSON_Delete(j);
            if (!ok) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                return json(401, "{\"error\":\"wrong password\"}");
            }
            uint8_t rnd[16];
            platform::randomBytes(rnd, sizeof rnd);
            char tok[33];
            for (int i = 0; i < 16; i++)
                snprintf(tok + 2 * i, 3, "%02x", rnd[i]);
            tokens.push_back({tok, platform::micros() / 1000 + 3600000});
            if (tokens.size() > 8)
                tokens.erase(tokens.begin());
            return json(200, std::string("{\"token\":\"") + tok + "\",\"expiresIn\":3600}");
        }
        if (r.path == "/api/project" && r.method == "GET") {
            host::HttpResponse resp = json(200, session.exportProject());
            resp.headers.push_back({"Content-Disposition", "attachment; filename=\"project.esp32daw.json\""});
            return resp;
        }
        if (r.path == "/api/project" && r.method == "POST") {
            if (!authorized(r))
                return json(401, "{\"error\":\"log in first\"}");
            std::string err;
            if (!session.importProject(r.body.data(), r.body.size(), &err))
                return json(400, "{\"error\":\"" + err + "\"}");
            return json(200, "{\"ok\":true}");
        }
        // the sample library: list, and upload (POST /api/samples?name=<file name>, the raw file as the body)
        if (r.path == "/api/samples" && r.method == "GET")
            return json(200, session.samplesJson());
        if (r.path == "/api/samples" && r.method == "POST") {
            std::string name;
            bool reclaimModules = false;
            for (size_t p = 0; p < r.query.size();) {
                size_t e = r.query.find('&', p);
                if (e == std::string::npos)
                    e = r.query.size();
                if (r.query.compare(p, 5, "name=") == 0)
                    name = host::HttpServer::urlDecode(r.query.substr(p + 5, e - p - 5));
                if (r.query.substr(p, e - p) == "reclaim=modules") reclaimModules = true;
                p = e + 1;
            }
            std::string err;
            if (!session.importSample(name, (const uint8_t *)r.body.data(), r.body.size(), &err, reclaimModules)) {
                cJSON *e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", err.c_str());
                char *t = cJSON_PrintUnformatted(e);
                host::HttpResponse resp = json(400, t ? t : "{}");
                cJSON_free(t);
                cJSON_Delete(e);
                return resp;
            }
            return json(200, session.samplesJson());
        }
        // device settings (admin token). On Linux they are kept in <data>/settings.json; Wi-Fi
        // fields mean nothing here, but the browser's settings page works the same as on the device.
        if (r.path == "/api/settings") {
            if (!authorized(r))
                return json(401, "{\"error\":\"log in first\"}");
            const std::string file = data + "/settings.json";
            std::string cur;
            if (FILE *f = fopen(file.c_str(), "rb")) {
                char b[2048];
                cur.assign(b, fread(b, 1, sizeof b, f));
                fclose(f);
            }
            cJSON *st = cJSON_Parse(cur.c_str());
            if (!cJSON_IsObject(st)) {
                cJSON_Delete(st);
                st = cJSON_CreateObject();
            }
            auto field = [&](const char *k, const char *def) {
                const cJSON *v = cJSON_GetObjectItemCaseSensitive(st, k);
                return std::string(cJSON_IsString(v) ? v->valuestring : def);
            };
            if (r.method == "GET") {
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "wifiMode", field("wifiMode", "AP").c_str());
                cJSON_AddStringToObject(o, "ssid", field("ssid", "ESP32-DAW").c_str());
                cJSON_AddStringToObject(o, "hostname", field("hostname", "daw-host").c_str());
                cJSON_AddBoolToObject(o, "wifiPasswordSet", field("wifiPassword", "").size() >= 8);
                cJSON_AddBoolToObject(o, "adminPasswordSet", !admin.empty());
                cJSON_AddBoolToObject(o, "changedOnDevice", cur.size() > 0);
                char *t = cJSON_PrintUnformatted(o);
                host::HttpResponse resp = json(200, t ? t : "{}");
                cJSON_free(t);
                cJSON_Delete(o);
                cJSON_Delete(st);
                return resp;
            }
            SettingsUpdate u;
            std::string err;
            if (!parseSettingsUpdate(r.body.data(), r.body.size(), u, &err)) {
                cJSON_Delete(st);
                cJSON *e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", err.c_str());
                char *t = cJSON_PrintUnformatted(e);
                host::HttpResponse resp = json(400, t ? t : "{}");
                cJSON_free(t);
                cJSON_Delete(e);
                return resp;
            }
            auto put = [&](const char *k, const std::optional<std::string> &v) {
                if (!v)
                    return;
                cJSON_DeleteItemFromObjectCaseSensitive(st, k);
                cJSON_AddStringToObject(st, k, v->c_str());
            };
            put("wifiMode", u.wifiMode), put("ssid", u.ssid), put("wifiPassword", u.wifiPassword), put("hostname", u.hostname);
            char *t = cJSON_PrintUnformatted(st);
            if (FILE *f = fopen(file.c_str(), "wb")) {
                fputs(t ? t : "{}", f);
                fclose(f);
            }
            cJSON_free(t);
            cJSON_Delete(st);
            if (u.adminPassword) {  // takes effect now: every token is revoked
                admin = *u.adminPassword;
                tokens.clear();
            }
            return json(200, "{\"ok\":true,\"restart\":true}");
        }
        if (r.path == "/api/ota" || r.path == "/api/reboot")
            return authorized(r) ? json(501, "{\"error\":\"not available on the Linux host\"}") : json(401, "{\"error\":\"log in first\"}");
        if (r.path == "/api/factory-reset" && r.method == "POST") {
            if (!authorized(r))
                return json(401, "{\"error\":\"log in first\"}");
            for (uint32_t s = 0; s < store.slots(); s++)
                store.remove(s);
            while (!samples.list().empty())
                samples.remove(samples.list().front().name);
            while (!presetStore.list().empty())
                presetStore.remove(presetStore.list().front().name);
            presets.mount();
            session.start(defaultProject());
            return json(200, "{\"ok\":true}");
        }
        // tests only (--test-hooks): simulate render load for the CPU guard (POST body: permille)
        if (testHooks && r.path == "/api/test/cpu-load" && r.method == "POST") {
            eng.status().testLoadPermille.store((uint32_t)atoi(r.body.c_str()));
            return json(200, "{\"ok\":true}");
        }
        if (r.path.compare(0, 5, "/api/") == 0)
            return json(404, "{\"error\":\"not found\"}");
        // static files; client-side routes get index.html
        std::string p = r.path == "/" ? "/index.html" : r.path;
        if (p.find("..") != std::string::npos)
            return json(400, "{\"error\":\"bad path\"}");
        std::string body;
        if (!readFile(web + p, body)) {
            if (p.find('.') != std::string::npos || !readFile(web + "/index.html", body))
                return host::HttpResponse{404, "text/html", "<h1>404</h1><p>Build the browser DAW: scripts/build-web.sh</p>", {}};
            p = "/index.html";
        }
        host::HttpResponse resp{200, mimeOf(p), body, {}};
        resp.headers.push_back({"Cache-Control", p.find("/assets/") == 0 ? "public, max-age=31536000, immutable" : "no-cache"});
        return resp;
    };
    server.setHandlers(h);
    std::string err;
    if (!server.listen(bind, port, &err)) {
        fprintf(stderr, "%s\n", err.c_str());
        g_stop = true;
        audio.join();
        return 1;
    }
    printf("daw-host: http://%s:%u/ (plugins: %zu, data: %s)\n", bind.c_str(), port, PluginRegistry::all().size(), data.c_str());
    fflush(stdout);
    while (!g_stop) {
        server.poll(10);
        for (int c; (c = server.takeResync()) >= 0;)
            session.resync(c);
        session.tick((uint32_t)(platform::micros() / 1000));
    }
    session.stopForMaintenance();
    g_stop = true;
    audio.join();
    printf("daw-host: stopped\n");
    return 0;
}
