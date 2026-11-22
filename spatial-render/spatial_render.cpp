// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
// FINALVERSE Viewer spatial renderer (Ω-XVIII): one Storm/Metal process that switches
// between admitted scene contexts (a geographic tile in metres, a building interior,
// an engineering assembly in millimetres...) on request from the Viewer, keeps live
// MutSea residents moving in the city context, and streams captured frames back.
//
//   finalverse-spatial-render MEDIA NEW_OUTPUT FRAMES visible|hidden [REGION_UUID]
//
// Control: owner-private OUTPUT/context.json (finalverse.storm-scene-context/0.1),
// residents: OUTPUT/scene-update.json (finalverse.scene-update/0.1, Ω-XV contract).
// Output (Ω-XIX, default): native shared-memory frames, finalverse.storm-frame-transport/1
// (frame_transport.h; discovery OUTPUT/transport.json). FINALVERSE_STORM_TRANSPORT=file
// keeps the Ω-XVIII fallback: frame.ppm + frame.json (finalverse.storm-view-frame/0.2);
// "both" writes both. No world token, no model, no persistent writer: presentation only.
#include "storm/fwd_scene.h"
#include "storm/render/renderer.h"
#include "storm/scene_context.h"
#include "bridge.h"
#include "frame_transport.h"
#include "shadows.h"
#include <SDL3/SDL.h>
#include <openssl/sha.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <chrono>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using living::Vec;
using Clock = std::chrono::steady_clock;

namespace {
void need(bool b, const std::string &s) {
  if (!b) throw std::runtime_error(s);
}
std::string read(const fs::path &p, size_t bound, bool private_file = false) {
  struct D {
    int fd;
    ~D() { if (fd >= 0) ::close(fd); }
  } f{::open(p.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK)};
  struct stat st{};
  need(f.fd >= 0 && ::fstat(f.fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && uint64_t(st.st_size) <= bound, "bounded regular file " + p.filename().string());
  if (private_file) need(st.st_uid == getuid() && (st.st_mode & 077) == 0, "private same-user control file");
  std::string raw(size_t(st.st_size), '\0');
  size_t got = 0;
  while (got < raw.size()) {
    auto n = ::read(f.fd, raw.data() + got, raw.size() - got);
    need(n > 0, "complete read");
    got += size_t(n);
  }
  return raw;
}
std::string sha(const std::string &s) {
  unsigned char d[32];
  SHA256(reinterpret_cast<const unsigned char *>(s.data()), s.size(), d);
  std::string o;
  for (auto b : d) { o += "0123456789abcdef"[b >> 4]; o += "0123456789abcdef"[b & 15]; }
  return o;
}
void save(const fs::path &p, const std::string &s) {
  auto part = p.string() + ".part";
  { std::ofstream f(part, std::ios::binary); f.write(s.data(), std::streamsize(s.size())); need(f.good(), "atomic write"); }
  fs::rename(part, p);
}

struct Control {
  std::string context, fwd, cas, definition, transition = "semantic", lighting = "daylight", target, presentation = "realistic";
  bool frameSelection = false;
  bool live = false, orbit = false, has_camera = false, reduced_motion = false;
  Vec center{}, extent{};
  double yaw = .6, pitch = -.45, distance = 0;
  std::set<std::string> highlight;
  struct Overlay { Vec position; double radius; };
  std::vector<Overlay> overlays;
  uint64_t sequence = 0;
};

Control control(const std::string &raw) {
  using namespace living;
  auto d = parse(raw);
  const std::string schema = text(d["schema"]);
  require(schema == "finalverse.storm-scene-context/0.1" || schema == "finalverse.storm-scene-context/0.2", "context schema");
  // 0.2 (Ω-XIX) adds presentation presets and frame-the-selection; 0.1 stays accepted.
  if (schema.ends_with("/0.1"))
    keys(d, {"schema", "sequence", "context", "fwd", "cas", "definition_sha256", "focus", "transition"},
         {"live", "camera", "highlight", "overlays", "lighting", "orbit", "target_resident", "reduced_motion"});
  else
    keys(d, {"schema", "sequence", "context", "fwd", "cas", "definition_sha256", "focus", "transition"},
         {"live", "camera", "highlight", "overlays", "lighting", "orbit", "target_resident", "reduced_motion", "presentation", "frame"});
  Control c;
  c.sequence = integer(d["sequence"]);
  c.context = text(d["context"]);
  require(uuid(c.context), "context must be a scene-manifest UUID");
  c.fwd = text(d["fwd"], 1024);
  c.cas = text(d["cas"], 1024);
  require(fs::path(c.fwd).is_absolute() && fs::path(c.cas).is_absolute(), "absolute FWD/CAS paths");
  c.definition = text(d["definition_sha256"]);
  require(hex(c.definition, 64), "definition digest");
  keys(d["focus"], {"center", "extent"});
  c.center = point(d["focus"]["center"], 1e9);
  c.extent = point(d["focus"]["extent"], 1e9);
  c.transition = text(d["transition"]);
  require(c.transition == "semantic" || c.transition == "geometric" || c.transition == "none", "transition kind");
  if (d.HasMember("live")) { require(d["live"].IsBool(), "live boolean"); c.live = d["live"].GetBool(); }
  if (d.HasMember("orbit")) { require(d["orbit"].IsBool(), "orbit boolean"); c.orbit = d["orbit"].GetBool(); }
  if (d.HasMember("reduced_motion")) { require(d["reduced_motion"].IsBool(), "reduced_motion boolean"); c.reduced_motion = d["reduced_motion"].GetBool(); }
  if (d.HasMember("lighting")) {
    c.lighting = text(d["lighting"]);
    require(c.lighting == "daylight" || c.lighting == "sunset" || c.lighting == "studio", "lighting preset");
  }
  if (d.HasMember("camera")) {
    keys(d["camera"], {"yaw", "pitch"}, {"distance_m"});
    c.yaw = number(d["camera"]["yaw"], -100, 100);
    c.pitch = number(d["camera"]["pitch"], -1.5, -.02);
    if (d["camera"].HasMember("distance_m")) c.distance = number(d["camera"]["distance_m"], .005, 5000);
    c.has_camera = true;
  }
  if (d.HasMember("highlight")) {
    require(d["highlight"].IsArray() && d["highlight"].Size() <= 64, "highlight list");
    for (auto &h : d["highlight"].GetArray()) { auto s = text(h); require(hex(s, 64), "highlight digest"); c.highlight.insert(s); }
  }
  if (d.HasMember("overlays")) {
    require(d["overlays"].IsArray() && d["overlays"].Size() <= 16, "overlay list");
    for (auto &o : d["overlays"].GetArray()) {
      keys(o, {"position", "radius"}, {"label"});
      c.overlays.push_back({point(o["position"], 1e9), number(o["radius"], .01, 500)});
    }
  }
  if (d.HasMember("presentation")) {
    c.presentation = text(d["presentation"]);
    require(c.presentation == "realistic" || c.presentation == "technical" || c.presentation == "inspection" || c.presentation == "xray" || c.presentation == "exploded", "presentation preset");
  }
  if (d.HasMember("frame")) {
    require(text(d["frame"]) == "selection" || text(d["frame"]) == "context", "frame target");
    c.frameSelection = text(d["frame"]) == "selection";
  }
  if (d.HasMember("target_resident") && !d["target_resident"].IsNull()) {
    c.target = text(d["target_resident"]);
    require(uuid(c.target), "target resident");
  }
  return c;
}

struct Body {
  std::string entity, sha, resident;
  Vec original{}, shown{}, target{};
  bool present = true;
};

std::map<std::string, Body> bindings(const std::string &raw) {
  rapidjson::Document d;
  d.Parse<rapidjson::kParseIterativeFlag>(raw.data(), raw.size());
  need(!d.HasParseError(), "admitted FWD document");
  std::map<std::string, Body> out;
  if (!d.HasMember("simulation_bindings")) return out;
  for (const auto &b : d["simulation_bindings"].GetArray()) {
    const auto &assets = d["assets"].GetArray();
    auto a = std::find_if(assets.begin(), assets.end(), [&](const auto &v) { return std::string(v["id"].GetString()) == b["asset_id"].GetString(); });
    need(a != assets.end(), "binding asset");
    bool resident = false;
    for (const auto &s : (*a)["source_ids"].GetArray()) resident = resident || std::string(s.GetString()).starts_with("resident-");
    if (!resident) continue;
    Body body;
    body.entity = b["entity_id"].GetString();
    body.sha = (*a)["sha256"].GetString();
    body.original = living::point((*a)["transform"]["translation"]);
    body.shown = body.target = body.original;
    out.emplace(body.entity, body);
  }
  return out;
}

void lighting(storm::Renderer &r, const std::string &p) {
  const float day[] = {-.35f, -.85f, -.28f}, eve[] = {-.85f, -.25f, .3f}, studio[] = {-.45f, -.8f, -.4f};
  const float dayC[] = {1.f, .96f, .88f}, eveC[] = {1.f, .65f, .35f}, studioC[] = {1.f, .98f, .95f};
  const float dayA[] = {.65f, .72f, .85f}, eveA[] = {.28f, .26f, .36f}, studioA[] = {.78f, .8f, .86f};
  const bool s = p == "sunset", st = p == "studio";
  r.set_sun_direction(st ? studio : s ? eve : day);
  r.set_sun_colour(st ? studioC : s ? eveC : dayC);
  r.set_sun_intensity(st ? 3.4f : s ? 2.f : 3.f);
  r.set_ambient(st ? studioA : s ? eveA : dayA);
}

std::vector<storm::RenderObjectId> sky(storm::Renderer &r) {
  constexpr unsigned rings = 48, segments = 96;
  std::vector<storm::RenderObjectId> ids;
  for (unsigned band = 0; band < rings; band++) {
    std::vector<float> v;
    std::vector<uint32_t> idx;
    const double a = -M_PI / 2 + M_PI * band / rings, b = -M_PI / 2 + M_PI * (band + 1) / rings;
    for (unsigned j = 0; j <= segments; j++)
      for (double e : {a, b}) {
        const double ang = 2 * M_PI * j / segments;
        v.insert(v.end(), {float(3500 * std::cos(e) * std::sin(ang)), float(3500 * std::sin(e)), float(3500 * std::cos(e) * std::cos(ang))});
      }
    for (unsigned j = 0; j < segments; j++) { uint32_t n = j * 2; idx.insert(idx.end(), {n, n + 1, n + 2, n + 1, n + 3, n + 2}); }
    const float t = float(band) / rings, c[] = {.20f + .28f * (1 - t), .38f + .25f * (1 - t), .63f + .16f * (1 - t)};
    ids.push_back(r.add_surface(v.data(), v.size() / 3, idx.data(), idx.size(), c, true, 1, 0));
  }
  return ids;
}

// Ω-XIX studio backdrop for engineering contexts: a graded dark dome (presentation only).
std::vector<storm::RenderObjectId> studio_dome(storm::Renderer &r) {
  constexpr unsigned rings = 40, segments = 96;
  std::vector<storm::RenderObjectId> ids;
  for (unsigned band = 0; band < rings; band++) {
    std::vector<float> v;
    std::vector<uint32_t> idx;
    const double a = -M_PI / 2 + M_PI * band / rings, b = -M_PI / 2 + M_PI * (band + 1) / rings;
    for (unsigned j = 0; j <= segments; j++)
      for (double e : {a, b}) {
        const double ang = 2 * M_PI * j / segments;
        v.insert(v.end(), {float(1500 * std::cos(e) * std::sin(ang)), float(1500 * std::sin(e)), float(1500 * std::cos(e) * std::cos(ang))});
      }
    for (unsigned j = 0; j < segments; j++) { uint32_t n = j * 2; idx.insert(idx.end(), {n, n + 2, n + 1, n + 1, n + 2, n + 3}); }
    const float t = float(band) / rings;  // 0 = nadir, 1 = zenith
    const float g = t < .5f ? .035f + .05f * t : .06f + .10f * (t - .5f);
    const float c[] = {g, g * 1.04f, g * 1.16f};
    ids.push_back(r.add_surface(v.data(), v.size() / 3, idx.data(), idx.size(), c, true, 1, 0));
  }
  return ids;
}

// Ω-XIX presentation stage under an engineering model (receives light and reflections;
// never part of the publication, never a measurement).
storm::RenderObjectId stage(storm::Renderer &r, double radius) {
  std::vector<float> v{0, 0, 0};
  std::vector<uint32_t> idx;
  const unsigned seg = 128;
  for (unsigned i = 0; i <= seg; i++) {
    const double a = 2 * M_PI * i / seg;
    v.insert(v.end(), {float(radius * std::cos(a)), 0.f, float(radius * std::sin(a))});
  }
  for (unsigned i = 1; i <= seg; i++) idx.insert(idx.end(), {0u, i + 1, i});
  const float colour[] = {.045f, .048f, .056f};
  return r.add_surface(v.data(), v.size() / 3, idx.data(), idx.size(), colour, false, .62f, 0);
}

// A presentation-only beacon (ring + pillar) for an overlay marker, render-space metres.
storm::RenderObjectId beacon(storm::Renderer &r, double radius) {
  std::vector<float> v;
  std::vector<uint32_t> idx;
  const unsigned seg = 48;
  const float R = float(radius), w = float(std::max(radius * .12, .2)), h = float(std::max(radius * 2.5, 6.0));
  auto quad = [&](float x0, float y0, float z0, float x1, float y1, float z1, float x2, float y2, float z2, float x3, float y3, float z3) {
    uint32_t n = uint32_t(v.size() / 3);
    v.insert(v.end(), {x0, y0, z0, x1, y1, z1, x2, y2, z2, x3, y3, z3});
    idx.insert(idx.end(), {n, n + 1, n + 2, n, n + 2, n + 3});
  };
  for (unsigned i = 0; i < seg; i++) {
    const float a0 = float(2 * M_PI * i / seg), a1 = float(2 * M_PI * (i + 1) / seg);
    const float c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
    quad(R * c0, .3f, R * s0, R * c1, .3f, R * s1, (R - w) * c1, .3f, (R - w) * s1, (R - w) * c0, .3f, (R - w) * s0);   // ground ring
    const float p = w * .8f;
    quad(p * c0, 0, p * s0, p * c1, 0, p * s1, p * c1, h, p * s1, p * c0, h, p * s0);                                   // pillar
  }
  const float colour[] = {.32f, .62f, 1.f};
  return r.add_surface(v.data(), v.size() / 3, idx.data(), idx.size(), colour, true, .35f, 0);
}

double geographic_far(const std::string &axes) { return axes == "x,y,z" ? 50.0 : 6000.0; }

volatile std::sig_atomic_t gStop = 0;
void on_signal(int) { gStop = 1; }

using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
void str(Writer &w, const char *k, const std::string &s) { w.Key(k); w.String(s.c_str()); }
}  // namespace

int main(int argc, char **argv) {
  try {
    umask(0077);
    need(argc == 5 || argc == 6, "usage: finalverse-spatial-render MEDIA NEW_OUTPUT FRAMES visible|hidden [REGION_UUID]");
    const int frames = std::stoi(argv[3]);
    need(frames >= 2 && frames <= 2000000, "frame budget");
    fs::path out = argv[2];
    need(!fs::exists(out), "new private output directory required");
    fs::create_directories(out);
    fs::permissions(out, fs::perms::owner_all);
    const std::string region = argc == 6 ? argv[5] : "";
    need(region.empty() || living::uuid(region), "region UUID");
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);
    const std::string transportMode = std::getenv("FINALVERSE_STORM_TRANSPORT") ? std::getenv("FINALVERSE_STORM_TRANSPORT") : "shm";
    need(transportMode == "shm" || transportMode == "file" || transportMode == "both", "FINALVERSE_STORM_TRANSPORT must be shm, file or both");
    const bool useShm = transportMode != "file", useFile = transportMode != "shm";
    uint32_t captureW = 1280, captureH = 720;
    if (const char *size = std::getenv("FINALVERSE_STORM_SIZE")) {
      need(std::sscanf(size, "%ux%u", &captureW, &captureH) == 2 && captureW >= 320 && captureH >= 180 && captureW <= fvft::kMaxWidth && captureH <= fvft::kMaxHeight, "FINALVERSE_STORM_SIZE WxH");
      need(!useFile || (captureW == 1280 && captureH == 720), "the file fallback carries 1280x720 only");
    }
    const int msaa = std::getenv("FINALVERSE_STORM_MSAA") ? std::atoi(std::getenv("FINALVERSE_STORM_MSAA")) : 1;
    need(msaa == 1 || msaa == 2 || msaa == 4 || msaa == 8, "FINALVERSE_STORM_MSAA must be 1, 2, 4 or 8");
    storm::RendererConfig cfg;
    cfg.title = "FINALVERSE · Storm spatial renderer";
    cfg.media_root = argv[1];
    cfg.capture_width = captureW;
    cfg.capture_height = captureH;
    cfg.capture_samples = uint8_t(msaa);
    const bool hidden = std::string(argv[4]) == "hidden";
    if (hidden) cfg.vsync = false;  // nothing is presented on screen; pacing is the loop's own
    storm::Renderer renderer;
    need(renderer.initialize(cfg), renderer.error());
    if (hidden) {
      int n = 0;
      SDL_Window **ws = SDL_GetWindows(&n);
      for (int i = 0; i < n; i++) SDL_HideWindow(ws[i]);
      SDL_free(ws);
      renderer.set_window_presentation(false);  // draw each frame once, into the capture target
    }
    fvft::Producer transport;
    if (useShm) {
      std::string e;
      need(transport.create(captureW, captureH, e), e);
      save(out / "transport.json", transport.discovery());
    }
    storm::SceneContexts contexts(renderer);
    const char *shadowSetting = std::getenv("FINALVERSE_STORM_SHADOWS");
    living::Shadows shadows(shadowSetting && std::string(shadowSetting) == "1");
    std::vector<storm::RenderObjectId> skyIds, beacons;
    storm::RenderObjectId stageId{};
    std::map<std::string, Vec> explode;  // Ω-XIX exploded-view offsets per surface (render space)
    std::string presentationActive = "realistic";
    bool frameOverride = false;
    Vec frameFocus{};
    double frameDistance = 0;
    double stageY = 0, stageX = 0, stageZ = 0;
    std::string environmentFor;
    const bool fidelity = !(std::getenv("FINALVERSE_STORM_FIDELITY") && std::string(std::getenv("FINALVERSE_STORM_FIDELITY")) == "0");
    std::vector<Vec> beaconPositions;
    Control active;
    bool haveContext = false;
    std::map<std::string, Body> bodies;
    living::Gate gate;
    std::string previousControl, previousUpdate, updateHash, sync = "none", lastError;
    uint64_t sourceRevision = 0, captures = 0, switches = 0, refusals = 0;
    double yaw = .6, pitch = -.45, distance = 50, transitionStart = 0;
    Vec focus{}, origin{};
    double unit = 1;
    double desiredYaw = yaw, desiredPitch = pitch, desiredDistance = distance;
    Vec desiredFocus{};
    double lastSwitchMs = 0, lastCaptureMs = 0;
    const auto started = Clock::now();
    std::vector<uint8_t> rgba;
    auto previous = Clock::now();
    const int captureEvery = std::max(1, std::atoi(std::getenv("FINALVERSE_STORM_CAPTURE_EVERY") ? std::getenv("FINALVERSE_STORM_CAPTURE_EVERY") : useShm ? "1" : "2"));
    uint64_t published = 0, dropped = 0;
    for (int i = 0; i < frames && !gStop; i++) {
      const auto t0 = Clock::now();
      const uint64_t t0ns = fvft::now_ns();
      const double dt = std::clamp(std::chrono::duration<double>(t0 - previous).count(), 1. / 240., .1);
      previous = t0;
      SDL_Event ev;
      while (SDL_PollEvent(&ev)) if (ev.type == SDL_EVENT_QUIT) i = frames;
      if (useShm) transport.heartbeat();
      // ---------------------------------------------------------- control
      try {
        if (fs::exists(out / "context.json")) {
          auto raw = read(out / "context.json", 65536, true);
          if (raw != previousControl) {
            previousControl = raw;
            Control c = control(raw);
            const bool newContext = !haveContext || c.context != active.context;
            if (newContext) {
              const auto s0 = Clock::now();
              const auto definition = read(c.fwd, 1 << 20);
              need(sha(definition) == c.definition, "FWD bytes do not match the scene manifest's world_definition digest");
              std::string e;
              need(contexts.load(c.context, c.fwd, c.cas, e), e);
              need(contexts.activate(c.context, e), e);
              for (auto id : skyIds) renderer.remove_object(id);
              skyIds.clear();
              if (!(stageId == storm::RenderObjectId{})) { renderer.remove_object(stageId); stageId = {}; }
              const auto *info = contexts.info(c.context);
              unit = info->unit_to_metre;
              origin = info->origin;
              const bool geographic = info->axes != "x,y,z";
              if (geographic) skyIds = sky(renderer);
              else if (fidelity) {
                // Ω-XIX: studio dome + stage under the model, framed from its declared bounds.
                skyIds = studio_dome(renderer);
                double span = 0;
                for (int k = 0; k < 2; k++) span = std::max(span, (info->maximum[k] - info->minimum[k]) * unit);
                stageId = stage(renderer, std::max(span * 1.6, .02));
                stageY = (info->minimum[2] - origin[2]) * unit - std::max(span * .004, .00005);
                stageX = ((info->minimum[0] + info->maximum[0]) / 2 - origin[0]) * unit;
                stageZ = ((info->minimum[1] + info->maximum[1]) / 2 - origin[1]) * unit;
              }
              // Without image-based lighting fully metallic PBR reads almost black, so the
              // Ω-XVIII path capped metallic (display only). With the Ω-XIX environment map the
              // authored GLB material is shown as authored.
              if (!geographic && !fidelity)
                for (auto &surface : *contexts.surfaces())
                  if (surface.mesh.metallic > .35f) {
                    surface.mesh.metallic = .35f;
                    surface.mesh.roughness = std::max(surface.mesh.roughness, .4f);
                    renderer.set_material(surface.id, surface.mesh.colour.data(), surface.mesh.roughness, surface.mesh.metallic);
                  }
              bodies = c.live ? bindings(definition) : std::map<std::string, Body>{};
              gate = living::Gate{};
              switches++;
              lastSwitchMs = std::chrono::duration<double, std::milli>(Clock::now() - s0).count();
            }
            // Overlays (presentation-only markers) are rebuilt on every control change.
            for (auto id : beacons) renderer.remove_object(id);
            beacons.clear();
            beaconPositions.clear();
            for (const auto &o : c.overlays) {
              beacons.push_back(beacon(renderer, o.radius * unit));
              beaconPositions.push_back(o.position);
            }
            const float tint[] = {.25f, .7f, 1.f};
            contexts.highlight(c.highlight, tint);
            // Ω-XIX presentation presets (display only; authored materials are restored by
            // highlight() above on every control change, so presets never accumulate).
            const bool engineering = contexts.info(c.context)->axes == "x,y,z";
            const std::string presentation = engineering ? c.presentation : "realistic";
            explode.clear();
            if (auto *surfaces = contexts.surfaces()) {
              Vec lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30}, mid{};
              std::vector<Vec> centres;
              for (auto &sf : *surfaces) {
                Vec a{1e30, 1e30, 1e30}, b{-1e30, -1e30, -1e30};
                for (size_t k = 0; k + 2 < sf.mesh.positions.size(); k += 3)
                  for (int j = 0; j < 3; j++) { a[j] = std::min(a[j], double(sf.mesh.positions[k + j])); b[j] = std::max(b[j], double(sf.mesh.positions[k + j])); }
                Vec cc{(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2};
                centres.push_back(cc);
                for (int j = 0; j < 3; j++) { lo[j] = std::min(lo[j], a[j]); hi[j] = std::max(hi[j], b[j]); }
              }
              for (int j = 0; j < 3; j++) mid[j] = (lo[j] + hi[j]) / 2;
              size_t si = 0;
              for (auto &sf : *surfaces) {
                const Vec cc = centres[si++];
                if (presentation == "technical" && !sf.highlighted) {
                  const float neutral[] = {.74f, .76f, .79f};
                  renderer.set_material(sf.id, neutral, .55f, 0.f);
                } else if (presentation == "inspection" && !sf.highlighted && !c.highlight.empty()) {
                  const float dim[] = {.16f, .17f, .19f};
                  renderer.set_material(sf.id, dim, .85f, 0.f);
                }
                renderer.set_opacity(sf.id, presentation == "xray" && !sf.highlighted ? .22f : 1.f);
                if (presentation == "exploded") {
                  // Separation along each part's offset from the assembly centre (render space;
                  // parts sharing the centre, e.g. concentric rings, stay in place).
                  Vec off{};
                  for (int j = 0; j < 3; j++) off[j] = (cc[j] - mid[j]) * .55;
                  explode[sf.mesh.asset_sha256 + "#" + std::to_string(si)] = off;
                }
              }
              if (c.frameSelection && !c.highlight.empty()) {
                // Frame the highlighted parts: their render-space bounds become the focus.
                Vec a{1e30, 1e30, 1e30}, b{-1e30, -1e30, -1e30};
                bool any = false;
                for (auto &sf : *surfaces)
                  if (sf.highlighted)
                    for (size_t k = 0; k + 2 < sf.mesh.positions.size(); k += 3) {
                      any = true;
                      for (int j = 0; j < 3; j++) { a[j] = std::min(a[j], double(sf.mesh.positions[k + j])); b[j] = std::max(b[j], double(sf.mesh.positions[k + j])); }
                    }
                if (any) {
                  // render (x, up, -north) -> declared-axis metres relative to the origin
                  frameOverride = true;
                  frameFocus = {(a[0] + b[0]) / 2, -(a[2] + b[2]) / 2, (a[1] + b[1]) / 2};
                  frameDistance = std::max({b[0] - a[0], b[1] - a[1], b[2] - a[2]}) * 1.9;
                }
              } else frameOverride = false;
            }
            presentationActive = presentation;
            const std::string preset = c.lighting == "daylight" && contexts.info(c.context)->axes == "x,y,z" ? "studio" : c.lighting;
            lighting(renderer, preset);
            if (fidelity && preset != environmentFor) {
              const auto e0 = Clock::now();
              const bool ok = renderer.set_environment(preset == "studio" ? storm::Renderer::Environment::Studio : preset == "sunset" ? storm::Renderer::Environment::Sunset : storm::Renderer::Environment::Daylight);
              std::cerr << "FINALVERSE spatial render: environment " << preset << (ok ? " ready in " : " refused: ") << (ok ? std::to_string(std::chrono::duration<double, std::milli>(Clock::now() - e0).count()) + " ms" : renderer.error()) << "\n";
              environmentFor = preset;
            }
            // Camera: focus in declared units -> metres relative to the context origin.
            for (int k = 0; k < 3; k++) desiredFocus[k] = (c.center[k] - origin[k]) * unit;
            double extentM = 0;
            for (auto e : c.extent) extentM = std::max(extentM, e * unit);
            desiredDistance = c.has_camera && c.distance > 0 ? c.distance : std::max(extentM * 1.7, .06);
            if (frameOverride) {
              desiredFocus = frameFocus;
              if (!(c.has_camera && c.distance > 0)) desiredDistance = std::max(frameDistance, .02);
            }
            desiredYaw = c.yaw;
            desiredPitch = c.pitch;
            if (newContext && c.transition == "semantic") {
              // Semantic transition: start wide and settle, so the new dataset reads as a
              // separate context rather than a continuation of the previous geometry.
              focus = desiredFocus;
              distance = desiredDistance * 3.2;
              pitch = std::max(desiredPitch - .25, -1.35);
              yaw = desiredYaw - .55;
              transitionStart = std::chrono::duration<double>(Clock::now() - started).count();
            } else if (newContext || c.reduced_motion) {
              // Reduced motion: every camera change (geometric zoom, keys, reset) is applied
              // at once instead of eased.
              focus = desiredFocus;
              distance = desiredDistance;
              pitch = desiredPitch;
              yaw = desiredYaw;
            }
            renderer.set_clip_distances(float(std::max(desiredDistance / 4000., .0004)), float(std::max(desiredDistance * 60., fidelity ? 3200. : geographic_far(contexts.info(c.context)->axes))));
            active = c;
            haveContext = true;
            lastError.clear();
          }
        }
      } catch (const std::exception &e) {
        refusals++;
        lastError = e.what();
      }
      // ---------------------------------------------------------- live residents (city context)
      if (haveContext && active.live && !region.empty()) {
        try {
          if (fs::exists(out / "scene-update.json")) {
            auto raw = read(out / "scene-update.json", 65536, true);
            if (raw != previousUpdate) {
              previousUpdate = raw;
              auto u = living::update(raw);
              need(u.definition == active.definition && u.region == region, "scene update scope");
              if (!gate.initialized) { gate.world = u.world; gate.region = region; gate.definition = active.definition; }
              if (gate.admit(u, raw)) {
                for (auto &[k, b] : bodies) { (void)k; b.present = false; }
                for (const auto &r : u.residents) {
                  auto b = bodies.find(r.entity);
                  if (b == bodies.end()) continue;
                  b->second.present = true;
                  b->second.resident = r.resident;
                  b->second.target = r.position;
                }
                sourceRevision = u.revision;
                updateHash = sha(raw);
                sync = "committed";
              }
            }
          }
        } catch (const std::exception &e) {
          refusals++;
          lastError = e.what();
          sync = "refused";
        }
      }
      // ---------------------------------------------------------- motion
      if (active.orbit) desiredYaw = std::remainder(desiredYaw + dt * .12, 2 * M_PI);
      const double now = std::chrono::duration<double>(Clock::now() - started).count();
      const double settle = active.transition == "semantic" ? std::clamp((now - transitionStart) / 1.2, 0., 1.) : 1.;
      const double ca = 1 - std::exp(-dt * (active.transition == "semantic" ? 3.2 + 6 * settle : 7.5)), pa = 1 - std::exp(-dt * 16);
      yaw += std::remainder(desiredYaw - yaw, 2 * M_PI) * ca;
      pitch += (desiredPitch - pitch) * ca;
      distance += (desiredDistance - distance) * ca;
      bool interpolating = std::abs(desiredDistance - distance) > desiredDistance * .01;
      Vec targetFocus = desiredFocus;
      for (auto &[k, b] : bodies) {
        (void)k;
        for (int j = 0; j < 3; j++) {
          b.shown[j] += (b.target[j] - b.shown[j]) * pa;
          if (std::abs(b.target[j] - b.shown[j]) > .001) interpolating = true;
        }
        if (!active.target.empty() && b.present && b.resident == active.target)
          for (int j = 0; j < 3; j++) targetFocus[j] = (b.shown[j] - origin[j]) * unit;
      }
      for (int j = 0; j < 3; j++) focus[j] += (targetFocus[j] - focus[j]) * ca;
      // Presentation rebasing: render space = (x, up, -north) relative to the focus.
      if (auto *surfaces = contexts.surfaces()) {
        size_t surfaceIndex = 0;
        for (auto &s : *surfaces) {
          ++surfaceIndex;
          Vec delta{};
          bool hidden = false;
          Vec ex{};
          if (!explode.empty())
            if (auto it = explode.find(s.mesh.asset_sha256 + "#" + std::to_string(surfaceIndex)); it != explode.end()) ex = it->second;
          for (auto &[k, b] : bodies) {
            (void)k;
            if (b.sha == s.mesh.asset_sha256) {
              if (!b.present) hidden = true;
              for (int j = 0; j < 3; j++) delta[j] = (b.shown[j] - b.original[j]) * unit;
            }
          }
          const float off[] = {float(delta[0] - focus[0] + ex[0]), float(delta[2] - focus[2] + ex[1]), float(-(delta[1] - focus[1]) + ex[2])};
          const float sc[] = {hidden ? 1e-6f : 1.f, hidden ? 1e-6f : 1.f, hidden ? 1e-6f : 1.f};
          renderer.set_transform(s.id, off, sc);
        }
      }
      for (size_t b = 0; b < beacons.size(); b++) {
        const Vec p = beaconPositions[b];
        const float off[] = {float((p[0] - origin[0]) * unit - focus[0]), float((p[2] - origin[2]) * unit - focus[2]), float(-((p[1] - origin[1]) * unit - focus[1]))};
        const float pulse = float(1 + .06 * std::sin(now * 3));
        const float sc[] = {pulse, 1, pulse};
        renderer.set_transform(beacons[b], off, sc);
      }
      if (!(stageId == storm::RenderObjectId{})) {
        const float off[] = {float(stageX - focus[0]), float(stageY - focus[2]), float(-(stageZ - focus[1]))};
        const float one[] = {1, 1, 1};
        renderer.set_transform(stageId, off, one);
      }
      renderer.set_camera_orbit(float(yaw), float(pitch), float(distance));
      need(renderer.render_frame(dt), renderer.error());
      shadows.after_first_frame();
      // ---------------------------------------------------------- capture
      if (haveContext && (i % captureEvery == 0)) {
        const auto *info = contexts.info(active.context);
        if (useShm) {
          // Native path: GPU readback straight into the transport's back slot, then publish.
          const auto c0 = Clock::now();
          uint32_t fw = 0, fh = 0;
          need(renderer.read_frame_into(transport.back_pixels(), transport.stride_for(captureW), captureW, captureH, fw, fh), renderer.error());
          lastCaptureMs = std::chrono::duration<double, std::milli>(Clock::now() - c0).count();
          fvft::SlotMeta &m = transport.back_meta();
          m.frame_index = renderer.stats().frame_index;
          m.render_start_ns = t0ns;
          m.control_sequence = active.sequence;
          m.source_revision = sourceRevision;
          m.triangles = renderer.triangle_count();
          m.context_switches = switches;
          m.refusals = refusals;
          m.width = fw; m.height = fh; m.stride = transport.stride_for(fw); m.objects = renderer.object_count();
          m.flags = (interpolating ? 0u : uint32_t(fvft::SETTLED)) | (active.live ? uint32_t(fvft::LIVE_WORLD) : 0u);
          m.capture_ms = float(lastCaptureMs); m.switch_ms = float(lastSwitchMs); m.cpu_submit_ms = float(renderer.stats().frame_ms); m.gpu_wait_ms = 0;
          m.unit_to_metre = unit; m.camera_distance_m = distance;
          fvft::copy_text(m.context, sizeof m.context, active.context);
          fvft::copy_text(m.definition_sha256, sizeof m.definition_sha256, active.definition);
          fvft::copy_text(m.frame_id, sizeof m.frame_id, info ? info->frame_id : "");
          fvft::copy_text(m.units, sizeof m.units, info ? info->units : "");
          fvft::copy_text(m.transition, sizeof m.transition, active.transition);
          fvft::copy_text(m.synchronization, sizeof m.synchronization, active.live ? sync : "static");
          fvft::copy_text(m.backend, sizeof m.backend, "OGRE Metal");
          fvft::copy_text(m.last_error, sizeof m.last_error, lastError);
          if (transport.publish()) published++; else { published++; dropped++; }
          captures++;
        }
        if (useFile) {
        const auto c0 = Clock::now();
        auto part = out / "frame.part.ppm";
        uint32_t fw = 0, fh = 0;
        need(renderer.read_frame(rgba, fw, fh), renderer.error());
        std::string image = "P6\n" + std::to_string(fw) + " " + std::to_string(fh) + "\n255\n";
        const size_t header = image.size();
        image.resize(header + size_t(fw) * fh * 3);
        for (size_t p = 0; p < size_t(fw) * fh; p++) std::memcpy(&image[header + p * 3], &rgba[p * 4], 3);
        save(part, image);
        const auto pixels = sha(image);
        fs::rename(part, out / "frame.ppm");
        lastCaptureMs = std::chrono::duration<double, std::milli>(Clock::now() - c0).count();
        rapidjson::StringBuffer buf;
        Writer w(buf);
        w.StartObject();
        str(w, "schema", "finalverse.storm-view-frame/0.2");
        str(w, "context", active.context);
        str(w, "definition_sha256", active.definition);
        str(w, "pixels_sha256", pixels);
        w.Key("control_sequence"); w.Uint64(active.sequence);
        w.Key("frame"); w.Uint64(renderer.stats().frame_index);
        w.Key("triangles"); w.Uint64(renderer.triangle_count());
        w.Key("objects"); w.Uint(renderer.object_count());
        str(w, "frame_id", info ? info->frame_id : "");
        str(w, "units", info ? info->units : "");
        w.Key("unit_to_metre"); w.Double(unit);
        str(w, "transition", active.transition);
        w.Key("transition_settled"); w.Bool(!interpolating);
        w.Key("camera_distance_m"); w.Double(distance);
        w.Key("source_revision"); w.Uint64(sourceRevision);
        str(w, "synchronization", active.live ? sync : "static");
        w.Key("live"); w.Bool(active.live);
        w.Key("highlighted"); w.Uint64(active.highlight.size());
        w.Key("context_switches"); w.Uint64(switches);
        w.Key("last_switch_ms"); w.Double(lastSwitchMs);
        w.Key("capture_ms"); w.Double(lastCaptureMs);
        w.Key("cpu_submit_ms"); w.Double(renderer.stats().frame_ms);
        w.Key("refusals"); w.Uint64(refusals);
        str(w, "last_error", lastError);
        str(w, "backend", "OGRE Metal");
        w.Key("presentation_only"); w.Bool(true);
        w.EndObject();
        save(out / "frame.json", {buf.GetString(), buf.GetSize()});
        if (!useShm) captures++;
        }
      }
      std::this_thread::sleep_until(t0 + std::chrono::nanoseconds(16666667));
    }
    contexts.release();
    transport.close();  // state CLOSED, shared-memory name unlinked
    std::cout << "{\"captures\":" << captures << ",\"switches\":" << switches << ",\"refusals\":" << refusals << ",\"transport\":\"" << transportMode
              << "\",\"published\":" << published << ",\"dropped_unconsumed\":" << dropped << ",\"world_mutated\":false}\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FINALVERSE spatial render: " << e.what() << "\n";
    return 1;
  }
}
