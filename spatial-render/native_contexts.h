// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
// Native (non-FWD) spatial contexts of the Ω-XX universal context system, renderer side:
//
//  ASTRONOMICAL  geocentric, equatorial-of-date axes mapped to render space (x, z_eq -> y up,
//                -y_eq -> z), kilometres. Earth (NASA Blue Marble colour map) rotates by GMST;
//                Moon and Sun from finalverse/astro.h (AA low-precision formulae, validated
//                against JPL Horizons). Sim time advances at a chosen rate. A descent moves the
//                camera focus to a surface point (it rotates with the Earth) before the Viewer
//                switches semantically to the geographic context.
//  SCIENTIFIC    angstrom (crystal) or micrometre (microstructure) contexts built from an
//                identified model: an idealised BCC alpha-iron lattice (a = 2.8665 A) with one
//                highlighted unit cell and the nearest-neighbour distance, or an ILLUSTRATIVE
//                tempered-martensite microstructure. Never a measurement of a real sample.
//
// Presentation only: nothing here writes world state.
#pragma once
#include "finalverse/astro.h"
#include "storm/genesis/runtime.h"
#include <array>
#include <cmath>
#include <string>

namespace living {

struct AstroParams {
  double unix0 = 0;            // sim time at start (UTC seconds)
  double scale = 1;            // sim seconds per real second
  double distance_km = 26000;  // camera distance from the focus
  double yaw = .35, pitch = -.18;
  bool descend = false;
  double lat = 45.5017, lon = -73.5673;  // descent target (Montréal)
};

struct ScienceParams {
  std::string model = "bcc-iron";  // bcc-iron | microstructure
  int cells = 4;
  double a = 2.8665;               // lattice parameter, angstrom
};

class NativeScene {
 public:
  enum class Kind { None, Astronomical, Scientific } kind = Kind::None;
  AstroParams astro;
  ScienceParams science;
  double started = 0;
  double descentStart = -1;
  std::array<double, 3> focus{};   // world-local focus (km / A / um)
  double distance = 26000;
  double unitKm = 1;               // km per render unit (astro), for the atmosphere
  bool settled = false;
  double simUnix = 0;

  static std::array<double, 3> render(const finalverse::astro::Vec3 &e) { return {e.x, e.z, -e.y}; }

  void start_astro(storm::genesis::Runtime &rt, const AstroParams &p, const std::string &earth_texture, double now) {
    rt.clear();
    kind = Kind::Astronomical;
    astro = p;
    started = now;
    descentStart = p.descend ? now : -1;
    focus = {0, 0, 0};
    distance = p.distance_km;
    using namespace storm::genesis;
    Request earth;
    earth.node.id = "earth"; earth.node.kind = "planet"; earth.priority = 0; earth.reveal_seconds = 0;
    earth.node.num = {{"radius", finalverse::astro::kEarthMeanRadiusKm}, {"segments", 256}, {"roughness", .78}};
    if (!earth_texture.empty()) earth.node.text["texture_file"] = earth_texture;
    else earth.node.num.insert({{"r", .2}, {"g", .35}, {"b", .6}});
    earth.at.on_ground = false;
    rt.submit(earth);
    Request moon;
    moon.node.id = "moon"; moon.node.kind = "planet"; moon.node.seed = 301; moon.priority = 0; moon.reveal_seconds = 0;
    moon.node.num = {{"radius", finalverse::astro::kMoonRadiusKm}, {"segments", 96}, {"roughness", .95}};
    moon.node.text["procedural"] = "moon";
    moon.at.on_ground = false;
    rt.submit(moon);
    // the Moon's true path over the next sidereal month (geocentric, of date)
    Request orbit;
    orbit.node.id = "moon-orbit"; orbit.node.kind = "tube"; orbit.priority = 1; orbit.reveal_seconds = 1.2;
    orbit.node.num = {{"radius", 220}, {"sides", 6}, {"glow", 1.6}, {"r", .35}, {"g", .62}, {"b", 1}};
    orbit.node.text["closed"] = "no";
    const double jd0 = finalverse::astro::unix_to_jd(p.unix0);
    for (int i = 0; i <= 160; i++) {
      const double jd = jd0 + 27.32 * i / 160;
      const auto r = render(finalverse::astro::equatorial(finalverse::astro::moon(jd), jd));
      orbit.node.points3.push_back(r);
    }
    orbit.at.on_ground = false;
    rt.submit(orbit);
  }

  void start_science(storm::genesis::Runtime &rt, const ScienceParams &p) {
    rt.clear();
    kind = Kind::Scientific;
    science = p;
    focus = {0, 0, 0};
    using namespace storm::genesis;
    if (p.model == "microstructure") {
      Request g;
      g.node.id = "microstructure"; g.node.kind = "grains"; g.node.seed = 52100; g.priority = 0; g.reveal_seconds = 1.6;
      g.node.num = {{"size", 60}, {"grains", 26}, {"carbides", 1400}};
      g.at.on_ground = false;
      rt.submit(g);
      distance = 95;
      return;
    }
    const int n = std::clamp(p.cells, 1, 8);
    const double a = p.a, half = n * a / 2;
    Request atoms, cellAtoms;
    atoms.node.id = "fe-atoms"; atoms.node.kind = "atoms"; atoms.priority = 0; atoms.reveal_seconds = 1.4;
    atoms.node.num = {{"radius", .55}, {"segments", 24}, {"r", .56}, {"g", .57}, {"b", .6}, {"metallic", .95}, {"roughness", .25}};
    cellAtoms.node.id = "fe-cell-atoms"; cellAtoms.node.kind = "atoms"; cellAtoms.priority = 0; cellAtoms.reveal_seconds = 1.8;
    cellAtoms.node.num = {{"radius", .62}, {"segments", 32}, {"r", .9}, {"g", .62}, {"b", .28}, {"metallic", .9}, {"roughness", .2}, {"er", .5}, {"eg", .25}, {"eb", .05}};
    const int c0 = n / 2;  // highlighted cell index
    for (int i = 0; i <= n; i++)
      for (int j = 0; j <= n; j++)
        for (int k = 0; k <= n; k++) {
          const std::array<double, 3> corner{i * a - half, j * a - half, k * a - half};
          const bool inCell = (i == c0 || i == c0 + 1) && (j == c0 || j == c0 + 1) && (k == c0 || k == c0 + 1);
          (inCell ? cellAtoms : atoms).node.points3.push_back(corner);
          if (i < n && j < n && k < n) {
            const std::array<double, 3> body{(i + .5) * a - half, (j + .5) * a - half, (k + .5) * a - half};
            (i == c0 && j == c0 && k == c0 ? cellAtoms : atoms).node.points3.push_back(body);
          }
        }
    atoms.at.on_ground = cellAtoms.at.on_ground = false;
    rt.submit(atoms);
    rt.submit(cellAtoms);
    Request cell;
    cell.node.id = "unit-cell"; cell.node.kind = "cell"; cell.priority = 1; cell.reveal_seconds = 1.0;
    cell.node.num = {{"radius", .035}, {"sides", 8}, {"glow", 3}, {"r", .25}, {"g", .8}, {"b", 1}};
    const double o = c0 * a - half;
    cell.node.points3 = {{a, 0, 0}, {0, a, 0}, {0, 0, a}, {o, o, o}};
    cell.at.on_ground = false;
    rt.submit(cell);
    // nearest-neighbour distance (body centre to a corner): sqrt(3) a / 2
    Request nn;
    nn.node.id = "nearest-neighbour"; nn.node.kind = "tube"; nn.priority = 1; nn.reveal_seconds = 1.0;
    nn.node.num = {{"radius", .05}, {"sides", 8}, {"glow", 5}, {"r", 1}, {"g", .78}, {"b", .2}};
    nn.node.points3 = {{o, o, o}, {o + a / 2, o + a / 2, o + a / 2}};
    nn.at.on_ground = false;
    rt.submit(nn);
    distance = n * a * 2.3;
  }

  // Per frame. Returns the world-local focus; fills the sun travel direction (astro).
  void update(storm::genesis::Runtime &rt, double now, float sun_travel[3], std::array<double, 3> &planet_world, double &yaw, double &pitch, double &dist) {
    settled = true;
    if (kind == Kind::Astronomical) {
      using namespace finalverse::astro;
      simUnix = astro.unix0 + (now - started) * astro.scale;
      const double jd = unix_to_jd(simUnix);
      const auto m = render(equatorial(moon(jd), jd));
      const auto s = render(equatorial(sun(jd), jd));
      const double gmst = gmst_deg(jd) * kDeg;
      rt.place("earth", {0, 0, 0}, gmst);
      rt.place("moon", m, std::atan2(m[2], -m[0]));
      const double sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
      for (int i = 0; i < 3; i++) sun_travel[i] = float(-s[i] / sl);
      planet_world = {0, 0, 0};
      if (descentStart >= 0) {
        const double t = std::clamp((now - descentStart) / 6.0, 0.0, 1.0), e = t * t * (3 - 2 * t);
        const auto p = render(fixed_to_equatorial(earth_fixed(astro.lat, astro.lon, 0), jd));
        for (int i = 0; i < 3; i++) focus[i] = p[i] * e;
        dist = astro.distance_km * (1 - e) + 900 * e;
        // look down towards the target as we arrive; yaw follows the local east
        const double tyaw = std::atan2(p[0], p[2]);
        yaw = astro.yaw * (1 - e) + tyaw * e;
        pitch = astro.pitch * (1 - e) + (-1.05) * e;
        settled = t >= 1;
      } else {
        focus = {0, 0, 0};
        dist = astro.distance_km;
        yaw = astro.yaw;
        pitch = astro.pitch;
      }
    } else if (kind == Kind::Scientific) {
      dist = distance;
    }
  }
};

}  // namespace living
