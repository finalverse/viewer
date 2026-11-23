// Copyright 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
// FINALVERSE visual style system (Ω-XX), renderer side: a named, versioned style
// (finalverse.visual-style/1, Core contracts/presentation/visual-styles.json) maps to a Storm
// presentation and a sun. Styles are presentation only: they never touch geometry,
// dimensions, authored materials or any authoritative value.
#pragma once
#include "storm/render/renderer.h"
#include <cmath>
#include <string>

namespace living {

struct Style {
  storm::Presentation look;
  double sun_elevation_deg = 24, sun_azimuth_deg = 225;  // geographic contexts
  float key_intensity = 1.f;                              // multiplier on the physical sun
  bool known = true;
};

inline const char *kStyleNames[] = {"cinematic", "engineering", "scientific", "architectural", "stylized", "fantasy", "scifi", "historical", "abstract", "educational"};

inline Style style_for(const std::string &name, bool engineering) {
  using P = storm::Presentation;
  Style s;
  P &p = s.look;
  p.backdrop = engineering ? P::Backdrop::Studio : P::Backdrop::Atmosphere;
  p.tone_curve = P::ToneCurve::AgX;
  p.aerial_scale = engineering ? 0.f : 3.f;
  if (!engineering) p.exposure_ev = -1.2f;  // physical sun (E/pi ~ 6.4) needs about -1.2 EV for mid-grey
  p.turbidity = 1.4f;
  p.vignette = .22f;
  p.grain = .008f;
  p.bloom_strength = .05f;
  p.bloom_threshold = 1.4f;
  if (engineering) {
    // dark graded cyclorama, cool top light
    p.background[0] = .018f; p.background[1] = .020f; p.background[2] = .026f;
    p.studio_top[0] = .16f; p.studio_top[1] = .175f; p.studio_top[2] = .21f;
    p.studio_glow = .55f;
  }
  if (name == "cinematic" || name.empty()) {
    p.contrast = 1.1f; p.saturation = 1.08f; p.temperature = .06f;
    p.exposure_ev = engineering ? -.35f : -1.3f;
    p.bloom_strength = .07f; p.vignette = .26f; p.grain = .01f;
    p.aerial_scale = engineering ? 0.f : 4.f; p.turbidity = 1.7f;
    s.sun_elevation_deg = 21; s.sun_azimuth_deg = 232;
  } else if (name == "engineering") {
    p.tone_curve = P::ToneCurve::Aces;
    p.exposure_ev = engineering ? -.5f : -1.2f;
    p.contrast = 1.f; p.saturation = .95f; p.vignette = .1f; p.grain = 0.f; p.bloom_strength = .02f;
    p.outline = .55f; p.outline_sensitivity = .05f;
    p.outline_colour[0] = .01f; p.outline_colour[1] = .012f; p.outline_colour[2] = .016f;
    s.sun_elevation_deg = 48;
  } else if (name == "scientific") {
    p.tone_curve = P::ToneCurve::Aces;
    p.backdrop = P::Backdrop::Studio;
    p.background[0] = .006f; p.background[1] = .010f; p.background[2] = .024f;
    p.studio_top[0] = .05f; p.studio_top[1] = .09f; p.studio_top[2] = .17f; p.studio_glow = .9f;
    p.contrast = 1.05f; p.saturation = 1.05f; p.vignette = .3f; p.grain = 0.f;
    p.bloom_strength = .12f; p.bloom_threshold = 1.f;
    p.outline = .3f; p.outline_additive = true; p.outline_sensitivity = .08f;
    p.outline_colour[0] = .15f; p.outline_colour[1] = .45f; p.outline_colour[2] = .9f;
  } else if (name == "architectural") {
    p.exposure_ev = -.9f; p.contrast = .96f; p.saturation = .94f; p.temperature = .03f;
    p.vignette = .12f; p.grain = 0.f; p.aerial_scale = engineering ? 0.f : 2.5f; p.turbidity = 1.2f;
    s.sun_elevation_deg = 38; s.sun_azimuth_deg = 210;
  } else if (name == "stylized") {
    p.tone_curve = P::ToneCurve::Hable;
    p.saturation = 1.35f; p.contrast = 1.12f; p.posterize = 7.f; p.grain = 0.f; p.vignette = .1f;
    p.outline = .9f; p.outline_sensitivity = .035f;
    s.sun_elevation_deg = 34;
  } else if (name == "fantasy") {
    p.exposure_ev = -1.2f; p.saturation = 1.22f; p.contrast = 1.05f; p.temperature = .22f; p.tint = .12f;
    p.lift[0] = .02f; p.lift[1] = 0.f; p.lift[2] = .04f;
    p.bloom_strength = .22f; p.bloom_threshold = .9f; p.vignette = .32f;
    p.turbidity = 4.f; p.aerial_scale = engineering ? 0.f : 7.f;
    p.fog_density = engineering ? 0.f : .0025f; p.fog_falloff = .02f; p.fog_max = .55f;
    s.sun_elevation_deg = 9; s.sun_azimuth_deg = 250;
  } else if (name == "scifi") {
    p.tone_curve = P::ToneCurve::Aces;
    p.exposure_ev = -1.4f; p.temperature = -.18f; p.saturation = 1.1f; p.contrast = 1.15f;
    p.lift[0] = 0.f; p.lift[1] = .012f; p.lift[2] = .03f;
    p.gain[0] = 1.05f; p.gain[1] = 1.f; p.gain[2] = .96f;
    p.bloom_strength = .3f; p.bloom_threshold = .8f; p.chromatic = 1.2f; p.vignette = .35f; p.grain = .014f;
    p.outline = .12f; p.outline_additive = true; p.outline_sensitivity = .25f;
    p.outline_colour[0] = .1f; p.outline_colour[1] = .7f; p.outline_colour[2] = 1.2f;
    p.turbidity = 2.f; p.star_brightness = 1.f;
    s.sun_elevation_deg = 4; s.sun_azimuth_deg = 255;
  } else if (name == "historical") {
    p.tone_curve = P::ToneCurve::Hable;
    p.sepia = .6f; p.saturation = .7f; p.contrast = .95f; p.grain = .05f; p.vignette = .5f;
    p.turbidity = 3.f;
    s.sun_elevation_deg = 30;
  } else if (name == "abstract") {
    p.backdrop = P::Backdrop::Colour;
    p.background[0] = .004f; p.background[1] = .006f; p.background[2] = .012f;
    p.saturation = .25f; p.exposure_ev = -.6f; p.bloom_strength = .45f; p.bloom_threshold = .6f;
    p.outline = 1.f; p.outline_additive = true; p.outline_sensitivity = .03f;
    p.outline_colour[0] = .2f; p.outline_colour[1] = 1.4f; p.outline_colour[2] = 2.2f;
    p.vignette = .4f; p.grain = 0.f;
  } else if (name == "educational") {
    p.exposure_ev = -1.f; p.saturation = 1.12f; p.contrast = 1.02f; p.vignette = .08f; p.grain = 0.f;
    p.outline = .35f; p.outline_sensitivity = .05f;
    s.sun_elevation_deg = 45;
  } else {
    s.known = false;
  }
  return s;
}

inline void sun_vector(double elevation_deg, double azimuth_deg, float out_travel[3]) {
  // Render space: x east, y up, -z north. Azimuth from north, clockwise. out = direction the
  // light travels (from the sun towards the scene).
  const double e = elevation_deg * M_PI / 180, a = azimuth_deg * M_PI / 180;
  const double toSun[3] = {std::cos(e) * std::sin(a), std::sin(e), -std::cos(e) * std::cos(a)};
  for (int i = 0; i < 3; i++) out_travel[i] = float(-toSun[i]);
}

}  // namespace living
