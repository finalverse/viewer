// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
// Read-only CPU frame stream from a frozen Storm API; no world credentials.
#include "storm/fwd_scene.h"
#include "storm/render/renderer.h"
#include "bridge.h"
#include "shadows.h"
#include <SDL3/SDL.h>
#include <openssl/sha.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
namespace fs = std::filesystem;
using living::Vec;
void need(bool b, const std::string& s)
{
    if (!b)
        throw std::runtime_error(s);
}
std::string read(const fs::path& p, size_t bound, bool private_file = false)
{
    struct Descriptor
    {
        int fd;
        ~Descriptor()
        {
            if (fd >= 0)
                ::close(fd);
        }
    } file{ ::open(p.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK) };
    struct stat st{};
    need(file.fd >= 0 && ::fstat(file.fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && static_cast<uint64_t>(st.st_size) <= bound,
         "bounded regular file");
    if (private_file)
        need(st.st_uid == getuid() && (st.st_mode & 077) == 0, "private same-user input");
    std::string raw(static_cast<size_t>(st.st_size), '\0');
    size_t      got = 0;
    while (got < raw.size())
    {
        auto n = ::read(file.fd, raw.data() + got, raw.size() - got);
        need(n > 0, "complete bounded file read");
        got += static_cast<size_t>(n);
    }
    char extra;
    need(::read(file.fd, &extra, 1) == 0, "input grew beyond budget");
    return raw;
}
std::string sha(const std::string& s)
{
    unsigned char d[32];
    SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), d);
    std::string out;
    for (auto b : d)
    {
        out += "0123456789abcdef"[b >> 4];
        out += "0123456789abcdef"[b & 15];
    }
    return out;
}
void save(const fs::path& p, const std::string& s)
{
    auto part = p.string() + ".part";
    {
        std::ofstream f(part, std::ios::binary);
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
        need(f.good(), "atomic output write");
    }
    fs::rename(part, p);
}
struct Body
{
    std::string entity, sha, resident;
    Vec         original{}, shown{}, target{};
    bool        present = true;
};
struct Surface
{
    storm::FwdPrimitive   mesh;
    storm::RenderObjectId id{};
    std::string           body;
};
using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
void vector(Writer& w, const Vec& p)
{
    w.StartArray();
    for (double v : p)
        w.Double(v);
    w.EndArray();
}
void str(Writer& w, const char* key, const std::string& s)
{
    w.Key(key);
    w.String(s.c_str());
}
std::map<std::string, Body> bindings(const std::string& raw)
{
    rapidjson::Document d;
    d.Parse<rapidjson::kParseIterativeFlag>(raw.data(), raw.size());
    need(!d.HasParseError(), "admitted FWD document");
    std::map<std::string, unsigned> copies;
    for (const auto& a : d["assets"].GetArray())
        copies[a["sha256"].GetString()]++;
    std::map<std::string, Body> out;
    if (!d.HasMember("simulation_bindings"))
        return out;
    for (const auto& b : d["simulation_bindings"].GetArray())
    {
        const auto& assets = d["assets"].GetArray();
        auto        a      = std::find_if(assets.begin(), assets.end(),
                                          [&](const auto& v) { return std::string(v["id"].GetString()) == b["asset_id"].GetString(); });
        need(a != assets.end(), "binding asset");
        bool resident = false;
        for (const auto& s : (*a)["source_ids"].GetArray())
            if (std::string(s.GetString()).starts_with("resident-"))
                resident = true;
        if (!resident)
            continue;
        Body body;
        body.entity   = b["entity_id"].GetString();
        body.sha      = (*a)["sha256"].GetString();
        body.original = living::point((*a)["transform"]["translation"]);
        body.shown = body.target = body.original;
        need(copies[body.sha] == 1 && out.emplace(body.entity, body).second, "ambiguous resident asset/entity binding");
    }
    need(out.size() <= 16, "body budget");
    return out;
}
void lighting(storm::Renderer& r, const std::string& preset)
{
    const bool  sunset         = preset == "sunset";
    const float dayDirection[] = { -.35f, -.85f, -.28f }, eveningDirection[] = { -.85f, -.25f, .3f };
    const float dayColour[] = { 1.f, .96f, .88f }, eveningColour[] = { 1.f, .65f, .35f };
    const float dayAmbient[] = { .65f, .72f, .85f }, eveningAmbient[] = { .28f, .26f, .36f };
    need(r.set_sun_direction(sunset ? eveningDirection : dayDirection) && r.set_sun_colour(sunset ? eveningColour : dayColour) &&
             r.set_sun_intensity(sunset ? 2.0f : 3.0f) && r.set_ambient(sunset ? eveningAmbient : dayAmbient),
         "lighting preset refused");
}
std::vector<storm::RenderObjectId> sky(storm::Renderer& r)
{
    // An explicitly authored, lit backdrop mesh, not atmospheric scattering or IBL.
    constexpr unsigned                 rings = 64, segments = 128;
    constexpr double                   pi = 3.141592653589793;
    std::vector<storm::RenderObjectId> ids;
    for (unsigned band = 0; band < rings; band++)
    {
        std::vector<float>    v;
        std::vector<uint32_t> indices;
        const double          a = -pi / 2 + pi * band / rings, b = -pi / 2 + pi * (band + 1) / rings;
        for (unsigned j = 0; j <= segments; j++)
            for (double elevation : { a, b })
            {
                const double angle = 2 * pi * j / segments;
                v.push_back(static_cast<float>(3500 * std::cos(elevation) * std::sin(angle)));
                v.push_back(static_cast<float>(3500 * std::sin(elevation)));
                v.push_back(static_cast<float>(3500 * std::cos(elevation) * std::cos(angle)));
            }
        for (unsigned j = 0; j < segments; j++)
        {
            uint32_t n = j * 2;
            indices.insert(indices.end(), { n, n + 1, n + 2, n + 1, n + 3, n + 2 });
        }
        const float t        = static_cast<float>(band) / rings;
        float       colour[] = { .20f + .28f * (1 - t), .38f + .25f * (1 - t), .63f + .16f * (1 - t) };
        auto        id       = r.add_surface(v.data(), v.size() / 3, indices.data(), indices.size(), colour, true, 1, 0);
        need(!(id == storm::RenderObjectId{}), r.error());
        ids.push_back(id);
    }
    return ids;
}
int main(int argc, char** argv)
{
    try
    {
        umask(0077);
        need(argc == 7 || argc == 10 || argc == 11 || argc == 12,
             "usage: finalverse-living-render FWD CAS MEDIA NEW_OUTPUT FRAMES visible|hidden [FOCUS_E FOCUS_N FOCUS_U [SOURCE_REVISION "
             "[REGION_UUID]]]");
        size_t    consumed = 0;
        const int frames   = std::stoi(argv[5], &consumed);
        need(consumed == std::string(argv[5]).size() && frames >= 2 && frames <= 36000, "frame budget");
        need(std::string(argv[6]) == "visible" || std::string(argv[6]) == "hidden", "visibility");
        fs::path out = argv[4];
        need(!fs::exists(out), "new private output directory required");
        fs::create_directories(out);
        fs::permissions(out, fs::perms::owner_all);
        storm::FwdScene scene;
        std::string     error;
        need(storm::load_fwd_scene(argv[1], argv[2], scene, error), error);
        auto       definition     = read(argv[1], 1024 * 1024);
        const auto definition_sha = sha(definition);
        auto       bodies         = bindings(definition);
        Vec        initialFocus{};
        double     extent = 0;
        for (size_t k = 0; k < 3; k++)
        {
            initialFocus[k] = (scene.minimum[k] + scene.maximum[k]) * .5;
            extent          = std::max(extent, scene.maximum[k] - scene.minimum[k]);
        }
        if (argc >= 10)
            for (size_t k = 0; k < 3; k++)
            {
                size_t n        = 0;
                initialFocus[k] = std::stod(argv[k + 7], &n);
                need(n == std::string(argv[k + 7]).size() && std::isfinite(initialFocus[k]) &&
                         std::abs(initialFocus[k] - scene.origin[k]) <= 1048576,
                     "focus origin bounds");
            }
        uint64_t source_revision = 0;
        if (argc >= 11)
        {
            size_t n        = 0;
            source_revision = std::stoull(argv[10], &n);
            need(n == std::string(argv[10]).size(), "source revision");
        }
        const std::string region = argc == 12 ? argv[11] : "";
        need(region.empty() || living::uuid(region), "region UUID");
        living::Gate gate;
        gate.world      = scene.world_id;
        gate.region     = region;
        gate.definition = definition_sha;
        storm::RendererConfig config;
        config.title      = "Finalverse Montréal · Living World Ω-XV";
        config.media_root = argv[3];
        storm::Renderer renderer;
        need(renderer.initialize(config), renderer.error());
        if (std::string(argv[6]) == "hidden")
        {
            int          count   = 0;
            SDL_Window** windows = SDL_GetWindows(&count);
            for (int i = 0; i < count; i++)
                SDL_HideWindow(windows[i]);
            SDL_free(windows);
        }
        std::vector<Surface> surfaces;
        surfaces.reserve(scene.primitives.size());
        for (auto& p : scene.primitives)
        {
            Surface s;
            s.mesh = std::move(p);
            for (const auto& [entity, b] : bodies)
                if (b.sha == s.mesh.asset_sha256)
                    s.body = entity;
            for (size_t i = 0; i < s.mesh.positions.size(); i += 3)
            {
                s.mesh.positions[i] -= static_cast<float>(initialFocus[0] - scene.origin[0]);
                s.mesh.positions[i + 1] -= static_cast<float>(initialFocus[2] - scene.origin[2]);
                s.mesh.positions[i + 2] += static_cast<float>(initialFocus[1] - scene.origin[1]);
            }
            s.id = renderer.add_surface(s.mesh.positions.data(), s.mesh.positions.size() / 3, s.mesh.indices.data(), s.mesh.indices.size(),
                                        s.mesh.colour.data(), s.mesh.double_sided, s.mesh.roughness, s.mesh.metallic);
            need(!(s.id == storm::RenderObjectId{}), renderer.error());
            surfaces.push_back(std::move(s));
        }
        auto sky_ids = sky(renderer);
        (void)sky_ids;
        // Deliberately authored horizon matte beneath the bounded geographic tile.
        // This adds no surveyed terrain and is excluded from authoritative geometry.
        const float    ground            = static_cast<float>(scene.minimum[2] - initialFocus[2] - 8.);
        const float    horizon[]         = { -3000, ground, -3000, 3000, ground, -3000, 3000, ground, 3000, -3000, ground, 3000 };
        const uint32_t horizon_indices[] = { 0, 2, 1, 0, 3, 2 };
        const float    horizon_colour[]  = { .31f, .36f, .30f };
        need(!(renderer.add_surface(horizon, 4, horizon_indices, 6, horizon_colour, true, 1, 0) == storm::RenderObjectId{}),
             renderer.error());
        lighting(renderer, "daylight");
        const char*     shadow_setting = std::getenv("FINALVERSE_STORM_SHADOWS");
        living::Shadows shadows(shadow_setting && std::string(shadow_setting) == "1");
        living::Camera  desired, current;
        desired.distance = current.distance = std::clamp(extent * 1.8, 8., 2000.);
        Vec         focus                   = initialFocus;
        bool        running                 = true;
        std::string previous_control, previous_update, update_hash, sync = "snapshot", last_error;
        uint64_t    captures = 0, updates = 0, refusals = 0, resyncs = 0;
        double      cpu      = 0;
        auto        previous = std::chrono::steady_clock::now();
        for (int i = 0; i < frames && running; i++)
        {
            const auto   start = std::chrono::steady_clock::now();
            const double dt    = std::clamp(std::chrono::duration<double>(start - previous).count(), 1. / 240., .1);
            previous           = start;
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_EVENT_QUIT)
                    running = false;
                if (event.type == SDL_EVENT_KEY_DOWN)
                    switch (event.key.key)
                    {
                        case SDLK_ESCAPE:
                            running = false;
                            break;
                        case SDLK_O:
                            desired.orbit = !desired.orbit;
                            break;
                        case SDLK_F:
                            desired.distance = 16;
                            break;
                        case SDLK_HOME:
                            desired.distance = std::clamp(extent * 1.8, 8., 2000.);
                            desired.pan      = {};
                            desired.target.clear();
                            break;
                        case SDLK_LEFT:
                            desired.yaw -= .12;
                            break;
                        case SDLK_RIGHT:
                            desired.yaw += .12;
                            break;
                        default:
                            break;
                    }
                if (event.type == SDL_EVENT_MOUSE_WHEEL)
                    desired.distance = std::clamp(desired.distance * std::pow(1.12, -event.wheel.y), 2., 2000.);
            }
            try
            {
                if (fs::exists(out / "camera.json"))
                {
                    auto raw = read(out / "camera.json", 4096, true);
                    if (raw != previous_control)
                    {
                        auto c           = living::camera(raw);
                        desired          = c;
                        previous_control = raw;
                        lighting(renderer, desired.lighting);
                    }
                }
                else if (fs::exists(out / "camera.txt"))
                {
                    auto raw = read(out / "camera.txt", 256, true);
                    if (raw != previous_control)
                    {
                        std::istringstream in(raw);
                        living::Camera     c     = desired;
                        int                orbit = 0;
                        std::string        trailing;
                        if (in >> c.yaw >> c.pitch >> c.distance >> orbit && !(in >> trailing) && std::isfinite(c.yaw) &&
                            std::abs(c.yaw) <= 100 && c.pitch >= -1.5 && c.pitch <= -.05 && c.distance >= 2 && c.distance <= 2000 &&
                            (orbit == 0 || orbit == 1))
                        {
                            c.orbit          = orbit;
                            desired          = c;
                            previous_control = raw;
                        }
                    }
                }
            }
            catch (const std::exception& e)
            {
                last_error = e.what();
            }
            try
            {
                if (fs::exists(out / "scene-update.json"))
                {
                    auto raw = read(out / "scene-update.json", 65536, true);
                    if (raw != previous_update)
                    {
                        auto u        = living::update(raw);
                        auto envelope = living::parse(raw);
                        need(envelope["assets"].Empty(), "static asset updates require admitted FWD reload");
                        need(!region.empty(), "region pin required for dynamic updates");
                        need(u.revision >= source_revision, "update older than source snapshot");
                        for (const auto& r : u.residents)
                        {
                            auto b = bodies.find(r.entity);
                            need(b != bodies.end() && (r.body.empty() || r.body == b->second.sha),
                                 "resident requires admitted known body/entity");
                            need(b->second.resident.empty() || b->second.resident == r.resident, "resident identity binding changed");
                            for (size_t k = 0; k < 3; k++)
                                need(std::abs(r.position[k] - initialFocus[k]) <= 1048576, "render offset bound");
                        }
                        const bool gap =
                            gate.initialized && (u.epoch != gate.last.epoch || !u.has_previous || u.previous != gate.last.revision);
                        if (gate.admit(u, raw))
                        {
                            for (auto& [entity, b] : bodies)
                            {
                                (void)entity;
                                b.present = false;
                            }
                            for (const auto& r : u.residents)
                            {
                                auto& b    = bodies.at(r.entity);
                                b.present  = true;
                                b.resident = r.resident;
                                b.target   = r.position;
                            }
                            source_revision = u.revision;
                            update_hash     = sha(raw);
                            updates++;
                            if (gap)
                                resyncs++;
                            sync = gap ? "snapshot-resynchronized" : "committed";
                            last_error.clear();
                        }
                        previous_update = raw;
                    }
                }
            }
            catch (const std::exception& e)
            {
                if (last_error != e.what())
                {
                    refusals++;
                    last_error = e.what();
                }
                sync = "refused";
            }
            if (desired.orbit)
                desired.yaw = std::remainder(desired.yaw + dt * .18, 6.283185307179586);
            const double camera_alpha = 1 - std::exp(-dt * 12), pose_alpha = 1 - std::exp(-dt * 18);
            bool         interpolation = false;
            current.yaw += std::remainder(desired.yaw - current.yaw, 6.283185307179586) * camera_alpha;
            current.pitch += (desired.pitch - current.pitch) * camera_alpha;
            current.distance += (desired.distance - current.distance) * camera_alpha;
            for (size_t k = 0; k < 3; k++)
                current.pan[k] += (desired.pan[k] - current.pan[k]) * camera_alpha;
            for (auto& [entity, b] : bodies)
            {
                (void)entity;
                for (size_t k = 0; k < 3; k++)
                {
                    b.shown[k] += (b.target[k] - b.shown[k]) * pose_alpha;
                    if (std::abs(b.target[k] - b.shown[k]) < .0001)
                        b.shown[k] = b.target[k];
                    if (std::abs(b.target[k] - b.shown[k]) > .001)
                        interpolation = true;
                }
            }
            Vec target = initialFocus;
            for (const auto& [entity, b] : bodies)
            {
                (void)entity;
                if (b.present && !desired.target.empty() && b.resident == desired.target)
                    target = b.shown;
            }
            for (size_t k = 0; k < 3; k++)
                focus[k] += (target[k] + current.pan[k] - focus[k]) * camera_alpha;
            const Vec shift = { focus[0] - initialFocus[0], focus[1] - initialFocus[1], focus[2] - initialFocus[2] };
            for (auto& s : surfaces)
            {
                Body* body = s.body.empty() ? nullptr : &bodies.at(s.body);
                if (body && !body->present)
                {
                    if (!(s.id == storm::RenderObjectId{}))
                    {
                        need(renderer.remove_object(s.id), renderer.error());
                        s.id = {};
                    }
                    continue;
                }
                if (s.id == storm::RenderObjectId{})
                {
                    s.id = renderer.add_surface(s.mesh.positions.data(), s.mesh.positions.size() / 3, s.mesh.indices.data(),
                                                s.mesh.indices.size(), s.mesh.colour.data(), s.mesh.double_sided, s.mesh.roughness,
                                                s.mesh.metallic);
                    need(!(s.id == storm::RenderObjectId{}), renderer.error());
                }
                Vec delta{};
                if (body)
                    for (size_t k = 0; k < 3; k++)
                        delta[k] = body->shown[k] - body->original[k];
                const float offset[] = { static_cast<float>(delta[0] - shift[0]), static_cast<float>(delta[2] - shift[2]),
                                         static_cast<float>(-(delta[1] - shift[1])) },
                            scale[]  = { 1, 1, 1 };
                need(renderer.set_transform(s.id, offset, scale), renderer.error());
            }
            renderer.set_camera_orbit(static_cast<float>(current.yaw), static_cast<float>(current.pitch),
                                      static_cast<float>(current.distance));
            need(renderer.render_frame(dt), renderer.error());
            cpu += renderer.stats().frame_ms;
            shadows.after_first_frame();
            if (i % 15 == 0 || i + 1 == frames)
            {
                auto partial = out / "frame.part.ppm";
                need(renderer.capture_frame(partial.string()), renderer.error());
                auto image  = read(partial, 1280 * 720 * 3 + 128);
                auto pixels = sha(image);
                fs::rename(partial, out / "frame.ppm");
                rapidjson::StringBuffer buffer;
                Writer                  w(buffer);
                w.StartObject();
                str(w, "schema", "finalverse.storm-view-frame/0.1");
                str(w, "world", scene.world_id);
                str(w, "definition_sha256", definition_sha);
                str(w, "pixels_sha256", pixels);
                w.Key("frame");
                w.Uint64(renderer.stats().frame_index);
                w.Key("triangles");
                w.Uint64(renderer.triangle_count());
                w.Key("objects");
                w.Uint(renderer.object_count());
                w.Key("source_revision");
                w.Uint64(source_revision);
                w.Key("source_cursor");
                w.Uint64(gate.initialized ? gate.last.cursor : 0);
                str(w, "source_epoch", gate.initialized ? gate.last.epoch : "");
                str(w, "applied_update_sha256", update_hash);
                str(w, "synchronization", sync);
                str(w, "backend", "OGRE Metal");
                w.Key("presentation_only");
                w.Bool(true);
                w.Key("interpolation_active");
                w.Bool(interpolation);
                w.Key("focus_origin");
                vector(w, focus);
                w.Key("registered_residents");
                w.StartArray();
                for (const auto& [entity, b] : bodies)
                {
                    (void)entity;
                    if (b.present && !b.resident.empty())
                    {
                        w.StartObject();
                        str(w, "resident", b.resident);
                        w.Key("position");
                        vector(w, b.shown);
                        w.EndObject();
                    }
                }
                w.EndArray();
                str(w, "registered_positions_kind", "interpolated committed presentation");
                str(w, "lighting", desired.lighting);
                w.Key("pssm_shadows");
                w.Bool(shadows.active());
                w.EndObject();
                need(buffer.GetSize() <= 4096, "frame metadata budget");
                save(out / "frame.json", { buffer.GetString(), buffer.GetSize() });
                captures++;
            }
            std::this_thread::sleep_until(start + std::chrono::nanoseconds(16666667));
        }
        rapidjson::StringBuffer buffer;
        Writer                  w(buffer);
        w.StartObject();
        w.Key("frames");
        w.Uint64(renderer.stats().frame_index);
        w.Key("captures");
        w.Uint64(captures);
        w.Key("mean_cpu_submit_ms");
        w.Double(cpu / std::max(uint64_t{ 1 }, renderer.stats().frame_index));
        w.Key("triangles");
        w.Uint64(renderer.triangle_count());
        w.Key("updates_applied");
        w.Uint64(updates);
        w.Key("updates_refused");
        w.Uint64(refusals);
        w.Key("snapshot_resynchronizations");
        w.Uint64(resyncs);
        w.Key("source_revision");
        w.Uint64(source_revision);
        str(w, "last_error", last_error);
        w.Key("pssm_shadows");
        w.Bool(shadows.active());
        str(w, "sky", "authored lit backdrop mesh and horizon matte; not atmosphere or IBL");
        str(w, "animation", "committed root interpolation; no skinning");
        w.Key("world_mutated");
        w.Bool(false);
        w.EndObject();
        std::string receipt(buffer.GetString(), buffer.GetSize());
        save(out / "receipt.json", receipt);
        std::cout << receipt << '\n';
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Living World render: " << e.what() << '\n';
        return 1;
    }
}
