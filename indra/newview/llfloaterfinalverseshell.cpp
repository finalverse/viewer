// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
// FINALVERSE shell — Ω-XVIII Viewer Renaissance.
//
//   SpatialDB ─FVP domain 4 (libfinalverse_spatial)─► engine thread ─► model snapshot ─► shell UI
//   MutSea ─FVP domain 3 (libmutsea_replica)───────► engine thread (live residents, WorldLine)
//   engine ─context.json / scene-update.json─► Storm spatial renderer ─frame.ppm/json─► viewport
//   AI Spatial Navigator ◄─request.json / status.json─► engine (typed plans; execution stays here)
//
// The shell never mutates world state through navigation. The only writes to the world are
// the Ω-XVII.3 authorized moves (MutSea session protocol, agent token, Modify scope), offered
// in Create mode. Everything the UI shows about identity, frames, units, accuracy and
// provenance comes from the published contracts; demonstration content is labelled.
#include "llviewerprecompiledheaders.h"
#include "llfloaterfinalverseshell.h"

#include "finalverse/convergence.h"
#include "finalverse/spatial.h"
#include "mutsea/replica.h"
#include "../../spatial-render/frame_transport.h"  // Ω-XIX native Storm frame transport

#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llimage.h"
#include "lllineeditor.h"
#include "llnavigationbar.h"
#include "llpaneltopinfobar.h"
#include "llrender.h"
#include "llrootview.h"
#include "llrender2dutils.h"
#include "llstatusbar.h"
#include "lltoolbarview.h"
#include "llviewermenu.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "llwindow.h"

#include <arpa/inet.h>
#include <netdb.h>
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
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
namespace bj = boost::json;

// ====================================================================== design system v1
// Restrained, premium, legible: neutral ink surfaces, one accent family, semantic status
// colours, classification colours that are never decorative. Spacing on an 4 px grid.
struct Tokens
{
    LLColor4 bg{ .016f, .02f, .03f, 1.f };
    LLColor4 ink{ .95f, .97f, 1.f, 1.f }, ink2{ .70f, .75f, .83f, 1.f }, ink3{ .48f, .53f, .62f, 1.f };
    LLColor4 surface{ .05f, .06f, .085f, .82f }, surfaceHi{ .09f, .105f, .14f, .9f }, hairline{ 1.f, 1.f, 1.f, .07f };
    LLColor4 accent{ .42f, .64f, 1.f, 1.f }, accentSoft{ .42f, .64f, 1.f, .16f }, violet{ .67f, .52f, 1.f, 1.f };
    LLColor4 live{ .26f, .86f, .58f, 1.f }, warn{ 1.f, .74f, .3f, 1.f }, bad{ 1.f, .42f, .42f, 1.f };
    LLColor4 authentic{ .3f, .82f, .62f, 1.f }, derived{ .45f, .7f, 1.f, 1.f }, demo{ 1.f, .69f, .32f, 1.f };
    LLColor4 focus{ .55f, .78f, 1.f, 1.f };
};
const Tokens T;
LLColor4 alpha(LLColor4 c, float a) { c.mV[3] = a; return c; }
LLColor4 mix(const LLColor4& a, const LLColor4& b, float t)
{
    return { a.mV[0] + (b.mV[0] - a.mV[0]) * t, a.mV[1] + (b.mV[1] - a.mV[1]) * t, a.mV[2] + (b.mV[2] - a.mV[2]) * t, a.mV[3] + (b.mV[3] - a.mV[3]) * t };
}

int gScale = 1;  // text scale: 0 compact, 1 normal, 2 large
LLFontGL* font(int size, bool bold = false)
{
    size = llclamp(size + gScale - 1, 0, 3);
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
S32 lineHeight(int size) { return (S32)font(size)->getLineHeight() + 4; }
std::string clip(const std::string& s, S32 maxW, int size, bool bold = false)
{
    if (width(s, size, bold) <= maxW) return s;
    std::string out = s;
    while (!out.empty() && width(out + "…", size, bold) > maxW)
    {
        out.pop_back();
        while (!out.empty() && (out.back() & 0xC0) == 0x80) out.pop_back();
    }
    return out + "…";
}
S32 wrapped(const std::string& s, S32 x, S32 y, S32 maxWidth, const LLColor4& c, int size, S32 maxLines)
{
    std::istringstream words(s);
    std::string word, line;
    S32 lines = 0;
    const S32 lh = lineHeight(size);
    auto flush = [&] {
        if (!line.empty() && lines < maxLines) { text(lines + 1 == maxLines ? clip(line, maxWidth, size) : line, x, y, c, size); y -= lh; ++lines; }
        line.clear();
    };
    while (words >> word)
    {
        const std::string next = line.empty() ? word : line + " " + word;
        if (width(next, size) > maxWidth && !line.empty()) { flush(); line = word; }
        else line = next;
    }
    flush();
    return y;
}

void quarter(F32 cx, F32 cy, F32 r, F32 a0, const LLColor4& c)
{
    gGL.color4fv(c.mV);
    gGL.begin(LLRender::TRIANGLES);
    for (int i = 0; i < 8; ++i)
    {
        const F32 t0 = a0 + F_PI_BY_TWO * i / 8, t1 = a0 + F_PI_BY_TWO * (i + 1) / 8;
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
    gl_rect_2d(l, t - rr, r, b + rr, c, true);
    gl_rect_2d(l + rr, t, r - rr, t - rr, c, true);
    gl_rect_2d(l + rr, b + rr, r - rr, b, c, true);
    quarter((F32)(r - rr), (F32)(t - rr), (F32)rr, 0.f, c);
    quarter((F32)(l + rr), (F32)(t - rr), (F32)rr, F_PI_BY_TWO, c);
    quarter((F32)(l + rr), (F32)(b + rr), (F32)rr, F_PI, c);
    quarter((F32)(r - rr), (F32)(b + rr), (F32)rr, F_PI + F_PI_BY_TWO, c);
}
void panel(S32 l, S32 t, S32 r, S32 b, F32 radius = 14.f, bool hi = false)
{
    roundRect(l - 1, t + 1, r + 1, b - 1, radius + 1, T.hairline);
    roundRect(l, t, r, b, radius, hi ? T.surfaceHi : T.surface);
}
void focusRing(const LLRect& rc, F32 radius = 10.f)
{
    roundRect(rc.mLeft - 3, rc.mTop + 3, rc.mRight + 3, rc.mBottom - 3, radius + 3, alpha(T.focus, .9f));
    roundRect(rc.mLeft - 1, rc.mTop + 1, rc.mRight + 1, rc.mBottom - 1, radius + 1, T.bg);
}
void dot(F32 x, F32 y, F32 r, const LLColor4& c)
{
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.color4fv(c.mV);
    gl_circle_2d(x, y, r, 24, true);
}
void line(F32 x0, F32 y0, F32 x1, F32 y1, const LLColor4& c, F32 w = 1.6f)
{
    // Thick anti-aliased-looking line as a thin quad.
    const F32 dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len <= 0) return;
    const F32 nx = -dy / len * w * .5f, ny = dx / len * w * .5f;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.color4fv(c.mV);
    gGL.begin(LLRender::TRIANGLES);
    gGL.vertex2f(x0 + nx, y0 + ny); gGL.vertex2f(x1 + nx, y1 + ny); gGL.vertex2f(x1 - nx, y1 - ny);
    gGL.vertex2f(x0 + nx, y0 + ny); gGL.vertex2f(x1 - nx, y1 - ny); gGL.vertex2f(x0 - nx, y0 - ny);
    gGL.end();
}
void ring(F32 x, F32 y, F32 r, const LLColor4& c, F32 w = 1.6f)
{
    for (int i = 0; i < 28; ++i)
    {
        const F32 a0 = F_TWO_PI * i / 28, a1 = F_TWO_PI * (i + 1) / 28;
        line(x + r * cosf(a0), y + r * sinf(a0), x + r * cosf(a1), y + r * sinf(a1), c, w);
    }
}

// Vector icons (16 px nominal, centred at x,y).
enum class Icon { Back, Forward, Home, Spark, Compass, Plus, Ruler, Code, Layers, Info, Chevron, Search, Cube, Live };
void icon(Icon k, F32 x, F32 y, const LLColor4& c, F32 s = 1.f)
{
    const F32 u = 7.f * s;
    switch (k)
    {
        case Icon::Back: line(x + u * .6f, y, x - u * .7f, y, c); line(x - u * .7f, y, x - u * .1f, y + u * .6f, c); line(x - u * .7f, y, x - u * .1f, y - u * .6f, c); break;
        case Icon::Forward: line(x - u * .6f, y, x + u * .7f, y, c); line(x + u * .7f, y, x + u * .1f, y + u * .6f, c); line(x + u * .7f, y, x + u * .1f, y - u * .6f, c); break;
        case Icon::Home: line(x - u, y, x, y + u, c); line(x, y + u, x + u, y, c); line(x - u * .7f, y - u * .1f, x - u * .7f, y - u, c); line(x + u * .7f, y - u * .1f, x + u * .7f, y - u, c); line(x - u * .7f, y - u, x + u * .7f, y - u, c); break;
        case Icon::Spark: line(x, y - u, x, y + u, c, 2.f); line(x - u, y, x + u, y, c, 2.f); line(x - u * .45f, y - u * .45f, x + u * .45f, y + u * .45f, c, 1.2f); line(x - u * .45f, y + u * .45f, x + u * .45f, y - u * .45f, c, 1.2f); break;
        case Icon::Compass: ring(x, y, u, c); line(x - u * .35f, y - u * .35f, x + u * .5f, y + u * .5f, c, 2.2f); dot(x, y, 1.6f * s, c); break;
        case Icon::Plus: line(x - u, y, x + u, y, c, 2.f); line(x, y - u, x, y + u, c, 2.f); break;
        case Icon::Ruler: line(x - u, y - u * .5f, x + u, y + u * .5f, c, 2.f); for (int i = -2; i <= 2; ++i) line(x + i * u * .38f, y + i * u * .19f, x + i * u * .38f - u * .2f, y + i * u * .19f + u * .4f, c, 1.2f); break;
        case Icon::Code: line(x - u * .3f, y + u * .7f, x - u, y, c); line(x - u, y, x - u * .3f, y - u * .7f, c); line(x + u * .3f, y + u * .7f, x + u, y, c); line(x + u, y, x + u * .3f, y - u * .7f, c); break;
        case Icon::Layers: line(x - u, y + u * .2f, x, y + u * .7f, c); line(x, y + u * .7f, x + u, y + u * .2f, c); line(x - u, y + u * .2f, x, y - u * .3f, c); line(x, y - u * .3f, x + u, y + u * .2f, c); line(x - u, y - u * .3f, x, y - u * .8f, c); line(x, y - u * .8f, x + u, y - u * .3f, c); break;
        case Icon::Info: ring(x, y, u, c); line(x, y - u * .5f, x, y + u * .15f, c, 2.f); dot(x, y + u * .5f, 1.4f * s, c); break;
        case Icon::Chevron: line(x - u * .3f, y + u * .6f, x + u * .3f, y, c); line(x + u * .3f, y, x - u * .3f, y - u * .6f, c); break;
        case Icon::Search: ring(x - u * .2f, y + u * .2f, u * .6f, c); line(x + u * .25f, y - u * .25f, x + u * .8f, y - u * .8f, c, 2.f); break;
        case Icon::Cube: line(x - u * .8f, y + u * .4f, x, y + u * .8f, c); line(x, y + u * .8f, x + u * .8f, y + u * .4f, c); line(x + u * .8f, y + u * .4f, x, y, c); line(x, y, x - u * .8f, y + u * .4f, c);
            line(x - u * .8f, y + u * .4f, x - u * .8f, y - u * .5f, c); line(x + u * .8f, y + u * .4f, x + u * .8f, y - u * .5f, c); line(x, y, x, y - u * .9f, c); line(x - u * .8f, y - u * .5f, x, y - u * .9f, c); line(x, y - u * .9f, x + u * .8f, y - u * .5f, c); break;
        case Icon::Live: dot(x, y, u * .45f, c); ring(x, y, u * .9f, alpha(c, .5f), 1.2f); break;
    }
}
void brandMark(F32 x, F32 y, F32 s, F32 t)
{
    // ◈ — nested diamonds: the brand mark of FINALVERSE.
    auto diamond = [&](F32 r, const LLColor4& c) {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.color4fv(c.mV);
        gGL.begin(LLRender::TRIANGLES);
        gGL.vertex2f(x, y + r); gGL.vertex2f(x - r, y); gGL.vertex2f(x + r, y);
        gGL.vertex2f(x - r, y); gGL.vertex2f(x, y - r); gGL.vertex2f(x + r, y);
        gGL.end();
    };
    diamond(11 * s, mix(T.accent, T.violet, .5f + .5f * sinf(t * .6f)));
    diamond(7.5f * s, T.bg);
    diamond(4 * s, T.ink);
}

// ====================================================================== small utilities
std::string hexOf(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) s += d[p[i] >> 4], s += d[p[i] & 15];
    return s;
}
std::string pctl(std::vector<double> xs, int p)
{
    if (xs.empty()) return "-";
    std::sort(xs.begin(), xs.end());
    std::ostringstream o;
    o << std::fixed << std::setprecision(2) << xs[std::min(xs.size() - 1, size_t(std::lround(p / 100.0 * double(xs.size() - 1))))];
    return o.str();
}

std::string sha256(const std::string& bytes)
{
    uint8_t h[32];
    SHA256(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), h);
    return hexOf(h, 32);
}
std::string uuidText(const uint8_t* p) { LLUUID id; std::memcpy(id.mData, p, 16); return id.asString(); }
bool parseUuid(const std::string& s, uint8_t out[16]) { if (!LLUUID::validate(s)) return false; LLUUID id(s); std::memcpy(out, id.mData, 16); return true; }
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
        { std::ofstream out(tmp, std::ios::binary | std::ios::trunc); out << bytes; if (!out.good()) return false; }
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
std::string str(const bj::value* v, const std::string& fallback = {}) { return v && v->is_string() ? std::string(v->as_string()) : fallback; }
std::string str(const bj::object& o, const char* k, const std::string& fallback = {}) { return str(o.if_contains(k), fallback); }
double num(const bj::value& v) { return v.is_double() ? v.as_double() : v.is_int64() ? (double)v.as_int64() : v.is_uint64() ? (double)v.as_uint64() : 0.0; }
std::string fmt(double v, int digits)
{
    std::ostringstream s;
    s << std::fixed << std::setprecision(digits) << v;
    return s.str();
}
std::string ago(double seconds)
{
    if (seconds < 1.5) return "just now";
    if (seconds < 60) return std::to_string((int)seconds) + " s ago";
    return std::to_string((int)(seconds / 60)) + " min ago";
}
LLColor4 classColour(const std::string& c, bool demo)
{
    if (demo) return T.demo;
    if (c == "verified") return T.authentic;
    if (c == "derived") return T.derived;
    return T.ink3;
}
std::string classLabel(const std::string& c, bool demo)
{
    if (demo) return "DEMONSTRATION";
    if (c == "verified") return "AUTHENTIC";
    if (c == "derived") return "DERIVED FROM AUTHENTIC DATA";
    if (c == "inferred") return "INFERRED";
    return c;
}
std::string kindTitle(const std::string& k)
{
    static const std::map<std::string, std::string> m{ { "planet", "Planet" }, { "city", "City" }, { "facility", "Facility" }, { "building", "Building" },
                                                        { "area", "Production area" }, { "machine", "Machine" }, { "component", "Component" },
                                                        { "material", "Material" }, { "resident", "AI resident" } };
    auto it = m.find(k);
    return it == m.end() ? k : it->second;
}

// ====================================================================== model
struct Child { std::string entity, label, kind, classification; bool demo = false, live = false; };
struct Property { std::string name, unit, method, classification; double value = 0; };
struct RepInfo { std::string id, kind, frame, units, label, scene; bool authorized = false; std::vector<std::string> caps; std::vector<Property> props; };
struct Crumb { std::string entity, label, kind; };
struct FrameLink { std::string id, cls, units, accuracy, transform, provenance; };
struct Context
{
    std::string entity, label, kind, domain, classification, description, transition, reason, frame, repId, repKind, repLabel, scene, sceneTitle;
    bool demo = false, live = false;
    std::vector<Crumb> path;
    std::vector<Child> children;
    std::vector<RepInfo> reps;
    std::vector<std::string> warnings;
    std::vector<FrameLink> frames;  // chain from the context frame to the root
    std::map<std::string, std::vector<std::string>> selectable;  // entity -> asset digests in this scene
    std::vector<std::string> sources;
    double focus[6]{};
    uint64_t edition = 0;
};
struct Resident { std::string id, asset, name; double region[3]{}, displayed[3]{}; bool registered = false; uint64_t revision = 0; };
struct Event { uint64_t seq = 0, revision = 0; std::string action, kind, entity; };
struct Frame
{
    std::vector<uint8_t> rgb;  // pixels, top row first, as delivered by Storm (RGB for the file path, RGBA native)
    int width = 1280, height = 720, comps = 3;
    uint64_t number = 0, triangles = 0, controlSeq = 0, sourceRevision = 0, sequence = 0;
    uint64_t renderStartNs = 0, producedNs = 0, acquiredNs = 0;
    std::string context, units, frameId, transition, transport = "file";
    bool settled = false;
    double captureMs = 0, switchMs = 0;
};
struct AiCard
{
    std::string id, stage, query, title, message, policy, model;
    std::vector<std::string> plan, route, routeLabels, missing;
    double planSeconds = 0;
    size_t executing = 0;
    // Ω-XIX grounded answers: catalog facts with their classification, kept apart from general background.
    std::string answer, general, subject;
    std::vector<std::pair<std::string, std::string>> facts;  // (classification, text)
};
struct Model
{
    // spatial service
    bool spatialUp = false;
    std::string spatialError, server, scopes;
    uint64_t edition = 0, calls = 0;
    double lastCallMs = 0, navigateMs = 0, resolveMs = 0;
    std::deque<double> latencies;
    // navigation
    Context ctx;
    bool navigating = false;
    std::string navTarget, navStage;
    uint64_t controlSeq = 0;
    std::string presentation = "realistic";  // Ω-XIX engineering presentation preset (display only)
    std::vector<std::string> history, historyEntities;
    size_t historyCursor = 0;
    // Ω-XIX command palette: entity search results (scope-filtered by the spatial service).
    struct Found { std::string entity, label, kind, domain; };
    std::string searchQuery;
    std::vector<Found> found;
    std::string selected, selectedLabel;
    // live world
    bool liveConfigured = false;
    ms_replica_status live{};
    std::vector<Resident> residents;
    std::deque<Event> events;
    std::string registrationLabel;
    // renderer
    std::shared_ptr<const Frame> frame;
    // Ω-XIX creation (AI World Director draft + user-approved publication)
    struct CreationObject { std::string id, label, asset, material, sha; double x = 0, y = 0, rot = 0; };
    struct Creation
    {
        std::string stage, message, request, draftId, name, summary, model, zone, pubStage, pubMessage, receipt, selected;
        uint64_t revision = 0;
        bool valid = false, published = false, previewActive = false;
        std::vector<std::string> findings, edits, pubSteps, checks;
        std::vector<CreationObject> objects;
        double seconds = 0, planSeconds = 0;
        size_t histCursor = 0, histSize = 0;
        std::map<std::string, std::vector<std::string>> materials;  // asset -> offered materials
    } creation;
    // Ω-XX universal contexts: native (non-FWD) context, visual style, Genesis, journey history
    std::string style;                       // "" = the renderer's default (cinematic)
    std::string nativeKind, nativeLabel;     // astronomical | scientific | abstract
    std::string nativeModel;                 // scientific model id
    double nativeDistanceKm = 0, simEpoch = 0, simScale = 0;
    Clock::time_point nativeSince = Clock::now();
    struct Genesis
    {
        bool active = false;
        std::string stage, phase, title, world, reality, model, error, requestId, style;
        double progress = 0, elapsed = 0, modelSeconds = 0;
        uint64_t applied = 0, refused = 0, triangles = 0, objects = 0, queued = 0;
        double frameP50 = 0, frameP95 = 0;
        std::map<std::string, int> resident;
        std::map<std::string, std::string> fieldSources;
        std::string summary, refined;
    } genesis;
    std::vector<std::string> journey;        // Ω-XX navigation history across all contexts
    // AI + actions
    AiCard ai;
    std::string actionNote;
    std::deque<std::pair<std::string, Clock::time_point>> toasts;
};

// ====================================================================== engine
struct Engine
{
    // configuration
    std::string spatialHost = "127.0.0.1", tokenFile, frames, navigatorDir, livingDir, startEntity, casMapFile;
    std::string creationDir, publisherDir, creationRequestId, publishRequestId;  // Ω-XIX creation channels
    Clock::time_point creationStarted = Clock::now();
    bj::object creationPreview;
    uint16_t spatialPort = 7871;
    bool reducedMotion = false;
    struct Cas { std::string fwdDir, fwdFile, objects; };
    std::map<std::string, Cas> cas;
    std::map<std::string, std::string> fwdBySha;  // world-definition digest -> path

    fv_spatial_client* client = nullptr;
    fv_spatial_history* history = nullptr;
    // live world (Ω-XVII.3)
    std::string world, region, sessionHost, replicationHost, observer, definition;
    uint16_t sessionPort = 0, replicationPort = 0;
    std::map<std::string, std::string> tokens;
    std::set<std::string> boundBodies;
    fv_wc_registration* registration = nullptr;
    uint64_t registrationRevision = 0;
    ms_replica* replica = nullptr;
    uint64_t lastSceneRevision = 0;
    bool wroteScene = false;

    std::thread thread;
    std::atomic<bool> stop{ false };
    std::mutex m;
    std::deque<std::function<void()>> jobs;
    std::shared_ptr<const Model> snapshot;
    Model model;  // engine-owned; published as snapshot
    std::shared_ptr<const Frame> frame;
    fs::file_time_type frameTime{};
    std::string aiRequestId;
    Clock::time_point aiStarted;
    std::vector<std::thread> actions;

    ~Engine()
    {
        stop = true;
        if (thread.joinable()) thread.join();
        for (auto& t : actions) if (t.joinable()) t.join();
        if (replica) ms_replica_stop(replica);
        if (registration) fv_wc_registration_close(registration);
        if (client) fv_spatial_close(client);
        if (history) fv_spatial_history_destroy(history);
    }

    void post(std::function<void()> f) { std::lock_guard g(m); if (jobs.size() < 64) jobs.push_back(std::move(f)); }
    void toast(const std::string& s) { model.toasts.emplace_back(s, Clock::now()); while (model.toasts.size() > 4) model.toasts.pop_front(); }

    void configure()
    {
        auto env = [](const char* k) { const char* v = std::getenv(k); return v ? std::string(v) : std::string(); };
        if (auto h = env("FINALVERSE_SPATIAL_ENDPOINT"); !h.empty())
        {
            auto c = h.rfind(':');
            spatialHost = h.substr(0, c);
            spatialPort = (uint16_t)std::atoi(h.substr(c + 1).c_str());
        }
        tokenFile = env("FINALVERSE_SPATIAL_TOKEN_FILE");
        frames = env("FINALVERSE_STORM_FRAMES");
        navigatorDir = env("FINALVERSE_NAVIGATOR_DIR");
        creationDir = env("FINALVERSE_CREATION_DIR");
        publisherDir = env("FINALVERSE_PUBLISHER_DIR");
        livingDir = env("FINALVERSE_WORLD_INTERACTION_DIR");
        startEntity = env("FINALVERSE_START_ENTITY");
        casMapFile = env("FINALVERSE_CAS_MAP");
        definition = env("FINALVERSE_STORM_DEFINITION_SHA256");
        reducedMotion = env("FINALVERSE_REDUCED_MOTION") == "1";
        genesisDir = env("FINALVERSE_GENESIS_DIR");
        genesisProgress = env("FINALVERSE_GENESIS_PROGRESS");
        graphFile = env("FINALVERSE_CIVILIZATION_GRAPH");
        try
        {
            auto j = bj::parse(readSmall(casMapFile, 65536)).as_object();
            for (auto& [k, v] : j)
            {
                auto& o = v.as_object();
                Cas c{ str(o, "fwd_dir"), str(o, "fwd_file"), str(o, "objects") };
                cas[std::string(k)] = c;
                if (!c.fwdFile.empty()) fwdBySha[sha256(readSmall(c.fwdFile, 1 << 20))] = c.fwdFile;
                if (!c.fwdDir.empty())
                    for (auto& e : fs::directory_iterator(c.fwdDir))
                        if (e.is_regular_file() && e.path().extension() == ".json") fwdBySha[sha256(readSmall(e.path(), 1 << 20))] = e.path().string();
            }
        }
        catch (const std::exception& e) { LL_WARNS("FinalverseShell") << "CAS map unreadable: " << e.what() << LL_ENDL; }
        history = fv_spatial_history_create(64);
        startLive();
    }

    // ------------------------------------------------------------------ FVP domain 4
    bool connect()
    {
        if (client) return true;
        std::string token = readSmall(tokenFile, 1024);
        while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) token.pop_back();
        char err[512] = {};
        const auto s = fv_spatial_connect(spatialHost.c_str(), spatialPort, token.c_str(), "FINALVERSE Viewer", 4000, &client, err, sizeof err);
        if (s != FV_SPATIAL_OK)
        {
            if (model.spatialUp || model.spatialError.empty())
                LL_INFOS("FinalverseShell") << "spatial service unavailable: " << fv_spatial_status_text(s) << " " << err << LL_ENDL;
            model.spatialUp = false;
            model.spatialError = std::string(fv_spatial_status_text(s)) + ": " + err;
            client = nullptr;
            return false;
        }
        try
        {
            auto w = bj::parse(fv_spatial_welcome(client)).as_object();
            model.server = str(w, "server");
            model.edition = w.at("catalog_edition").to_number<uint64_t>();
            std::string sc;
            for (auto& s2 : w.at("scopes").as_array()) sc += (sc.empty() ? "" : " · ") + std::string(s2.as_string());
            model.scopes = sc.empty() ? "public" : sc;
        }
        catch (const std::exception&) {}
        model.spatialUp = true;
        model.spatialError.clear();
        LL_INFOS("FinalverseShell") << "spatial service " << spatialHost << ":" << spatialPort << " " << model.server << " edition " << model.edition << LL_ENDL;
        return true;
    }
    // Returns status; body (response or refusal body) in out. A request that meets a closed session
    // (e.g. the spatial service restarted) is retried once on a fresh session (Ω-XIX recovery).
    fv_spatial_status call(const char* op, const bj::object& req, bj::object& out, std::string& why)
    {
        auto s = callOnce(op, req, out, why);
        if (s == FV_SPATIAL_TRANSPORT || s == FV_SPATIAL_PROTOCOL)
        {
            out = {};
            s = callOnce(op, req, out, why);
            if (s == FV_SPATIAL_OK) LL_INFOS("FinalverseShell") << "spatial request " << op << " succeeded after reconnecting" << LL_ENDL;
        }
        return s;
    }
    fv_spatial_status callOnce(const char* op, const bj::object& req, bj::object& out, std::string& why)
    {
        if (!connect()) { why = model.spatialError; return FV_SPATIAL_TRANSPORT; }
        char* resp = nullptr;
        size_t len = 0;
        char err[512] = {};
        const auto t0 = Clock::now();
        auto s = fv_spatial_call(client, op, bj::serialize(req).c_str(), &resp, &len, err, sizeof err);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        model.lastCallMs = ms;
        model.calls++;
        model.latencies.push_back(ms);
        while (model.latencies.size() > 120) model.latencies.pop_front();
        why = err;
        if (resp)
        {
            try { out = bj::parse(std::string_view(resp, len)).as_object(); } catch (...) {}
            fv_spatial_free(resp);
        }
        if (s == FV_SPATIAL_TRANSPORT || s == FV_SPATIAL_TIMEOUT || s == FV_SPATIAL_PROTOCOL)
        {
            LL_INFOS("FinalverseShell") << "spatial service unavailable during " << op << ": " << fv_spatial_status_text(s) << " " << why << LL_ENDL;
            fv_spatial_close(client);
            client = nullptr;
            model.spatialUp = false;
            model.spatialError = why;
        }
        return s;
    }

    std::map<std::string, Child> childCache;
    bool resolveChild(const std::string& id, Child& c)
    {
        if (auto it = childCache.find(id); it != childCache.end()) { c = it->second; return true; }
        bj::object out;
        std::string why;
        if (call("SPATIAL.RESOLVE_ENTITY", { { "entity", id } }, out, why) != FV_SPATIAL_OK) return false;
        try
        {
            auto d = bj::parse(str(out, "document")).as_object();
            c = { id, str(d, "label"), str(d, "kind"), str(d, "content_classification"), d.at("demonstration").as_bool(), str(d, "kind") == "resident" };
            childCache[id] = c;
            return true;
        }
        catch (...) { return false; }
    }

    // The single navigation path used by the UI, keyboard, breadcrumbs, history and the AI.
    bool navigate(const std::string& target, const std::string& capability = {}, bool pushHistory = true, const std::string& repKind = {})
    {
        const auto t0 = Clock::now();
        model.navigating = true;
        navStart = Clock::now();
        model.navStage = "Resolving";
        publish();
        bj::object req{ { "target", target } };
        if (!model.ctx.entity.empty() && model.ctx.entity != target) req["from"] = model.ctx.entity;
        if (!capability.empty()) req["capability"] = capability;
        if (!repKind.empty()) req["representation_kind"] = repKind;
        bj::object nav;
        std::string why;
        const auto s = call("SPATIAL.NAVIGATE", req, nav, why);
        model.navigateMs = model.lastCallMs;
        if (s != FV_SPATIAL_OK)
        {
            model.navigating = false;
            toast(s == FV_SPATIAL_NO_REPRESENTATION ? why : std::string(fv_spatial_status_text(s)) + " · " + why);
            LL_INFOS("FinalverseShell") << "navigate refused target=" << target << " status=" << fv_spatial_status_text(s) << " " << why << LL_ENDL;
            return false;
        }
        Context c;
        try
        {
            c.entity = target;
            c.transition = str(nav, "transition");
            c.reason = str(nav, "reason");
            c.frame = str(nav, "frame");
            c.repId = str(nav, "representation");
            c.repKind = str(nav, "representation_kind");
            c.scene = str(nav, "scene");
            c.edition = nav.at("catalog_edition").to_number<uint64_t>();
            for (auto& w : nav.at("warnings").as_array()) c.warnings.push_back(std::string(w.as_string()));
            for (auto& p : nav.at("path").as_array())
            {
                auto& o = p.as_object();
                c.path.push_back({ str(o, "entity"), str(o, "label"), str(o, "kind") });
            }
            auto& f = nav.at("focus").as_array();
            for (size_t i = 0; i < 6 && i < f.size(); ++i) c.focus[i] = num(f[i]);
            // Entity document.
            bj::object ent;
            call("SPATIAL.RESOLVE_ENTITY", { { "entity", target } }, ent, why);
            auto d = bj::parse(str(ent, "document")).as_object();
            c.label = str(d, "label");
            c.kind = str(d, "kind");
            c.domain = str(d, "domain");
            c.classification = str(d, "content_classification");
            c.description = str(d, "description");
            c.demo = d.at("demonstration").as_bool();
            for (auto& s2 : d.at("provenance").as_object().at("sources").as_array())
            {
                auto& o = s2.as_object();
                c.sources.push_back(str(o, "id") + " · " + str(o, "classification") + (o.contains("sha256") ? " · " + str(o, "sha256").substr(0, 12) : ""));
            }
            model.navStage = "Representations";
            publish();
            bj::object reps;
            call("SPATIAL.GET_REPRESENTATIONS", { { "entity", target } }, reps, why);
            for (auto& r : reps.at("representations").as_array())
            {
                auto& o = r.as_object();
                RepInfo ri{ str(o, "representation"), str(o, "kind"), str(o, "frame"), str(o, "units"), {}, str(o, "scene"), o.at("authorized").to_number<uint32_t>() == 1, {}, {} };
                for (auto& cap : o.at("capabilities").as_array()) ri.caps.push_back(std::string(cap.as_string()));
                if (ri.authorized)
                {
                    auto rd = bj::parse(str(o, "document")).as_object();
                    ri.label = str(rd, "label");
                    if (auto* props = rd.if_contains("properties"))
                        for (auto& p : props->as_array())
                        {
                            auto& po = p.as_object();
                            ri.props.push_back({ str(po, "name"), str(po, "unit"), str(po, "method"), str(po, "classification"), num(po.at("value")) });
                        }
                }
                if (ri.id == c.repId) c.repLabel = ri.label;
                c.reps.push_back(ri);
            }
            // Children (contains), resolved through the same authorization.
            for (auto& rel : d.at("relationships").as_array())
            {
                auto& o = rel.as_object();
                if (str(o, "predicate") != "contains") continue;
                Child ch;
                if (resolveChild(str(o, "object"), ch)) c.children.push_back(ch);
            }
            // Reference frame chain.
            std::string fid = c.frame;
            for (int guard = 0; guard < 12 && !fid.empty(); ++guard)
            {
                bj::object fr;
                if (call("SPATIAL.GET_REFERENCE_FRAME", { { "frame", fid } }, fr, why) != FV_SPATIAL_OK) break;
                auto fd = bj::parse(str(fr, "document")).as_object();
                FrameLink link{ fid, str(fd, "class"), str(fd, "units"), str(fd, "accuracy"), {}, {} };
                if (auto* t = fd.if_contains("transform_to_parent"))
                {
                    link.transform = str(t->as_object(), "type") + " · " + str(t->as_object(), "accuracy");
                    link.provenance = str(t->as_object(), "provenance");
                }
                c.frames.push_back(link);
                fid = str(fd, "parent");
            }
            // Scene manifest -> Storm context.
            model.navStage = "Loading scene";
            publish();
            bj::object sc;
            if (call("SPATIAL.GET_SCENE", { { "scene", c.scene } }, sc, why) != FV_SPATIAL_OK) throw std::runtime_error("scene refused: " + why);
            auto man = bj::parse(str(sc, "document")).as_object();
            c.sceneTitle = str(man, "title");
            c.live = man.contains("live");
            if (auto* sel = man.if_contains("selectable"))
                for (auto& e : sel->as_array())
                {
                    auto& o = e.as_object();
                    for (auto& a : o.at("assets").as_array()) c.selectable[str(o, "entity")].push_back(std::string(a.as_string()));
                }
            pan[0] = pan[1] = pan[2] = 0;  // a new context starts framed on its own focus
            model.creation.previewActive = false;
            frameSelection = false;
            writeContext(c, man, c.transition == "none" ? "semantic" : c.transition, {});
            currentManifest = man;
        }
        catch (const std::exception& e)
        {
            model.navigating = false;
            toast(std::string("Navigation failed: ") + e.what());
            LL_WARNS("FinalverseShell") << "navigate failed: " << e.what() << LL_ENDL;
            return false;
        }
        model.ctx = c;
        model.selected.clear();
        model.selectedLabel.clear();
        if (pushHistory && history)
        {
            bj::array crumbs;
            for (auto& p : c.path) crumbs.push_back(bj::object{ { "entity", p.entity }, { "label", p.label } });
            const auto h = bj::serialize(bj::object{ { "entity", c.entity }, { "label", c.label }, { "representation", c.repId }, { "kind", c.repKind }, { "path", crumbs } });
            fv_spatial_history_push(history, h.data(), h.size());
        }
        refreshHistory();
        journeyPush(c.label);
        model.resolveMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        model.navStage = "Rendering";
        LL_INFOS("FinalverseShell") << "navigate target=" << c.label << " transition=" << c.transition << " representation=" << c.repKind << " frame=" << c.frame
                                    << " scene=" << c.sceneTitle << " fvp_navigate_ms=" << fmt(model.navigateMs, 3) << " resolve_total_ms=" << fmt(model.resolveMs, 1)
                                    << " control_seq=" << model.controlSeq << LL_ENDL;
        return true;
    }

    bj::object currentManifest;
    double camYaw = .6, camPitch = -.45, camDistance = 0;
    bool userCamera = false, orbit = false;
    // Ω-XIX camera + presentation: pan offset (declared units, added to the focus centre),
    // frame-the-selection and the engineering presentation preset.
    double pan[3]{};
    bool frameSelection = false;
    std::string presentation = "realistic";

    void writeContext(const Context& c, const bj::object& man, const std::string& transition, const std::vector<std::string>& highlight)
    {
        if (!privateDir(frames)) throw std::runtime_error("Storm renderer directory unavailable");
        std::string wd, label;
        for (auto& r : man.at("resources").as_array())
            if (str(r.as_object(), "role") == "world_definition") { wd = str(r.as_object(), "sha256"); label = str(r.as_object(), "cas"); }
        auto f = fwdBySha.find(wd);
        if (f == fwdBySha.end())  // a publication made after start-up: reindex the CAS map's definition folders
        {
            for (auto& [k, c] : cas)
                if (!c.fwdDir.empty() && fs::is_directory(c.fwdDir))
                    for (auto& e : fs::directory_iterator(c.fwdDir))
                        if (e.is_regular_file() && e.path().extension() == ".json") fwdBySha[sha256(readSmall(e.path(), 1 << 20))] = e.path().string();
            f = fwdBySha.find(wd);
        }
        auto cm = cas.find(label);
        if (f == fwdBySha.end() || cm == cas.end()) throw std::runtime_error("scene world definition " + wd.substr(0, 12) + " is not in the local CAS (" + label + ")");
        auto& home = man.at("camera").as_object().at("home").as_object();
        if (!userCamera) { camYaw = num(home.at("yaw")); camPitch = num(home.at("pitch")); camDistance = 0; }
        bj::object ctl{ { "schema", "finalverse.storm-scene-context/0.3" }, { "sequence", ++model.controlSeq }, { "context", c.scene }, { "fwd", f->second },
                        { "cas", cm->second.objects }, { "definition_sha256", wd },
                        { "focus", bj::object{ { "center", bj::array{ c.focus[0] + pan[0], c.focus[1] + pan[1], c.focus[2] + pan[2] } }, { "extent", bj::array{ c.focus[3], c.focus[4], c.focus[5] } } } },
                        { "transition", reducedMotion ? "none" : transition }, { "live", man.contains("live") }, { "orbit", orbit } };
        if (reducedMotion) ctl["reduced_motion"] = true;
        bj::object cam{ { "yaw", camYaw }, { "pitch", camPitch } };
        if (camDistance > 0) cam["distance_m"] = camDistance;
        ctl["camera"] = cam;
        bj::array hl;
        for (auto& h : highlight) hl.push_back(bj::value(h));
        ctl["highlight"] = hl;
        bj::array ov;
        if (auto* o = man.if_contains("overlays"))
            for (auto& x : o->as_array())
                ov.push_back(bj::object{ { "position", x.as_object().at("position") }, { "radius", x.as_object().contains("radius") ? x.as_object().at("radius") : bj::value(10.0) } });
        ctl["overlays"] = ov;
        if (presentation != "realistic") ctl["presentation"] = presentation;
        if (frameSelection && !highlight.empty()) ctl["frame"] = "selection";
        // Ω-XX: visual style (presentation only) and the Genesis world growing on this context
        if (!model.style.empty()) ctl["style"] = model.style;
        if (!genesisWorld.empty() && c.scene == genesisScene) ctl["genesis"] = bj::object{ { "stream", genesisStream }, { "world", genesisWorld } };
        model.nativeKind.clear();
        if (!writePrivate(fs::path(frames) / "context.json", bj::serialize(ctl))) throw std::runtime_error("context.json write failed");
    }

    void select(const std::string& entity, const std::string& label)
    {
        model.selected = entity;
        model.selectedLabel = label;
        std::vector<std::string> hl;
        if (auto it = model.ctx.selectable.find(entity); it != model.ctx.selectable.end()) hl = it->second;
        try { writeContext(model.ctx, currentManifest, "none", hl); } catch (const std::exception& e) { toast(e.what()); }
    }
    void camera(double dyaw, double dpitch, double zoom)
    {
        if (model.ctx.scene.empty()) return;
        userCamera = true;
        camYaw += dyaw;
        camPitch = llclamp(camPitch + dpitch, -1.45, -.03);
        if (zoom != 1.0)
        {
            if (camDistance <= 0)
            {
                double ext = 0;
                for (int i = 3; i < 6; ++i) ext = std::max(ext, model.ctx.focus[i]);
                double unit = 1;
                for (auto& r : model.ctx.reps) if (r.id == model.ctx.repId && r.units == "millimetre") unit = .001;
                camDistance = std::max(ext * unit * 1.7, .06);
            }
            camDistance = llclamp(camDistance * zoom, .02, 4000.0);
        }
        std::vector<std::string> hl;
        if (auto it = model.ctx.selectable.find(model.selected); it != model.ctx.selectable.end()) hl = it->second;
        try { writeContext(model.ctx, currentManifest, "none", hl); } catch (...) {}
    }
    double declaredUnit() const
    {
        for (auto& r : model.ctx.reps) if (r.id == model.ctx.repId && r.units == "millimetre") return .001;
        return 1.0;
    }
    std::vector<std::string> selectionAssets() const
    {
        if (auto it = model.ctx.selectable.find(model.selected); it != model.ctx.selectable.end()) return it->second;
        return {};
    }
    // Screen-plane pan (presentation only): drag pixels -> declared-unit focus offset, using the
    // renderer's orbit convention (camera at (sin yaw, -sin pitch, cos yaw) * distance).
    void panView(double dx, double dy)
    {
        if (model.ctx.scene.empty()) return;
        userCamera = true;
        double dist = camDistance;
        if (dist <= 0) { double ext = 0; for (int i = 3; i < 6; ++i) ext = std::max(ext, model.ctx.focus[i]); dist = std::max(ext * declaredUnit() * 1.7, .06); }
        const double k = dist * .0016 / declaredUnit();
        const double cy = std::cos(camYaw), sy = std::sin(camYaw), sp = std::sin(-camPitch), cp = std::cos(camPitch);
        // render-space right = (cos yaw, 0, -sin yaw); up ≈ (-sp*sin yaw, cp, -sp*cos yaw)
        const double rx = cy, rz = -sy, ux = -sp * sy, uy = cp, uz = -sp * cy;
        const double wx = -dx * k * rx + dy * k * ux, wy = dy * k * uy, wz = -dx * k * rz + dy * k * uz;
        pan[0] += wx; pan[1] += -wz; pan[2] += wy;  // render (x, up, -north) -> declared (x, north, up)
        frameSelection = false;
        try { writeContext(model.ctx, currentManifest, "none", selectionAssets()); } catch (...) {}
    }
    void frameSelected()
    {
        if (model.ctx.scene.empty()) return;
        if (selectionAssets().empty()) { toast("Select a part to frame it (Tab or the Here list)"); return; }
        frameSelection = true;
        camDistance = 0;
        pan[0] = pan[1] = pan[2] = 0;
        try { writeContext(model.ctx, currentManifest, "geometric", selectionAssets()); } catch (...) {}
    }
    void setPresentation(const std::string& p)
    {
        presentation = p;
        model.presentation = p;
        if (!model.ctx.scene.empty()) try { writeContext(model.ctx, currentManifest, "none", selectionAssets()); } catch (...) {}
    }
    // ------------------------------------------------------------------ Ω-XX universal contexts
    // Native contexts are built by the renderer from identified models (no FWD): astronomical
    // (ephemeris, Blue Marble Earth), scientific (idealised crystal / illustrative
    // microstructure), abstract (civilization graph). Every jump is recorded in the journey.
    std::string genesisDir, genesisProgress, genesisStream, genesisWorld, genesisScene, graphFile;
    void journeyPush(const std::string& label)
    {
        if (model.journey.empty() || model.journey.back() != label) model.journey.push_back(label);
        while (model.journey.size() > 24) model.journey.erase(model.journey.begin());
    }
    void writeNative(const std::string& kind, const bj::object& params, const std::string& transition)
    {
        if (!privateDir(frames)) { toast("Storm renderer directory unavailable"); return; }
        static const std::map<std::string, std::string> ids = { { "astronomical", "4f9d0c8e-0000-5000-8000-0000000000a1" },
                                                                { "scientific", "4f9d0c8e-0000-5000-8000-0000000000b2" },
                                                                { "abstract", "4f9d0c8e-0000-5000-8000-0000000000c3" } };
        bj::object ctl{ { "schema", "finalverse.storm-scene-context/0.3" }, { "sequence", ++model.controlSeq }, { "context", ids.at(kind) }, { "kind", kind },
                        { "transition", reducedMotion ? "none" : transition }, { kind, params } };
        if (reducedMotion) ctl["reduced_motion"] = true;
        if (!model.style.empty() && kind != "astronomical") ctl["style"] = model.style;
        if (!writePrivate(fs::path(frames) / "context.json", bj::serialize(ctl))) { toast("context.json write failed"); return; }
        navStart = Clock::now();
        model.navigating = true;
    }
    void goAstronomical(bool descend)
    {
        const double now = (double)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (model.nativeKind != "astronomical" || model.simEpoch == 0) { model.simEpoch = now; model.nativeSince = Clock::now(); }
        model.simScale = descend ? 0 : 120;
        model.nativeDistanceKm = descend ? 2600 : 23000;
        bj::object a{ { "time_unix", model.simEpoch }, { "time_scale", model.simScale }, { "distance_km", 23000.0 }, { "yaw", -.05 }, { "pitch", .2 } };
        if (descend) { a["descend"] = true; a["lat"] = 45.5017; a["lon"] = -73.5673; a["yaw"] = .95; a["pitch"] = .06; a["distance_km"] = 15000.0; }
        writeNative("astronomical", a, "semantic");
        model.nativeKind = "astronomical";
        model.nativeLabel = descend ? "Descending to Montréal" : "Earth · Moon";
        journeyPush(descend ? "Descent to Montréal" : "Earth from space");
        LL_INFOS("FinalverseShell") << "native astronomical descend=" << descend << LL_ENDL;
    }
    void goScientific(const std::string& modelId)
    {
        bj::object sc{ { "model", modelId } };
        if (modelId == "bcc-iron") sc["cells"] = 4;
        writeNative("scientific", sc, "semantic");
        model.nativeKind = "scientific";
        model.nativeModel = modelId;
        model.nativeLabel = modelId == "bcc-iron" ? "alpha-iron crystal (idealised BCC)" : "Tempered martensite (illustrative)";
        journeyPush(modelId == "bcc-iron" ? "Crystal structure" : "Microstructure");
        LL_INFOS("FinalverseShell") << "native scientific model=" << modelId << LL_ENDL;
    }
    void goGraph()
    {
        if (graphFile.empty()) { toast("No civilization graph configured"); return; }
        writeNative("abstract", bj::object{ { "graph", graphFile } }, "semantic");
        model.nativeKind = "abstract";
        model.nativeLabel = "Civilization relationships (demonstration)";
        journeyPush("Civilization graph");
    }
    void setStyle(const std::string& st)
    {
        model.style = st == "default" ? std::string() : st;
        LL_INFOS("FinalverseShell") << "style " << (model.style.empty() ? "default" : model.style) << LL_ENDL;
        if (model.nativeKind == "scientific") goScientific(model.nativeModel);
        else if (model.nativeKind == "abstract") goGraph();
        else if (model.nativeKind.empty() && !model.ctx.scene.empty()) try { writeContext(model.ctx, currentManifest, "none", selectionAssets()); } catch (...) {}
        toast("Style: " + (model.style.empty() ? std::string("cinematic (default)") : model.style) + " — presentation only");
    }
    void returnBack()
    {
        // Ω-XX return path: scientific -> engineering component, abstract/astronomical -> the
        // last geographic/engineering context; geographic -> Earth orbit.
        if (model.nativeKind == "scientific" || model.nativeKind == "abstract") { model.nativeKind.clear(); journeyPush(model.ctx.label); try { writeContext(model.ctx, currentManifest, "semantic", selectionAssets()); } catch (...) {} }
        else if (model.nativeKind.empty()) goAstronomical(false);
    }
    // ------------------------------------------------------------------ Ω-XX Genesis
    std::string genesisRequestId;
    Clock::time_point genesisStarted = Clock::now();
    void genesisCreate(const std::string& prompt)
    {
        if (!privateDir(genesisDir) || !privateDir(frames)) { toast("Genesis is not configured"); return; }
        if (prompt.size() < 3 || prompt.size() > 600) { toast("Describe the world in 3–600 characters"); return; }
        genesisRequestId = LLUUID::generateNewID().asString();
        genesisStream = (fs::path(frames) / ("genesis-" + genesisRequestId.substr(0, 8) + ".jsonl")).string();
        genesisWorld.clear();
        model.genesis = Model::Genesis{};
        model.genesis.active = true;
        model.genesis.stage = "understanding";
        model.genesis.requestId = genesisRequestId;
        genesisStarted = Clock::now();
        // grow it beside the Montréal dataset (hybrid world), camera on the campus site
        if (model.ctx.entity != startEntity) navigate(startEntity);
        genesisScene = model.ctx.scene;
        userCamera = true; camYaw = 3.9; camPitch = -.16; camDistance = 560;
        pan[0] = 620 - model.ctx.focus[0]; pan[1] = -250 - model.ctx.focus[1]; pan[2] = 0;
        bj::object req{ { "schema", "finalverse.genesis-request/1" }, { "request_id", genesisRequestId }, { "prompt", prompt }, { "stream", genesisStream } };
        if (!model.style.empty()) req["style"] = model.style;
        writePrivate(fs::path(genesisDir) / ("request-" + genesisRequestId + ".json"), bj::serialize(req));
        journeyPush("Genesis: " + prompt.substr(0, 40));
        try { writeContext(model.ctx, currentManifest, "none", {}); } catch (const std::exception& e) { toast(e.what()); }
        LL_INFOS("FinalverseShell") << "genesis request id=" << genesisRequestId << " prompt=" << prompt << LL_ENDL;
    }
    void pollGenesis()
    {
        if (!model.genesis.active) return;
        auto& g = model.genesis;
        g.elapsed = std::chrono::duration<double>(Clock::now() - genesisStarted).count();
        if (auto raw = readSmall(genesisProgress, 1 << 20); !raw.empty())
            try
            {
                auto j = bj::parse(raw).as_object();
                if (str(j, "request_id") == genesisRequestId)
                {
                    g.stage = str(j, "phase");
                    g.phase = str(j, "phase");
                    if (auto* p = j.if_contains("progress")) g.progress = num(*p);
                    g.title = str(j, "title", g.title);
                    g.reality = str(j, "reality", g.reality);
                    g.error = str(j, "error", g.error);
                    if (auto* m2 = j.if_contains("model"); m2 && m2->is_object()) { g.model = str(m2->as_object(), "model"); if (auto* sec = m2->as_object().if_contains("seconds")) g.modelSeconds = num(*sec); }
                    if (auto* fsrc = j.if_contains("field_sources"); fsrc && fsrc->is_object()) for (auto& [k, x] : fsrc->as_object()) g.fieldSources[std::string(k)] = x.is_string() ? std::string(x.as_string()) : "";
                    if (auto* sm = j.if_contains("summary"); sm && sm->is_object()) g.summary = bj::serialize(*sm);
                    g.refined = str(j, "refined", g.refined);
                    const std::string w = str(j, "world");
                    if (!w.empty() && w != genesisWorld)
                    {
                        genesisWorld = w;
                        g.world = w;
                        if (model.nativeKind.empty() && model.ctx.scene == genesisScene) try { writeContext(model.ctx, currentManifest, "none", {}); } catch (...) {}
                        LL_INFOS("FinalverseShell") << "genesis world=" << w << " title=" << g.title << LL_ENDL;
                    }
                }
            }
            catch (...) {}
        if (auto raw = readSmall(fs::path(frames) / "genesis-status.json", 1 << 20); !raw.empty())
            try
            {
                auto j = bj::parse(raw).as_object();
                if (str(j, "world") == genesisWorld && !genesisWorld.empty())
                {
                    auto u = [&](const char* k) { auto* x = j.if_contains(k); return x ? (uint64_t)num(*x) : 0ull; };
                    g.applied = u("applied"); g.refused = u("refused"); g.triangles = u("triangles"); g.objects = u("objects"); g.queued = u("queued");
                    if (auto* x = j.if_contains("frame_ms_p50")) g.frameP50 = num(*x);
                    if (auto* x = j.if_contains("frame_ms_p95")) g.frameP95 = num(*x);
                    g.style = str(j, "style", g.style);
                    g.resident.clear();
                    if (auto* r = j.if_contains("resident"); r && r->is_object()) for (auto& [k, x] : r->as_object()) g.resident[std::string(k)] = (int)num(x);
                }
            }
            catch (...) {}
    }

    void resetCamera() { userCamera = false; camDistance = 0; pan[0] = pan[1] = pan[2] = 0; frameSelection = false; if (!model.ctx.scene.empty()) try { writeContext(model.ctx, currentManifest, "geometric", {}); } catch (...) {} }

    void refreshHistory()
    {
        char* js = nullptr;
        size_t len = 0;
        if (!history || fv_spatial_history_json(history, &js, &len) != FV_SPATIAL_OK) return;
        try
        {
            auto h = bj::parse(std::string_view(js, len)).as_object();
            model.history.clear();
            model.historyEntities.clear();
            for (auto& e : h.at("entries").as_array()) { model.history.push_back(str(e.as_object(), "label")); model.historyEntities.push_back(str(e.as_object(), "entity")); }
            model.historyCursor = h.at("cursor").to_number<size_t>();
        }
        catch (...) {}
        fv_spatial_free(js);
    }
    void back(bool forward)
    {
        char* js = nullptr;
        size_t len = 0;
        if (!history || (forward ? fv_spatial_history_forward(history, &js, &len) : fv_spatial_history_back(history, &js, &len)) != FV_SPATIAL_OK)
        {
            toast(forward ? "No later context" : "No earlier context");
            return;
        }
        try
        {
            auto e = bj::parse(std::string_view(js, len)).as_object();
            navigate(str(e, "entity"), {}, false, str(e, "kind"));
        }
        catch (...) {}
        fv_spatial_free(js);
        refreshHistory();
    }

    // ------------------------------------------------------------------ AI Spatial Navigator
    // ------------------------------------------------------------------ Ω-XIX creation
    static bool creationIntent(const std::string& text)
    {
        static const std::regex verbs("\\b(create|build|design|set up|add|make|furnish|lay out)\\b", std::regex::icase);
        return std::regex_search(text, verbs);
    }
    void creationRequest(bj::object req)
    {
        if (!privateDir(creationDir)) { toast("The World Director is not configured"); return; }
        creationRequestId = LLUUID::generateNewID().asString();
        req["schema"] = "finalverse.creation-request/0.1";
        req["id"] = creationRequestId;
        creationStarted = Clock::now();
        model.creation.stage = "working";
        writePrivate(fs::path(creationDir) / "request.json", bj::serialize(req));
        LL_INFOS("FinalverseShell") << "creation request kind=" << str(req, "kind") << " id=" << creationRequestId << LL_ENDL;
    }
    void create(const std::string& text)
    {
        model.ai = AiCard{};  // the creation card replaces any previous answer
        model.creation = Model::Creation{};
        model.creation.request = text;
        model.creation.message = "The World Director is proposing a typed creation plan…";
        creationRequest({ { "kind", "create" }, { "text", text }, { "context_entity", model.ctx.entity } });
    }
    void creationEdit(const std::string& op, const std::string& object, bj::value value)
    {
        if (model.creation.draftId.empty() || model.creation.published) return;
        model.creation.message = "Applying " + op + "…";
        creationRequest({ { "kind", "edit" }, { "draft_id", model.creation.draftId }, { "op", op }, { "object", object }, { "value", value } });
    }
    void creationHistory(bool redo)
    {
        if (model.creation.draftId.empty() || model.creation.published) return;
        creationRequest({ { "kind", redo ? "redo" : "undo" }, { "draft_id", model.creation.draftId } });
    }
    void previewDraft(bool first)
    {
        if (creationPreview.empty() || !privateDir(frames)) return;
        const auto& pv = creationPreview;
        auto& focus = pv.at("focus").as_object();
        std::vector<std::string> hl;
        for (auto& o : model.creation.objects) if (o.id == model.creation.selected) hl.push_back(o.sha);
        bj::object ctl{ { "schema", "finalverse.storm-scene-context/0.2" }, { "sequence", ++model.controlSeq }, { "context", str(pv, "context_id") },
                        { "fwd", str(pv, "fwd") }, { "cas", str(pv, "cas") }, { "definition_sha256", str(pv, "definition_sha256") },
                        { "focus", bj::object{ { "center", focus.at("center") }, { "extent", focus.at("extent") } } },
                        { "transition", reducedMotion ? "none" : (first ? "semantic" : "none") }, { "live", false }, { "orbit", false } };
        if (reducedMotion) ctl["reduced_motion"] = true;
        bj::object cam{ { "yaw", first ? .35 : camYaw }, { "pitch", first ? -.55 : camPitch } };
        if (first) { camYaw = .35; camPitch = -.55; camDistance = 0; }
        if (camDistance > 0 && !first) cam["distance_m"] = camDistance;
        ctl["camera"] = cam;
        bj::array h;
        for (auto& x : hl) h.push_back(bj::value(x));
        ctl["highlight"] = h;
        writePrivate(fs::path(frames) / "context.json", bj::serialize(ctl));
        model.creation.previewActive = true;
        navStart = Clock::now();
        model.navigating = true;
    }
    void selectDraftObject(const std::string& id)
    {
        model.creation.selected = id;
        previewDraft(false);
    }
    void pollCreation()
    {
        if (creationRequestId.empty() || model.creation.stage != "working") return;
        if (Clock::now() - creationStarted > std::chrono::seconds(180)) { model.creation.stage = "refused"; model.creation.message = "The World Director did not answer within 180 s; nothing changed."; return; }
        auto raw = readSmall(fs::path(creationDir) / "status.json", 4 << 20);
        if (raw.empty()) return;
        try
        {
            auto j = bj::parse(raw).as_object();
            if (str(j, "schema") != "finalverse.creation-status/0.1" || str(j, "id") != creationRequestId) return;
            const std::string stage = str(j, "stage");
            if (stage == "working") return;
            auto& c = model.creation;
            c.message = str(j, "message");
            c.seconds = std::chrono::duration<double>(Clock::now() - creationStarted).count();
            if (stage == "refused") { c.stage = c.draftId.empty() ? "refused" : "preview_ready"; toast(c.message); LL_INFOS("FinalverseShell") << "creation refused: " << c.message << LL_ENDL; return; }
            auto& d = j.at("draft").as_object();
            const bool first = c.draftId != str(d, "draft_id");
            c.draftId = str(d, "draft_id");
            c.revision = d.at("revision").to_number<uint64_t>();
            c.name = str(d, "name");
            for (auto& ch : c.name) if (ch == '_') ch = ' ';
            c.summary = str(d, "summary");
            c.zone = str(d, "zone_label");
            auto& v = d.at("validation").as_object();
            c.valid = v.at("passed").as_bool();
            c.findings.clear();
            for (auto& f : v.at("findings").as_array()) c.findings.push_back(str(f.as_object(), "message"));
            c.checks.clear();
            for (auto& kv : v.at("checks").as_object()) c.checks.push_back(std::string(kv.key()) + (kv.value().as_bool() ? " ✓" : " ✕"));
            c.objects.clear();
            for (auto& o : d.at("objects").as_array())
            {
                auto& x = o.as_object();
                Model::CreationObject co{ str(x, "id"), str(x, "label"), str(x, "asset"), str(x, "material"), str(x, "sha256") };
                co.x = num(x.at("position").as_array()[0]);
                co.y = num(x.at("position").as_array()[1]);
                co.rot = num(x.at("rotation_deg"));
                c.objects.push_back(co);
            }
            if (c.selected.empty() || std::none_of(c.objects.begin(), c.objects.end(), [&](auto& o) { return o.id == c.selected; }))
                c.selected = c.objects.empty() ? std::string() : c.objects.front().id;
            auto& h = d.at("history").as_object();
            c.histCursor = h.at("cursor").to_number<size_t>();
            c.histSize = h.at("revisions").as_array().size();
            c.edits.clear();
            for (auto& e : d.at("edits").as_array()) c.edits.push_back(str(e.as_object(), "description"));
            if (auto* pv = d.if_contains("provenance")) c.model = str(pv->as_object(), "model");
            if (auto* st = d.if_contains("steps"))
                for (auto& s2 : st->as_array()) if (str(s2.as_object(), "stage") == "plan" && s2.as_object().contains("seconds")) c.planSeconds = num(s2.as_object().at("seconds"));
            c.published = str(d.at("publication").as_object(), "status") == "published";
            creationPreview = d.at("preview").as_object();
            c.stage = "preview_ready";
            LL_INFOS("FinalverseShell") << "creation preview draft=" << c.draftId << " revision=" << c.revision << " objects=" << c.objects.size() << " valid=" << c.valid
                                        << " seconds=" << fmt(c.seconds, 1) << " " << c.message << LL_ENDL;
            previewDraft(first);
        }
        catch (const std::exception& e) { LL_WARNS("FinalverseShell") << "creation status: " << e.what() << LL_ENDL; }
    }
    std::string ownerPrincipal() const
    {
        const char* tokenPath = std::getenv("FINALVERSE_LIVE_TOKENS");
        if (!tokenPath) return {};
        try
        {
            auto j = bj::parse(readSmall(tokenPath, 65536)).as_object();
            for (auto& kv : j.at("tokens").as_object()) { std::string k(kv.key()); if (k.rfind("owner:", 0) == 0) return k.substr(6); }
        }
        catch (...) {}
        return {};
    }
    // Called only from the confirmation dialog's Approve action (the owner's explicit decision).
    void publishApproved()
    {
        auto& c = model.creation;
        if (c.draftId.empty() || !c.valid || c.published) { toast("Only a valid, unpublished draft can be published"); return; }
        if (!privateDir(publisherDir)) { toast("The publisher is not configured"); return; }
        const std::string owner = ownerPrincipal();
        if (owner.empty()) { toast("No owner identity is available for approval"); return; }
        publishRequestId = LLUUID::generateNewID().asString();
        bj::object req{ { "schema", "finalverse.publication-request/0.1" }, { "id", publishRequestId }, { "draft_id", c.draftId }, { "revision", c.revision },
                        { "approval", bj::object{ { "decision", "approved" }, { "principal", owner }, { "via", "FINALVERSE Viewer confirmation dialog" },
                                                  { "statement", "Publish draft '" + c.name + "' revision " + std::to_string(c.revision) + " to the live world" },
                                                  { "approved_at", (int64_t)std::time(nullptr) } } } };
        writePrivate(fs::path(publisherDir) / "request.json", bj::serialize(req));
        c.pubStage = "publishing";
        c.pubMessage = "Publishing…";
        c.pubSteps.clear();
        LL_INFOS("FinalverseShell") << "publication approved by owner=" << owner << " draft=" << c.draftId << " revision=" << c.revision << LL_ENDL;
    }
    void pollPublisher()
    {
        auto& c = model.creation;
        if (publishRequestId.empty() || c.pubStage != "publishing") return;
        auto raw = readSmall(fs::path(publisherDir) / "status.json", 1 << 20);
        if (raw.empty()) return;
        try
        {
            auto j = bj::parse(raw).as_object();
            if (str(j, "id") != publishRequestId) return;
            c.pubSteps.clear();
            if (auto* st = j.if_contains("steps")) for (auto& s2 : st->as_array()) c.pubSteps.push_back(str(s2.as_object(), "step"));
            c.pubMessage = str(j, "message");
            const std::string stage = str(j, "stage");
            if (stage == "published")
            {
                c.pubStage = "published";
                c.published = true;
                auto& r = j.at("receipt").as_object();
                c.receipt = "catalog edition " + std::to_string(r.at("spatialdb").as_object().at("catalog_edition").to_number<int64_t>()) + " · MutSea revision " +
                            std::to_string(r.at("mutsea").as_object().at("head_after").to_number<int64_t>());
                const std::string lab = str(r.at("spatialdb").as_object(), "lab_entity");
                LL_INFOS("FinalverseShell") << "publication done " << c.receipt << " lab=" << lab << " total_s=" << fmt(num(r.at("timings").as_object().at("total_s")), 2) << LL_ENDL;
                model.creation.previewActive = false;
                navigate(lab);  // the published lab, served by SpatialDB from the new edition
            }
            else if (stage == "refused") { c.pubStage = "refused"; toast("Publication refused: " + c.pubMessage); LL_INFOS("FinalverseShell") << "publication refused: " << c.pubMessage << LL_ENDL; }
        }
        catch (const std::exception& e) { LL_WARNS("FinalverseShell") << "publisher status: " << e.what() << LL_ENDL; }
    }

    // Ω-XIX: palette entity search over FVP SPATIAL.DISCOVER (results already scope-filtered).
    void search(const std::string& q)
    {
        model.searchQuery = q;
        model.found.clear();
        if (q.size() < 2 || !client) return;
        bj::object out;
        std::string why;
        if (call("SPATIAL.DISCOVER", { { "query", q }, { "limit", 6 } }, out, why) != FV_SPATIAL_OK) return;
        for (auto& e : out.at("entities").as_array())
        {
            auto& o = e.as_object();
            model.found.push_back({ str(o, "entity"), str(o, "label"), str(o, "kind"), str(o, "domain") });
        }
    }
    // Set from the UI thread directly (the engine may be inside a synchronous route execution).
    std::atomic<bool> aiCancel{false};
    std::string lastAsk;
    void ask(const std::string& text)
    {
        if (!creationDir.empty() && creationIntent(text) && !std::regex_search(text, std::regex("\\b(take me|go to|show me|navigate)\\b", std::regex::icase)))
        {
            create(text);
            return;
        }
        aiCancel = false;
        lastAsk = text;
        model.ai = AiCard{};
        model.ai.query = text;
        if (!privateDir(navigatorDir))
        {
            // No model channel: deterministic discovery only, labelled as such.
            bj::object out;
            std::string why;
            if (call("SPATIAL.DISCOVER", { { "query", text }, { "limit", 1 } }, out, why) == FV_SPATIAL_OK && !out.at("entities").as_array().empty())
            {
                auto& e = out.at("entities").as_array()[0].as_object();
                model.ai.stage = "done";
                model.ai.title = "Search (no model configured)";
                model.ai.message = "Best match: " + str(e, "label");
                navigate(str(e, "entity"));
            }
            else { model.ai.stage = "refused"; model.ai.message = "No AI navigator is configured and search found nothing."; }
            return;
        }
        aiRequestId = LLUUID::generateNewID().asString();
        aiStarted = Clock::now();
        bj::object req{ { "schema", "finalverse.spatial-navigation-request/0.1" }, { "id", aiRequestId }, { "text", text },
                        { "context_entity", model.ctx.entity }, { "selected_entity", model.selected } };
        model.ai.id = aiRequestId;
        model.ai.stage = "planning";
        model.ai.title = text;
        model.ai.message = "The local model is proposing a typed navigation plan…";
        writePrivate(fs::path(navigatorDir) / "request.json", bj::serialize(req));
        LL_INFOS("FinalverseShell") << "ai request id=" << aiRequestId << " text=" << text << LL_ENDL;
    }
    void pollAi()
    {
        if (aiRequestId.empty() || model.ai.stage != "planning") return;
        if (aiCancel)
        {
            model.ai.stage = "cancelled";
            model.ai.message = "Cancelled before any step ran; nothing was executed.";
            LL_INFOS("FinalverseShell") << "ai cancelled id=" << aiRequestId << " stage=planning" << LL_ENDL;
            aiRequestId.clear();
            return;
        }
        if (Clock::now() - aiStarted > std::chrono::seconds(120))
        {
            model.ai.stage = "refused";
            model.ai.message = "The AI navigator did not answer within 120 s; nothing was executed.";
            return;
        }
        auto raw = readSmall(fs::path(navigatorDir) / "status.json", 1 << 20);
        if (raw.empty()) return;
        try
        {
            auto j = bj::parse(raw).as_object();
            if (str(j, "schema") != "finalverse.spatial-navigation-status/0.1" || str(j, "id") != aiRequestId) return;
            const std::string stage = str(j, "stage");
            if (stage == "planning") return;
            AiCard& c = model.ai;
            c.stage = stage;
            c.message = str(j, "message");
            c.policy = str(j, "policy");
            c.model = str(j, "model");
            c.planSeconds = std::chrono::duration<double>(Clock::now() - aiStarted).count();
            if (auto* p = j.if_contains("plan_lines")) for (auto& x : p->as_array()) c.plan.push_back(std::string(x.as_string()));
            if (auto* r = j.if_contains("route"))
                for (auto& x : r->as_array()) { c.route.push_back(str(x.as_object(), "entity")); c.routeLabels.push_back(str(x.as_object(), "label")); }
            if (auto* mi = j.if_contains("missing")) for (auto& x : mi->as_array()) c.missing.push_back(std::string(x.as_string()));
            if (stage == "answered")
            {
                c.answer = str(j, "answer");
                c.general = str(j, "general_knowledge");
                c.subject = str(j, "subject");
                if (auto* f = j.if_contains("facts"))
                    for (auto& x : f->as_array()) c.facts.push_back({ str(x.as_object(), "class"), str(x.as_object(), "text") });
                c.stage = "done";
                LL_INFOS("FinalverseShell") << "ai answered subject=" << c.subject << " facts=" << c.facts.size() << " seconds=" << fmt(c.planSeconds, 1) << LL_ENDL;
                return;
            }
            LL_INFOS("FinalverseShell") << "ai status stage=" << stage << " route=" << c.route.size() << " seconds=" << fmt(c.planSeconds, 1) << " " << c.message << LL_ENDL;
            if (stage == "ready" && !c.route.empty())
            {
                // Deterministic execution of the validated route through FVP NAVIGATE.
                const auto route = c.route;
                const std::string capability = str(j, "capability");
                c.stage = "executing";
                for (size_t i = 0; i < route.size(); ++i)
                {
                    if (aiCancel)
                    {
                        model.ai.stage = "cancelled";
                        model.ai.message = "Cancelled after " + std::to_string(i) + " of " + std::to_string(route.size()) + " steps; navigation stopped where it was.";
                        LL_INFOS("FinalverseShell") << "ai cancelled id=" << aiRequestId << " after_steps=" << i << LL_ENDL;
                        return;
                    }
                    model.ai.executing = i + 1;
                    publish();
                    const bool last = i + 1 == route.size();
                    if (route[i] == model.ctx.entity && !last) continue;
                    if (!navigate(route[i], last ? capability : std::string())) { model.ai.stage = "refused"; model.ai.message = "Execution stopped: " + (model.toasts.empty() ? std::string() : model.toasts.back().first); return; }
                    waitSettled(last ? 0.2 : (reducedMotion ? .4 : 1.35));
                }
                model.ai.stage = "done";
            }
        }
        catch (const std::exception& e) { LL_WARNS("FinalverseShell") << "navigator status: " << e.what() << LL_ENDL; }
    }
    void waitSettled(double dwell)
    {
        const auto t0 = Clock::now();
        while (!stop && !aiCancel && std::chrono::duration<double>(Clock::now() - t0).count() < 6.0)
        {
            tickFrames();
            tickLive();
            publish();
            auto f = frame;
            if (f && f->controlSeq >= model.controlSeq && f->settled && std::chrono::duration<double>(Clock::now() - t0).count() >= dwell) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        model.navigating = false;
    }

    // ------------------------------------------------------------------ live world (Ω-XVII.3 paths)
    static std::pair<std::string, uint16_t> hostPort(const std::string& s)
    {
        auto c = s.rfind(':');
        return c == std::string::npos ? std::make_pair(s, (uint16_t)0) : std::make_pair(s.substr(0, c), (uint16_t)std::atoi(s.substr(c + 1).c_str()));
    }
    void startLive()
    {
        const char* tokenPath = std::getenv("FINALVERSE_LIVE_TOKENS");
        if (!tokenPath) return;
        try
        {
            auto j = bj::parse(readSmall(tokenPath, 65536)).as_object();
            world = str(j, "world");
            region = str(j, "region");
            std::tie(sessionHost, sessionPort) = hostPort(str(j, "session"));
            std::tie(replicationHost, replicationPort) = hostPort(str(j, "replication"));
            for (auto& [k, v] : j.at("tokens").as_object()) tokens[std::string(k)] = std::string(v.as_string());
            observer = tokens.at("observer");
        }
        catch (const std::exception& e) { LL_WARNS("FinalverseShell") << "live tokens: " << e.what() << LL_ENDL; return; }
        if (const char* fwd = std::getenv("FINALVERSE_LIVE_FWD"))
            try
            {
                const bj::value d = bj::parse(readSmall(fwd, 8 << 20));
                std::map<std::string, bool> residentAsset;
                for (auto& a : d.as_object().at("assets").as_array())
                {
                    bool r = false;
                    for (auto& s : a.as_object().at("source_ids").as_array()) r = r || std::string(s.as_string()).starts_with("resident-");
                    residentAsset[str(a.as_object(), "id")] = r;
                }
                for (auto& b : d.as_object().at("simulation_bindings").as_array())
                    if (residentAsset[str(b.as_object(), "asset_id")]) boundBodies.insert(str(b.as_object(), "entity_id"));
            }
            catch (...) {}
        if (const char* reg = std::getenv("FINALVERSE_GEO_REGISTRATION"))
        {
            auto raw = readSmall(reg, 16384);
            try
            {
                auto j = bj::parse(raw).as_object();
                registrationRevision = j.at("revision").to_number<uint64_t>();
                char e[256] = {};
                if (fv_wc_registration_open(raw.data(), raw.size(), world.c_str(), region.c_str(), registrationRevision, &registration, e, sizeof e) == 0)
                    model.registrationLabel = j.at("validation").as_object().at("surveyed").as_bool() ? "Surveyed registration" : "Presentation-only registration · not surveyed";
                else model.registrationLabel = std::string("Registration refused: ") + e;
            }
            catch (...) {}
        }
        ms_replica_config cfg{};
        cfg.struct_size = sizeof cfg;
        cfg.host = replicationHost.c_str();
        cfg.port = replicationPort;
        cfg.token = observer.c_str();
        if (!parseUuid(world, cfg.world) || !parseUuid(region, cfg.region)) return;
        std::string checkpoint;
        if (const char* c = std::getenv("FINALVERSE_LIVE_CHECKPOINT")) checkpoint = c;
        cfg.checkpoint_path = checkpoint.empty() ? nullptr : checkpoint.c_str();
        if (ms_replica_start(&cfg, &replica) == MS_REPLICA_OK) model.liveConfigured = true;
    }
    uint64_t lastLiveVersion = UINT64_MAX;
    std::vector<ms_replica_entity> entities;
    void tickLive()
    {
        if (!replica) return;
        model.live.struct_size = sizeof model.live;
        const auto previousState = model.live.state;
        ms_replica_get_status(replica, &model.live);
        if (model.live.state != previousState)
            LL_INFOS("FinalverseShell") << "live state " << (int)previousState << " -> " << (int)model.live.state << " revision=" << model.live.revision
                                        << " resyncs=" << model.live.resyncs << " reconnects=" << model.live.reconnects << LL_ENDL;
        if (model.live.version != lastLiveVersion)
        {
            lastLiveVersion = model.live.version;
            size_t n = 0;
            entities.resize(512);
            ms_replica_entities(replica, entities.data(), entities.size(), &n, nullptr);
            entities.resize(std::min<size_t>(n, 512));
            model.residents.clear();
            for (const auto& e : entities)
            {
                std::string label = e.label;
                if (!label.starts_with("AI resident")) continue;
                Resident r;
                r.id = uuidText(e.id);
                auto first = label.find("·"), second = label.find("·", first == std::string::npos ? 0 : first + 2);
                r.name = first != std::string::npos ? label.substr(first + 3, second == std::string::npos ? std::string::npos : second - first - 4) : label;
                LLStringUtil::trim(r.name);
                std::copy(e.position, e.position + 3, r.region);
                r.revision = e.revision;
                if (registration && fv_wc_to_asset(registration, registrationRevision, r.region, r.displayed) == 0) r.registered = true;
                model.residents.push_back(r);
            }
            char* json = nullptr;
            size_t len = 0;
            if (ms_replica_entities_json(replica, &json, &len) == MS_REPLICA_OK)
            {
                try
                {
                    const bj::value arr = bj::parse(std::string_view(json, len));
                    std::map<std::string, std::string> assets;
                    for (auto& x : arr.as_array())
                        if (auto* a = x.as_object().if_contains("asset"); a && a->is_object()) assets[str(x.as_object(), "id")] = str(a->as_object(), "id");
                    for (auto& r : model.residents) r.asset = assets[r.id];
                }
                catch (...) {}
                ms_replica_free(json);
            }
            if (ms_replica_recent_events_json(replica, &json, &len) == MS_REPLICA_OK)
            {
                try
                {
                    model.events.clear();
                    const bj::value doc = bj::parse(std::string_view(json, len));
                    for (auto& x : doc.as_array())
                    {
                        auto& o = x.as_object();
                        Event ev;
                        ev.seq = o.at("seq").to_number<uint64_t>();
                        ev.revision = o.at("revision").to_number<uint64_t>();
                        auto& entry = o.at("entry").as_object();
                        ev.action = str(entry, "action");
                        ev.kind = str(entry, "principal_kind");
                        if (auto& eff = entry.at("effects").as_array(); !eff.empty()) ev.entity = str(eff[0].as_object(), "entity");
                        model.events.push_back(ev);
                    }
                }
                catch (...) {}
                ms_replica_free(json);
            }
        }
        if (model.live.state == MS_REPLICA_LIVE) writeScene();
    }
    void writeScene()
    {
        if (!privateDir(frames) || definition.size() != 64 || !registration) return;
        const bool present = fs::exists(fs::path(frames) / "scene-update.json");
        if (wroteScene && model.live.revision == lastSceneRevision && present) return;
        if (!present) wroteScene = false;
        bj::array list;
        for (const auto& r : model.residents)
        {
            if (!r.registered || r.asset.empty() || !boundBodies.count(r.id) || list.size() >= 16) continue;
            list.push_back(bj::object{ { "resident", r.asset }, { "entity", r.id }, { "position", bj::array{ r.displayed[0], r.displayed[1], r.displayed[2] } },
                                       { "source_position_region_m", bj::array{ r.region[0], r.region[1], r.region[2] } }, { "motion", "committed" } });
        }
        bj::object u{ { "schema", "finalverse.scene-update/0.1" }, { "world", world }, { "region", region }, { "epoch", hexOf(model.live.generation, 16) },
                      { "cursor", model.live.revision }, { "revision", model.live.revision },
                      { "previous_revision", wroteScene ? bj::value(lastSceneRevision) : bj::value(nullptr) }, { "full_snapshot", true },
                      { "definition_sha256", definition }, { "residents", list }, { "assets", bj::array{} }, { "event_refs", bj::array{} } };
        if (writePrivate(fs::path(frames) / "scene-update.json", bj::serialize(u))) { wroteScene = true; lastSceneRevision = model.live.revision; }
    }
    // Authorized movement through MutSea's own session protocol (Ω-XVII.3 semantics).
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
        if (fd >= 0) { timeval tv{ 5, 0 }; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
        return fd;
    }
    static bool sendMsg(int fd, const bj::value& v)
    {
        auto body = bj::serialize(v);
        uint32_t n = htonl((uint32_t)body.size());
        std::string f(reinterpret_cast<const char*>(&n), 4);
        f += body;
        return ::send(fd, f.data(), f.size(), 0) == (ssize_t)f.size();
    }
    static bool recvMsg(int fd, bj::object& out)
    {
        uint8_t len[4];
        size_t got = 0;
        while (got < 4) { auto n = ::recv(fd, len + got, 4 - got, 0); if (n <= 0) return false; got += (size_t)n; }
        uint32_t n = (uint32_t)len[0] << 24 | (uint32_t)len[1] << 16 | (uint32_t)len[2] << 8 | len[3];
        if (n > (1u << 20)) return false;
        std::string body(n, '\0');
        got = 0;
        while (got < n) { auto r = ::recv(fd, body.data() + got, n - got, 0); if (r <= 0) return false; got += (size_t)r; }
        try { out = bj::parse(body).as_object(); return true; } catch (...) { return false; }
    }
    void move(const std::string& residentId, double dx, double dy)
    {
        const ms_replica_entity* e = nullptr;
        uint8_t raw[16];
        if (!parseUuid(residentId, raw)) return;
        for (const auto& x : entities) if (!std::memcmp(x.id, raw, 16)) e = &x;
        if (!e) return;
        const ms_replica_entity copy = *e;
        const std::string owner = uuidText(copy.owner), label = copy.label;
        std::string token;
        if (auto it = tokens.find("agent:" + owner); it != tokens.end()) token = it->second;
        if (token.empty()) { model.actionNote = "No agent mandate for " + label + "; nothing was submitted."; return; }
        model.actionNote = "Submitting a bounded move to the MutSea authority…";
        if (actions.size() > 8) { for (auto& t : actions) if (t.joinable()) t.join(); actions.clear(); }
        actions.emplace_back([this, copy, token, dx, dy, label] {
            const auto started = Clock::now();
            std::string note;
            int fd = dial(sessionHost, sessionPort);
            if (fd < 0) note = "The authority's command gateway is unreachable. Nothing was committed.";
            else
            {
                bj::object msg;
                bool ok = sendMsg(fd, bj::object{ { "type", "hello" }, { "schema", "mutsea.local-session/v0" }, { "token", token }, { "region", region }, { "interest", bj::object{ { "kind", "region" } } } });
                while (ok && (ok = recvMsg(fd, msg)) && str(msg, "type") != "snapshot")
                    if (str(msg, "type") == "error") { ok = false; note = "Refused: " + str(msg, "reason"); }
                if (ok)
                {
                    bj::object transform{ { "position", bj::array{ copy.position[0] + dx, copy.position[1] + dy, copy.position[2] } },
                                          { "rotation", bj::array{ copy.rotation[0], copy.rotation[1], copy.rotation[2], copy.rotation[3] } },
                                          { "scale", bj::array{ copy.scale[0], copy.scale[1], copy.scale[2] } } };
                    bj::object cmd{ { "schema", "mutsea.local-command/v0" }, { "command_id", LLUUID::generateNewID().asString() }, { "expected_revision", copy.revision },
                                    { "payload", bj::object{ { "type", "set_transform" }, { "entity", uuidText(copy.id) }, { "transform", transform } } } };
                    ok = sendMsg(fd, bj::object{ { "type", "command" }, { "command", cmd } });
                    while (ok && (ok = recvMsg(fd, msg)))
                    {
                        const std::string type = str(msg, "type");
                        if (type == "receipt")
                        {
                            const auto ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
                            auto& outcome = msg.at("receipt").as_object().at("outcome").as_object();
                            note = str(outcome, "status") == "applied" ? "Committed by MutSea in " + fmt(ms, 0) + " ms · durable before acknowledgement" : "MutSea rejected the move";
                            break;
                        }
                        if (type == "refused" || type == "error") { note = "Refused: " + str(msg, "reason"); break; }
                    }
                    sendMsg(fd, bj::object{ { "type", "bye" } });
                }
                if (note.empty()) note = "The command channel closed; commit status is unknown until the stream shows it.";
                ::close(fd);
            }
            std::lock_guard g(m);
            pendingNote = note;
        });
    }
    std::string pendingNote;
    Clock::time_point navStart = Clock::now();
    uint64_t framesTorn = 0;
    // Ω-XIX native transport (finalverse.storm-frame-transport/1). The UI thread owns the
    // shared-memory consumer and uploads straight from the acquired slot (no engine hop, no
    // copy); it hands the slot metadata here so navigation timing and the model stay current.
    std::atomic<bool> nativeActive{false};
    std::mutex nativeMetaM;
    fvft::SlotMeta nativeMeta{};
    uint64_t nativeMetaSeen = 0;
    std::string transportMode = "file";

    void nativePresented(const fvft::SlotMeta& mm)
    {
        std::lock_guard g(nativeMetaM);
        nativeMeta = mm;
    }

    void framePresented(const std::shared_ptr<Frame>& f)
    {
        frame = f;
        if (model.navigating && f->controlSeq >= model.controlSeq && f->settled && model.ai.stage != "executing")
        {
            model.navigating = false;
            LL_INFOS("FinalverseShell") << "navigation presented context=" << model.ctx.label << " request_to_settled_frame_ms="
                                        << fmt(std::chrono::duration<double, std::milli>(Clock::now() - navStart).count(), 1)
                                        << " storm_switch_ms=" << fmt(f->switchMs, 1) << " triangles=" << f->triangles
                                        << " transport=" << f->transport << LL_ENDL;
        }
    }

    // Returns true while the native transport is in charge of frames.
    bool tickNativeFrames()
    {
        if (!nativeActive) return false;
        fvft::SlotMeta mm;
        {
            std::lock_guard g(nativeMetaM);
            if (nativeMeta.sequence == nativeMetaSeen) return true;
            mm = nativeMeta;
            nativeMetaSeen = mm.sequence;
        }
        auto f = std::make_shared<Frame>();  // metadata only: the pixels never leave the UI thread
        f->width = int(mm.width); f->height = int(mm.height); f->comps = 4;
        f->number = mm.frame_index; f->triangles = mm.triangles; f->controlSeq = mm.control_sequence; f->sourceRevision = mm.source_revision;
        f->sequence = mm.sequence; f->renderStartNs = mm.render_start_ns; f->producedNs = mm.produced_ns;
        f->context = fvft::text(mm.context, sizeof mm.context);
        f->units = fvft::text(mm.units, sizeof mm.units);
        f->frameId = fvft::text(mm.frame_id, sizeof mm.frame_id);
        f->transition = fvft::text(mm.transition, sizeof mm.transition);
        f->settled = (mm.flags & fvft::SETTLED) != 0;
        f->captureMs = mm.capture_ms; f->switchMs = mm.switch_ms;
        f->transport = "native";
        transportMode = "native";
        framePresented(f);
        return true;
    }

    // ------------------------------------------------------------------ Storm frames
    void tickFrames()
    {
        if (!privateDir(frames)) return;
        try
        {
            fs::path dir(frames);
            // A restarted renderer starts from an empty directory: give it the current context again.
            if (model.creation.previewActive && !fs::exists(dir / "context.json")) { previewDraft(false); return; }
            if (!model.ctx.scene.empty() && !fs::exists(dir / "context.json"))
            {
                std::vector<std::string> hl;
                if (auto it = model.ctx.selectable.find(model.selected); it != model.ctx.selectable.end()) hl = it->second;
                writeContext(model.ctx, currentManifest, "none", hl);
                LL_INFOS("FinalverseShell") << "renderer restarted: context re-sent control_seq=" << model.controlSeq << LL_ENDL;
            }
            if (tickNativeFrames()) return;
            transportMode = "file";
            if (!fs::exists(dir / "frame.json")) return;
            auto t = fs::last_write_time(dir / "frame.json");
            if (t == frameTime && frame) return;
            auto j = bj::parse(readSmall(dir / "frame.json", 16384)).as_object();
            if (str(j, "schema") != "finalverse.storm-view-frame/0.2") return;
            auto bytes = readSmall(dir / "frame.ppm", 1280 * 720 * 3 + 128);
            if (bytes.empty()) return;
            const std::string pixels = sha256(bytes);
            if (pixels != str(j, "pixels_sha256"))
            {
                // The renderer replaced frame.ppm after we read frame.json; its metadata
                // follows immediately, so re-read it once instead of dropping the frame.
                auto j2 = bj::parse(readSmall(dir / "frame.json", 16384)).as_object();
                if (str(j2, "schema") != "finalverse.storm-view-frame/0.2" || str(j2, "pixels_sha256") != pixels) { ++framesTorn; return; }  // keep the last frame
                j = std::move(j2);
                t = fs::last_write_time(dir / "frame.json");
            }
            std::istringstream hdr(bytes.substr(0, 64));
            std::string magic;
            int w = 0, h = 0, mx = 0;
            hdr >> magic >> w >> h >> mx;
            hdr.get();
            const auto off = (size_t)hdr.tellg();
            if (magic != "P6" || w != 1280 || h != 720 || bytes.size() != off + 1280 * 720 * 3) return;
            auto f = std::make_shared<Frame>();
            f->number = j.at("frame").to_number<uint64_t>();
            f->triangles = j.at("triangles").to_number<uint64_t>();
            f->controlSeq = j.at("control_sequence").to_number<uint64_t>();
            f->sourceRevision = j.at("source_revision").to_number<uint64_t>();
            f->context = str(j, "context");
            f->units = str(j, "units");
            f->frameId = str(j, "frame_id");
            f->transition = str(j, "transition");
            f->settled = j.at("transition_settled").as_bool();
            f->captureMs = num(j.at("capture_ms"));
            f->switchMs = num(j.at("last_switch_ms"));
            f->rgb.assign(bytes.begin() + off, bytes.end());  // one copy; no per-pixel conversion
            frameTime = t;
            framePresented(f);
        }
        catch (const std::exception&) {}
    }

    void publish()
    {
        auto s = std::make_shared<Model>(model);
        s->frame = frame;
        std::lock_guard g(m);
        if (!pendingNote.empty()) { s->actionNote = model.actionNote = pendingNote; pendingNote.clear(); }
        snapshot = s;
    }

    void run()
    {
        connect();
        if (!startEntity.empty()) navigate(startEntity);
        auto lastConnectTry = Clock::now();
        while (!stop)
        {
            std::function<void()> job;
            {
                std::lock_guard g(m);
                if (!jobs.empty()) { job = std::move(jobs.front()); jobs.pop_front(); }
            }
            if (job) job();
            if (!client && Clock::now() - lastConnectTry > std::chrono::seconds(2))
            {
                lastConnectTry = Clock::now();
                if (connect() && model.ctx.entity.empty() && !startEntity.empty()) navigate(startEntity);
            }
            tickLive();
            tickFrames();
            pollAi();
            pollCreation();
            pollPublisher();
            pollGenesis();
            while (!model.toasts.empty() && Clock::now() - model.toasts.front().second > std::chrono::seconds(6)) model.toasts.pop_front();
            publish();
            // The native transport is polled at ~250 Hz so a published frame waits at most ~4 ms.
            if (!job) std::this_thread::sleep_for(std::chrono::milliseconds(12));
        }
    }
};

struct Hit
{
    LLRect rect;
    std::function<void()> action;
    std::string id;  // keyboard focus identity
};

enum class Mode { Explore = 0, Create = 1, Analyze = 2, Develop = 3 };
const char* modeName(Mode m) { static const char* n[] = { "Explore", "Create", "Analyze", "Develop" }; return n[(int)m]; }
}  // namespace

// ====================================================================== floater
struct LLFloaterFinalverseShell::Impl
{
    std::unique_ptr<Engine> engine;
    std::shared_ptr<const Model> view;
    LLPointer<LLViewerTexture> image;
    LLPointer<LLImageRaw> staging;
    uint64_t imageFrame = 0;
    // Measurement (script command `perf`): shell draws and Storm frames presented since the last report.
    uint64_t perfDraws = 0, perfPresented = 0;
    std::deque<double> displayMs, transportMs;  // native: produced -> uploaded / produced -> acquired, last 600 frames
    fvft::Consumer native;
    std::string nativeError;
    double nativeCheckAt = -1;
    int nativeW = 0, nativeH = 0;
    uint64_t nativeSeq = 0;
    // Native transport on the UI thread: discover, attach, acquire the newest frame and upload it
    // straight from the shared slot. Returns true when a native image is available to draw.
    bool pumpNative(double t)
    {
        Engine* e = engine.get();
        if (!e || e->frames.empty()) return false;
        const fs::path disc = fs::path(e->frames) / "transport.json";
        if (native.attached() && (native.state() == fvft::CLOSED || !native.producer_alive()))
        {
            LL_INFOS("FinalverseShell") << "native transport: producer gone (state=" << native.state() << ") published=" << native.published()
                                        << " dropped=" << native.dropped() << "; detaching" << LL_ENDL;
            native.detach();
            e->nativeActive = false;
            nativeW = nativeH = 0;
        }
        if (!native.attached())
        {
            if (t < nativeCheckAt) return false;
            nativeCheckAt = t + .5;  // discovery is a file read: at most twice a second
            try
            {
                if (!fs::exists(disc)) return false;
                auto j = bj::parse(readSmall(disc, 4096)).as_object();
                if (str(j, "schema") != fvft::kSchema) return false;
                std::string err;
                if (!native.attach(str(j, "shm"), str(j, "nonce"), err))
                {
                    if (err != nativeError) LL_WARNS("FinalverseShell") << "native transport refused: " << err << " (file fallback stays available)" << LL_ENDL;
                    nativeError = err;
                    return false;
                }
                nativeError.clear();
                LL_INFOS("FinalverseShell") << "native transport attached shm=" << str(j, "shm") << " max=" << j.at("max_width").to_number<int>() << "x"
                                            << j.at("max_height").to_number<int>() << LL_ENDL;
                e->nativeActive = true;
            }
            catch (const std::exception&) { return false; }
        }
        fvft::Acquired a;
        if (native.acquire(a))
        {
            const auto& mm = a.meta;
            auto pow2 = [](int v) { int p = 1; while (p < v) p <<= 1; return p; };
            const int TW = pow2(int(mm.width)), TH = pow2(int(mm.height));
            if (staging.isNull() || staging->getComponents() != 4 || staging->getWidth() != TW || staging->getHeight() != TH || image.isNull())
            {
                staging = new LLImageRaw(TW, TH, 4);
                LLImageDataLock lock(staging);
                if (!staging->isBufferInvalid()) std::memset(staging->getData(), 0, size_t(TW) * TH * 4);
                image = LLViewerTextureManager::getLocalTexture(staging.get(), false);
            }
            if (image.notNull() && image->getGLTexture() && mm.stride == mm.width * 4)
            {
                image->getGLTexture()->setSubImage(a.pixels, int(mm.width), int(mm.height), 0, 0, int(mm.width), int(mm.height), true);
                nativeW = int(mm.width); nativeH = int(mm.height); nativeSeq = mm.sequence;
                perfPresented++;
                transportMs.push_back(double(a.acquired_ns - mm.produced_ns) / 1e6);
                displayMs.push_back(double(fvft::now_ns() - mm.produced_ns) / 1e6);
                if (transportMs.size() > 600) transportMs.pop_front();
                if (displayMs.size() > 600) displayMs.pop_front();
                e->nativePresented(mm);
            }
        }
        return nativeW > 0 && image.notNull();
    }
    double perfT0 = 0;
    Mode mode = Mode::Explore;
    std::vector<Hit> hits;
    std::vector<std::string> focusOrder;
    int focusIndex = -1;
    bool showHelp = false, dragging = false, dragMoved = false;
    // Ω-XIX command palette (Ctrl/Cmd+K): keyboard-first, context- and permission-aware.
    bool paletteOpen = false;
    std::string paletteQuery;
    int paletteSel = 0;
    struct PaletteItem { std::string group, label, detail; bool enabled = true; std::function<void()> run; };
    std::vector<PaletteItem> paletteItems;  // rebuilt every frame from the snapshot
    struct SavedView { std::string label, entity, presentation; };
    std::vector<SavedView> savedViews;
    // Ω-XIX publication confirmation dialog (the owner's explicit approval).
    bool confirmPublish = false;
    // Ω-XIX progressive inspector: advanced sections the user opened.
    std::set<std::string> openSections;
    void openPalette() { paletteOpen = true; paletteQuery.clear(); paletteSel = 0; }
    void closePalette() { paletteOpen = false; paletteQuery.clear(); }
    S32 dragX = 0, dragY = 0;
    Clock::time_point opened = Clock::now(), lastNavStart{}, lastCamera = Clock::now();
    std::string lastContext;
    double transitionShownAt = -10;
    std::vector<std::pair<double, std::string>> script;
    size_t scriptNext = 0;
    std::map<std::string, Clock::time_point> moved;
    std::map<std::string, uint64_t> lastRevision;

    double now() const { return std::chrono::duration<double>(Clock::now() - opened).count(); }
    void hit(const LLRect& r, std::function<void()> f, const std::string& id = {})
    {
        hits.push_back({ r, std::move(f), id });
        if (!id.empty()) focusOrder.push_back(id);
    }
    bool focused(const std::string& id) const { return focusIndex >= 0 && focusIndex < (int)focusOrder.size() && focusOrder[focusIndex] == id; }
};

LLFloaterFinalverseShell::LLFloaterFinalverseShell(const LLSD& key) : LLFloater(key), m(std::make_unique<Impl>()) {}
LLFloaterFinalverseShell::~LLFloaterFinalverseShell() = default;

bool LLFloaterFinalverseShell::postBuild()
{
    getChild<LLLineEditor>("ask")->setCommitCallback([this](LLUICtrl* c, const LLSD&) {
        auto* e = static_cast<LLLineEditor*>(c);
        std::string t = e->getText();
        LLStringUtil::trim(t);
        e->setText(LLStringUtil::null);
        if (t.empty() || !m->engine) return;
        std::string lower = t;
        LLStringUtil::toLower(lower);
        if (lower == "back") m->engine->post([this] { m->engine->back(false); });
        else if (lower == "forward") m->engine->post([this] { m->engine->back(true); });
        else if (lower == "home") m->engine->post([this] { m->engine->navigate(m->engine->startEntity); });
        else m->engine->post([this, t] { m->engine->ask(t); });
    });
    setBackgroundVisible(false);
    arrange();
    return true;
}

void LLFloaterFinalverseShell::onOpen(const LLSD&)
{
    // The shell replaces the legacy client chrome while it is open.
    if (gLoginMenuBarView) gLoginMenuBarView->setVisible(false);
    if (gMenuBarView) gMenuBarView->setVisible(false);
    if (gStatusBar) gStatusBar->setVisible(false);
    if (gToolBarView) gToolBarView->setToolBarsVisible(false);
    if (const char* s = std::getenv("FINALVERSE_UI_SCALE")) gScale = llclamp(std::atoi(s), 0, 2);
    if (getParent()) setShape(getParent()->getLocalRect());
    m->engine = std::make_unique<Engine>();
    m->engine->configure();
    m->engine->thread = std::thread([e = m->engine.get()] { e->run(); });
    m->opened = Clock::now();
    if (const char* sp = std::getenv("FINALVERSE_SHELL_SCRIPT"))
    {
        std::ifstream in(sp);
        std::string l;
        while (std::getline(in, l) && m->script.size() < 512)
        {
            std::istringstream ls(l);
            double at = 0;
            std::string rest;
            if (ls >> at && std::getline(ls, rest)) { LLStringUtil::trim(rest); if (!rest.empty()) m->script.emplace_back(at, rest); }
        }
    }
    setFocus(true);
    gFocusMgr.setKeyboardFocus(this);  // shortcuts work at once; the AI field takes focus on click, / or Ctrl+K
    LL_INFOS("FinalverseShell") << "FINALVERSE shell opened" << LL_ENDL;
}

void LLFloaterFinalverseShell::onClose(bool)
{
    m->engine.reset();
    m->view.reset();
    m->image = nullptr;
    if (gStatusBar) gStatusBar->setVisible(true);
    if (gToolBarView) gToolBarView->setToolBarsVisible(true);
}

void LLFloaterFinalverseShell::reshape(S32 w, S32 h, bool parent)
{
    LLFloater::reshape(w, h, parent);
    if (getChildView("ask", false)) arrange();
}

void LLFloaterFinalverseShell::arrange()
{
    const S32 W = getRect().getWidth();
    const S32 barW = llclamp(W - 520, 420, 760), barL = (W - barW) / 2;
    LLRect r;
    r.setOriginAndSize(barL + 46, 30, barW - 46 - 18, 26);
    getChildView("ask")->setShape(r);
}

bool LLFloaterFinalverseShell::handleMouseDown(S32 x, S32 y, MASK mask)
{
    for (auto it = m->hits.rbegin(); it != m->hits.rend(); ++it)
        if (it->rect.pointInRect(x, y)) { it->action(); return true; }
    if (getChild<LLLineEditor>("ask")->getRect().pointInRect(x, y)) return LLFloater::handleMouseDown(x, y, mask);
    // Empty viewport: orbit the camera (presentation only).
    m->dragging = true;
    m->dragMoved = false;
    m->dragX = x;
    m->dragY = y;
    setFocus(true);
    gFocusMgr.setMouseCapture(this);
    return true;
}
bool LLFloaterFinalverseShell::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (m->dragging)
    {
        m->dragging = false;
        gFocusMgr.setKeyboardFocus(this);
        gFocusMgr.setMouseCapture(nullptr);
        return true;
    }
    return LLFloater::handleMouseUp(x, y, mask);
}
bool LLFloaterFinalverseShell::handleHover(S32 x, S32 y, MASK mask)
{
    if (m->dragging && m->engine)
    {
        const S32 dx = x - m->dragX, dy = y - m->dragY;
        if ((std::abs(dx) + std::abs(dy) > 1) && Clock::now() - m->lastCamera > std::chrono::milliseconds(16))
        {
            m->dragX = x;
            m->dragY = y;
            m->lastCamera = Clock::now();
            if (mask & (MASK_SHIFT | MASK_ALT)) m->engine->post([e = m->engine.get(), dx, dy] { e->panView(dx, dy); });  // Shift/Option-drag pans
            else m->engine->post([e = m->engine.get(), dx, dy] { e->camera(dx * .006, dy * .004, 1.0); });
        }
        return true;
    }
    return LLFloater::handleHover(x, y, mask);
}
bool LLFloaterFinalverseShell::handleDoubleClick(S32 x, S32 y, MASK mask) { return handleMouseDown(x, y, mask); }
bool LLFloaterFinalverseShell::handleScrollWheel(S32, S32, S32 clicks)
{
    if (m->engine) m->engine->post([e = m->engine.get(), clicks] { e->camera(0, 0, std::pow(1.15, clicks)); });
    return true;
}

bool LLFloaterFinalverseShell::handleUnicodeCharHere(llwchar ch)
{
    if (m->paletteOpen && ch >= 32 && ch != 127)
    {
        m->paletteQuery += wstring_to_utf8str(LLWString(1, ch));
        m->paletteSel = 0;
        if (m->engine) m->engine->post([e = m->engine.get(), q = m->paletteQuery] { e->search(q); });
        return true;
    }
    return LLFloater::handleUnicodeCharHere(ch);
}

bool LLFloaterFinalverseShell::handleKeyHere(KEY key, MASK mask)
{
    auto* ask = getChild<LLLineEditor>("ask");
    // Ω-XIX command palette: Ctrl/Cmd+K toggles; while open it owns the keyboard.
    if (m->confirmPublish)
    {
        if (key == KEY_ESCAPE) { m->confirmPublish = false; return true; }
        if (key == KEY_RETURN && m->engine && m->view && m->view->creation.valid) { m->confirmPublish = false; m->engine->post([e = m->engine.get()] { e->publishApproved(); }); return true; }
        return true;
    }
    if (key == 'K' && (mask & MASK_CONTROL)) { if (m->paletteOpen) m->closePalette(); else { m->openPalette(); setFocus(true); gFocusMgr.setKeyboardFocus(this); } return true; }
    if (m->paletteOpen)
    {
        const int n = (int)m->paletteItems.size();
        if (key == KEY_ESCAPE) { m->closePalette(); return true; }
        if (key == KEY_DOWN) { if (n) m->paletteSel = (m->paletteSel + 1) % n; return true; }
        if (key == KEY_UP) { if (n) m->paletteSel = (m->paletteSel + n - 1) % n; return true; }
        if (key == KEY_BACKSPACE)
        {
            if (!m->paletteQuery.empty()) { LLWString w = utf8str_to_wstring(m->paletteQuery); w.pop_back(); m->paletteQuery = wstring_to_utf8str(w); }
            m->paletteSel = 0;
            if (m->engine) m->engine->post([e = m->engine.get(), q = m->paletteQuery] { e->search(q); });
            return true;
        }
        if (key == KEY_RETURN)
        {
            if (m->paletteSel < n && m->paletteItems[m->paletteSel].enabled)
            {
                auto run = m->paletteItems[m->paletteSel].run;
                LL_INFOS("FinalverseShell") << "palette run group=" << m->paletteItems[m->paletteSel].group << " label=" << m->paletteItems[m->paletteSel].label << LL_ENDL;
                m->closePalette();
                run();
            }
            else if (n == 0 && !m->paletteQuery.empty() && m->engine) { auto q = m->paletteQuery; m->closePalette(); m->engine->post([e = m->engine.get(), q] { e->ask(q); }); }
            return true;
        }
        return true;  // swallow other keys while the palette is open
    }
    if (ask->hasFocus())
    {
        // Typing stays in the field; Esc leaves it, and non-text shortcuts still work.
        if (key == KEY_ESCAPE) { gFocusMgr.setKeyboardFocus(this); return true; }
        const bool empty = ask->getText().empty();
        if (!((mask & MASK_CONTROL) && (key == '=' || key == '-' || key == KEY_ADD || key == KEY_SUBTRACT)) &&
            !(empty && (mask & MASK_ALT) && (key == KEY_LEFT || key == KEY_RIGHT)))
            return LLFloater::handleKeyHere(key, mask);
    }
    if (!m->engine) return LLFloater::handleKeyHere(key, mask);
    Engine* e = m->engine.get();
    if (key == '?' || (key == '/' && (mask & MASK_SHIFT))) { m->showHelp = !m->showHelp; return true; }
    if (key == '/') { ask->setFocus(true); return true; }
    if (key == KEY_LEFT && (mask & MASK_ALT)) { e->post([e] { e->back(false); }); return true; }
    if (key == KEY_RIGHT && (mask & MASK_ALT)) { e->post([e] { e->back(true); }); return true; }
    if ((key == '=' || key == KEY_ADD) && (mask & MASK_CONTROL)) { gScale = llmin(gScale + 1, 2); return true; }
    if ((key == '-' || key == KEY_SUBTRACT) && (mask & MASK_CONTROL)) { gScale = llmax(gScale - 1, 0); return true; }
    if (mask != MASK_NONE && mask != MASK_SHIFT) return LLFloater::handleKeyHere(key, mask);
    if (key >= '1' && key <= '4') { m->mode = (Mode)(key - '1'); return true; }
    if (key == KEY_TAB)
    {
        if (!m->focusOrder.empty())
            m->focusIndex = (m->focusIndex + ((mask & MASK_SHIFT) ? -1 : 1) + (int)m->focusOrder.size()) % (int)m->focusOrder.size();
        return true;
    }
    if (key == KEY_RETURN && m->focusIndex >= 0 && m->focusIndex < (int)m->focusOrder.size())
    {
        for (auto& h : m->hits)
            if (h.id == m->focusOrder[m->focusIndex]) { h.action(); break; }
        return true;
    }
    if (key == KEY_BACKSPACE) { e->post([e] { e->back(false); }); return true; }
    if (key == 'H') { e->post([e] { e->navigate(e->startEntity); }); return true; }
    if (key == 'R') { e->post([e] { e->resetCamera(); }); return true; }
    if (key == 'F') { e->post([e] { e->frameSelected(); }); return true; }
    if (key == 'V')
    {
        static const char* order[] = { "realistic", "technical", "inspection", "xray", "exploded" };
        e->post([e] { int i = 0; while (i < 5 && e->presentation != order[i]) ++i; e->setPresentation(order[(i + 1) % 5]); });
        return true;
    }
    if (mask == MASK_SHIFT && (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN))
    {
        const double dx = key == KEY_LEFT ? -40 : key == KEY_RIGHT ? 40 : 0, dy = key == KEY_UP ? 40 : key == KEY_DOWN ? -40 : 0;
        e->post([e, dx, dy] { e->panView(dx, dy); });
        return true;
    }
    if (key == 'O') { e->post([e] { e->orbit = !e->orbit; e->camera(0, 0, 1.0); }); return true; }
    if (key == KEY_ESCAPE && m->view && (m->view->ai.stage == "planning" || m->view->ai.stage == "executing")) { e->aiCancel = true; return true; }
    // Ω-XIX draft editing in Create mode (keyboard-first; same operations as the panel buttons).
    if (m->mode == Mode::Create && m->view && !m->view->creation.draftId.empty() && !m->view->creation.published)
    {
        const std::string o = m->view->creation.selected;
        const double step = (mask & MASK_SHIFT) ? .1 : .5;
        if ((key == 'Z') && (mask & MASK_CONTROL)) { e->post([e, redo = (mask & MASK_SHIFT) != 0] { e->creationHistory(redo); }); return true; }
        if (!o.empty() && mask != MASK_CONTROL)
        {
            if (key == KEY_LEFT) { e->post([e, o, step] { e->creationEdit("move", o, bj::array{ -step, 0.0 }); }); return true; }
            if (key == KEY_RIGHT) { e->post([e, o, step] { e->creationEdit("move", o, bj::array{ step, 0.0 }); }); return true; }
            if (key == KEY_UP) { e->post([e, o, step] { e->creationEdit("move", o, bj::array{ 0.0, step }); }); return true; }
            if (key == KEY_DOWN) { e->post([e, o, step] { e->creationEdit("move", o, bj::array{ 0.0, -step }); }); return true; }
            if (key == '[' || key == ']') { e->post([e, o, d = key == '[' ? 15.0 : -15.0] { e->creationEdit("rotate", o, d); }); return true; }
            if (key == 'M') { e->post([e, o] { e->creationEdit("material", o, nullptr); }); return true; }
            if (key == KEY_DELETE) { e->post([e, o] { e->creationEdit("remove", o, nullptr); }); return true; }
        }
    }
    if (key == KEY_ESCAPE) { m->focusIndex = -1; m->showHelp = false; e->post([e] { e->select({}, {}); }); return true; }
    if (key == KEY_LEFT) { e->post([e] { e->camera(-.12, 0, 1.0); }); return true; }
    if (key == KEY_RIGHT) { e->post([e] { e->camera(.12, 0, 1.0); }); return true; }
    if (key == KEY_UP) { e->post([e] { e->camera(0, .06, 1.0); }); return true; }
    if (key == KEY_DOWN) { e->post([e] { e->camera(0, -.06, 1.0); }); return true; }
    if (key == '?' || (key == '/' && (mask & MASK_SHIFT))) { m->showHelp = !m->showHelp; return true; }
    return LLFloater::handleKeyHere(key, mask);
}

void LLFloaterFinalverseShell::draw()
{
    // Startup may re-show legacy chrome after the shell opened; keep it hidden.
    if (gLoginMenuBarView && gLoginMenuBarView->getVisible()) gLoginMenuBarView->setVisible(false);
    if (gMenuBarView && gMenuBarView->getVisible()) gMenuBarView->setVisible(false);
    if (gStatusBar && gStatusBar->getVisible()) gStatusBar->setVisible(false);
    // Collapse the legacy menu/status strip so the world takes the whole window.
    if (LLView* strip = gViewerWindow ? gViewerWindow->getRootView()->findChildView("status_bar_container", true) : nullptr; strip && strip->getVisible())
        strip->setVisible(false);
    if (getParent() && getRect() != getParent()->getLocalRect()) setShape(getParent()->getLocalRect());
    const S32 W = getRect().getWidth(), H = getRect().getHeight();
    const double t = m->now();
    m->perfDraws++;
    m->hits.clear();
    m->focusOrder.clear();
    if (m->engine)
    {
        std::lock_guard g(m->engine->m);
        m->view = m->engine->snapshot;
    }
    const Model* v = m->view.get();
    Engine* eng = m->engine.get();
    const bool compact = W < 1180, wide = W > 1700;

    // ---- viewport: Storm frame, full bleed, cropped not stretched
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gl_rect_2d(0, H, W, 0, T.bg, true);
    const Frame* fr = v && v->frame ? v->frame.get() : nullptr;
    const bool nativeImage = m->pumpNative(t);
    if (nativeImage || (fr && fr->comps == 3 && fr->rgb.size() == size_t(fr->width) * fr->height * 3 && fr->width > 0))
    {
        auto pow2 = [](int v) { int p = 1; while (p < v) p <<= 1; return p; };
        const int fw = nativeImage ? m->nativeW : fr->width, fh = nativeImage ? m->nativeH : fr->height;
        if (!nativeImage && (fr->number != m->imageFrame || m->image.isNull()))
        {
            // File fallback (Ω-XVIII): one persistent RGB staging image; only the 1280x720 region is copied and uploaded.
            if (m->staging.isNull() || m->staging->getComponents() != 3)
            {
                m->staging = new LLImageRaw(2048, 1024, 3);
                LLImageDataLock lock(m->staging);
                if (!m->staging->isBufferInvalid()) std::memset(m->staging->getData(), 0, 2048 * 1024 * 3);
                m->image = nullptr;
            }
            LLPointer<LLImageRaw> raw = m->staging;
            {
                LLImageDataLock lock(raw);
                if (!raw->isBufferInvalid())
                    for (int y = 0; y < 720; y++) std::memcpy(raw->getData() + y * 2048 * 3, fr->rgb.data() + (719 - y) * 1280 * 3, 1280 * 3);
            }
            if (!raw->isBufferInvalid())
            {
                if (m->image.isNull()) m->image = LLViewerTextureManager::getLocalTexture(raw.get(), false);
                else m->image->setSubImage(raw.get(), 0, 0, 1280, 720);
                m->imageFrame = fr->number;
                m->perfPresented++;
            }
        }
        const float aspect = (float)W / std::max(1, H), imageAspect = (float)fw / fh;
        const float texW = nativeImage ? (float)pow2(fw) : 2048.f, texH = nativeImage ? (float)pow2(fh) : 1024.f;
        float l = 0.f, r = fw / texW, b = 0.f, tp = fh / texH;
        if (aspect > imageAspect) { const float span = (tp - b) * imageAspect / aspect; b = (tp - span) * .5f; tp = b + span; }
        else { const float span = (r - l) * aspect / imageAspect; l = (r - span) * .5f; r = l + span; }
        if (nativeImage) std::swap(b, tp);  // native rows are top-first: flip with texture coordinates
        gGL.color4f(1, 1, 1, 1);
        gl_draw_scaled_image(0, 0, W, H, m->image, LLColor4::white, LLRectf(l, tp, r, b));
    }
    else
    {
        for (S32 x = 0; x < W; x += 56) gl_line_2d(x, 0, x, H, LLColor4(1, 1, 1, .022f));
        for (S32 y = 0; y < H; y += 56) gl_line_2d(0, y, W, y, LLColor4(1, 1, 1, .022f));
    }
    // Legibility gradients (top and bottom), never over the centre of the world.
    for (int i = 0; i < 20; ++i)
    {
        const float a = (1.f - i / 20.f);
        gl_rect_2d(0, H - i * 6, W, H - i * 6 - 6, LLColor4(0, 0, 0, a * .38f), true);
        gl_rect_2d(0, i * 6 + 6, W, i * 6, LLColor4(0, 0, 0, a * .42f), true);
    }
    if (!v)
    {
        text("FINALVERSE", W / 2, H / 2, T.ink, 3, true, LLFontGL::HCENTER);
        LLFloater::draw();
        return;
    }
    const Context& c = v->ctx;
    // Semantic transition dim (fades out when the new context settles).
    if (!c.entity.empty() && c.entity != m->lastContext)
    {
        m->lastContext = c.entity;
        m->transitionShownAt = t;
    }
    const bool settled = fr && fr->controlSeq >= v->controlSeq && fr->settled;
    const double sinceSwitch = t - m->transitionShownAt;
    if (c.transition == "semantic" && sinceSwitch < 1.6)
    {
        const float a = (float)llclamp(settled ? (1.0 - (sinceSwitch - .35) / .9) * .55 : .55, 0.0, .55);
        if (a > 0) gl_rect_2d(0, H, W, 0, LLColor4(T.bg.mV[0], T.bg.mV[1], T.bg.mV[2], a), true);
    }

    // ================================================================ top bar
    const S32 top = H - 18;
    brandMark(34, (F32)top - 20, 1.f, (float)t);
    {
        // Letter-spaced wordmark.
        S32 x = 56;
        const std::string word = "FINALVERSE";
        for (char ch : word) { x = text(std::string(1, ch), x, top - 26, T.ink, 1, true) + 3; }
    }
    // Mode switch (segmented control).
    {
        const S32 segW = compact ? 84 : 100, n = 4, totalW = segW * n, x0 = (W - totalW) / 2, y0 = top - 6, y1 = top - 40;
        panel(x0 - 4, y0 + 2, x0 + totalW + 4, y1 - 2, 18.f, true);
        static const Icon icons[] = { Icon::Compass, Icon::Plus, Icon::Ruler, Icon::Code };
        for (int i = 0; i < n; ++i)
        {
            const S32 l = x0 + i * segW, r = l + segW;
            const bool sel = (int)m->mode == i;
            const std::string id = std::string("mode-") + modeName((Mode)i);
            if (sel) roundRect(l + 2, y0 - 2, r - 2, y1 + 2, 15.f, T.accentSoft);
            if (m->focused(id)) focusRing(LLRect(l + 2, y0 - 2, r - 2, y1 + 2), 14);
            icon(icons[i], (F32)l + 20, (F32)(y0 + y1) / 2, sel ? T.accent : T.ink3, .9f);
            text(modeName((Mode)i), l + 34, (y0 + y1) / 2 - 5, sel ? T.ink : T.ink2, 0, sel);
            m->hit(LLRect(l, y0, r, y1), [this, i] { m->mode = (Mode)i; }, id);
        }
    }
    // Status pill: live world + spatial service.
    {
        const auto state = v->live.state;
        std::string label = "SPATIAL OFFLINE";
        LLColor4 col = T.bad;
        if (v->spatialUp)
        {
            if (!v->liveConfigured) { label = "CONNECTED"; col = T.live; }
            else if (state == MS_REPLICA_LIVE) { label = "LIVE"; col = T.live; }
            else if (state == MS_REPLICA_SYNCING || state == MS_REPLICA_CONNECTING) { label = "SYNCING"; col = T.warn; }
            else if (state == MS_REPLICA_BACKOFF) { label = "RECONNECTING"; col = T.warn; }
            else { label = "WORLD OFFLINE"; col = T.warn; }
        }
        std::string detail = v->liveConfigured ? "rev " + std::to_string(v->live.revision) : v->server;
        if (m->mode == Mode::Develop && v->spatialUp) detail += " · ed " + std::to_string(v->edition);
        const S32 pw = width(label, 0, true) + width(detail, 0) + 54, px = W - 20 - pw;
        panel(px, top - 6, W - 20, top - 40, 17.f, true);
        const float pulse = (state == MS_REPLICA_LIVE && !eng->reducedMotion) ? (float)(.55 + .45 * std::sin(t * 3.2)) : 1.f;
        dot((F32)px + 18, (F32)top - 23, 8 * pulse + 2, alpha(col, .22f));
        dot((F32)px + 18, (F32)top - 23, 4.5f, col);
        S32 after = text(label, px + 32, top - 28, col, 0, true);
        text(detail, after + 8, top - 28, T.ink2, 0);
    }

    // ================================================================ breadcrumbs + history
    const S32 crumbY = top - 64;
    {
        S32 x = 22;
        const bool canBack = v->historyCursor > 0, canFwd = v->historyCursor + 1 < v->history.size();
        auto chip = [&](Icon ic, bool enabled, const std::string& id, std::function<void()> f) {
            LLRect rc(x, crumbY + 14, x + 30, crumbY - 14);
            roundRect(rc.mLeft, rc.mTop, rc.mRight, rc.mBottom, 15, enabled ? T.surfaceHi : alpha(T.surface, .5f));
            if (m->focused(id)) focusRing(rc, 14);
            icon(ic, (F32)x + 15, (F32)crumbY, enabled ? T.ink : T.ink3, .8f);
            if (enabled) m->hit(rc, f, id);
            x += 36;
        };
        chip(Icon::Back, canBack, "back", [eng] { eng->post([eng] { eng->back(false); }); });
        chip(Icon::Forward, canFwd, "forward", [eng] { eng->post([eng] { eng->back(true); }); });
        chip(Icon::Home, true, "home", [eng] { eng->post([eng] { eng->navigate(eng->startEntity); }); });
        x += 6;
        // Crumbs: collapse the middle when space is short.
        std::vector<Crumb> crumbs = c.path;
        const S32 maxX = W - (compact ? 40 : 330);
        for (size_t i = 0; i < crumbs.size(); ++i)
        {
            const bool last = i + 1 == crumbs.size();
            std::string lbl = clip(crumbs[i].label, last ? 360 : 200, 0, last);
            const S32 w = width(lbl, 0, last) + 20;
            if (x + w > maxX && !last) { text("…", x, crumbY - 5, T.ink3, 0); x += 18; continue; }
            LLRect rc(x, crumbY + 13, x + w, crumbY - 13);
            const std::string id = "crumb-" + crumbs[i].entity;
            if (last) roundRect(rc.mLeft, rc.mTop, rc.mRight, rc.mBottom, 13, alpha(T.accentSoft, .9f));
            if (m->focused(id)) focusRing(rc, 12);
            text(lbl, x + 10, crumbY - 5, last ? T.ink : T.ink2, 0, last);
            if (!last) m->hit(rc, [eng, e = crumbs[i].entity] { eng->post([eng, e] { eng->navigate(e); }); }, id);
            x += w;
            if (!last) { icon(Icon::Chevron, (F32)x + 6, (F32)crumbY, T.ink3, .7f); x += 14; }
        }
        if (v->navigating)
        {
            gGL.pushMatrix();
            gGL.translatef((F32)x + 18, (F32)crumbY, 0);
            gl_washer_segment_2d(8, 5.5f, (F32)(t * 5), (F32)(t * 5 + 4.4), 18, T.accent, alpha(T.accent, 0));
            gGL.popMatrix();
            text(v->navStage, x + 32, crumbY - 5, T.ink3, 0);
        }
    }

    // ================================================================ context chip (honest data label)
    if (!c.entity.empty() && !compact)
    {
        std::string units;
        for (auto& r : c.reps) if (r.id == c.repId) units = r.units;
        const std::string cl = classLabel(c.classification, c.demo);
        const std::string line1 = c.frame + " · " + units + (c.frames.empty() ? "" : " · " + c.frames.front().accuracy);
        const S32 cw = std::max(width(line1, 0), width(cl, 0, true) + 22) + 28, cx = W - 20 - cw, cy = crumbY - 22;
        panel(cx, cy, W - 20, cy - 46, 12.f);
        dot((F32)cx + 16, (F32)cy - 15, 4, classColour(c.classification, c.demo));
        text(cl, cx + 26, cy - 20, classColour(c.classification, c.demo), 0, true);
        text(line1, cx + 14, cy - 38, T.ink2, 0);
    }

    // ================================================================ left: here / residents
    const S32 railL = 20, railR = compact ? 270 : 330, railTop = crumbY - 30, railBottom = 120;
    const bool showRail = m->mode != Mode::Develop || wide;
    const bool nativeCtx = !v->nativeKind.empty();
    if (showRail && !c.entity.empty() && !nativeCtx)
    {
        panel(railL, railTop, railR, railBottom);
        S32 y = railTop - 30;
        // Context title.
        text(kindTitle(c.kind).empty() ? "" : kindTitle(c.kind), railL + 18, y, T.ink3, 0, true);
        y -= lineHeight(2) + 4;
        text(clip(c.label, railR - railL - 36, 2, true), railL + 18, y, T.ink, 2, true);
        y -= 18;
        if (!c.description.empty() && m->mode != Mode::Create) y = wrapped(c.description, railL + 18, y - 4, railR - railL - 36, T.ink2, 0, compact ? 3 : 4) - 6;
        // Transition reason (semantic vs geometric) — the core honesty of semantic zoom.
        if (!c.transition.empty() && c.transition != "none")
        {
            const LLColor4 tc = c.transition == "semantic" ? T.violet : T.accent;
            roundRect(railL + 16, y + 2, railL + 16 + width(c.transition == "semantic" ? "SEMANTIC TRANSITION" : "GEOMETRIC ZOOM", 0, true) + 16, y - 18, 9, alpha(tc, .16f));
            text(c.transition == "semantic" ? "SEMANTIC TRANSITION" : "GEOMETRIC ZOOM", railL + 24, y - 12, tc, 0, true);
            y -= 28;
        }
        // Here list.
        std::vector<Child> items = c.children;
        if (c.live)
            for (auto& r : v->residents)
            {
                bool found = false;
                for (auto& ch : items) if (ch.label == r.name) found = true;
                if (!found) items.push_back({ {}, r.name, "resident", "derived", false, true });
            }
        if (m->mode == Mode::Create && v->liveConfigured)
        {
            text("AUTHORIZED ACTIONS", railL + 18, y - 4, T.ink3, 0, true);
            y -= 22;
            y = wrapped("Moves go through MutSea's session protocol with the resident owner's agent token (Modify scope only). MutSea policy decides.", railL + 18, y, railR - railL - 36, T.ink2, 0, 3) - 6;
        }
        text(c.live ? "HERE · LIVE" : "HERE", railL + 18, y - 4, T.ink3, 0, true);
        if (items.empty()) text("Nothing is contained here in the catalog.", railL + 18, y - 26, T.ink3, 0);
        text(std::to_string(items.size()), railR - 18, y - 4, T.ink3, 0, true, LLFontGL::RIGHT);
        y -= 16;
        for (auto& it : items)
        {
            if (y - 48 < railBottom + 8) break;
            const bool sel = !it.entity.empty() && it.entity == v->selected;
            const std::string id = "here-" + it.label;
            LLRect rc(railL + 10, y, railR - 10, y - 46);
            if (sel) roundRect(rc.mLeft, rc.mTop, rc.mRight, rc.mBottom, 11, T.accentSoft);
            if (m->focused(id)) focusRing(rc, 11);
            const LLColor4 kc = it.live ? T.live : classColour(it.classification, it.demo);
            dot((F32)railL + 32, (F32)y - 23, 13, alpha(kc, .18f));
            if (it.live) icon(Icon::Live, (F32)railL + 32, (F32)y - 23, kc, .8f);
            else icon(it.kind == "building" ? Icon::Layers : it.kind == "machine" || it.kind == "component" ? Icon::Cube : Icon::Compass, (F32)railL + 32, (F32)y - 23, kc, .75f);
            text(clip(it.label, railR - railL - 110, 1, true), railL + 54, y - 20, T.ink, 1, true);
            std::string sub = kindTitle(it.kind) + (it.demo ? " · demonstration" : it.live ? " · live in MutSea" : "");
            if (it.live)
                for (auto& r : v->residents)
                    if (r.name == it.label)
                    {
                        auto& lr = m->lastRevision[r.id];
                        auto& lm = m->moved[r.id];
                        if (lr && r.revision != lr) lm = Clock::now();
                        if (!lr) lm = Clock::now() - std::chrono::hours(1);
                        lr = r.revision;
                        const double since = std::chrono::duration<double>(Clock::now() - lm).count();
                        sub = since < 3 ? "moved " + ago(since) : "rev " + std::to_string(r.revision);
                    }
            text(sub, railL + 54, y - 37, it.live ? T.live : T.ink3, 0);
            if (m->mode == Mode::Create && it.live)
            {
                // ±5 m authorized moves (Ω-XVII.3), offered only in Create.
                static const char* lbl[] = { "←", "↑", "↓", "→" };
                static const double d[4][2] = { { -5, 0 }, { 0, 5 }, { 0, -5 }, { 5, 0 } };
                for (int k = 0; k < 4; ++k)
                {
                    const S32 bx = railR - 18 - (4 - k) * 26;
                    LLRect br(bx, y - 12, bx + 22, y - 34);
                    roundRect(br.mLeft, br.mTop, br.mRight, br.mBottom, 7, T.surfaceHi);
                    text(lbl[k], bx + 11, y - 28, T.ink, 0, true, LLFontGL::HCENTER);
                    for (auto& r : v->residents)
                        if (r.name == it.label) m->hit(br, [eng, id2 = r.id, dx = d[k][0], dy = d[k][1]] { eng->post([eng, id2, dx, dy] { eng->move(id2, dx, dy); }); });
                }
            }
            else if (!it.entity.empty())
                m->hit(rc, [this, eng, it] {
                    if (m->view && m->view->selected == it.entity) eng->post([eng, e = it.entity] { eng->navigate(e); });  // second activation enters
                    else eng->post([eng, it] { eng->select(it.entity, it.label); });
                }, id);
            y -= 50;
        }
    }

    // ================================================================ right: inspector (Analyze) / action card (Explore)
    const S32 insR = W - 20, insL = W - (compact ? 300 : 380), insTop = crumbY - 78, insBottom = 120;
    if (m->mode == Mode::Analyze && !c.entity.empty() && !nativeCtx)
    {
        // Ω-XIX progressive inspector: an overview first (what it is, how it is shown, its
        // primary dimensions, material if the catalog states one, key actions); technical
        // detail sits in sections the user opens (geometry, frames, representations,
        // provenance, validation).
        panel(insL, insTop, insR, insBottom);
        const S32 innerW = insR - insL - 36;
        S32 y = insTop - 28;
        icon(Icon::Ruler, (F32)insL + 24, (F32)y + 4, T.accent, .8f);
        text("INSPECTOR", insL + 40, y, T.ink3, 0, true);
        text(kindTitle(c.kind), insR - 18, y, T.ink3, 0, false, LLFontGL::RIGHT);
        y -= 28;
        text(clip(c.label, innerW, 2, true), insL + 18, y, T.ink, 2, true);
        y -= 22;
        if (!c.description.empty()) y = wrapped(c.description, insL + 18, y, innerW, T.ink2, 0, 3) - 4;
        // Visual representation.
        std::string units;
        for (auto& r : c.reps) if (r.id == c.repId) units = r.units;
        text("Shown as", insL + 18, y, T.ink3, 0);
        text(clip((c.repLabel.empty() ? c.repKind : c.repLabel) + (units.empty() ? "" : " · " + units), innerW - 80, 0), insL + 88, y, T.ink, 0);
        y -= 20;
        // Primary dimensions (kernel-derived or design inputs, coloured by classification).
        std::vector<Property> props;
        for (auto& r : c.reps) if (r.kind == "cad_brep" && r.authorized) props = r.props;
        if (props.empty()) for (auto& r : c.reps) if (r.id == c.repId) props = r.props;
        auto findProp = [&](const char* n) -> const Property* { for (auto& p : props) if (p.name == n) return &p; return nullptr; };
        std::vector<const Property*> primary;
        for (const char* n : { "bore_d", "outside_D", "width_B" }) if (auto* p = findProp(n)) primary.push_back(p);
        if (primary.empty()) for (const char* n : { "envelope_x", "envelope_y", "envelope_z" }) if (auto* p = findProp(n)) primary.push_back(p);
        auto valueText = [](const Property& p) { return (std::fabs(p.value - std::round(p.value)) < 1e-9 ? fmt(p.value, 0) : fmt(p.value, p.value < 100 ? 3 : 1)) + " " + p.unit; };
        auto propColour = [](const Property& p) { return p.classification == "verified" ? T.authentic : p.classification == "inferred" ? T.warn : T.derived; };
        if (!primary.empty())
        {
            static const std::map<std::string, std::string> nice{ { "bore_d", "Bore d" }, { "outside_D", "Outside D" }, { "width_B", "Width B" },
                                                                  { "envelope_x", "Length" }, { "envelope_y", "Width" }, { "envelope_z", "Height" } };
            const S32 colW = innerW / (S32)primary.size();
            S32 x = insL + 18;
            for (auto* p : primary)
            {
                roundRect(x, y + 6, x + colW - 8, y - 38, 10, T.surfaceHi);
                text(nice.count(p->name) ? nice.at(p->name) : p->name, x + 10, y - 10, T.ink3, 0);
                text(valueText(*p), x + 10, y - 30, T.ink, 1, true);
                dot((F32)(x + colW - 18), (F32)y - 6, 3, propColour(*p));
                x += colW;
            }
            y -= 54;
        }
        // Material: only what the catalog states; never invented.
        const Property* density = findProp("density");
        text("Material", insL + 18, y, T.ink3, 0);
        text(density ? "density " + valueText(*density) + (density->classification == "inferred" ? " (assumed)" : "") : "not specified in the catalog",
             insL + 88, y, density ? T.ink : T.ink3, 0);
        y -= 26;
        // Key actions.
        {
            std::vector<std::pair<std::string, std::function<void()>>> acts;
            const bool eng = c.domain == "engineering";
            if (eng) acts.push_back({ "Frame", [e = eng ? m->engine.get() : nullptr] { if (e) e->post([e] { e->frameSelected(); }); } });
            if (eng) acts.push_back({ "X-Ray", [e = m->engine.get()] { e->post([e] { e->setPresentation(e->presentation == "xray" ? "realistic" : "xray"); }); } });
            if (eng) acts.push_back({ "Exploded", [e = m->engine.get()] { e->post([e] { e->setPresentation(e->presentation == "exploded" ? "realistic" : "exploded"); }); } });
            acts.push_back({ "Ask AI", [e = m->engine.get(), q = "What is " + c.label + ", and how does it work?"] { e->post([e, q] { e->ask(q); }); } });
            S32 bx = insL + 18;
            for (auto& a : acts)
            {
                const S32 bw = width(a.first, 0, true) + 22;
                if (bx + bw > insR - 14) break;
                LLRect br(bx, y + 8, bx + bw, y - 18);
                const std::string id = "ins-" + a.first;
                roundRect(br.mLeft, br.mTop, br.mRight, br.mBottom, 12, &a == &acts.front() ? T.accentSoft : T.surfaceHi);
                if (m->focused(id)) focusRing(br, 12);
                text(a.first, bx + bw / 2, y - 9, T.ink, 0, true, LLFontGL::HCENTER);
                m->hit(br, a.second, id);
                bx += bw + 6;
            }
            y -= 34;
        }
        // Advanced sections (collapsed until opened).
        auto section = [&](const std::string& key, const std::string& title, const std::string& summary, const std::function<S32(S32)>& body) {
            if (y < insBottom + 24) return;
            const bool open = m->openSections.count(key) > 0;
            LLRect hr(insL + 10, y + 12, insR - 10, y - 8);
            const std::string id = "sec-" + key;
            if (m->focused(id)) focusRing(hr, 8);
            gl_rect_2d(insL + 18, y + 14, insR - 18, y + 13, T.hairline, true);
            icon(Icon::Chevron, (F32)insL + 24, (F32)y + 2, T.ink3, open ? .7f : .6f);
            text(title, insL + 38, y - 2, T.ink2, 0, true);
            text(clip(summary, insR - insL - 200, 0), insR - 18, y - 2, T.ink3, 0, false, LLFontGL::RIGHT);
            m->hit(hr, [this, key] { if (!m->openSections.erase(key)) m->openSections.insert(key); }, id);
            y -= 22;
            if (open) y = body(y) - 4;
        };
        section("geometry", "Geometry & measurements", std::to_string(props.size()) + " values", [&](S32 yy) {
            for (auto& p : props)
            {
                if (yy < insBottom + 30) break;
                text(p.name, insL + 30, yy, T.ink2, 0);
                text(valueText(p), insR - 30, yy, p.classification == "inferred" ? T.warn : T.ink, 0, true, LLFontGL::RIGHT);
                dot((F32)insR - 20, (F32)yy + 4, 3, propColour(p));
                yy -= 17;
            }
            text("● design input (standard)  ● kernel-derived  ● assumed", insL + 30, yy - 2, T.ink3, 0);
            return yy - 18;
        });
        section("frames", "Coordinate frames", c.frames.empty() ? "none" : c.frames.front().id, [&](S32 yy) {
            for (size_t i = 0; i < c.frames.size(); ++i)
            {
                if (yy < insBottom + 30) break;
                const auto& f = c.frames[i];
                text(std::string(i ? "↑ " : "") + clip(f.id, insR - insL - 150, 0, i == 0), insL + 30, yy, i == 0 ? T.ink : T.ink2, 0, i == 0);
                text(f.cls + " · " + f.units, insR - 18, yy, T.ink3, 0, false, LLFontGL::RIGHT);
                yy -= 15;
                if (!f.transform.empty()) { text("   " + clip(f.transform, insR - insL - 60, 0), insL + 30, yy, f.transform.find("presentation") != std::string::npos || f.transform.find("demonstration") != std::string::npos ? T.warn : T.ink3, 0); yy -= 15; }
            }
            return yy;
        });
        section("reps", "Representations", std::to_string(c.reps.size()) + " of this entity", [&](S32 yy) {
            for (auto& r : c.reps)
            {
                if (yy < insBottom + 30) break;
                const bool cur = r.id == c.repId;
                const LLColor4 col = !r.authorized ? T.ink3 : cur ? T.accent : T.ink2;
                dot((F32)insL + 34, (F32)yy + 4, 3.5f, col);
                text(r.kind, insL + 44, yy, col, 0, cur);
                text(r.authorized ? r.units : "requires engineering scope", insR - 18, yy, r.authorized ? T.ink3 : T.warn, 0, false, LLFontGL::RIGHT);
                if (r.authorized && !r.scene.empty() && !cur)
                    m->hit(LLRect(insL + 20, yy + 12, insR - 10, yy - 6), [eng, e = c.entity, k = r.kind] { eng->post([eng, e, k] { eng->navigate(e, {}, true, k); }); }, "rep-" + r.id);
                yy -= 18;
            }
            return yy;
        });
        section("provenance", "Provenance", c.sources.empty() ? "none recorded" : std::to_string(c.sources.size()) + " sources", [&](S32 yy) {
            for (auto& src : c.sources) { if (yy < insBottom + 16) break; text(clip(src, insR - insL - 50, 0), insL + 30, yy, T.ink2, 0); yy -= 15; }
            return yy;
        });
        section("validation", "Validation", classLabel(c.classification, c.demo), [&](S32 yy) {
            text(classLabel(c.classification, c.demo), insL + 30, yy, classColour(c.classification, c.demo), 0, true);
            yy -= 16;
            for (auto& w : c.warnings) { if (yy < insBottom + 16) break; yy = wrapped(w, insL + 30, yy, insR - insL - 50, T.demo, 0, 2); }
            if (c.warnings.empty()) { text("No validation warnings for this context.", insL + 30, yy, T.ink3, 0); yy -= 15; }
            return yy;
        });
    }
    else if (m->mode == Mode::Create && !v->creation.draftId.empty())
    {
        // Ω-XIX Create panel: the draft, its validation, objects, edits, history and publication.
        const auto& cr = v->creation;
        const S32 cl = insL - (compact ? 0 : 20), ct = insTop, cb = insBottom;
        panel(cl, ct, insR, cb, 16.f, true);
        S32 y = ct - 28;
        icon(Icon::Plus, (F32)cl + 24, (F32)y + 4, T.violet, .8f);
        text(cr.published ? "PUBLISHED" : "DRAFT", cl + 40, y, cr.published ? T.live : T.violet, 0, true);
        text("revision " + std::to_string(cr.revision) + " · demonstration", insR - 18, y, T.ink3, 0, false, LLFontGL::RIGHT);
        y -= 28;
        text(clip(cr.name, insR - cl - 36, 2, true), cl + 18, y, T.ink, 2, true);
        y -= 20;
        y = wrapped(cr.summary, cl + 18, y, insR - cl - 36, T.ink2, 0, 2) - 2;
        text(clip(cr.zone, insR - cl - 36, 0), cl + 18, y, T.ink3, 0);
        y -= 22;
        // Validation.
        {
            const std::string vl = cr.valid ? "VALID" : std::to_string(cr.findings.size()) + " PROBLEM" + (cr.findings.size() == 1 ? "" : "S");
            const LLColor4 vc = cr.valid ? T.live : T.bad;
            roundRect(cl + 18, y + 8, cl + 30 + width(vl, 0, true), y - 12, 9, alpha(vc, .18f));
            text(vl, cl + 24, y - 6, vc, 0, true);
            std::string checks;
            for (auto& c2 : cr.checks) checks += (checks.empty() ? "" : "  ") + c2;
            text(clip(checks, insR - cl - 80 - width(vl, 0, true), 0), cl + 40 + width(vl, 0, true), y - 6, T.ink3, 0);
            y -= 22;
            for (size_t i = 0; i < cr.findings.size() && i < 2; ++i) y = wrapped(cr.findings[i], cl + 18, y, insR - cl - 36, T.bad, 0, 2);
        }
        // Objects.
        text("OBJECTS · " + std::to_string(cr.objects.size()), cl + 18, y - 4, T.ink3, 0, true);
        y -= 22;
        for (auto& o : cr.objects)
        {
            if (y < cb + 150) break;
            const bool sel = o.id == cr.selected;
            LLRect rc(cl + 10, y + 12, insR - 10, y - 8);
            const std::string id = "obj-" + o.id;
            if (sel) roundRect(rc.mLeft, rc.mTop, rc.mRight, rc.mBottom, 8, T.accentSoft);
            if (m->focused(id)) focusRing(rc, 8);
            text(clip(o.label, insR - cl - 170, 0, sel), cl + 22, y - 2, sel ? T.ink : T.ink2, 0, sel);
            text(o.material + " · " + fmt(o.x, 1) + ", " + fmt(o.y, 1) + (o.rot != 0 ? " · " + fmt(o.rot, 0) + "°" : ""), insR - 18, y - 2, T.ink3, 0, false, LLFontGL::RIGHT);
            if (!cr.published) m->hit(rc, [eng, oid = o.id] { eng->post([eng, oid] { eng->selectDraftObject(oid); }); }, id);
            y -= 20;
        }
        // Edit controls for the selected object.
        if (!cr.published && !cr.selected.empty())
        {
            y -= 4;
            std::vector<std::pair<std::string, std::function<void()>>> btn = {
                { "←", [eng, o = cr.selected] { eng->post([eng, o] { eng->creationEdit("move", o, bj::array{ -.5, 0.0 }); }); } },
                { "→", [eng, o = cr.selected] { eng->post([eng, o] { eng->creationEdit("move", o, bj::array{ .5, 0.0 }); }); } },
                { "↑", [eng, o = cr.selected] { eng->post([eng, o] { eng->creationEdit("move", o, bj::array{ 0.0, .5 }); }); } },
                { "↓", [eng, o = cr.selected] { eng->post([eng, o] { eng->creationEdit("move", o, bj::array{ 0.0, -.5 }); }); } },
                { "⟲ 15°", [eng, o = cr.selected] { eng->post([eng, o] { eng->creationEdit("rotate", o, 15.0); }); } },
                { "Material", [eng, o = cr.selected] { eng->post([eng, o] { eng->creationEdit("material", o, nullptr); }); } },
                { "Remove", [eng, o = cr.selected] { eng->post([eng, o] { eng->creationEdit("remove", o, nullptr); }); } } };
            S32 bx = cl + 18;
            for (auto& b : btn)
            {
                const S32 bw = width(b.first, 0, true) + 18;
                if (bx + bw > insR - 12) { bx = cl + 18; y -= 30; }
                LLRect br(bx, y + 8, bx + bw, y - 16);
                const std::string id = "edit-" + b.first;
                roundRect(br.mLeft, br.mTop, br.mRight, br.mBottom, 10, T.surfaceHi);
                if (m->focused(id)) focusRing(br, 10);
                text(b.first, bx + bw / 2, y - 7, T.ink, 0, true, LLFontGL::HCENTER);
                m->hit(br, b.second, id);
                bx += bw + 5;
            }
            y -= 32;
        }
        // History + publish.
        {
            std::vector<std::tuple<std::string, bool, std::function<void()>>> btn;
            if (!cr.published)
            {
                btn.push_back({ "Undo", cr.histCursor > 0, [eng] { eng->post([eng] { eng->creationHistory(false); }); } });
                btn.push_back({ "Redo", cr.histCursor + 1 < cr.histSize, [eng] { eng->post([eng] { eng->creationHistory(true); }); } });
                btn.push_back({ "Publish…", cr.valid && cr.pubStage != "publishing", [this] { m->confirmPublish = true; } });
            }
            S32 bx = cl + 18;
            for (auto& [label, enabled, f] : btn)
            {
                const S32 bw = width(label, 0, true) + 24;
                LLRect br(bx, y + 8, bx + bw, y - 18);
                const std::string id = "cr-" + label;
                const bool primary = label == "Publish…";
                roundRect(br.mLeft, br.mTop, br.mRight, br.mBottom, 12, !enabled ? alpha(T.surfaceHi, .4f) : primary ? T.accent : T.surfaceHi);
                if (m->focused(id)) focusRing(br, 12);
                text(label, bx + bw / 2, y - 8, !enabled ? T.ink3 : primary ? T.bg : T.ink, 0, true, LLFontGL::HCENTER);
                if (enabled) m->hit(br, f, id);
                bx += bw + 6;
            }
            y -= 34;
        }
        // Publication progress / receipt.
        if (!cr.pubStage.empty())
        {
            const LLColor4 pc = cr.pubStage == "published" ? T.live : cr.pubStage == "refused" ? T.bad : T.accent;
            static const char* names[] = { "verify", "package", "spatialdb", "mutsea" };
            static const char* labels[] = { "Verify approved revision", "Package (Forge, immutable)", "SpatialDB catalog edition", "MutSea owner commit · WorldLine" };
            for (int i = 0; i < 4 && y > cb + 30; ++i)
            {
                const bool reached = std::find(cr.pubSteps.begin(), cr.pubSteps.end(), names[i]) != cr.pubSteps.end();
                const bool current = reached && cr.pubStage == "publishing" && cr.pubSteps.back() == names[i];
                const bool ok = reached && !current && cr.pubStage != "refused";
                text(ok || cr.pubStage == "published" ? "✓" : current ? "…" : "○", cl + 22, y, ok || cr.pubStage == "published" ? T.live : current ? T.accent : T.ink3, 0, true);
                text(labels[i], cl + 40, y, reached ? T.ink2 : T.ink3, 0);
                y -= 18;
            }
            y = wrapped(cr.pubStage == "published" ? "Published · " + cr.receipt : cr.pubMessage, cl + 18, y - 2, insR - cl - 36, pc, 0, 2);
        }
        if (!cr.edits.empty() && y > cb + 34) text(clip("Last edit: " + cr.edits.back(), insR - cl - 36, 0), cl + 18, cb + 14, T.ink3, 0);
        else text("Arrows move · [ ] rotate · M material · Ctrl+Z undo", cl + 18, cb + 14, T.ink3, 0);
    }
    else if ((m->mode == Mode::Explore || m->mode == Mode::Create) && !v->selected.empty() && !compact && !nativeCtx)
    {
        // Contextual action card for the selection.
        std::string kind, label = v->selectedLabel;
        bool demo = false;
        for (auto& ch : c.children) if (ch.entity == v->selected) { kind = ch.kind; demo = ch.demo; }
        const S32 cl = insL + 40, ct = insTop - 10, cb = ct - 150;
        panel(cl, ct, insR, cb, 16.f, true);
        text(kindTitle(kind), cl + 18, ct - 26, T.ink3, 0, true);
        text(clip(label, insR - cl - 36, 2, true), cl + 18, ct - 54, T.ink, 2, true);
        if (demo) text("Demonstration fixture", cl + 18, ct - 74, T.demo, 0);
        std::vector<std::pair<std::string, std::function<void()>>> acts;
        const std::string sel = v->selected;
        acts.push_back({ kind == "building" ? "Enter" : kind == "area" ? "Zoom in" : "Open", [eng, sel] { eng->post([eng, sel] { eng->navigate(sel); }); } });
        acts.push_back({ "Inspect", [this, eng, sel] { m->mode = Mode::Analyze; eng->post([eng, sel] { eng->navigate(sel); }); } });
        acts.push_back({ "Ask AI", [this] { getChild<LLLineEditor>("ask")->setFocus(true); } });
        S32 bx = cl + 18;
        for (auto& a : acts)
        {
            const S32 bw = width(a.first, 0, true) + 28;
            LLRect br(bx, cb + 46, bx + bw, cb + 18);
            const std::string id = "act-" + a.first;
            roundRect(br.mLeft, br.mTop, br.mRight, br.mBottom, 14, &a == &acts.front() ? T.accent : T.surfaceHi);
            if (m->focused(id)) focusRing(br, 13);
            text(a.first, bx + bw / 2, cb + 27, &a == &acts.front() ? T.bg : T.ink, 0, true, LLFontGL::HCENTER);
            m->hit(br, a.second, id);
            bx += bw + 8;
        }
    }

    // ================================================================ Develop drawer
    if (m->mode == Mode::Develop)
    {
        const S32 dl = showRail ? railR + 16 : 20, dr = W - 20, dt = H / 2 + 40, db = 120;
        panel(dl, dt, dr, db);
        S32 y = dt - 28;
        icon(Icon::Code, (F32)dl + 24, (F32)y + 4, T.accent, .8f);
        text("DEVELOP · protocols, replication, rendering", dl + 40, y, T.ink3, 0, true);
        y -= 26;
        const S32 col = (dr - dl - 36) / 3;
        auto kv = [&](S32 x, S32& yy, const std::string& k, const std::string& val, const LLColor4& vc = T.ink) {
            text(k, x, yy, T.ink3, 0);
            text(clip(val, col - 120, 0), x + col - 16, yy, vc, 0, false, LLFontGL::RIGHT);
            yy -= 16;
        };
        S32 y1 = y, y2 = y, y3 = y;
        const S32 x1 = dl + 18, x2 = x1 + col, x3 = x2 + col;
        text("FVP DOMAIN 4 · SpatialDB", x1, y1, T.ink, 0, true); y1 -= 20;
        kv(x1, y1, "server", v->server);
        kv(x1, y1, "catalog edition", std::to_string(v->edition));
        kv(x1, y1, "scopes", v->scopes);
        kv(x1, y1, "calls", std::to_string(v->calls));
        kv(x1, y1, "last call", fmt(v->lastCallMs, 2) + " ms");
        kv(x1, y1, "navigate", fmt(v->navigateMs, 2) + " ms");
        kv(x1, y1, "context resolve", fmt(v->resolveMs, 1) + " ms");
        if (!v->latencies.empty())
        {
            std::vector<double> l(v->latencies.begin(), v->latencies.end());
            std::sort(l.begin(), l.end());
            kv(x1, y1, "p50 / p95", fmt(l[l.size() / 2], 2) + " / " + fmt(l[(size_t)(l.size() * .95) == l.size() ? l.size() - 1 : (size_t)(l.size() * .95)], 2) + " ms");
        }
        if (!v->spatialError.empty()) kv(x1, y1, "error", v->spatialError, T.bad);
        text("MUTSEA · FVP DOMAIN 3", x2, y2, T.ink, 0, true); y2 -= 20;
        if (v->liveConfigured)
        {
            kv(x2, y2, "state", v->live.state == MS_REPLICA_LIVE ? "LIVE" : "not live", v->live.state == MS_REPLICA_LIVE ? T.live : T.warn);
            kv(x2, y2, "revision", std::to_string(v->live.revision));
            kv(x2, y2, "WorldLine", std::to_string(v->live.worldline_length));
            kv(x2, y2, "commit → viewer", fmt(v->live.last_latency_ms, 2) + " ms");
            kv(x2, y2, "resyncs / reconnects", std::to_string(v->live.resyncs) + " / " + std::to_string(v->live.reconnects));
            kv(x2, y2, "registration", v->registrationLabel, T.warn);
            y2 -= 6;
            for (auto it = v->events.rbegin(); it != v->events.rend() && y2 > db + 14; ++it)
            {
                text("#" + std::to_string(it->seq) + " " + it->action + " · " + it->kind, x2, y2, T.ink2, 0);
                y2 -= 15;
            }
        }
        else text("No live world configured", x2, y2, T.ink3, 0);
        text("STORM · scene context", x3, y3, T.ink, 0, true); y3 -= 20;
        if (fr)
        {
            kv(x3, y3, "frame id", fr->frameId);
            kv(x3, y3, "units", fr->units);
            kv(x3, y3, "triangles", std::to_string(fr->triangles));
            kv(x3, y3, "capture", fmt(fr->captureMs, 1) + " ms");
            kv(x3, y3, "transport", fr->transport + " · " + std::to_string(fr->width) + "×" + std::to_string(fr->height));
            kv(x3, y3, "context switch", fmt(fr->switchMs, 1) + " ms");
            kv(x3, y3, "frame", std::to_string(fr->number));
            kv(x3, y3, "control seq", std::to_string(fr->controlSeq) + " / " + std::to_string(v->controlSeq));
            kv(x3, y3, "source revision", std::to_string(fr->sourceRevision));
        }
        y3 -= 6;
        text("NAVIGATION HISTORY", x3, y3, T.ink, 0, true);
        y3 -= 18;
        const size_t room = (size_t)std::max<S32>(1, (y3 - db - 14) / 15);
        const size_t first = v->historyCursor + 1 > room ? v->historyCursor + 1 - room : 0;
        for (size_t i = first; i < v->history.size() && y3 > db + 14; ++i)
        {
            text((i == v->historyCursor ? "▸ " : "  ") + clip(v->history[i], col - 30, 0), x3, y3, i == v->historyCursor ? T.accent : T.ink2, 0, i == v->historyCursor);
            y3 -= 15;
        }
    }

    // ================================================================ AI bar + chips + card
    {
        const S32 barW = llclamp(W - 520, 420, 760), barL = (W - barW) / 2, barR = barL + barW, barT = 70, barB = 22;
        panel(barL, barT, barR, barB, 24.f, true);
        icon(Icon::Spark, (F32)barL + 26, (F32)(barT + barB) / 2, mix(T.accent, T.violet, .5f + .5f * (float)std::sin(t * 1.3)), .85f);
        if (getChild<LLLineEditor>("ask")->hasFocus()) roundRect(barL - 2, barT + 2, barR + 2, barB - 2, 26, alpha(T.focus, .25f));
        // Contextual suggestion chips.
        std::vector<std::pair<std::string, std::string>> chips;
        if (c.kind == "city") chips = { { "Show the robotics facility", "ask" }, { "Find the industrial robot and show me its bearing", "ask" } };
        else if (c.kind == "facility") chips = { { "Enter Assembly Hall B", "ask" }, { "Back to Montréal", "home" } };
        else if (c.kind == "building" || c.kind == "area") chips = { { "Show me the robot in this hall", "ask" }, { "Show its bearing assembly", "ask" } };
        else if (c.kind == "machine") chips = { { "Show the joint-2 bearing", "ask" }, { "Open the gearbox CAD", "ask" } };
        else if (c.kind == "component") chips = { { "What is it made of?", "ask" }, { "Return to Montréal", "home" } };
        S32 cx = barL + 8;
        const S32 chipY = barT + 30;
        // Ω-XIX presentation presets for engineering contexts (display only; V cycles, F frames).
        if (c.domain == "engineering" && m->mode != Mode::Develop)
        {
            static const std::pair<const char*, const char*> presets[] = { { "realistic", "Realistic" }, { "technical", "Technical" }, { "inspection", "Inspect" },
                                                                         { "xray", "X-Ray" }, { "exploded", "Exploded" } };
            S32 total = 0;
            for (auto& p : presets) total += width(p.second, 0) + 26;
            const S32 py = chipY + 36, pl = barR - total - 8;
            panel(pl - 6, py + 16, barR, py - 16, 16.f, true);
            S32 px = pl;
            for (auto& p : presets)
            {
                const S32 pw = width(p.second, 0) + 22;
                LLRect rc(px, py + 12, px + pw, py - 12);
                const bool sel = v->presentation == p.first;
                const std::string id = std::string("preset-") + p.first;
                if (sel) roundRect(rc.mLeft, rc.mTop, rc.mRight, rc.mBottom, 12.f, T.accentSoft);
                if (m->focused(id)) focusRing(rc, 12);
                text(p.second, px + 11, py - 4, sel ? T.ink : T.ink2, 0, sel);
                m->hit(rc, [eng, k = std::string(p.first)] { eng->post([eng, k] { eng->setPresentation(k); }); }, id);
                px += pw + 4;
            }
            text("Display only · F frame selection · Shift-drag pan", pl, py + 22, T.ink3, 0);
        }
        if (m->mode != Mode::Develop)
            for (auto& ch : chips)
            {
                const S32 cw = width(ch.first, 0) + 24;
                if (cx + cw > barR) break;
                LLRect rc(cx, chipY + 12, cx + cw, chipY - 12);
                const std::string id = "chip-" + ch.first;
                panel(rc.mLeft, rc.mTop, rc.mRight, rc.mBottom, 12.f);
                if (m->focused(id)) focusRing(rc, 12);
                text(ch.first, cx + 12, chipY - 4, T.ink2, 0);
                if (ch.second == "home") m->hit(rc, [eng] { eng->post([eng] { eng->navigate(eng->startEntity); }); }, id);
                else m->hit(rc, [eng, q = ch.first] { eng->post([eng, q] { eng->ask(q); }); }, id);
                cx += cw + 8;
            }
        // Ω-XIX creation task card: what the World Director did, stage by stage (no model reasoning).
        const auto& cr = v->creation;
        static double creationShownAt = -100;
        static std::string creationKey;
        if (cr.stage == "preview_ready" && creationKey != cr.draftId + std::to_string(cr.revision)) { creationKey = cr.draftId + std::to_string(cr.revision); creationShownAt = t; }
        const bool creationCard = cr.stage == "working" || cr.stage == "refused" || (cr.stage == "preview_ready" && cr.revision == 1 && t - creationShownAt < 12.0);
        if (creationCard && (v->ai.stage.empty() || v->ai.stage == "done" || v->ai.stage == "cancelled"))
        {
            const bool working = cr.stage == "working", refused = cr.stage == "refused";
            struct St { std::string label; int state; };
            std::vector<St> steps;
            const int done = working ? 0 : refused ? 0 : 2;
            steps.push_back({ "Understand the request (" + (cr.model.empty() ? std::string("local model") : cr.model) + ")", working ? 1 : refused ? 3 : 2 });
            steps.push_back({ "Resolve the place through the spatial service (FVP)", done });
            steps.push_back({ "Typed creation plan · " + (cr.objects.empty() ? std::string("library assets only") : std::to_string(cr.objects.size()) + " objects"), done });
            steps.push_back({ "Asset discovery · demonstration library + Forge CAD", done });
            steps.push_back({ "Scene assembly · deterministic layout, draft world definition", done });
            steps.push_back({ "Validation · zone, clearance, egress, Core FWD, Forge", working || refused ? 0 : (cr.valid ? 2 : 3) });
            steps.push_back({ "Preview in the Viewer (draft, not published)", done });
            const bool panelShown = m->mode == Mode::Create && !cr.draftId.empty();
            const S32 cl2 = panelShown ? std::max(20, insL - 40 - (barR - barL)) : barL, cr2 = panelShown ? insL - 40 : barR, cb2 = chipY + 22;
            const S32 ct2 = cb2 + 96 + (S32)steps.size() * 19 + 34;
            panel(cl2, ct2, cr2, cb2, 16.f, true);
            const LLColor4 sc = refused ? T.bad : working ? T.violet : cr.valid ? T.live : T.warn;
            const std::string st = working ? "CREATING" : refused ? "NOT POSSIBLE" : cr.valid ? "DRAFT READY" : "DRAFT NEEDS FIXES";
            roundRect(cl2 + 16, ct2 - 12, cl2 + 28 + width(st, 0, true), ct2 - 32, 9, alpha(sc, .18f));
            text(st, cl2 + 22, ct2 - 26, sc, 0, true);
            text(clip(cr.request, cr2 - cl2 - 220, 0, true), cl2 + 40 + width(st, 0, true), ct2 - 26, T.ink, 0, true);
            int ndone = 0;
            for (auto& x : steps) ndone += x.state == 2;
            roundRect(cl2 + 18, ct2 - 44, cr2 - 18, ct2 - 48, 2, T.hairline);
            if (ndone) roundRect(cl2 + 18, ct2 - 44, cl2 + 18 + (S32)((cr2 - cl2 - 36) * ndone / (float)steps.size()), ct2 - 48, 2, sc);
            S32 y = ct2 - 70;
            for (auto& x : steps)
            {
                const LLColor4 col = x.state == 2 ? T.live : x.state == 1 ? T.violet : x.state == 3 ? T.bad : T.ink3;
                if (x.state == 1 && !eng->reducedMotion)
                {
                    gGL.pushMatrix();
                    gGL.translatef((F32)cl2 + 26, (F32)y + 5, 0);
                    gl_washer_segment_2d(6, 4.f, (F32)(t * 5), (F32)(t * 5 + 4.4), 14, col, alpha(col, 0));
                    gGL.popMatrix();
                }
                else text(x.state == 2 ? "✓" : x.state == 3 ? "✕" : "○", cl2 + 21, y, col, 0, true);
                text(clip(x.label, cr2 - cl2 - 70, 0), cl2 + 40, y, x.state == 0 ? T.ink3 : T.ink2, 0, x.state == 1);
                y -= 19;
            }
            wrapped(cr.message, cl2 + 18, y - 4, cr2 - cl2 - 36, refused ? T.bad : T.ink2, 0, 2);
            text("Creates a draft only · publishing needs your approval", cl2 + 18, cb2 + 10, T.ink3, 0);
            if (!working) text(fmt(cr.seconds, 1) + " s" + (cr.planSeconds > 0 ? " · plan " + fmt(cr.planSeconds, 1) + " s" : ""), cr2 - 18, cb2 + 10, T.ink3, 0, false, LLFontGL::RIGHT);
        }
        // Ω-XIX AI task panel: intent, typed plan, step-by-step execution with progress, result,
        // errors and approvals; cancel and retry. Shows concise action traces, never model reasoning.
        const AiCard& ai = v->ai;
        static std::string finishedId;
        static double finishedAt = 0;
        const bool terminal = ai.stage == "done" || ai.stage == "refused" || ai.stage == "cancelled";
        if (terminal && finishedId != ai.id + ai.stage) { finishedId = ai.id + ai.stage; finishedAt = t; }
        const bool expired = terminal && t - finishedAt > (ai.answer.empty() ? 20.0 : 60.0);
        if (!ai.stage.empty() && !expired && !ai.answer.empty())
        {
            // Grounded answer: the model's wording, then the catalog facts it may use, each with its class.
            const S32 cl2 = barL, cr2 = barR, cb2 = chipY + 22 + (c.domain == "engineering" && m->mode != Mode::Develop ? 56 : 0);
            std::vector<std::pair<std::string, std::string>> shownFacts;
            for (auto& f : ai.facts) if (f.first != "catalog" && shownFacts.size() < 5 && std::none_of(shownFacts.begin(), shownFacts.end(), [&](auto& x) { return x.second == f.second; })) shownFacts.push_back(f);
            const S32 ct2 = cb2 + 120 + 17 * 4 + 17 * 3 + (S32)shownFacts.size() * 17 + 30;
            panel(cl2, ct2, cr2, cb2, 16.f, true);
            roundRect(cl2 + 16, ct2 - 12, cl2 + 28 + width("ANSWER", 0, true), ct2 - 32, 9, alpha(T.live, .18f));
            text("ANSWER", cl2 + 22, ct2 - 26, T.live, 0, true);
            text(clip(ai.query, cr2 - cl2 - 260, 0, true), cl2 + 40 + width("ANSWER", 0, true), ct2 - 26, T.ink, 0, true);
            {
                LLRect br(cr2 - 100, ct2 - 10, cr2 - 16, ct2 - 34);
                roundRect(br.mLeft, br.mTop, br.mRight, br.mBottom, 12, T.surfaceHi);
                text("Dismiss", cr2 - 58, ct2 - 27, T.ink, 0, true, LLFontGL::HCENTER);
                m->hit(br, [] { finishedAt = -100; }, "ai-Dismiss");
            }
            S32 y = ct2 - 54;
            text("FROM THE CATALOG · " + ai.subject, cl2 + 18, y, T.ink3, 0, true);
            y = wrapped(ai.answer, cl2 + 18, y - 18, cr2 - cl2 - 36, T.ink, 0, 4) - 6;
            text("GENERAL BACKGROUND · not from the catalog", cl2 + 18, y, T.ink3, 0, true);
            y = wrapped(ai.general, cl2 + 18, y - 18, cr2 - cl2 - 36, T.ink2, 0, 3) - 6;
            text("FACTS USED", cl2 + 18, y, T.ink3, 0, true);
            y -= 18;
            for (auto& f : shownFacts)
            {
                const LLColor4 col = f.first.find("assum") != std::string::npos ? T.warn : f.first.find("kernel") != std::string::npos ? T.derived : f.first.find("design") != std::string::npos ? T.authentic : T.ink3;
                dot((F32)cl2 + 22, (F32)y + 4, 3, col);
                text(clip(f.first + " · " + f.second, cr2 - cl2 - 50, 0), cl2 + 32, y, T.ink2, 0);
                y -= 17;
            }
            text("Grounded answer · catalog facts via FVP · no world changes", cl2 + 18, cb2 + 10, T.ink3, 0);
            if (!ai.model.empty() || ai.planSeconds > 0) text(fmt(ai.planSeconds, 1) + " s", cr2 - 18, cb2 + 10, T.ink3, 0, false, LLFontGL::RIGHT);
        }
        else if (!ai.stage.empty() && !expired)
        {
            struct Step { std::string label; int state; };  // 0 pending, 1 running, 2 done, 3 failed
            std::vector<Step> steps;
            const bool planned = ai.stage == "ready" || ai.stage == "executing" || ai.stage == "done" || !ai.route.empty() || (ai.stage == "refused" && !ai.plan.empty());
            const bool failed = ai.stage == "refused", cancelled = ai.stage == "cancelled";
            steps.push_back({ "Understand the request (" + (ai.model.empty() ? std::string("local model") : ai.model) + ")", planned ? 2 : (cancelled ? 3 : 1) });
            steps.push_back({ "Validate the typed plan against the spatial service", planned ? (failed && ai.route.empty() ? 3 : 2) : 0 });
            for (size_t i = 0; i < ai.routeLabels.size(); ++i)
            {
                int st = 0;
                if (ai.stage == "done") st = 2;
                else if (ai.stage == "executing") st = i + 1 < ai.executing ? 2 : i + 1 == ai.executing ? 1 : 0;
                else if (failed || cancelled) st = i + 1 < ai.executing ? 2 : i + 1 == ai.executing ? 3 : 0;
                steps.push_back({ "Navigate → " + ai.routeLabels[i] + "  (SPATIAL.NAVIGATE)", st });
            }
            int doneSteps = 0;
            for (auto& st : steps) doneSteps += st.state == 2;
            const S32 shown = (S32)std::min<size_t>(steps.size(), 7);
            const S32 cl2 = barL, cr2 = barR, cb2 = chipY + 22 + (c.domain == "engineering" && m->mode != Mode::Develop ? 56 : 0);
            const S32 msgLines = ai.message.empty() ? 0 : 2;
            const S32 ct2 = cb2 + 96 + shown * 19 + msgLines * 17 + (S32)std::min<size_t>(ai.missing.size(), 2) * 17;
            panel(cl2, ct2, cr2, cb2, 16.f, true);
            const LLColor4 sc = failed ? T.bad : cancelled ? T.warn : ai.stage == "done" ? T.live : T.accent;
            const std::string st = ai.stage == "planning" ? "PLANNING" : ai.stage == "executing" ? "RUNNING" : ai.stage == "done" ? "DONE" : ai.stage == "ready" ? "READY" : cancelled ? "CANCELLED" : "NOT POSSIBLE";
            roundRect(cl2 + 16, ct2 - 12, cl2 + 28 + width(st, 0, true), ct2 - 32, 9, alpha(sc, .18f));
            text(st, cl2 + 22, ct2 - 26, sc, 0, true);
            text(clip(ai.query, cr2 - cl2 - 260, 0, true), cl2 + 40 + width(st, 0, true), ct2 - 26, T.ink, 0, true);
            // Cancel / retry / dismiss.
            {
                std::vector<std::pair<std::string, std::function<void()>>> btn;
                if (ai.stage == "planning" || ai.stage == "executing") btn.push_back({ "Cancel", [eng] { eng->aiCancel = true; } });
                if (failed || cancelled || ai.stage == "done") btn.push_back({ "Retry", [eng] { eng->post([eng] { eng->ask(eng->lastAsk); }); } });
                if (terminal) btn.push_back({ "Dismiss", [] { finishedAt = -100; } });
                S32 bx = cr2 - 16;
                for (auto it = btn.rbegin(); it != btn.rend(); ++it)
                {
                    const S32 bw = width(it->first, 0, true) + 20;
                    LLRect br(bx - bw, ct2 - 10, bx, ct2 - 34);
                    const std::string id = "ai-" + it->first;
                    roundRect(br.mLeft, br.mTop, br.mRight, br.mBottom, 12, it->first == "Cancel" ? alpha(T.bad, .18f) : T.surfaceHi);
                    if (m->focused(id)) focusRing(br, 12);
                    text(it->first, bx - bw / 2, ct2 - 27, T.ink, 0, true, LLFontGL::HCENTER);
                    m->hit(br, it->second, id);
                    bx -= bw + 6;
                }
            }
            // Progress.
            const float frac = steps.empty() ? 0.f : (float)doneSteps / steps.size();
            roundRect(cl2 + 18, ct2 - 44, cr2 - 18, ct2 - 48, 2, T.hairline);
            if (frac > 0) roundRect(cl2 + 18, ct2 - 44, cl2 + 18 + (S32)((cr2 - cl2 - 36) * frac), ct2 - 48, 2, sc);
            S32 y = ct2 - 70;
            for (S32 i = 0; i < shown; ++i)
            {
                const auto& s2 = steps[i];
                const LLColor4 col = s2.state == 2 ? T.live : s2.state == 1 ? T.accent : s2.state == 3 ? T.bad : T.ink3;
                if (s2.state == 1 && !eng->reducedMotion)
                {
                    gGL.pushMatrix();
                    gGL.translatef((F32)cl2 + 26, (F32)y + 5, 0);
                    gl_washer_segment_2d(6, 4.f, (F32)(t * 5), (F32)(t * 5 + 4.4), 14, col, alpha(col, 0));
                    gGL.popMatrix();
                }
                else text(s2.state == 2 ? "✓" : s2.state == 3 ? "✕" : "○", cl2 + 21, y, col, 0, true);
                text(clip(s2.label, cr2 - cl2 - 70, 0), cl2 + 40, y, s2.state == 0 ? T.ink3 : T.ink2, 0, s2.state == 1);
                y -= 19;
            }
            y -= 4;
            if (!ai.message.empty()) y = wrapped(ai.message, cl2 + 18, y, cr2 - cl2 - 36, failed ? T.bad : T.ink2, 0, 2);
            for (size_t i = 0; i < ai.missing.size() && i < 2; ++i) { text(clip("Missing: " + ai.missing[i], cr2 - cl2 - 36, 0), cl2 + 18, y, T.warn, 0); y -= 17; }
            const std::string foot = "Navigation only · no world changes · no approval needed";
            text(foot, cl2 + 18, cb2 + 10, T.ink3, 0);
            if (!ai.model.empty()) text(clip(ai.model + " · " + fmt(ai.planSeconds, 1) + " s", cr2 - cl2 - width(foot, 0) - 60, 0), cr2 - 18, cb2 + 10, T.ink3, 0, false, LLFontGL::RIGHT);
        }
    }

    // ================================================================ bottom status line (nonintrusive)
    {
        std::string left = c.sceneTitle.empty() ? "" : c.sceneTitle;
        if (fr) left += "  ·  Storm " + fr->units + (fr->units == "millimetre" ? " (engineering)" : "");
        text(clip(left, W / 2 - 420, 0), 22, 32, T.ink3, 0);
        std::string right = std::string(modeName(m->mode)) + "  ·  ? shortcuts";
        if (!v->actionNote.empty() && m->mode == Mode::Create) right = v->actionNote;
        text(clip(right, W / 2 - 420, 0), W - 22, 32, T.ink3, 0, false, LLFontGL::RIGHT);
        if (!c.warnings.empty() && !compact) text(clip(c.warnings.front(), W / 2 - 420, 0), 22, 50, T.demo, 0);
    }

    // ================================================================ transition card
    if (!c.entity.empty() && c.transition == "semantic" && sinceSwitch < 2.2 && !settled)
    {
        std::string units;
        for (auto& r : c.reps) if (r.id == c.repId) units = r.units;
        const std::string l1 = "Entering " + c.label, l2 = c.repKind + " · frame " + c.frame + " · " + units + " · separate dataset";
        const S32 w = std::max(width(l1, 2, true), width(l2, 0)) + 60, l = (W - w) / 2, tt = H / 2 + 44;
        panel(l, tt, l + w, tt - 88, 18.f, true);
        text("SEMANTIC TRANSITION", W / 2, tt - 26, T.violet, 0, true, LLFontGL::HCENTER);
        text(l1, W / 2, tt - 54, T.ink, 2, true, LLFontGL::HCENTER);
        text(l2, W / 2, tt - 74, T.ink2, 0, false, LLFontGL::HCENTER);
    }
    else if (v->spatialError.size() && !v->spatialUp)
    {
        const std::string l1 = "Spatial service unreachable", l2 = v->spatialError + " · retrying every 2 s";
        const S32 w = std::max(width(l1, 2, true), width(l2, 0)) + 60, l = (W - w) / 2, tt = H / 2 + 34;
        panel(l, tt, l + w, tt - 70, 18.f, true);
        text(l1, W / 2, tt - 30, T.bad, 2, true, LLFontGL::HCENTER);
        text(clip(l2, w - 40, 0), W / 2, tt - 52, T.ink2, 0, false, LLFontGL::HCENTER);
    }
    else if (!fr && !c.entity.empty())
    {
        text("Storm renderer starting…", W / 2, H / 2, T.ink2, 1, false, LLFontGL::HCENTER);
    }

    // ================================================================ toasts
    {
        S32 y = 150;
        for (auto it = v->toasts.rbegin(); it != v->toasts.rend(); ++it)
        {
            const S32 w = std::min(width(it->first, 0) + 40, W - 80), l = (W - w) / 2;
            panel(l, y + 30, l + w, y, 15.f, true);
            text(clip(it->first, w - 30, 0), W / 2, y + 10, T.ink, 0, false, LLFontGL::HCENTER);
            y += 38;
        }
    }

    // ================================================================ shortcuts help
    if (m->showHelp)
    {
        static const char* rows[][2] = { { "1 – 4", "Explore · Create · Analyze · Develop" }, { "Ctrl+K", "Command palette (places, actions, AI)" }, { "/", "Ask FINALVERSE" }, { "F · V · Shift-drag", "Frame selection · presentation · pan" }, { "Tab / Shift+Tab", "Move focus" },
                                          { "Enter", "Activate focused item (twice on a place: enter it)" }, { "Alt+← / Alt+→  ·  Backspace", "Back / forward" },
                                          { "H", "Home (Montréal)" }, { "Arrows · drag · wheel", "Orbit and zoom (presentation only)" }, { "R  ·  O", "Reset camera · orbit" },
                                          { "Ctrl + / Ctrl −", "Text size" }, { "Esc", "Clear selection" } };
        const S32 w = 600, h = 60 + 12 * 22, l = (W - w) / 2, tt = (H + h) / 2;
        panel(l, tt, l + w, tt - h, 18.f, true);
        text("Keyboard", l + 24, tt - 32, T.ink, 1, true);
        S32 y = tt - 60;
        for (auto& r : rows) { text(r[0], l + 24, y, T.accent, 0, true); text(r[1], l + 230, y, T.ink2, 0); y -= 22; }
    }

    // ================================================================ Ω-XIX draft banner + publish confirmation
    if (v->creation.previewActive && eng)
    {
        const std::string b = "DRAFT PREVIEW · " + v->creation.name + " · not published · demonstration";
        const S32 bw = width(b, 0, true) + 40, bl = (W - bw) / 2, bt = crumbY - 24;
        roundRect(bl, bt, bl + bw, bt - 26, 13, alpha(T.violet, .22f));
        text(b, W / 2, bt - 18, T.violet, 0, true, LLFontGL::HCENTER);
    }
    if (m->confirmPublish && eng)
    {
        const auto& cr = v->creation;
        gl_rect_2d(0, H, W, 0, alpha(T.bg, .55f), true);
        const S32 w = 620, h = 330, l = (W - w) / 2, tt = (H + h) / 2;
        panel(l, tt, l + w, tt - h, 18.f, true);
        text("Publish to the live world?", l + 26, tt - 40, T.ink, 2, true);
        S32 y = tt - 70;
        if (!cr.valid)
            y = wrapped("Revision " + std::to_string(cr.revision) + " has validation problems (" + (cr.findings.empty() ? std::string("see the panel") : cr.findings.front()) + "). Fix or undo first; it cannot be published.", l + 26, y, w - 52, T.bad, 0, 3);
        else
            y = wrapped("'" + cr.name + "' (revision " + std::to_string(cr.revision) + ", " + std::to_string(cr.objects.size()) + " objects) passed validation. Approving it will:", l + 26, y, w - 52, T.ink2, 0, 2);
        static const char* what[] = { "Package the validated world definition immutably (Forge receipt checked)",
                                      "Publish a new SpatialDB catalog edition: the hall shows the lab; a new demonstration entity",
                                      "Create one MutSea world entity with your owner authority, pinned to that publication",
                                      "Record the change in MutSea's hash-chained WorldLine" };
        for (auto* x : what) { text("•", l + 30, y, T.accent, 0, true); text(x, l + 46, y, T.ink2, 0); y -= 20; }
        y -= 6;
        text("The AI proposed this draft; only your approval publishes it. Demonstration placement, not surveyed.", l + 26, y, T.ink3, 0);
        const S32 by = tt - h + 34;
        LLRect cancel(l + w - 330, by + 14, l + w - 220, by - 14), ok(l + w - 210, by + 14, l + w - 26, by - 14);
        roundRect(cancel.mLeft, cancel.mTop, cancel.mRight, cancel.mBottom, 13, T.surfaceHi);
        text("Cancel", (cancel.mLeft + cancel.mRight) / 2, by - 5, T.ink, 0, true, LLFontGL::HCENTER);
        roundRect(ok.mLeft, ok.mTop, ok.mRight, ok.mBottom, 13, cr.valid ? T.accent : alpha(T.surfaceHi, .5f));
        text("Approve & publish", (ok.mLeft + ok.mRight) / 2, by - 5, cr.valid ? T.bg : T.ink3, 0, true, LLFontGL::HCENTER);
        m->hits.clear();  // modal: only the dialog is clickable
        m->hit(cancel, [this] { m->confirmPublish = false; }, "modal-cancel");
        if (cr.valid) m->hit(ok, [this, eng] { m->confirmPublish = false; eng->post([eng] { eng->publishApproved(); }); }, "modal-approve");
        text("Enter approves · Esc cancels", l + 26, by - 5, T.ink3, 0);
    }

    // ================================================================ Ω-XIX command palette
    if (m->paletteOpen && eng)
    {
        auto lower = [](std::string x) { for (auto& ch : x) ch = (char)std::tolower((unsigned char)ch); return x; };
        const std::string q = lower(m->paletteQuery);
        auto match = [&](const std::string& label) { return q.empty() || lower(label).find(q) != std::string::npos; };
        std::vector<Impl::PaletteItem> items;
        // Places and entities: live search (scope-filtered by the spatial service) or what is here.
        if (q.size() >= 2 && v->searchQuery == m->paletteQuery)
            for (auto& f : v->found)
                items.push_back({ "Go to", f.label, kindTitle(f.kind) + " · " + f.domain, true, [eng, e = f.entity] { eng->post([eng, e] { eng->navigate(e); }); } });
        for (auto& ch : c.children)
            if (match(ch.label)) items.push_back({ "Here", (ch.kind == "building" ? "Enter " : "Open ") + ch.label, kindTitle(ch.kind) + (ch.demo ? " · demonstration" : ""), true,
                                                   [eng, e = ch.entity] { eng->post([eng, e] { eng->navigate(e); }); } });
        // Recent locations (navigation history, newest first).
        for (size_t i = v->history.size(); i-- > 0;)
            if (i < v->historyEntities.size() && !v->historyEntities[i].empty() && v->historyEntities[i] != c.entity && match(v->history[i]))
            {
                bool dup = false;
                for (auto& it : items) dup = dup || it.label == v->history[i];
                if (!dup && items.size() < 40) items.push_back({ "Recent", v->history[i], "history", true, [eng, e = v->historyEntities[i]] { eng->post([eng, e] { eng->navigate(e); }); } });
            }
        // Saved views.
        for (auto& sv : m->savedViews)
            if (match(sv.label)) items.push_back({ "Saved views", sv.label, sv.presentation, true, [eng, sv] { eng->post([eng, sv] { eng->navigate(sv.entity); eng->setPresentation(sv.presentation); }); } });
        // Representations of this entity (permission-aware: withheld ones are shown, disabled, with the reason).
        for (auto& r : c.reps)
            if (r.id != c.repId && match("open " + r.kind))
                items.push_back({ "Representations", "Open " + r.kind, r.authorized ? r.units : "requires engineering scope", r.authorized && !r.scene.empty(),
                                  [eng, e = c.entity, k = r.kind] { eng->post([eng, e, k] { eng->navigate(e, {}, true, k); }); } });
        // Actions and tools (context-aware).
        auto act = [&](const std::string& label, const std::string& detail, std::function<void()> f, bool enabled = true) {
            if (match(label)) items.push_back({ "Actions", label, detail, enabled, std::move(f) });
        };
        for (int i = 0; i < 4; ++i) act(std::string("Switch to ") + modeName((Mode)i), std::to_string(i + 1), [this, i] { m->mode = (Mode)i; });
        if (c.domain == "engineering")
        {
            static const std::pair<const char*, const char*> presets[] = { { "realistic", "Realistic" }, { "technical", "Technical" }, { "inspection", "Inspection" }, { "xray", "X-Ray" }, { "exploded", "Exploded" } };
            for (auto& p : presets) act(std::string("Presentation: ") + p.second, "display only", [eng, k = std::string(p.first)] { eng->post([eng, k] { eng->setPresentation(k); }); });
            act("Frame selection", "F", [eng] { eng->post([eng] { eng->frameSelected(); }); }, !v->selected.empty());
        }
        act("Reset camera", "R", [eng] { eng->post([eng] { eng->resetCamera(); }); });
        act("Toggle orbit", "O", [eng] { eng->post([eng] { eng->orbit = !eng->orbit; eng->camera(0, 0, 1.0); }); });
        act("Save this view", c.label, [this, v] { m->savedViews.push_back({ v->ctx.label + (v->presentation != "realistic" ? " · " + v->presentation : ""), v->ctx.entity, v->presentation }); });
        act("Go home (Montréal)", "H", [eng] { eng->post([eng] { eng->navigate(eng->startEntity); }); });
        act("Larger text", "Ctrl +", [] { gScale = llmin(gScale + 1, 2); });
        act("Keyboard shortcuts", "?", [this] { m->showHelp = true; });
        // Ω-XX universal contexts, styles and Genesis.
        auto uni = [&](const std::string& label, const std::string& detail, std::function<void()> f) {
            if (match(label)) items.push_back({ "Universe", label, detail, true, std::move(f) });
        };
        uni("Earth from space", "astronomical · ephemeris", [eng] { eng->post([eng] { eng->goAstronomical(false); }); });
        uni("Descend to Montréal", "astronomical → geographic", [eng] { eng->post([eng] { eng->goAstronomical(true); }); });
        uni("Material structure: crystal", "scientific · idealised BCC α-iron", [eng] { eng->post([eng] { eng->goScientific("bcc-iron"); }); });
        uni("Material structure: microstructure", "scientific · illustrative", [eng] { eng->post([eng] { eng->goScientific("microstructure"); }); });
        uni("Civilization relationships", "abstract · demonstration graph", [eng] { eng->post([eng] { eng->goGraph(); }); });
        uni("Return", "back through the journey", [eng] { eng->post([eng] { eng->returnBack(); }); });
        static const char* styles[] = { "cinematic", "engineering", "scientific", "architectural", "stylized", "fantasy", "scifi", "historical", "abstract", "educational" };
        for (auto* st : styles)
            if (match(std::string("Style: ") + st)) items.push_back({ "Styles", std::string("Style: ") + st, v->style == st ? "current · presentation only" : "presentation only", true,
                                                                       [eng, k = std::string(st)] { eng->post([eng, k] { eng->setStyle(k); }); } });
        if (!m->paletteQuery.empty())
            items.push_back({ "Genesis", "Create a world: " + m->paletteQuery, "generated while you explore · designed content", true,
                              [eng, qq = m->paletteQuery] { eng->post([eng, qq] { eng->genesisCreate(qq); }); } });
        // AI instruction (always offered for free text).
        if (!m->paletteQuery.empty())
            items.push_back({ "AI", "Ask FINALVERSE: " + m->paletteQuery, "typed plan · validated before anything runs", true, [eng, qq = m->paletteQuery] { eng->post([eng, qq] { eng->ask(qq); }); } });
        m->paletteItems = items;
        if (m->paletteSel >= (int)items.size()) m->paletteSel = std::max(0, (int)items.size() - 1);
        // Draw.
        const S32 pw = llclamp(W - 200, 520, 720), pl = (W - pw) / 2, ptop = H - 150;
        const S32 rows = (S32)std::min<size_t>(items.size(), 11);
        std::set<std::string> groups;
        for (S32 i = 0; i < rows; ++i) groups.insert(items[i].group);
        const S32 ph = 70 + rows * 30 + (S32)groups.size() * 22 + 30;
        gl_rect_2d(0, H, W, 0, alpha(T.bg, .45f), true);
        panel(pl, ptop, pl + pw, ptop - ph, 18.f, true);
        icon(Icon::Search, (F32)pl + 28, (F32)ptop - 33, T.accent, .9f);
        if (m->paletteQuery.empty()) text("Go to a place, run an action, or ask FINALVERSE…", pl + 50, ptop - 40, T.ink3, 1);
        else
        {
            const S32 tx = text(m->paletteQuery, pl + 50, ptop - 40, T.ink, 1);
            if (std::fmod(t, 1.0) < .55) gl_rect_2d(tx + 2, ptop - 22, tx + 4, ptop - 44, T.accent, true);
        }
        text("Ctrl K", pl + pw - 22, ptop - 38, T.ink3, 0, false, LLFontGL::RIGHT);
        gl_rect_2d(pl + 16, ptop - 58, pl + pw - 16, ptop - 59, T.hairline, true);
        S32 y = ptop - 66;
        std::string lastGroup;
        for (S32 i = 0; i < rows; ++i)
        {
            const auto& it = items[i];
            if (it.group != lastGroup) { y -= 6; text(it.group, pl + 22, y - 12, T.ink3, 0, true); y -= 22; lastGroup = it.group; }
            LLRect rc(pl + 10, y, pl + pw - 10, y - 28);
            if (i == m->paletteSel) roundRect(rc.mLeft, rc.mTop, rc.mRight, rc.mBottom, 10, it.enabled ? T.accentSoft : alpha(T.ink3, .12f));
            text(clip(it.label, pw - 260, 1, i == m->paletteSel), pl + 24, y - 20, it.enabled ? T.ink : T.ink3, 1, i == m->paletteSel);
            text(clip(it.detail, 210, 0), pl + pw - 24, y - 19, it.enabled ? T.ink3 : T.warn, 0, false, LLFontGL::RIGHT);
            if (it.enabled) m->hit(rc, [this, run = it.run] { m->closePalette(); run(); }, "pal-" + std::to_string(i));
            y -= 30;
        }
        if (items.empty()) text("No matches. Press Enter to ask FINALVERSE.", pl + 24, y - 20, T.ink3, 1);
        text("↑↓ select · Enter run · Esc close · results are filtered by your access", pl + 22, ptop - ph + 14, T.ink3, 0);
    }

    // ================================================================ Ω-XX overlays
    // Context-aware HUD (astronomical / scientific / abstract), the Genesis panel and the
    // journey line. The world stays dominant: compact glass cards at the edges.
    if (v)
    {
        const S32 cardL = 24, cardW = compact ? 300 : 360, cardTop = H - 120;
        auto chip = [&](const std::string& s2, S32 x, S32 y, const LLColor4& col) { const S32 w = width(s2, 0, true) + 16; roundRect(x, y, x + w, y - 20, 10, alpha(col, .18f)); text(s2, x + 8, y - 15, col, 0, true); return x + w + 6; };
        if (nativeCtx)
        {
            std::vector<std::pair<std::string, std::string>> rows;
            std::string title = v->nativeLabel, badge, kindLabel;
            LLColor4 badgeCol = T.accent;
            if (v->nativeKind == "astronomical")
            {
                kindLabel = "ASTRONOMICAL";
                const double sim = v->simEpoch + std::chrono::duration<double>(Clock::now() - v->nativeSince).count() * v->simScale;
                const time_t tt = (time_t)sim;
                char buf[64] = {};
                std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M UTC", std::gmtime(&tt));
                rows = { { "Body", "Earth · Moon · Sun (direction)" }, { "Sim time", std::string(buf) + (v->simScale > 0 ? "  ×" + fmt(v->simScale, 0) : "") },
                         { "Frame", fr && !fr->frameId.empty() ? fr->frameId : "geocentric-equatorial-of-date" }, { "Units", "kilometre" },
                         { "Ephemeris", "AA low-precision · checked vs JPL Horizons" }, { "Earth", "NASA Blue Marble (observed, ~20–40 km/px)" }, { "Moon", "orbit computed · surface illustrative" } };
                badge = "OBSERVED + COMPUTED";
            }
            else if (v->nativeKind == "scientific")
            {
                kindLabel = "SCIENTIFIC";
                const bool crystal = v->nativeModel == "bcc-iron";
                rows = crystal ? std::vector<std::pair<std::string, std::string>>{ { "Material", "AISI 52100 bearing steel" }, { "Structure", "α-iron, body-centred cubic (Im-3m)" },
                                                                                  { "a", "2.8665 Å (reference value)" }, { "Nearest neighbour", "√3/2 · a = 2.4825 Å (highlighted)" },
                                                                                  { "Units", "ångström" }, { "Note", "hardened matrix is tetragonal martensite" } }
                               : std::vector<std::pair<std::string, std::string>>{ { "Material", "AISI 52100 bearing steel" }, { "Shows", "prior-austenite grains, lath bands, carbides" },
                                                                                  { "Units", "micrometre" }, { "This bearing", "grain size, hardness: unknown" } };
                badge = crystal ? "IDEALISED MODEL" : "ILLUSTRATIVE";
                badgeCol = T.warn;
            }
            else
            {
                kindLabel = "CIVILIZATION";
                rows = { { "Nodes", "observed · fictional (demonstration) · designed" }, { "Edges", "factual · demonstration · designed · hypothetical" },
                         { "Operator of the facility", "Port Robotics Cooperative (fictional)" }, { "Layout", "positions carry no meaning" } };
                badge = "DEMONSTRATION";
                badgeCol = T.warn;
            }
            const S32 h = 92 + (S32)rows.size() * 22;
            panel(cardL, cardTop, cardL + cardW, cardTop - h, 16.f);
            text(kindLabel, cardL + 18, cardTop - 24, T.accent, 0, true);
            chip(badge, cardL + cardW - width(badge, 0, true) - 32, cardTop - 10, badgeCol);
            text(clip(title, cardW - 36, 2, true), cardL + 18, cardTop - 52, T.ink, 2, true);
            S32 y = cardTop - 80;
            for (auto& r : rows)
            {
                text(r.first, cardL + 18, y, T.ink3, 0);
                text(clip(r.second, cardW - 150, 0), cardL + 132, y, T.ink, 0);
                y -= 22;
            }
            // context actions
            std::vector<std::pair<std::string, std::function<void()>>> acts;
            if (v->nativeKind == "astronomical") { acts.push_back({ "Descend to Montréal", [eng] { eng->post([eng] { eng->goAstronomical(true); }); } });
                                                    acts.push_back({ "Enter Montréal", [eng] { eng->post([eng] { eng->navigate(eng->startEntity); }); } }); }
            else if (v->nativeKind == "scientific") { acts.push_back({ v->nativeModel == "bcc-iron" ? "Microstructure" : "Crystal", [eng, cr = v->nativeModel == "bcc-iron"] { eng->post([eng, cr] { eng->goScientific(cr ? "microstructure" : "bcc-iron"); }); } });
                                                       acts.push_back({ "Back to component", [eng] { eng->post([eng] { eng->returnBack(); }); } }); }
            else acts.push_back({ "Back", [eng] { eng->post([eng] { eng->returnBack(); }); } });
            S32 bx = cardL + 18;
            for (auto& a2 : acts)
            {
                const S32 bw = width(a2.first, 0, true) + 24;
                roundRect(bx, cardTop - h + 34, bx + bw, cardTop - h + 10, 12, T.accentSoft);
                text(a2.first, bx + bw / 2, cardTop - h + 17, T.ink, 0, true, LLFontGL::HCENTER);
                m->hit(LLRect(bx, cardTop - h + 34, bx + bw, cardTop - h + 10), a2.second, "xx-" + a2.first);
                bx += bw + 8;
            }
        }
        // Genesis panel (top right)
        if (v->genesis.active)
        {
            const auto& g = v->genesis;
            const S32 gR = W - 24, gL = gR - (compact ? 320 : 380), gTop = H - 120, gH = 230;
            panel(gL, gTop, gR, gTop - gH, 16.f, true);
            text("GENESIS", gL + 18, gTop - 24, T.accent, 0, true);
            chip(g.reality.empty() ? "DESIGNED" : (g.reality == "hybrid" ? "HYBRID · DESIGNED" : "DESIGNED"), gR - 150, gTop - 10, T.warn);
            text(clip(g.title.empty() ? "Understanding your request…" : g.title, gR - gL - 36, 2, true), gL + 18, gTop - 52, T.ink, 2, true);
            static const char* phases[] = { "understanding", "landform", "layout", "streets", "architecture", "landscape", "life", "complete" };
            S32 px = gL + 18;
            const S32 pw = (gR - gL - 36) / 8;
            int cur = 0;
            for (int i = 0; i < 8; ++i) if (g.phase == phases[i]) cur = i;
            for (int i = 0; i < 8; ++i)
            {
                const LLColor4 col = i < cur ? T.accent : i == cur ? T.live : alpha(T.ink3, .35f);
                roundRect(px + i * pw, gTop - 70, px + (i + 1) * pw - 4, gTop - 76, 3, col);
            }
            text(g.phase.empty() ? "starting" : g.phase, gL + 18, gTop - 96, T.ink, 0, true);
            text(fmt(g.progress * 100, 0) + " %  ·  " + fmt(g.elapsed, 0) + " s", gR - 18, gTop - 96, T.ink3, 0, false, LLFontGL::RIGHT);
            auto res = [&](const char* k) { auto it = g.resident.find(k); return it == g.resident.end() ? 0 : it->second; };
            S32 y = gTop - 122;
            const std::string l1 = std::to_string(res("building") + res("asset")) + " buildings · " + std::to_string(res("grove")) + " groves · " + std::to_string(res("road")) + " roads";
            const std::string l2 = std::to_string(res("vehicle")) + " pods · " + std::to_string(res("robot")) + " robots · " + fmt(g.triangles / 1000.0, 0) + "k triangles";
            text(l1, gL + 18, y, T.ink, 0); y -= 20;
            text(l2, gL + 18, y, T.ink2, 0); y -= 20;
            text("frame p50 " + fmt(g.frameP50, 1) + " ms · p95 " + fmt(g.frameP95, 1) + " ms · " + std::to_string(g.refused) + " refused", gL + 18, y, T.ink3, 0); y -= 20;
            if (!g.model.empty()) { text(clip("spec by " + g.model + " in " + fmt(g.modelSeconds, 1) + " s · style " + (g.style.empty() ? "default" : g.style), gR - gL - 36, 0), gL + 18, y, T.ink3, 0); y -= 20; }
            if (!g.refined.empty()) { text(clip(g.refined, gR - gL - 36, 0), gL + 18, y, T.live, 0); y -= 20; }
            if (!g.error.empty()) text(clip(g.error, gR - gL - 36, 0), gL + 18, y, T.bad, 0);
        }
        // Journey line (bottom left, above the ask bar)
        if (!v->journey.empty())
        {
            std::string line;
            const size_t from = v->journey.size() > 6 ? v->journey.size() - 6 : 0;
            for (size_t i = from; i < v->journey.size(); ++i) line += (line.empty() ? "" : "  ›  ") + v->journey[i];
            text(clip(line, W - 120, 0), 24, 96, alpha(T.ink2, .9f), 0);
        }
        if (!v->style.empty()) chip("STYLE · " + v->style, W - 24 - width("STYLE · " + v->style, 0, true) - 16, 112, T.accent);
    }

    // ---- scripted UI (same code paths as the controls; for reproducible evidence). Runs after
    // the frame built its hit and focus lists, exactly like real input between frames.
    while (m->scriptNext < m->script.size() && t >= m->script[m->scriptNext].first && eng)
    {
        std::string cmd = m->script[m->scriptNext++].second, arg;
        auto sp = cmd.find(' ');
        if (sp != std::string::npos) { arg = cmd.substr(sp + 1); cmd = cmd.substr(0, sp); }
        LL_INFOS("FinalverseShell") << "script t=" << fmt(t, 1) << " " << cmd << " " << arg << LL_ENDL;
        if (cmd == "mode") m->mode = (Mode)llclamp(std::atoi(arg.c_str()) - 1, 0, 3);
        else if (cmd == "go") eng->post([eng, arg] {
            bj::object out; std::string why;
            if (LLUUID::validate(arg)) { eng->navigate(arg); return; }
            if (eng->call("SPATIAL.DISCOVER", { { "query", arg }, { "limit", 1 } }, out, why) == FV_SPATIAL_OK && !out.at("entities").as_array().empty())
                eng->navigate(str(out.at("entities").as_array()[0].as_object(), "entity"));
        });
        else if (cmd == "goc") { auto c2 = arg.find(' '); eng->post([eng, a = arg.substr(c2 + 1), cap = arg.substr(0, c2)] {
            bj::object out; std::string why;
            if (eng->call("SPATIAL.DISCOVER", { { "query", a }, { "limit", 1 } }, out, why) == FV_SPATIAL_OK && !out.at("entities").as_array().empty())
                eng->navigate(str(out.at("entities").as_array()[0].as_object(), "entity"), cap);
        }); }
        else if (cmd == "select" && v) { for (auto& c : v->ctx.children) if (c.label.find(arg) != std::string::npos) { eng->post([eng, c] { eng->select(c.entity, c.label); }); break; } }
        else if (cmd == "ask") { getChild<LLLineEditor>("ask")->setText(arg); getChild<LLLineEditor>("ask")->onCommit(); }
        else if (cmd == "back") eng->post([eng] { eng->back(false); });
        else if (cmd == "forward") eng->post([eng] { eng->back(true); });
        else if (cmd == "home") eng->post([eng] { eng->navigate(eng->startEntity); });
        else if (cmd == "orbit") eng->post([eng] { eng->orbit = !eng->orbit; eng->camera(0, 0, 1.0); });
        else if (cmd == "zoom") eng->post([eng, z = std::atof(arg.c_str())] { eng->camera(0, 0, z); });
        else if (cmd == "preset") eng->post([eng, arg] { eng->setPresentation(arg); });
        else if (cmd == "palette") { if (arg == "open") m->openPalette(); else m->closePalette(); }
        else if (cmd == "palettetype") { for (llwchar ch : utf8str_to_wstring(arg)) handleUnicodeCharHere(ch); }
        else if (cmd == "palettekey") { handleKeyHere(arg == "DOWN" ? KEY_DOWN : arg == "UP" ? KEY_UP : arg == "ESC" ? KEY_ESCAPE : KEY_RETURN, MASK_NONE); }
        else if (cmd == "section") { if (!m->openSections.erase(arg)) m->openSections.insert(arg); }
        else if (cmd == "cancelai") eng->aiCancel = true;
        else if (cmd == "create") eng->post([eng, arg] { eng->create(arg); });
        else if (cmd == "draftselect") eng->post([eng, arg] { eng->selectDraftObject(arg); });
        else if (cmd == "draftedit")
        {
            std::istringstream is(arg);
            std::string op, obj;
            is >> op >> obj;
            if (op == "move") { double dx = 0, dy = 0; is >> dx >> dy; eng->post([eng, obj, dx, dy] { eng->creationEdit("move", obj, bj::array{ dx, dy }); }); }
            else if (op == "rotate") { double d = 0; is >> d; eng->post([eng, obj, d] { eng->creationEdit("rotate", obj, d); }); }
            else if (op == "material") { std::string mm; is >> mm; eng->post([eng, obj, mm] { eng->creationEdit("material", obj, mm.empty() ? bj::value(nullptr) : bj::value(mm)); }); }
            else eng->post([eng, op, obj] { eng->creationEdit(op, obj, nullptr); });
        }
        else if (cmd == "undo") eng->post([eng] { eng->creationHistory(false); });
        else if (cmd == "redo") eng->post([eng] { eng->creationHistory(true); });
        else if (cmd == "publishdialog") m->confirmPublish = true;
        else if (cmd == "approve") { if (m->confirmPublish) { m->confirmPublish = false; eng->post([eng] { eng->publishApproved(); }); } }
        else if (cmd == "window")
        {
            S32 ww = 0, hh = 0;
            std::istringstream(arg) >> ww >> hh;
            if (ww > 400 && hh > 300 && gViewerWindow && gViewerWindow->getWindow()) gViewerWindow->getWindow()->setSize(LLCoordScreen(ww, hh));
            LL_INFOS("FinalverseShell") << "UITEST window " << ww << "x" << hh << LL_ENDL;
        }
        else if (cmd == "frame") eng->post([eng] { eng->frameSelected(); });
        else if (cmd == "pan") eng->post([eng, arg] { double dx = 0, dy = 0; std::istringstream(arg) >> dx >> dy; eng->panView(dx, dy); });
        else if (cmd == "pitch") eng->post([eng, z = std::atof(arg.c_str())] { eng->camera(0, z, 1.0); });
        else if (cmd == "yaw") eng->post([eng, z = std::atof(arg.c_str())] { eng->camera(z, 0, 1.0); });
        else if (cmd == "scale") gScale = llclamp(std::atoi(arg.c_str()), 0, 2);
        else if (cmd == "help") m->showHelp = arg == "on";
        else if (cmd == "focus") m->focusIndex = std::atoi(arg.c_str());
        else if (cmd == "move" && v) { auto p = arg.find(' '); std::string name = arg.substr(0, p); double dx = 0, dy = 0; std::istringstream(arg.substr(p + 1)) >> dx >> dy;
            for (auto& r : v->residents) if (r.name == name) eng->post([eng, id = r.id, dx, dy] { eng->move(id, dx, dy); }); }
        // UI input test hooks: these go through the floater's own event handlers (hit-testing,
        // focus traversal, keyboard routing, the line editor), not through the engine.
        else if (cmd == "key")
        {
            static const std::map<std::string, std::pair<KEY, MASK>> keys{
                { "TAB", { KEY_TAB, MASK_NONE } }, { "SHIFTTAB", { KEY_TAB, MASK_SHIFT } }, { "RETURN", { KEY_RETURN, MASK_NONE } },
                { "ESC", { KEY_ESCAPE, MASK_NONE } }, { "BACKSPACE", { KEY_BACKSPACE, MASK_NONE } }, { "ALTLEFT", { KEY_LEFT, MASK_ALT } },
                { "ALTRIGHT", { KEY_RIGHT, MASK_ALT } }, { "SLASH", { '/', MASK_NONE } }, { "HELP", { '/', MASK_SHIFT } }, { "H", { 'H', MASK_NONE } },
                { "1", { '1', MASK_NONE } }, { "2", { '2', MASK_NONE } }, { "3", { '3', MASK_NONE } }, { "4", { '4', MASK_NONE } },
                { "CTRLPLUS", { '=', MASK_CONTROL } }, { "CTRLMINUS", { '-', MASK_CONTROL } }, { "LEFT", { KEY_LEFT, MASK_NONE } } };
            if (auto it = keys.find(arg); it != keys.end())
            {
                auto* ask = getChild<LLLineEditor>("ask");
                // Same route as LLViewerWindow: the keyboard-focus view first, then up its parents.
                LLView* target = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus());
                if (!target || (target != this && !target->hasAncestor(this))) target = this;
                const bool handled = target->handleKey(it->second.first, it->second.second, false);
                LL_INFOS("FinalverseShell") << "UITEST key " << arg << " handled=" << handled << " focus=" << (m->focusIndex >= 0 && m->focusIndex < (int)m->focusOrder.size() ? m->focusOrder[m->focusIndex] : "-")
                                            << " ask_focused=" << ask->hasFocus() << " mode=" << modeName(m->mode) << LL_ENDL;
            }
        }
        else if (cmd == "click")
        {
            // click X% Y% of the window (origin bottom-left, as LLView coordinates)
            double fx = 0, fy = 0;
            std::istringstream(arg) >> fx >> fy;
            const S32 x = (S32)(fx * W / 100.0), y = (S32)(fy * H / 100.0);
            const bool down = handleMouseDown(x, y, MASK_NONE);
            handleMouseUp(x, y, MASK_NONE);
            LL_INFOS("FinalverseShell") << "UITEST click " << x << "," << y << " handled=" << down << " mode=" << modeName(m->mode) << LL_ENDL;
        }
        else if (cmd == "typeask")
        {
            auto* ask = getChild<LLLineEditor>("ask");
            for (llwchar ch : utf8str_to_wstring(arg)) ask->handleUnicodeCharHere(ch);
            LL_INFOS("FinalverseShell") << "UITEST typed " << arg.size() << " bytes ask_text=" << ask->getText() << LL_ENDL;
        }
        else if (cmd == "probe")
        {
            std::string order;
            for (auto& id : m->focusOrder) order += id + "|";
            LL_INFOS("FinalverseShell") << "UITEST probe focus_order=" << order << " focus=" << m->focusIndex << " context=" << (v ? v->ctx.label : "") << " selected=" << (v ? v->selectedLabel : "") << LL_ENDL;
        }
        else if (cmd == "perf")
        {
            const double span = std::max(1e-3, t - m->perfT0);
            if (arg != "reset")
                LL_INFOS("FinalverseShell") << "PERF " << arg << " seconds=" << fmt(span, 2) << " viewer_fps=" << fmt(m->perfDraws / span, 1)
                                            << " storm_presented_fps=" << fmt(m->perfPresented / span, 1) << " context=" << (v ? v->ctx.label : "")
                                            << " switch_ms=" << (fr ? fmt(fr->switchMs, 1) : std::string("-")) << " capture_ms=" << (fr ? fmt(fr->captureMs, 2) : std::string("-"))
                                            << " transport=" << (fr ? fr->transport : std::string("-")) << " frame=" << (fr ? std::to_string(fr->width) + "x" + std::to_string(fr->height) : std::string("-"))
                                            << " transport_ms_p50=" << pctl(std::vector<double>(m->transportMs.begin(), m->transportMs.end()), 50)
                                            << " transport_ms_p95=" << pctl(std::vector<double>(m->transportMs.begin(), m->transportMs.end()), 95)
                                            << " display_ms_p50=" << pctl(std::vector<double>(m->displayMs.begin(), m->displayMs.end()), 50)
                                            << " display_ms_p95=" << pctl(std::vector<double>(m->displayMs.begin(), m->displayMs.end()), 95)
                                            << " native_published=" << m->native.published() << " native_dropped=" << m->native.dropped() << LL_ENDL;
            m->displayMs.clear();
            m->transportMs.clear();
            m->perfDraws = m->perfPresented = 0;
            m->perfT0 = t;
        }
        else if (cmd == "native")
        {
            if (arg == "astro") eng->post([eng] { eng->goAstronomical(false); });
            else if (arg == "descend") eng->post([eng] { eng->goAstronomical(true); });
            else if (arg == "crystal") eng->post([eng] { eng->goScientific("bcc-iron"); });
            else if (arg == "micro") eng->post([eng] { eng->goScientific("microstructure"); });
            else if (arg == "graph") eng->post([eng] { eng->goGraph(); });
        }
        else if (cmd == "style") eng->post([eng, arg] { eng->setStyle(arg); });
        else if (cmd == "genesis") eng->post([eng, arg] { eng->genesisCreate(arg); });
        else if (cmd == "return") eng->post([eng] { eng->returnBack(); });
        else if (cmd == "campus") eng->post([eng, arg] { double y = .75, p = -.2, d = 480, fx = 620, fy = -230; std::istringstream(arg) >> y >> p >> d >> fx >> fy;
                                                        eng->userCamera = true; eng->camYaw = y; eng->camPitch = p; eng->camDistance = d;
                                                        eng->pan[0] = fx - eng->model.ctx.focus[0]; eng->pan[1] = fy - eng->model.ctx.focus[1];
                                                        try { eng->writeContext(eng->model.ctx, eng->currentManifest, "geometric", {}); } catch (...) {} });
        else if (cmd == "mark") LL_INFOS("FinalverseShell") << "MARK " << arg << " context=" << (v ? v->ctx.label : "") << " rev=" << (v ? v->live.revision : 0) << LL_ENDL;
    }

    LLFloater::draw();  // the AI line editor
}

void llfinalverseShellMaybeOpen()
{
    static bool opened = false;
    const char* s = std::getenv("FINALVERSE_SHELL");
    if (!opened && s && std::string(s) == "1")
    {
        opened = true;
        LLFloaterReg::showInstance("finalverse_shell");
    }
}
