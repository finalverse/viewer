// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
// Versioned out-of-process Metal presentation bridge. No world credentials.
#include "storm/fwd_scene.h"
#include "storm/render/renderer.h"
#include <SDL3/SDL.h>
#include <openssl/sha.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
namespace fs = std::filesystem;
void need(bool b, const std::string& s)
{
    if (!b)
        throw std::runtime_error(s);
}
std::string read(const fs::path& p, size_t n)
{
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    need(f.good() && f.tellg() > 0 && f.tellg() <= static_cast<std::streamoff>(n), "bounded file");
    std::string s(static_cast<size_t>(f.tellg()), '\0');
    f.seekg(0);
    f.read(s.data(), static_cast<std::streamsize>(s.size()));
    need(f.good(), "file read");
    return s;
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
        need(f.good(), "frame receipt write");
    }
    fs::rename(part, p);
}
int main(int argc, char** argv)
{
    try
    {
        need(argc == 7 || argc == 10 || argc == 11,
             "usage: finalverse-living-render FWD CAS MEDIA NEW_OUTPUT FRAMES visible|hidden [FOCUS_E FOCUS_N FOCUS_U [SOURCE_REVISION]]");
        size_t    used   = 0;
        const int frames = std::stoi(argv[5], &used);
        need(used == std::string(argv[5]).size() && frames >= 2 && frames <= 36000, "bounded frame count");
        need(std::string(argv[6]) == "visible" || std::string(argv[6]) == "hidden", "visibility");
        const fs::path out = argv[4];
        need(!fs::exists(out), "new private output directory required");
        fs::create_directories(out);
        fs::permissions(out, fs::perms::owner_all);
        storm::FwdScene scene;
        std::string     error;
        need(storm::load_fwd_scene(argv[1], argv[2], scene, error), error);
        const auto            definition_sha = sha(read(argv[1], 1024 * 1024));
        storm::RendererConfig config;
        config.title      = "Finalverse Montréal — Living World · Ω-XIV";
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
        need(renderer.set_sun_intensity(3.5f), "sun");
        const float ambient[] = { .35f, .38f, .43f };
        need(renderer.set_ambient(ambient), "ambient");
        double centre[3];
        for (size_t k = 0; k < 3; k++)
            centre[k] = (scene.minimum[k] + scene.maximum[k]) * .5 - scene.origin[k];
        if (argc >= 10)
            for (size_t k = 0; k < 3; k++)
            {
                size_t n = 0;
                double v = std::stod(argv[k + 7], &n);
                need(n == std::string(argv[k + 7]).size() && std::isfinite(v) && std::abs(v - scene.origin[k]) <= 1048576,
                     "view origin bounds");
                centre[k] = v - scene.origin[k];
            }
        uint64_t source_revision = 0;
        if (argc == 11)
        {
            size_t n        = 0;
            source_revision = std::stoull(argv[10], &n);
            need(n == std::string(argv[10]).size(), "source revision");
        }
        double extent = 0;
        for (size_t k = 0; k < 3; k++)
            extent = std::max(extent, scene.maximum[k] - scene.minimum[k]);
        // The composer's explicit view-origin is encoded by the optional first line
        // of an operator-created camera file. No model can alter authoritative poses.
        for (auto& p : scene.primitives)
        {
            for (size_t i = 0; i < p.positions.size(); i += 3)
            {
                p.positions[i] -= static_cast<float>(centre[0]);
                p.positions[i + 1] -= static_cast<float>(centre[2]);
                p.positions[i + 2] += static_cast<float>(centre[1]);
            }
            need(renderer.add_triangles(p.positions.data(), p.positions.size() / 3, p.indices.data(), p.indices.size(), p.colour.data(),
                                        p.double_sided, p.roughness, p.metallic),
                 renderer.error());
        }
        float       yaw = .4f, pitch = -.65f, distance = static_cast<float>(std::max(extent * 1.8, 8.));
        bool        running = true, orbit = false;
        double      cpu      = 0;
        uint64_t    captures = 0;
        std::string previous_control;
        for (int i = 0; i < frames && running; i++)
        {
            const auto frame_start = std::chrono::steady_clock::now();
            SDL_Event  event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_EVENT_QUIT)
                    running = false;
                if (event.type == SDL_EVENT_KEY_DOWN)
                {
                    switch (event.key.key)
                    {
                        case SDLK_ESCAPE:
                            running = false;
                            break;
                        case SDLK_O:
                            orbit = !orbit;
                            break;
                        case SDLK_F:
                            distance = 20;
                            break;
                        case SDLK_HOME:
                            distance = static_cast<float>(extent * 1.8);
                            break;
                        case SDLK_LEFT:
                            yaw -= .12f;
                            break;
                        case SDLK_RIGHT:
                            yaw += .12f;
                            break;
                        default:
                            break;
                    }
                }
                if (event.type == SDL_EVENT_MOUSE_WHEEL)
                    distance = std::clamp(distance * std::pow(1.12f, -event.wheel.y), 2.f, 2000.f);
            }
            {
                std::ifstream control(out / "camera.txt");
                char          buffer[257]{};
                control.read(buffer, 256);
                const auto count = control.gcount();
                if (count > 0 && count < 256)
                {
                    std::string raw(buffer, static_cast<size_t>(count));
                    if (raw != previous_control)
                    {
                        std::istringstream input(raw);
                        float              y, p, d;
                        int                o = 0;
                        std::string        trailing;
                        if (input >> y >> p >> d >> o && !(input >> trailing) && std::isfinite(y) && std::isfinite(p) && std::isfinite(d) &&
                            std::abs(y) <= 100 && p >= -1.5f && p <= -.05f && d >= 2 && d <= 2000 && (o == 0 || o == 1))
                        {
                            yaw              = y;
                            pitch            = p;
                            distance         = d;
                            orbit            = o == 1;
                            previous_control = raw;
                        }
                    }
                }
            }
            if (orbit)
                yaw = std::remainder(yaw + .003f, 6.283185307f);
            renderer.set_camera_orbit(yaw, pitch, distance);
            need(renderer.render_frame(1. / 60.), renderer.error());
            cpu += renderer.stats().frame_ms;
            if (i % 15 == 0 || i + 1 == frames)
            {
                auto partial = out / "frame.part.ppm";
                need(renderer.capture_frame(partial.string()), renderer.error());
                auto image     = read(partial, 3 * 1280 * 720 + 128);
                auto image_sha = sha(image);
                fs::rename(partial, out / "frame.ppm");
                std::ostringstream report;
                report << "{\"schema\":\"finalverse.storm-view-frame/0.1\",\"world\":\"" << scene.world_id << "\",\"definition_sha256\":\""
                       << definition_sha << "\",\"pixels_sha256\":\"" << image_sha << "\",\"frame\":" << renderer.stats().frame_index
                       << ",\"triangles\":" << scene.triangles << ",\"objects\":" << renderer.object_count()
                       << ",\"source_revision\":" << source_revision << ",\"backend\":\"OGRE Metal\",\"presentation_only\":true}";
                save(out / "frame.json", report.str());
                captures++;
            }
            std::this_thread::sleep_until(frame_start + std::chrono::nanoseconds(16666667));
        }
        std::ostringstream receipt;
        receipt << "{\"frames\":" << renderer.stats().frame_index << ",\"captures\":" << captures
                << ",\"mean_cpu_submit_ms\":" << cpu / static_cast<double>(std::max(uint64_t{ 1 }, renderer.stats().frame_index))
                << ",\"triangles\":" << scene.triangles << ",\"world_mutated\":false}";
        save(out / "receipt.json", receipt.str());
        std::cout << receipt.str() << '\n';
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Living World render: " << e.what() << '\n';
        return 1;
    }
}
