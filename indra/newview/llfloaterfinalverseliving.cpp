// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
// Ω-XVII.3 Living World console.
//
//   MutSea authority ─(finalverse.world-replication/1, FVP domain 3)─► libmutsea_replica
//     ─► this console (residents, WorldLine, cursor, recovery state)
//     ─► scene-update.json (registered positions, presentation-only) ─► Storm/Metal child
//     ─► captured frames ─► this console's viewport
//
// The console never writes world state itself except through the authority's own
// authenticated command gateway (an agent token with the Modify scope only), and
// MutSea's policy decides. AI requests go to the owning agent service through a
// private request/status channel; model output never reaches the authority directly.
#include "llviewerprecompiledheaders.h"
#include "llfloaterfinalverseliving.h"

#include "finalverse/convergence.h"
#include "mutsea/replica.h"

#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llimage.h"
#include "lllineeditor.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llsdjson.h"
#include "llviewertexture.h"
#include "llwindow.h"
#include "llviewerwindow.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/sha.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

// ------------------------------------------------------------------ design tokens
struct Theme
{
    LLColor4 ink{ .93f, .96f, 1.f, 1.f }, muted{ .62f, .68f, .78f, 1.f }, faint{ .42f, .47f, .56f, 1.f };
    LLColor4 glass{ .045f, .055f, .078f, .78f }, glassHi{ .085f, .1f, .14f, .86f }, line{ 1.f, 1.f, 1.f, .08f };
    LLColor4 accent{ .38f, .62f, 1.f, 1.f }, accent2{ .66f, .45f, 1.f, 1.f };
    LLColor4 live{ .2f, .86f, .55f, 1.f }, warn{ 1.f, .74f, .26f, 1.f }, bad{ 1.f, .38f, .38f, 1.f };
    LLColor4 bg{ .018f, .024f, .036f, 1.f };
};
const Theme T;

LLColor4 alpha(LLColor4 c, float a) { c.mV[3] = a; return c; }

void quarter(F32 cx, F32 cy, F32 r, F32 a0, const LLColor4& c)
{
    const int steps = 8;
    gGL.color4fv(c.mV);
    gGL.begin(LLRender::TRIANGLES);
    for (int i = 0; i < steps; ++i)
    {
        const F32 t0 = a0 + F_PI_BY_TWO * i / steps, t1 = a0 + F_PI_BY_TWO * (i + 1) / steps;
        gGL.vertex2f(cx, cy);
        gGL.vertex2f(cx + r * cosf(t0), cy + r * sinf(t0));
        gGL.vertex2f(cx + r * cosf(t1), cy + r * sinf(t1));
    }
    gGL.end();
}

void roundRect(S32 l, S32 t, S32 r, S32 b, F32 radius, const LLColor4& c)
{
    if (r <= l || t <= b) return;
    const S32 rr = (S32)llmin(radius, (F32)(r - l) * .5f, (F32)(t - b) * .5f);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    // Non-overlapping pieces, so translucent fills stay uniform.
    gl_rect_2d(l, t - rr, r, b + rr, c, true);
    gl_rect_2d(l + rr, t, r - rr, t - rr, c, true);
    gl_rect_2d(l + rr, b + rr, r - rr, b, c, true);
    quarter((F32)(r - rr), (F32)(t - rr), (F32)rr, 0.f, c);
    quarter((F32)(l + rr), (F32)(t - rr), (F32)rr, F_PI_BY_TWO, c);
    quarter((F32)(l + rr), (F32)(b + rr), (F32)rr, F_PI, c);
    quarter((F32)(r - rr), (F32)(b + rr), (F32)rr, F_PI + F_PI_BY_TWO, c);
}

void glassPanel(S32 l, S32 t, S32 r, S32 b, F32 radius = 14.f, bool hi = false)
{
    roundRect(l - 1, t + 1, r + 1, b - 1, radius + 1, T.line);
    roundRect(l, t, r, b, radius, hi ? T.glassHi : T.glass);
}

void dot(F32 x, F32 y, F32 radius, const LLColor4& c)
{
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.color4fv(c.mV);
    gl_circle_2d(x, y, radius, 20, true);
}

LLFontGL* font(int size, bool bold = false)
{
    if (size >= 3) return LLFontGL::getFontSansSerifHuge();
    if (size == 2) return LLFontGL::getFontSansSerifBig();
    if (size == 1) return bold ? LLFontGL::getFontSansSerifBold() : LLFontGL::getFontSansSerif();
    return bold ? LLFontGL::getFontSansSerifSmallBold() : LLFontGL::getFontSansSerifSmall();
}

S32 text(const std::string& s, S32 x, S32 y, const LLColor4& c, int size = 1, bool bold = false, LLFontGL::HAlign h = LLFontGL::LEFT, S32 maxPixels = S32_MAX)
{
    F32 right = 0;
    font(size, bold)->renderUTF8(s, 0, (F32)x, (F32)y, c, h, LLFontGL::BASELINE, bold ? LLFontGL::BOLD : LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, maxPixels, &right, true);
    return (S32)right;
}

S32 width(const std::string& s, int size = 1, bool bold = false) { return font(size, bold)->getWidth(s); }

// Greedy word wrap; returns the y below the last line.
S32 wrapped(const std::string& s, S32 x, S32 y, S32 maxWidth, const LLColor4& c, int size, S32 maxLines, S32 lineHeight)
{
    std::istringstream words(s);
    std::string word, line;
    S32 lines = 0;
    auto flush = [&] {
        if (!line.empty() && lines < maxLines) { text(line, x, y, c, size); y -= lineHeight; ++lines; }
        line.clear();
    };
    while (words >> word)
    {
        std::string next = line.empty() ? word : line + " " + word;
        if (width(next, size) > maxWidth && !line.empty()) { flush(); line = word; }
        else line = next;
    }
    flush();
    return y;
}

std::string hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) s += d[p[i] >> 4], s += d[p[i] & 15];
    return s;
}

std::string uuidText(const uint8_t* p)
{
    LLUUID id;
    std::memcpy(id.mData, p, 16);
    return id.asString();
}

bool parseUuid(const std::string& s, uint8_t out[16])
{
    if (!LLUUID::validate(s)) return false;
    LLUUID id(s);
    std::memcpy(out, id.mData, 16);
    return true;
}

std::string ago(double seconds)
{
    if (seconds < 1.5) return "just now";
    if (seconds < 60) return std::to_string((int)seconds) + " s ago";
    if (seconds < 3600) return std::to_string((int)(seconds / 60)) + " min ago";
    return std::to_string((int)(seconds / 3600)) + " h ago";
}

LLColor4 residentColor(const std::string& name)
{
    static const LLColor4 palette[] = { { .38f, .62f, 1.f, 1.f }, { .66f, .45f, 1.f, 1.f }, { .2f, .86f, .64f, 1.f }, { 1.f, .6f, .3f, 1.f },
                                        { 1.f, .42f, .62f, 1.f }, { .35f, .85f, .95f, 1.f }, { .95f, .85f, .3f, 1.f } };
    size_t h = std::hash<std::string>{}(name);
    return palette[h % (sizeof palette / sizeof palette[0])];
}

std::string readSmall(const fs::path& p, size_t bound)
{
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in.good()) return {};
    auto size = in.tellg();
    if (size <= 0 || (size_t)size > bound) return {};
    std::string s((size_t)size, '\0');
    in.seekg(0);
    in.read(s.data(), size);
    return in.good() ? s : std::string();
}

bool writePrivate(const fs::path& p, const std::string& bytes)
{
    try
    {
        auto tmp = p;
        tmp += ".part";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << bytes;
            if (!out.good()) return false;
        }
        fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write);
        fs::rename(tmp, p);
        return true;
    }
    catch (const std::exception&) { return false; }
}

bool privateDir(const std::string& d)
{
    try
    {
        fs::path p(d);
        return !d.empty() && p.is_absolute() && fs::is_directory(fs::symlink_status(p)) &&
               (fs::status(p).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none;
    }
    catch (const std::exception&) { return false; }
}

// ------------------------------------------------------------------ data model
struct Resident
{
    std::string id, asset, name, label;
    double region[3]{}, displayed[3]{};
    bool registered = false;
    uint64_t revision = 0;
};

struct Event
{
    uint64_t seq = 0, revision = 0;
    std::string action, kind, entity, hash;
};

struct Frame
{
    std::vector<uint8_t> rgba;
    uint64_t number = 0, triangles = 0, sourceRevision = 0;
    bool interpolating = false;
    std::string world;
};

struct AiCard
{
    std::string id, stage, title, answer, plan, policy, focus;
    std::vector<std::string> results;
};

struct View
{
    ms_replica_status status{};
    std::vector<ms_replica_entity> entities;
    std::vector<Resident> residents;
    std::deque<Event> events;
    std::shared_ptr<const Frame> frame;
    uint64_t sceneRevision = 0, sceneWrites = 0;
    std::string sceneError, registrationLabel, registrationDetail;
    bool registrationSurveyed = false, hasRegistration = false;
    AiCard ai;
    std::string actionNote;
};

// ------------------------------------------------------------------ bridge
struct Bridge
{
    // configuration
    std::string world, region, sessionHost, replicationHost, observer, worldName = "Montréal · Vieux-Port";
    uint16_t sessionPort = 0, replicationPort = 0;
    std::map<std::string, std::string> tokens;
    std::string frames, definition, interaction;
    std::set<std::string> boundBodies;
    fv_wc_registration* registration = nullptr;
    uint64_t registrationRevision = 0;
    std::string registrationLabel = "No geographic registration · region-local only", registrationDetail;
    bool hasRegistration = false;

    ms_replica* replica = nullptr;
    std::thread thread;
    std::atomic<bool> stop{ false };
    std::mutex m;
    std::shared_ptr<const View> view;
    std::string error;

    // scene-update state
    uint64_t lastSceneRevision = 0;
    bool wroteScene = false;
    uint64_t sceneWrites = 0;
    std::string sceneError;
    // AI + actions
    std::string aiRequestId;
    AiCard ai;
    std::string actionNote;
    std::vector<std::thread> actions;
    std::shared_ptr<const Frame> frame;
    fs::file_time_type frameTime{};

    ~Bridge()
    {
        stop = true;
        if (thread.joinable()) thread.join();
        for (auto& t : actions)
            if (t.joinable()) t.join();
        if (replica) ms_replica_stop(replica);
        if (registration) fv_wc_registration_close(registration);
    }

    static std::pair<std::string, uint16_t> hostPort(const std::string& s)
    {
        auto c = s.rfind(':');
        if (c == std::string::npos) return { s, 0 };
        return { s.substr(0, c), (uint16_t)std::atoi(s.substr(c + 1).c_str()) };
    }

    bool start(std::string& why)
    {
        const char* tokenPath = std::getenv("FINALVERSE_LIVE_TOKENS");
        if (!tokenPath) { why = "FINALVERSE_LIVE_TOKENS is not set"; return false; }
        try
        {
            auto raw = readSmall(tokenPath, 65536);
            auto j = boost::json::parse(raw).as_object();
            world = std::string(j.at("world").as_string());
            region = std::string(j.at("region").as_string());
            std::tie(sessionHost, sessionPort) = hostPort(std::string(j.at("session").as_string()));
            std::tie(replicationHost, replicationPort) = hostPort(std::string(j.at("replication").as_string()));
            for (auto& [k, v] : j.at("tokens").as_object()) tokens[std::string(k)] = std::string(v.as_string());
            observer = tokens.at("observer");
        }
        catch (const std::exception& e) { why = std::string("live token file: ") + e.what(); return false; }
        if (const char* n = std::getenv("FINALVERSE_LIVE_WORLD_NAME")) worldName = n;
        if (const char* f = std::getenv("FINALVERSE_STORM_FRAMES")) frames = f;
        if (const char* d = std::getenv("FINALVERSE_STORM_DEFINITION_SHA256")) definition = d;
        if (const char* i = std::getenv("FINALVERSE_WORLD_INTERACTION_DIR")) interaction = i;
        if (const char* fwd = std::getenv("FINALVERSE_LIVE_FWD"))
        {
            try
            {
                auto d = boost::json::parse(readSmall(fwd, 8 << 20)).as_object();
                std::map<std::string, bool> residentAsset;
                for (auto& a : d.at("assets").as_array())
                {
                    bool r = false;
                    for (auto& s : a.at("source_ids").as_array()) r = r || std::string(s.as_string()).starts_with("resident-");
                    residentAsset[std::string(a.at("id").as_string())] = r;
                }
                for (auto& b : d.at("simulation_bindings").as_array())
                    if (residentAsset[std::string(b.at("asset_id").as_string())] && std::string(b.at("world_id").as_string()) == world)
                        boundBodies.insert(std::string(b.at("entity_id").as_string()));
            }
            catch (const std::exception&) { boundBodies.clear(); }
        }
        if (const char* reg = std::getenv("FINALVERSE_GEO_REGISTRATION"))
        {
            auto raw = readSmall(reg, 16384);
            try
            {
                auto j = boost::json::parse(raw).as_object();
                registrationRevision = j.at("revision").to_number<uint64_t>();
                auto& v = j.at("validation").as_object();
                const bool surveyed = v.at("surveyed").as_bool();
                const std::string status(v.at("status").as_string()), scope(j.at("authorization").as_object().at("scope").as_string());
                char e[256] = {};
                if (fv_wc_registration_open(raw.data(), raw.size(), world.c_str(), region.c_str(), registrationRevision, &registration, e, sizeof e) == 0)
                {
                    hasRegistration = true;
                    // The classification is displayed exactly as declared; never upgraded.
                    registrationLabel = surveyed ? "Surveyed registration" : "Presentation-only registration · not surveyed";
                    registrationDetail = status + " · scope " + scope + " · EPSG:2950 → region-local · rev " + std::to_string(registrationRevision);
                }
                else registrationLabel = std::string("Registration refused: ") + e;
            }
            catch (const std::exception& ex) { registrationLabel = std::string("Registration unreadable: ") + ex.what(); }
        }
        ms_replica_config cfg{};
        cfg.struct_size = sizeof cfg;
        cfg.host = replicationHost.c_str();
        cfg.port = replicationPort;
        cfg.token = observer.c_str();
        if (!parseUuid(world, cfg.world) || !parseUuid(region, cfg.region)) { why = "invalid world/region"; return false; }
        std::string checkpoint;
        if (const char* c = std::getenv("FINALVERSE_LIVE_CHECKPOINT")) checkpoint = c;
        cfg.checkpoint_path = checkpoint.empty() ? nullptr : checkpoint.c_str();
        if (ms_replica_start(&cfg, &replica) != MS_REPLICA_OK) { why = "native replica did not start"; return false; }
        thread = std::thread([this] { run(); });
        return true;
    }

    // ---------------------------------------------------------------- storm frames
    std::shared_ptr<const Frame> readFrame()
    {
        if (!privateDir(frames)) return nullptr;
        try
        {
            fs::path dir(frames);
            auto t = fs::last_write_time(dir / "frame.json");
            if (t == frameTime && frame) return frame;
            auto age = fs::file_time_type::clock::now() - t;
            if (age > std::chrono::seconds(3)) return nullptr;
            auto meta = readSmall(dir / "frame.json", 8192);
            auto j = boost::json::parse(meta).as_object();
            if (std::string(j.at("schema").as_string()) != "finalverse.storm-view-frame/0.1") return nullptr;
            if (!definition.empty() && std::string(j.at("definition_sha256").as_string()) != definition) return nullptr;
            auto bytes = readSmall(dir / "frame.ppm", 1280 * 720 * 3 + 128);
            if (bytes.empty()) return nullptr;
            uint8_t h[32];
            SHA256(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), h);
            if (hex(h, 32) != std::string(j.at("pixels_sha256").as_string())) return nullptr;  // torn write
            std::istringstream hdr(bytes.substr(0, 64));
            std::string magic;
            int w = 0, hh = 0, mx = 0;
            hdr >> magic >> w >> hh >> mx;
            hdr.get();
            auto off = (size_t)hdr.tellg();
            if (magic != "P6" || w != 1280 || hh != 720 || bytes.size() != off + 1280 * 720 * 3) return nullptr;
            auto f = std::make_shared<Frame>();
            f->world = std::string(j.at("world").as_string());
            f->number = j.at("frame").to_number<uint64_t>();
            f->triangles = j.at("triangles").to_number<uint64_t>();
            if (auto* sr = j.if_contains("source_revision")) f->sourceRevision = sr->to_number<uint64_t>();
            if (auto* ia = j.if_contains("interpolation_active"); ia && ia->is_bool()) f->interpolating = ia->as_bool();
            f->rgba.resize(1280 * 720 * 4);
            for (size_t i = 0; i < 1280 * 720; ++i)
            {
                for (size_t k = 0; k < 3; ++k) f->rgba[i * 4 + k] = (uint8_t)bytes[off + i * 3 + k];
                f->rgba[i * 4 + 3] = 255;
            }
            frameTime = t;
            return f;
        }
        catch (const std::exception&) { return nullptr; }
    }

    // ---------------------------------------------------------------- scene update
    void writeScene(const ms_replica_status& s, const std::vector<Resident>& residents)
    {
        if (!privateDir(frames) || definition.size() != 64 || !hasRegistration) return;
        // A restarted renderer starts from a new output directory: send it the full state again.
        const bool present = fs::exists(fs::path(frames) / "scene-update.json");
        if (wroteScene && s.revision == lastSceneRevision && present) return;
        if (!present) wroteScene = false;
        boost::json::array list;
        for (const auto& r : residents)
        {
            if (!r.registered || r.asset.empty() || !boundBodies.count(r.id) || list.size() >= 16) continue;
            list.push_back(boost::json::object{ { "resident", r.asset }, { "entity", r.id },
                                                { "position", boost::json::array{ r.displayed[0], r.displayed[1], r.displayed[2] } },
                                                { "source_position_region_m", boost::json::array{ r.region[0], r.region[1], r.region[2] } },
                                                { "motion", "committed" } });
        }
        boost::json::object u{ { "schema", "finalverse.scene-update/0.1" }, { "world", world }, { "region", region },
                               { "epoch", hex(s.generation, 16) }, { "cursor", s.revision }, { "revision", s.revision },
                               { "previous_revision", wroteScene ? boost::json::value(lastSceneRevision) : boost::json::value(nullptr) },
                               { "full_snapshot", true }, { "definition_sha256", definition }, { "residents", list },
                               { "assets", boost::json::array{} }, { "event_refs", boost::json::array{} } };
        if (writePrivate(fs::path(frames) / "scene-update.json", boost::json::serialize(u)))
        {
            wroteScene = true;
            lastSceneRevision = s.revision;
            ++sceneWrites;
            sceneError.clear();
        }
        else sceneError = "scene update write failed";
    }

    // ---------------------------------------------------------------- AI channel
    void pollAi()
    {
        if (aiRequestId.empty() || !privateDir(interaction)) return;
        auto raw = readSmall(fs::path(interaction) / "status.json", 65536);
        if (raw.empty()) return;
        try
        {
            auto j = boost::json::parse(raw).as_object();
            if (std::string(j.at("schema").as_string()) != "finalverse.living-status/0.1" || std::string(j.at("id").as_string()) != aiRequestId) return;
            AiCard c;
            c.id = aiRequestId;
            c.stage = std::string(j.at("stage").as_string());
            auto str = [&](const char* k) { auto* v = j.if_contains(k); return v && v->is_string() ? std::string(v->as_string()) : std::string(); };
            c.title = str("title");
            c.answer = str("answer");
            c.policy = str("policy");
            c.focus = str("focus_entity");
            if (auto* p = j.if_contains("plan")) c.plan = boost::json::serialize(*p);
            if (auto* r = j.if_contains("results"); r && r->is_array())
                for (auto& x : r->as_array())
                    if (x.is_string() && c.results.size() < 8) c.results.push_back(std::string(x.as_string()));
            std::lock_guard g(m);
            ai = c;
        }
        catch (const std::exception&) {}
    }

    bool askAi(const std::string& text, const std::string& selected, uint64_t revision)
    {
        if (!privateDir(interaction) || text.size() > 1024) return false;
        aiRequestId = LLUUID::generateNewID().asString();
        boost::json::object req{ { "schema", "finalverse.living-request/0.1" }, { "id", aiRequestId }, { "world", world }, { "region", region },
                                 { "text", text }, { "selected_entity", selected }, { "source_revision", std::to_string(revision) } };
        {
            std::lock_guard g(m);
            ai = AiCard{};
            ai.id = aiRequestId;
            ai.stage = "planning";
            ai.title = text;
            ai.answer = "The local model is proposing a typed plan…";
        }
        return writePrivate(fs::path(interaction) / "request.json", boost::json::serialize(req));
    }

    // ---------------------------------------------------------------- authorized movement
    static int dial(const std::string& host, uint16_t port)
    {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return -1;
        int fd = -1;
        for (auto* a = res; a && fd < 0; a = a->ai_next)
        {
            fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd >= 0 && ::connect(fd, a->ai_addr, a->ai_addrlen) != 0) { ::close(fd); fd = -1; }
        }
        freeaddrinfo(res);
        if (fd >= 0)
        {
            timeval tv{ 5, 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        }
        return fd;
    }
    static bool sendMsg(int fd, const boost::json::value& v)
    {
        auto body = boost::json::serialize(v);
        uint32_t n = htonl((uint32_t)body.size());
        std::string f(reinterpret_cast<const char*>(&n), 4);
        f += body;
        return ::send(fd, f.data(), f.size(), 0) == (ssize_t)f.size();
    }
    static bool recvMsg(int fd, boost::json::object& out)
    {
        uint8_t len[4];
        size_t got = 0;
        while (got < 4) { auto n = ::recv(fd, len + got, 4 - got, 0); if (n <= 0) return false; got += (size_t)n; }
        uint32_t n = (uint32_t)len[0] << 24 | (uint32_t)len[1] << 16 | (uint32_t)len[2] << 8 | len[3];
        if (n > (1u << 20)) return false;
        std::string body(n, '\0');
        got = 0;
        while (got < n) { auto r = ::recv(fd, body.data() + got, n - got, 0); if (r <= 0) return false; got += (size_t)r; }
        try { out = boost::json::parse(body).as_object(); return true; } catch (...) { return false; }
    }

    void move(const ms_replica_entity e, double dx, double dy)
    {
        std::string owner = uuidText(e.owner), token;
        if (auto it = tokens.find("agent:" + owner); it != tokens.end()) token = it->second;
        std::string label = e.label;
        if (token.empty()) { std::lock_guard g(m); actionNote = "No agent mandate for " + label + "; nothing was submitted."; return; }
        {
            std::lock_guard g(m);
            actionNote = "Submitting a bounded move for " + label + " to the MutSea authority…";
        }
        const auto started = Clock::now();
        std::string note;
        int fd = dial(sessionHost, sessionPort);
        if (fd < 0) note = "The authority's command gateway is unreachable. Nothing was committed.";
        else
        {
            boost::json::object hello{ { "type", "hello" }, { "schema", "mutsea.local-session/v0" }, { "token", token }, { "region", region },
                                       { "interest", boost::json::object{ { "kind", "region" } } } };
            boost::json::object msg;
            bool ok = sendMsg(fd, hello);
            while (ok && (ok = recvMsg(fd, msg)) && std::string(msg.at("type").as_string()) != "snapshot")
                if (std::string(msg.at("type").as_string()) == "error") { ok = false; note = "Refused: " + std::string(msg.at("reason").as_string()); }
            if (ok)
            {
                boost::json::object transform{ { "position", boost::json::array{ e.position[0] + dx, e.position[1] + dy, e.position[2] } },
                                               { "rotation", boost::json::array{ e.rotation[0], e.rotation[1], e.rotation[2], e.rotation[3] } },
                                               { "scale", boost::json::array{ e.scale[0], e.scale[1], e.scale[2] } } };
                boost::json::object cmd{ { "schema", "mutsea.local-command/v0" }, { "command_id", LLUUID::generateNewID().asString() }, { "expected_revision", e.revision },
                                         { "payload", boost::json::object{ { "type", "set_transform" }, { "entity", uuidText(e.id) }, { "transform", transform } } } };
                ok = sendMsg(fd, boost::json::object{ { "type", "command" }, { "command", cmd } });
                while (ok && (ok = recvMsg(fd, msg)))
                {
                    const std::string type(msg.at("type").as_string());
                    if (type == "receipt")
                    {
                        auto& outcome = msg.at("receipt").as_object().at("outcome").as_object();
                        const auto ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
                        std::ostringstream s;
                        s << std::fixed << std::setprecision(0);
                        if (std::string(outcome.at("status").as_string()) == "applied")
                            s << "Committed by MutSea in " << ms << " ms · " << label << " moved " << std::hypot(dx, dy) << " m · durable before acknowledgement";
                        else s << "MutSea rejected the move: " << boost::json::serialize(outcome.at("reason"));
                        note = s.str();
                        break;
                    }
                    if (type == "refused" || type == "error") { note = "Refused: " + std::string(msg.at("reason").as_string()); break; }
                }
                sendMsg(fd, boost::json::object{ { "type", "bye" } });
            }
            if (!ok && note.empty()) note = "The command channel closed; commit status is unknown until the stream shows it.";
            ::close(fd);
        }
        std::lock_guard g(m);
        actionNote = note;
    }

    void requestMove(const ms_replica_entity& e, double dx, double dy)
    {
        std::lock_guard g(m);
        if (actions.size() > 16)
        {
            for (auto& t : actions) if (t.joinable()) t.join();
            actions.clear();
        }
        actions.emplace_back([this, e, dx, dy] { move(e, dx, dy); });
    }

    // ---------------------------------------------------------------- loop
    void run()
    {
        std::deque<Event> events;
        uint64_t lastVersion = UINT64_MAX;
        std::vector<ms_replica_entity> entities;
        std::vector<Resident> residents;
        auto lastFrameRead = Clock::now() - std::chrono::seconds(1);
        while (!stop)
        {
            auto v = std::make_shared<View>();
            v->status.struct_size = sizeof v->status;
            ms_replica_get_status(replica, &v->status);
            if (v->status.version != lastVersion)
            {
                lastVersion = v->status.version;
                size_t n = 0;
                entities.resize(512);
                ms_replica_entities(replica, entities.data(), entities.size(), &n, nullptr);
                entities.resize(std::min<size_t>(n, 512));
                residents.clear();
                for (const auto& e : entities)
                {
                    std::string label = e.label;
                    if (!label.starts_with("AI resident")) continue;
                    Resident r;
                    r.id = uuidText(e.id);
                    r.label = label;
                    auto first = label.find("·"), second = label.find("·", first == std::string::npos ? 0 : first + 2);
                    r.name = first != std::string::npos ? label.substr(first + 3, second == std::string::npos ? std::string::npos : second - first - 4) : label;
                    LLStringUtil::trim(r.name);
                    std::copy(e.position, e.position + 3, r.region);
                    r.revision = e.revision;
                    if (registration && fv_wc_to_asset(registration, registrationRevision, r.region, r.displayed) == 0) r.registered = true;
                    residents.push_back(r);
                }
                // Resident asset identities come from the authoritative entity JSON.
                char* json = nullptr;
                size_t len = 0;
                if (ms_replica_entities_json(replica, &json, &len) == MS_REPLICA_OK)
                {
                    try
                    {
                        auto arr = boost::json::parse(std::string_view(json, len)).as_array();
                        std::map<std::string, std::string> assets;
                        for (auto& x : arr)
                        {
                            auto& o = x.as_object();
                            if (auto* a = o.if_contains("asset"); a && a->is_object()) assets[std::string(o.at("id").as_string())] = std::string(a->as_object().at("id").as_string());
                        }
                        for (auto& r : residents) r.asset = assets[r.id];
                    }
                    catch (const std::exception&) {}
                    ms_replica_free(json);
                }
                if (ms_replica_recent_events_json(replica, &json, &len) == MS_REPLICA_OK)
                {
                    try
                    {
                        events.clear();
                        const boost::json::value doc = boost::json::parse(std::string_view(json, len));  // keep alive for the loop
                        for (auto& x : doc.as_array())
                        {
                            auto& o = x.as_object();
                            Event ev;
                            ev.seq = o.at("seq").to_number<uint64_t>();
                            ev.revision = o.at("revision").to_number<uint64_t>();
                            ev.hash = std::string(o.at("hash").as_string());
                            auto& entry = o.at("entry").as_object();
                            ev.action = std::string(entry.at("action").as_string());
                            ev.kind = std::string(entry.at("principal_kind").as_string());
                            if (auto& eff = entry.at("effects").as_array(); !eff.empty())
                                if (auto* en = eff[0].as_object().if_contains("entity"); en && en->is_string()) ev.entity = std::string(en->as_string());
                            events.push_back(ev);
                        }
                    }
                    catch (const std::exception& ex) { LL_WARNS_ONCE("FinalverseLiving") << "WorldLine feed parse: " << ex.what() << LL_ENDL; }
                    ms_replica_free(json);
                }
            }
            if (v->status.state == MS_REPLICA_LIVE) writeScene(v->status, residents);
            if (Clock::now() - lastFrameRead > std::chrono::milliseconds(120))
            {
                frame = readFrame();
                lastFrameRead = Clock::now();
            }
            pollAi();
            v->entities = entities;
            v->residents = residents;
            v->events = events;
            v->frame = frame;
            v->sceneRevision = lastSceneRevision;
            v->sceneWrites = sceneWrites;
            v->sceneError = sceneError;
            v->registrationLabel = registrationLabel;
            v->registrationDetail = registrationDetail;
            v->hasRegistration = hasRegistration;
            {
                std::lock_guard g(m);
                v->ai = ai;
                v->actionNote = actionNote;
                view = v;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    }
};

struct Hit
{
    LLRect rect;
    std::function<void()> action;
};
}  // namespace

// ====================================================================== floater
struct LLFloaterFinalverseLiving::Impl
{
    std::unique_ptr<Bridge> bridge;
    std::string startError;
    std::shared_ptr<const View> view;
    LLPointer<LLViewerTexture> image;
    uint64_t imageFrame = 0;
    std::string selected;  // entity id
    std::map<std::string, std::pair<uint64_t, Clock::time_point>> lastMove;
    std::map<uint64_t, Clock::time_point> eventSeen;
    std::vector<Hit> hits;
    Clock::time_point opened = Clock::now();
    // camera presentation
    float yaw = .4f, pitch = -.42f, distance = 220.f;
    bool orbit = false;
    std::string cameraTarget, lighting = "daylight";
    bool follow = false;
    uint64_t lastLogged = UINT64_MAX;
    Clock::time_point lastCamera = Clock::now();
    // Optional reproducible demo script (FINALVERSE_LIVE_DEMO_SCRIPT): "<seconds> <command>" lines run
    // through the same code paths as the controls: ask, select, focus, move NAME DX DY, overview.
    std::vector<std::pair<double, std::string>> script;
    size_t scriptNext = 0;

    void camera()
    {
        if (!bridge || !privateDir(bridge->frames)) return;
        boost::json::object c{ { "schema", "finalverse.presentation-control/0.1" }, { "yaw", yaw }, { "pitch", pitch }, { "distance", distance },
                               { "orbit", orbit }, { "pan", boost::json::array{ 0., 0., 0. } }, { "lighting", lighting } };
        if (!cameraTarget.empty()) c["target_resident"] = cameraTarget;
        writePrivate(fs::path(bridge->frames) / "camera.json", boost::json::serialize(c));
    }
    const Resident* resident(const std::string& id) const
    {
        if (!view) return nullptr;
        for (const auto& r : view->residents)
            if (r.id == id) return &r;
        return nullptr;
    }
    const ms_replica_entity* entity(const std::string& id) const
    {
        if (!view) return nullptr;
        uint8_t raw[16];
        if (!parseUuid(id, raw)) return nullptr;
        for (const auto& e : view->entities)
            if (!std::memcmp(e.id, raw, 16)) return &e;
        return nullptr;
    }
    void focus(const std::string& id, float dist = 26.f)
    {
        selected = id;
        if (const auto* r = resident(id); r && !r->asset.empty()) cameraTarget = r->asset;
        distance = dist;
        pitch = -.38f;
        camera();
    }
    void overview()
    {
        cameraTarget.clear();
        follow = false;
        distance = 420.f;
        pitch = -.55f;
        orbit = false;
        camera();
    }
};

LLFloaterFinalverseLiving::LLFloaterFinalverseLiving(const LLSD& key) : LLFloater(key), m(std::make_unique<Impl>()) {}
LLFloaterFinalverseLiving::~LLFloaterFinalverseLiving() = default;

bool LLFloaterFinalverseLiving::postBuild()
{
    getChild<LLLineEditor>("ask")->setCommitCallback([this](LLUICtrl* c, const LLSD&) {
        auto* e = static_cast<LLLineEditor*>(c);
        std::string t = e->getText();
        LLStringUtil::trim(t);
        if (!t.empty()) ask(t);
        e->setText(LLStringUtil::null);
    });
    setBackgroundVisible(false);
    arrange();
    return true;
}

void LLFloaterFinalverseLiving::onOpen(const LLSD&)
{
    if (getParent())
    {
        // Immersive: fill the Viewer window with a small margin.
        const auto available = getParent()->getLocalRect();
        setShape(LLRect(6, available.getHeight() - 4, available.getWidth() - 6, 6));
    }
    m->bridge = std::make_unique<Bridge>();
    if (!m->bridge->start(m->startError)) m->bridge.reset();
    m->opened = Clock::now();
    m->overview();
    if (const char* sp = std::getenv("FINALVERSE_LIVE_DEMO_SCRIPT"))
    {
        std::ifstream in(sp);
        std::string line;
        while (std::getline(in, line) && m->script.size() < 256)
        {
            std::istringstream ls(line);
            double at = 0;
            std::string rest;
            if (ls >> at && std::getline(ls, rest)) { LLStringUtil::trim(rest); if (!rest.empty()) m->script.emplace_back(at, rest); }
        }
    }
}

void LLFloaterFinalverseLiving::onClose(bool)
{
    m->bridge.reset();
    m->view.reset();
    m->image = nullptr;
}

void LLFloaterFinalverseLiving::reshape(S32 w, S32 h, bool parent)
{
    LLFloater::reshape(w, h, parent);
    if (getChildView("ask", false)) arrange();
}

void LLFloaterFinalverseLiving::arrange()
{
    const S32 w = getRect().getWidth();
    const S32 barL = 340, barR = w - 240, barW = barR - barL;
    LLRect r;
    r.setOriginAndSize(barL + 44, 34, barW - 44 - 92, 26);
    getChildView("ask")->setShape(r);
}

void LLFloaterFinalverseLiving::ask(const std::string& text)
{
    std::string lower = text;
    LLStringUtil::toLower(lower);
    auto pick = [&](const char* name) -> std::string {
        if (!m->view) return {};
        for (const auto& r : m->view->residents)
        {
            std::string n = r.name;
            LLStringUtil::toLower(n);
            if (n == name) return r.id;
        }
        return {};
    };
    // Presentation commands are local and never touch world state.
    if (lower == "sunset" || lower == "daylight") { m->lighting = lower; m->camera(); return; }
    if (lower == "overview") { m->overview(); return; }
    if (lower.starts_with("follow "))
    {
        auto id = pick(lower.substr(7).c_str());
        if (!id.empty()) { m->follow = true; m->focus(id); }
        return;
    }
    // Everything else is a world request: the agent service plans, policy validates,
    // deterministic services execute and MutSea authorizes any mutation.
    if (m->bridge)
        m->bridge->askAi(text, m->selected, m->view ? m->view->status.revision : 0);
}

bool LLFloaterFinalverseLiving::handleMouseDown(S32 x, S32 y, MASK mask)
{
    for (const auto& h : m->hits)
        if (h.rect.pointInRect(x, y)) { h.action(); return true; }
    return LLFloater::handleMouseDown(x, y, mask);
}

bool LLFloaterFinalverseLiving::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    m->distance = llclamp(m->distance * std::pow(1.12f, (F32)clicks), 4.f, 1800.f);
    m->camera();
    return true;
}

bool LLFloaterFinalverseLiving::handleKeyHere(KEY key, MASK mask)
{
    if (getChild<LLLineEditor>("ask")->hasFocus()) return LLFloater::handleKeyHere(key, mask);
    if (mask != MASK_NONE) return LLFloater::handleKeyHere(key, mask);
    if (key == 'F' && !m->selected.empty()) m->focus(m->selected);
    else if (key == KEY_HOME) m->overview();
    else if (key == 'O') { m->orbit = !m->orbit; m->camera(); }
    else if (key == KEY_LEFT) { m->yaw -= .12f; m->camera(); }
    else if (key == KEY_RIGHT) { m->yaw += .12f; m->camera(); }
    else if (key == KEY_UP) { m->pitch = llclamp(m->pitch + .06f, -1.4f, -.08f); m->camera(); }
    else if (key == KEY_DOWN) { m->pitch = llclamp(m->pitch - .06f, -1.4f, -.08f); m->camera(); }
    else return LLFloater::handleKeyHere(key, mask);
    return true;
}

void LLFloaterFinalverseLiving::draw()
{
    const S32 W = getRect().getWidth(), H = getRect().getHeight();
    const auto now = Clock::now();
    const double t = std::chrono::duration<double>(now - m->opened).count();
    m->hits.clear();
    if (m->bridge)
    {
        std::lock_guard g(m->bridge->m);
        m->view = m->bridge->view;
    }
    const View* v = m->view.get();
    const auto state = v ? v->status.state : MS_REPLICA_IDLE;
    const bool live = state == MS_REPLICA_LIVE;
    const bool hasData = v && (v->status.snapshots || v->status.commits_applied);

    // Track movement and new WorldLine records for highlights.
    if (v)
    {
        for (const auto& r : v->residents)
        {
            auto& lm = m->lastMove[r.id];
            if (lm.first != r.revision) { lm = { r.revision, lm.first ? now : now - std::chrono::hours(1) }; }
        }
        for (const auto& e : v->events)
            if (!m->eventSeen.count(e.seq)) m->eventSeen[e.seq] = m->eventSeen.empty() ? now - std::chrono::hours(1) : now;
        if (m->selected.empty() && !v->residents.empty())
        {
            for (const auto& r : v->residents)
                if (r.name == "Elara") m->selected = r.id;
            if (m->selected.empty()) m->selected = v->residents.front().id;
        }
        if (m->lastLogged != v->status.revision)
        {
            LL_INFOS("FinalverseLiving") << "live revision=" << v->status.revision << " worldline=" << v->status.worldline_length << " entities=" << v->status.entity_count
                                         << " state=" << (int)state << " mode=" << v->status.mode << " scene_revision=" << v->sceneRevision
                                         << " frame=" << (v->frame ? v->frame->number : 0) << " frame_source_revision=" << (v->frame ? v->frame->sourceRevision : 0) << LL_ENDL;
            m->lastLogged = v->status.revision;
        }
    }

    // ---- viewport: Storm frame, full bleed, cropped not stretched
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gl_rect_2d(0, H, W, 0, T.bg, true);
    if (v && v->frame && v->frame->world == (m->bridge ? m->bridge->world : ""))
    {
        if (v->frame->number != m->imageFrame || m->image.isNull())
        {
            LLPointer<LLImageRaw> raw = new LLImageRaw(2048, 1024, 4);
            {
                LLImageDataLock lock(raw);
                if (!raw->isBufferInvalid())
                {
                    std::memset(raw->getData(), 0, 2048 * 1024 * 4);
                    for (int y = 0; y < 720; y++) std::memcpy(raw->getData() + y * 2048 * 4, v->frame->rgba.data() + (719 - y) * 1280 * 4, 1280 * 4);
                }
            }
            if (!raw->isBufferInvalid())
            {
                if (m->image.isNull()) m->image = LLViewerTextureManager::getLocalTexture(raw.get(), false);
                else m->image->setSubImage(raw.get(), 0, 0, 2048, 1024);
                m->imageFrame = v->frame->number;
            }
        }
        const float aspect = (float)W / std::max(1, H), imageAspect = 1280.f / 720.f;
        float l = 0.f, r = 1280.f / 2048.f, b = 0.f, tp = 720.f / 1024.f;
        if (aspect > imageAspect) { const float span = (tp - b) * imageAspect / aspect; b = (tp - span) * .5f; tp = b + span; }
        else { const float span = (r - l) * aspect / imageAspect; l = (r - span) * .5f; r = l + span; }
        gGL.color4f(1, 1, 1, 1);
        gl_draw_scaled_image(0, 0, W, H, m->image, live ? LLColor4::white : LLColor4(.55f, .55f, .6f, 1.f), LLRectf(l, tp, r, b));
    }
    else
    {
        // Ambient grid while no frame is available.
        for (S32 x = 0; x < W; x += 48) gl_line_2d(x, 0, x, H, LLColor4(1, 1, 1, .025f));
        for (S32 y = 0; y < H; y += 48) gl_line_2d(0, y, W, y, LLColor4(1, 1, 1, .025f));
    }
    // vignettes for legibility
    for (int i = 0; i < 24; ++i)
    {
        const float a = .5f * (1.f - i / 24.f);
        gl_rect_2d(0, H - i * 4, W, H - i * 4 - 4, LLColor4(0, 0, 0, a * .55f), true);
        gl_rect_2d(0, i * 5 + 5, W, i * 5, LLColor4(0, 0, 0, a * .6f), true);
    }

    // ---- top bar
    const S32 topY = H - 22;  // below the floater title strip
    {
        // brand mark: two overlapping gradient dots
        dot(30, topY - 28, 11, T.accent);
        dot(38, topY - 28, 11, alpha(T.accent2, .85f));
        text("FINALVERSE", 58, topY - 24, T.ink, 1, true);
        text("LIVING WORLD", 58, topY - 40, T.muted, 0);
        const std::string worldName = m->bridge ? m->bridge->worldName : "No world configured";
        text(worldName, W / 2, topY - 26, T.ink, 2, false, LLFontGL::HCENTER);
        if (m->bridge)
            text("MutSea region " + m->bridge->region.substr(0, 8) + " · world " + m->bridge->world.substr(0, 8), W / 2, topY - 44, T.faint, 0, false, LLFontGL::HCENTER);
        // status pill
        std::string label = "OFFLINE";
        LLColor4 c = T.bad;
        if (!m->bridge) label = "NOT CONFIGURED";
        else if (live) { label = "LIVE"; c = T.live; }
        else if (state == MS_REPLICA_SYNCING || state == MS_REPLICA_CONNECTING) { label = state == MS_REPLICA_CONNECTING ? "CONNECTING" : "SYNCING"; c = T.warn; }
        else if (state == MS_REPLICA_REFUSED) label = "REFUSED";
        else if (state == MS_REPLICA_BACKOFF) label = "RECONNECTING";
        std::ostringstream detail;
        if (v)
        {
            detail << "rev " << v->status.revision << "  ·  WL " << v->status.worldline_length;
            if (v->status.commits_applied) detail << "  ·  " << std::fixed << std::setprecision(2) << v->status.last_latency_ms << " ms";
        }
        const S32 pw = width(label, 0, true) + width(detail.str(), 0) + 54, px = W - 20 - pw;
        glassPanel(px, topY - 12, W - 20, topY - 44, 16.f, true);
        const float pulse = live ? (float)(.55 + .45 * std::sin(t * 3.2)) : 1.f;
        dot((F32)px + 18, (F32)topY - 28, 9 * pulse + 2, alpha(c, .22f));
        dot((F32)px + 18, (F32)topY - 28, 5, c);
        S32 after = text(label, px + 32, topY - 33, c, 0, true);
        text(detail.str(), after + 10, topY - 33, T.muted, 0);
    }

    // ---- residents rail
    const S32 railL = 20, railR = 320, railTop = H - 92, railBottom = 150;
    glassPanel(railL, railTop, railR, railBottom);
    {
        text("RESIDENTS", railL + 18, railTop - 28, T.muted, 0, true);
        if (v) text(std::to_string(v->residents.size()), railR - 18, railTop - 28, T.faint, 0, true, LLFontGL::RIGHT);
        S32 y = railTop - 46;
        if (v)
            for (const auto& r : v->residents)
            {
                if (y - 62 < railBottom + 8) break;
                const bool sel = r.id == m->selected;
                const LLColor4 c = residentColor(r.name);
                const double since = std::chrono::duration<double>(now - m->lastMove[r.id].second).count();
                const bool fresh = since < 3.0;
                if (sel) roundRect(railL + 10, y, railR - 10, y - 58, 12, LLColor4(c.mV[0], c.mV[1], c.mV[2], .16f));
                if (fresh) roundRect(railL + 10, y, railL + 13, y - 58, 1.5f, alpha(T.live, (float)(1.0 - since / 3.0)));
                dot((F32)railL + 38, (F32)y - 29, 17, alpha(c, .25f));
                dot((F32)railL + 38, (F32)y - 29, 13, c);
                text(r.name.substr(0, 1), railL + 38, y - 34, T.bg, 1, true, LLFontGL::HCENTER);
                text(r.name, railL + 62, y - 24, T.ink, 1, true);
                std::ostringstream sub;
                sub << "AI resident  ·  " << (since > 3000 ? "rev " + std::to_string(r.revision) : "moved " + ago(since));
                text(sub.str(), railL + 62, y - 42, fresh ? T.live : T.muted, 0);
                const std::string id = r.id;
                LLRect hr(railL + 10, y, railR - 10, y - 58);
                m->hits.push_back({ hr, [this, id] { m->selected = id; m->focus(id); } });
                y -= 64;
            }
        if (!hasData)
        {
            const char* msg = !m->bridge ? m->startError.c_str() : state == MS_REPLICA_REFUSED ? "The authority refused this Viewer." : "Synchronizing with the authority…";
            text(msg, railL + 18, y - 20, T.muted, 0);
            if (m->bridge && state != MS_REPLICA_REFUSED)
            {
                gGL.pushMatrix();
                gGL.translatef((F32)(railL + railR) / 2, (F32)y - 70, 0);
                gl_washer_segment_2d(18, 14, (F32)(t * 4), (F32)(t * 4 + 4.2), 24, T.accent, alpha(T.accent, 0));
                gGL.popMatrix();
            }
        }
    }

    // ---- inspector + live WorldLine
    const S32 inL = W - 360, inR = W - 20, inTop = H - 92, inBottom = 150;
    glassPanel(inL, inTop, inR, inBottom);
    {
        S32 y = inTop - 28;
        const Resident* r = m->resident(m->selected);
        const ms_replica_entity* e = m->entity(m->selected);
        text("INSPECTOR", inL + 18, y, T.muted, 0, true);
        if (r && e)
        {
            const LLColor4 c = residentColor(r->name);
            text(r->name, inL + 18, y - 34, T.ink, 3, true);
            S32 tagX = inL + 22 + width(r->name, 3, true) + 10;
            roundRect(tagX, y - 18, tagX + width("AI RESIDENT", 0, true) + 16, y - 36, 9, alpha(c, .22f));
            text("AI RESIDENT", tagX + 8, y - 31, c, 0, true);
            y -= 62;
            auto row = [&](const std::string& k, const std::string& val, const LLColor4& col = T.ink) {
                text(k, inL + 18, y, T.faint, 0);
                text(val, inR - 18, y, col, 0, false, LLFontGL::RIGHT, inR - inL - 130);
                y -= 19;
            };
            std::ostringstream p;
            p << std::fixed << std::setprecision(2) << r->region[0] << ", " << r->region[1] << ", " << r->region[2];
            row("Authoritative (region m)", p.str());
            if (r->registered)
            {
                std::ostringstream q;
                q << std::fixed << std::setprecision(2) << r->displayed[0] << ", " << r->displayed[1] << ", " << r->displayed[2];
                row("Displayed E,N,U (m)", q.str(), T.warn);
            }
            row("Entity revision", std::to_string(r->revision));
            row("Entity", r->id.substr(0, 18) + "…");
            row("Body asset", r->asset.empty() ? "none" : r->asset.substr(0, 18) + "…");
            // actions: bounded moves through the authority
            y -= 6;
            text("AUTHORIZED ACTIONS", inL + 18, y, T.muted, 0, true);
            y -= 10;
            const S32 bw = (inR - inL - 36 - 18) / 4;
            const char* labels[] = { "← West", "North ↑", "South ↓", "East →" };
            const double d[][2] = { { -5, 0 }, { 0, 5 }, { 0, -5 }, { 5, 0 } };
            for (int i = 0; i < 4; ++i)
            {
                const S32 bl = inL + 18 + i * (bw + 6);
                roundRect(bl, y, bl + bw, y - 28, 10, alpha(T.accent, .18f));
                text(labels[i], bl + bw / 2, y - 18, T.ink, 0, true, LLFontGL::HCENTER);
                const auto ent = *e;
                const double dx = d[i][0], dy = d[i][1];
                m->hits.push_back({ LLRect(bl, y, bl + bw, y - 28), [this, ent, dx, dy] { if (m->bridge) m->bridge->requestMove(ent, dx, dy); } });
            }
            y -= 38;
            if (v && !v->actionNote.empty())
            {
                y = wrapped(v->actionNote, inL + 18, y, inR - inL - 36, T.muted, 0, 3, 16) - 4;
            }
            text("Agent token · Modify scope only · MutSea policy decides", inL + 18, y, T.faint, 0);
            y -= 26;
        }
        else
        {
            text("Select a resident.", inL + 18, y - 30, T.muted, 1);
            y -= 60;
        }
        // live WorldLine feed
        gl_line_2d(inL + 18, y + 8, inR - 18, y + 8, T.line);
        text("LIVE WORLDLINE", inL + 18, y - 12, T.muted, 0, true);
        if (v) text("#" + std::to_string(v->status.worldline_length), inR - 18, y - 12, T.faint, 0, true, LLFontGL::RIGHT);
        y -= 32;
        if (v && v->events.empty()) text("Listening for the next committed event…", inL + 18, y, T.faint, 0);
        if (v)
            for (auto it = v->events.rbegin(); it != v->events.rend() && y > inBottom + 18; ++it)
            {
                const double since = std::chrono::duration<double>(now - m->eventSeen[it->seq]).count();
                if (since < 2.5) roundRect(inL + 12, y + 14, inR - 12, y - 8, 8, alpha(T.accent, (float)(.22 * (1 - since / 2.5))));
                std::string who = "entity " + it->entity.substr(0, 8);
                if (const auto* rr = m->resident(it->entity)) who = rr->name;
                text("#" + std::to_string(it->seq), inL + 18, y, T.faint, 0);
                text(it->action, inL + 74, y, T.ink, 0, true);
                text(who + " · " + it->kind, inL + 74 + width(it->action, 0, true) + 8, y, T.muted, 0, false, LLFontGL::LEFT, inR - inL - 200);
                y -= 22;
            }
    }

    // ---- AI command bar + response card
    const S32 barL = 340, barR = W - 240, barW = barR - barL;
    {
        glassPanel(barL, 70, barR, 24, 23, true);
        // spark icon
        dot((F32)barL + 24, 47, 9, alpha(T.accent2, .3f));
        dot((F32)barL + 24, 47, 4.5f, T.accent2);
        roundRect(barR - 86, 62, barR - 10, 32, 15, T.accent);
        text("Ask", barR - 48, 42, T.bg, 1, true, LLFontGL::HCENTER);
        m->hits.push_back({ LLRect(barR - 86, 62, barR - 10, 32), [this] {
                                auto* e = getChild<LLLineEditor>("ask");
                                std::string s = e->getText();
                                LLStringUtil::trim(s);
                                if (!s.empty()) ask(s);
                                e->setText(LLStringUtil::null);
                            } });
        // suggestion chips
        const std::string name = [&] { const auto* r = m->resident(m->selected); return r ? r->name : std::string("Elara"); }();
        const std::vector<std::string> chips = { "Find " + name + " and show me the buildings near her", "Move " + name + " 5 m east", "Follow " + name, "Sunset", "Overview" };
        S32 cx = barL;
        for (const auto& c : chips)
        {
            const S32 cw = width(c, 0) + 24;
            if (cx + cw > barR) break;
            roundRect(cx, 104, cx + cw, 80, 12, LLColor4(1, 1, 1, .07f));
            text(c, cx + 12, 88, T.muted, 0);
            m->hits.push_back({ LLRect(cx, 104, cx + cw, 80), [this, c] { ask(c); } });
            cx += cw + 8;
        }
        if (v && !v->ai.id.empty())
        {
            const auto& a = v->ai;
            const S32 cardTop = 118 + 210, cardBottom = 116, barR = std::min(W - 380, barL + barW), barW = barR - barL;
            glassPanel(barL, cardTop, barR, cardBottom, 16, true);
            LLColor4 sc = a.stage == "done" ? T.live : a.stage == "refused" || a.stage == "error" ? T.bad : T.warn;
            std::string stage = a.stage;
            LLStringUtil::toUpper(stage);
            roundRect(barL + 16, cardTop - 14, barL + 30 + width(stage, 0, true), cardTop - 32, 9, alpha(sc, .2f));
            text(stage, barL + 23, cardTop - 27, sc, 0, true);
            text(a.title, barL + 40 + width(stage, 0, true), cardTop - 27, T.ink, 0, true, LLFontGL::LEFT, barW - 80 - width(stage, 0, true));
            S32 ry = wrapped(a.answer, barL + 16, cardTop - 52, barW - 32, T.ink, 1, 5, 19) - 4;
            for (const auto& line : a.results)
            {
                if (ry < cardBottom + 30) break;
                text("•  " + line, barL + 20, ry, T.muted, 0, false, LLFontGL::LEFT, barW - 40);
                ry -= 17;
            }
            if (!a.policy.empty()) wrapped(a.policy, barL + 16, cardBottom + 26, barW - 32, T.faint, 0, 2, 14);
            if (!a.focus.empty() && a.stage == "done" && m->cameraTarget.empty()) m->focus(a.focus);
        }
    }

    // ---- registration badge (top, centered) + stream metrics (bottom-left) + minimap
    {
        const std::string label = v ? v->registrationLabel : "No geographic registration";
        const LLColor4 c = v && v->hasRegistration ? T.warn : T.faint;
        const S32 bw = width(label, 0, true) + 40, bl = W / 2 - bw / 2, bt = topY - 56;
        roundRect(bl, bt, bl + bw, bt - 22, 11, alpha(c, .14f));
        gGL.pushMatrix();
        gGL.translatef((F32)bl + 14, (F32)bt - 11, 0);
        gl_washer_2d(5, 3.5f, 20, c, c);
        gGL.popMatrix();
        text(label, bl + 26, bt - 16, c, 0, true);

        glassPanel(20, 136, 320, 24, 12);
        text("STREAM", 36, 114, T.muted, 0, true);
        if (v)
        {
            std::ostringstream a, b, c2;
            a << "FVP domain 3 · " << (v->status.mode[0] ? v->status.mode : "—") << " · commits " << v->status.commits_applied;
            b << "p95 " << std::fixed << std::setprecision(2) << v->status.p95_latency_ms << " ms · resyncs " << v->status.resyncs << " · reconnects " << v->status.reconnects;
            c2 << "scene rev " << v->sceneRevision << (v->frame ? " · Storm frame " + std::to_string(v->frame->number) + (v->frame->interpolating ? " (settling)" : "") : " · no Storm frame");
            text(a.str(), 36, 94, T.ink, 0);
            text(b.str(), 36, 76, T.muted, 0);
            text(c2.str(), 36, 58, T.muted, 0);
            text(v->registrationDetail.empty() ? std::string("Durable cursor · WorldLine verified natively") : v->registrationDetail, 36, 38, T.faint, 0, false, LLFontGL::LEFT, 270);
        }

        // minimap: live authoritative positions, region-local metres
        const S32 mmR = W - 20, mmL = mmR - 200, mmT = 136, mmB = 24;
        glassPanel(mmL, mmT + 0, mmR, mmB + 0, 12);
        if (v && !v->residents.empty())
        {
            double minx = 1e9, maxx = -1e9, miny = 1e9, maxy = -1e9;
            for (const auto& r : v->residents) minx = std::min(minx, r.region[0]), maxx = std::max(maxx, r.region[0]), miny = std::min(miny, r.region[1]), maxy = std::max(maxy, r.region[1]);
            const double span = std::max({ maxx - minx, maxy - miny, 20.0 }) * 1.25, cxm = (minx + maxx) / 2, cym = (miny + maxy) / 2;
            for (const auto& r : v->residents)
            {
                const F32 px = (F32)(mmL + 100 + (r.region[0] - cxm) / span * 180), py = (F32)(mmB + 56 + (r.region[1] - cym) / span * 100);
                const double since = std::chrono::duration<double>(now - m->lastMove[r.id].second).count();
                const LLColor4 c = residentColor(r.name);
                if (since < 3) dot(px, py, (F32)(4 + since * 6), alpha(c, (float)(.35 * (1 - since / 3))));
                dot(px, py, r.id == m->selected ? 5.f : 3.5f, c);
                if (r.id == m->selected) text(r.name, (S32)px + 7, (S32)py - 4, T.ink, 0, true);
            }
        }
        text("LIVE MAP · region-local", mmL + 12, mmT - 18, T.faint, 0, true);
    }

    // ---- disconnect / resync banners
    if (m->bridge && hasData && !live)
    {
        const std::string msg = state == MS_REPLICA_REFUSED ? "The authority refused this Viewer (" + std::to_string(v->status.refused_code) + ")"
                                                            : "Connection lost · showing last confirmed revision " + std::to_string(v->status.revision) + " · reconnecting…";
        const S32 mw = width(msg, 1, true) + 60;
        glassPanel(W / 2 - mw / 2, H / 2 + 24, W / 2 + mw / 2, H / 2 - 24, 20, true);
        dot((F32)W / 2 - mw / 2 + 24, (F32)H / 2, 5, T.bad);
        text(msg, W / 2 + 10, H / 2 - 6, T.ink, 1, true, LLFontGL::HCENTER);
    }
    else if (v && live && std::string(v->status.mode) == "resync" && v->status.snapshots > 1 && v->status.commits_applied == 0)
        text("Resynchronized from a fresh authoritative snapshot", W / 2, H - 120, T.warn, 0, true, LLFontGL::HCENTER);

    // Demo script: runs only once the projection is live, through the same paths as the controls.
    while (v && live && m->scriptNext < m->script.size() && t >= m->script[m->scriptNext].first)
    {
        const std::string cmd = m->script[m->scriptNext++].second;
        LL_INFOS("FinalverseLiving") << "demo step: " << cmd << LL_ENDL;
        auto byName = [&](const std::string& n) -> std::string {
            for (const auto& r : v->residents) if (r.name == n) return r.id;
            return {};
        };
        std::istringstream cs(cmd);
        std::string verb, name;
        cs >> verb;
        if (verb == "ask") { std::string q; std::getline(cs, q); LLStringUtil::trim(q); ask(q); }
        else if (verb == "select" || verb == "focus") { cs >> name; if (auto id = byName(name); !id.empty()) { m->selected = id; if (verb == "focus") m->focus(id); } }
        else if (verb == "overview") m->overview();
        else if (verb == "move")
        {
            double dx = 0, dy = 0;
            cs >> name >> dx >> dy;
            if (const auto* e = m->entity(byName(name)); e && m->bridge) m->bridge->requestMove(*e, dx, dy);
        }
    }
    // Keep the renderer's presentation control present across renderer restarts.
    if (m->bridge && now - m->lastCamera > std::chrono::seconds(1))
    {
        m->lastCamera = now;
        if (privateDir(m->bridge->frames) && !fs::exists(fs::path(m->bridge->frames) / "camera.json")) m->camera();
    }
    LLFloater::draw();  // line editor and title strip on top
    // follow mode keeps the camera on the selected resident while they move
    if (m->follow && !m->selected.empty())
        if (const auto* r = m->resident(m->selected); r && m->cameraTarget != r->asset) m->focus(m->selected);
}

void llfinalverseLivingMaybeOpen()
{
    static bool opened = false;
    // The Ω-XVIII FINALVERSE shell hosts the live world itself; do not open both.
    const char* shell = std::getenv("FINALVERSE_SHELL");
    if (!opened && std::getenv("FINALVERSE_LIVE_TOKENS") && !(shell && std::string(shell) == "1"))
    {
        opened = true;
        LLFloaterReg::showInstance("finalverse_living");
    }
}
