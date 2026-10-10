#pragma once

#include <Arduino.h>
#include <FastLED.h>
#include <math.h>
#include <esp_system.h>
#include "Config.h"
#include "Timing.h"

namespace cmrender {

inline float clamp01(float x) {
  if (x < 0.0f) return 0.0f;
  if (x > 1.0f) return 1.0f;
  return x;
}

inline float smoothstep(float edge0, float edge1, float x) {
  if (edge1 <= edge0) return x >= edge1 ? 1.0f : 0.0f;
  float t = clamp01((x - edge0) / (edge1 - edge0));
  return t * t * (3.0f - 2.0f * t);
}

struct HeartbeatVisualSample {
  float intensity = 0.0f;
  float radius = 0.0f;
};

inline float heartPulseLobe(float phase,
                           float start,
                           float attack,
                           float decay,
                           float amplitude) {
  if (phase < start) return 0.0f;

  const float local = phase - start;
  if (local < attack) {
    const float x = clamp01(local / attack);
    // v8: cosine ease-in/out gives a much gentler ramp than the earlier
    // cubic ease-out. Slope is zero both at the bottom and at the peak.
    const float y = 0.5f - 0.5f * cosf(PI * x);
    return amplitude * clamp01(y);
  }

  if (local < attack + decay) {
    const float x = clamp01((local - attack) / decay);
    // Matching cosine release: no plateau, but a rounded top and a slower,
    // flatter fall back to black.
    const float y = 0.5f + 0.5f * cosf(PI * x);
    return amplitude * clamp01(y);
  }

  return 0.0f;
}

inline HeartbeatVisualSample heartbeatVisual(float phase) {
  HeartbeatVisualSample s;
  phase = phase - floorf(phase);

  // v8 single-pulse test: one pulse, no dub and no flat peak-hold.
  // Both rise and fall are deliberately longer and cosine-eased so the
  // heartbeat reads as a soft pulse rather than a flash.
  const float primary = heartPulseLobe(
      phase,
      0.0f,
      cmcfg::HEART_PRIMARY_ATTACK_FRACTION,
      cmcfg::HEART_PRIMARY_DECAY_FRACTION,
      1.0f);

  s.intensity = clamp01(primary);

  // Radius is retained for compatibility with earlier render experiments,
  // although the default whole-lamp renderer applies one uniform intensity
  // to all 16 LEDs.
  s.radius = (s.intensity > 0.0f)
      ? powf(s.intensity, cmcfg::HEARTBEAT_RADIUS_GAMMA)
      : 0.0f;
  s.radius = clamp01(s.radius);
  return s;
}

inline uint8_t randomMode1to7() {
  return (uint8_t)(1 + (esp_random() % 7));
}

inline CRGB solidForMode(uint8_t mode) {
  switch (mode) {
    case 1: return CRGB::White;
    case 2: return CRGB::Red;
    case 3: return CRGB::Yellow;
    case 4: return CRGB::Green;
    case 5: return CRGB::Purple;
    default: return CRGB::Black;
  }
}

inline void renderModeFrame(uint8_t mode, uint32_t nowMs, CRGB* frame) {
  if (mode >= 1 && mode <= 5) {
    const CRGB c = solidForMode(mode);
    for (uint8_t i = 0; i < cmcfg::NUM_LEDS; ++i) frame[i] = c;
    return;
  }

  if (mode == 6) { // Rainbow: moving multi-color gradient
    const uint32_t wrapped = nowMs % cmcfg::RAINBOW_CYCLE_MS;
    const uint8_t baseHue = (uint8_t)((wrapped * 256ULL) / cmcfg::RAINBOW_CYCLE_MS);
    for (uint8_t col = 0; col < cmcfg::COLS; ++col) {
      for (uint8_t row = 0; row < cmcfg::ROWS; ++row) {
        const uint8_t logical = col * cmcfg::ROWS + row;
        const uint8_t spatialOffset = (uint8_t)(row * 18 + col * 28);
        frame[logical] = CHSV(baseHue + spatialOffset, 255, 255);
      }
    }
    return;
  }

  if (mode == 7) { // Color Cycle: one color across whole lamp
    const uint32_t wrapped = nowMs % cmcfg::COLOR_CYCLE_MS;
    const uint8_t hue = (uint8_t)((wrapped * 256ULL) / cmcfg::COLOR_CYCLE_MS);
    CRGB c;
    c = CHSV(hue, 255, 255);
    for (uint8_t i = 0; i < cmcfg::NUM_LEDS; ++i) frame[i] = c;
    return;
  }

  // Mode 9 (Off/Black), invalid modes, and unresolved mode 8 fall back to black.
  for (uint8_t i = 0; i < cmcfg::NUM_LEDS; ++i) frame[i] = CRGB::Black;
}

class EmoteMixer {
public:
  struct Slot {
    bool used = false;
    bool pending = false;
    uint64_t senderId = 0;
    uint32_t sessionId = 0;
    uint32_t sequence = 0;
    uint8_t wireMode = 0;
    uint8_t resolvedMode = 0;
    uint8_t remainingPulses = 0;
    uint32_t admittedAtMs = 0;
  };

  void add(uint64_t senderId, uint32_t sessionId, uint32_t sequence, uint8_t mode, uint32_t nowMs) {
    int target = -1;
    for (uint8_t i = 0; i < cmcfg::MAX_ACTIVE_EMOTES; ++i) {
      if (!slots_[i].used) {
        target = i;
        break;
      }
    }

    if (target < 0) {
      // Replace the oldest admitted active/pending emote.
      uint32_t oldest = 0;
      target = 0;
      for (uint8_t i = 1; i < cmcfg::MAX_ACTIVE_EMOTES; ++i) {
        const uint32_t ageBest = nowMs - slots_[target].admittedAtMs;
        const uint32_t ageThis = nowMs - slots_[i].admittedAtMs;
        if (ageThis > ageBest) target = i;
      }
    }

    Slot& s = slots_[target];
    s.used = true;
    s.pending = true;
    s.senderId = senderId;
    s.sessionId = sessionId;
    s.sequence = sequence;
    s.wireMode = mode;
    s.resolvedMode = (mode == 8) ? randomMode1to7() : mode;
    s.remainingPulses = 0;
    s.admittedAtMs = nowMs;
  }

  void onPulseBoundary() {
    for (uint8_t i = 0; i < cmcfg::MAX_ACTIVE_EMOTES; ++i) {
      Slot& s = slots_[i];
      if (!s.used) continue;

      if (s.pending) {
        s.pending = false;
        s.remainingPulses = 3;
        if (s.wireMode == 8) s.resolvedMode = randomMode1to7();
        continue;
      }

      if (s.remainingPulses > 0) {
        --s.remainingPulses;
        if (s.remainingPulses == 0) {
          s.used = false;
          continue;
        }
        if (s.wireMode == 8) s.resolvedMode = randomMode1to7();
      }
    }
  }

  bool hasVisibleEmotes() const {
    for (uint8_t i = 0; i < cmcfg::MAX_ACTIVE_EMOTES; ++i) {
      if (slots_[i].used && !slots_[i].pending && slots_[i].remainingPulses > 0) return true;
    }
    return false;
  }

  void render(uint32_t nowMs, CRGB* out) const {
    uint16_t sumR[cmcfg::NUM_LEDS] = {};
    uint16_t sumG[cmcfg::NUM_LEDS] = {};
    uint16_t sumB[cmcfg::NUM_LEDS] = {};
    uint8_t count = 0;

    CRGB tmp[cmcfg::NUM_LEDS];
    for (uint8_t i = 0; i < cmcfg::MAX_ACTIVE_EMOTES; ++i) {
      const Slot& s = slots_[i];
      if (!s.used || s.pending || s.remainingPulses == 0) continue;
      renderModeFrame(s.resolvedMode, nowMs, tmp);
      for (uint8_t p = 0; p < cmcfg::NUM_LEDS; ++p) {
        sumR[p] += tmp[p].r;
        sumG[p] += tmp[p].g;
        sumB[p] += tmp[p].b;
      }
      ++count;
    }

    if (count == 0) {
      for (uint8_t p = 0; p < cmcfg::NUM_LEDS; ++p) out[p] = CRGB::Black;
      return;
    }

    for (uint8_t p = 0; p < cmcfg::NUM_LEDS; ++p) {
      out[p] = CRGB(sumR[p] / count, sumG[p] / count, sumB[p] / count);
    }
  }

  uint8_t visibleCount() const {
    uint8_t count = 0;
    for (uint8_t i = 0; i < cmcfg::MAX_ACTIVE_EMOTES; ++i) {
      if (slots_[i].used && !slots_[i].pending && slots_[i].remainingPulses > 0) ++count;
    }
    return count;
  }

private:
  Slot slots_[cmcfg::MAX_ACTIVE_EMOTES];
};

class GreetingLayer {
public:
  void trigger(uint32_t nowMs) {
    active_ = true;
    startedAtMs_ = nowMs;
  }

  bool blackNow(uint32_t nowMs) {
    if (!active_) return false;

    const uint32_t onePair = cmcfg::GREETING_BLACK_MS + cmcfg::GREETING_NORMAL_MS;
    const uint32_t total = onePair * cmcfg::GREETING_REPETITIONS;
    const uint32_t elapsed = nowMs - startedAtMs_;

    if (elapsed >= total) {
      active_ = false;
      return false;
    }

    return (elapsed % onePair) < cmcfg::GREETING_BLACK_MS;
  }

private:
  bool active_ = false;
  uint32_t startedAtMs_ = 0;
};

inline uint8_t brightnessScaleFromPercent(uint8_t percent) {
  return (uint8_t)((uint16_t)percent * 255U / 100U);
}

inline void applyHeartbeatAndBrightness(CRGB* frame, float phase, uint8_t brightnessPercent) {
  const HeartbeatVisualSample hb = heartbeatVisual(phase);
  const float brightness = brightnessPercent / 100.0f;

  // v3 test: the entire 2 x 8 lamp follows exactly the same heartbeat
  // intensity. The previous center-out spatial mask made individual rows
  // appear/disappear at different moments, which read more like a flash than
  // a soft heartbeat. We keep the lub-dub timing envelope, but remove spatial
  // intensity differences so the whole lamp expands/contracts perceptually as
  // one coherent light source.
  const float scaleF = clamp01(hb.intensity * brightness);
  const uint8_t scale = (uint8_t)(scaleF * 255.0f + 0.5f);

  for (uint8_t i = 0; i < cmcfg::NUM_LEDS; ++i) {
    frame[i].nscale8_video(scale);
  }
}

} // namespace cmrender
