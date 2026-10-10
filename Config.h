#pragma once

#include <Arduino.h>

namespace cmcfg {

// ----- Hardware (locked by project spec) -----
constexpr uint8_t PIN_LED_LEFT  = 16;
constexpr uint8_t PIN_LED_RIGHT = 17;
constexpr uint8_t PIN_TOUCH     = 4;   // external capacitive touch module, digital active-HIGH

constexpr uint8_t ROWS = 8;
constexpr uint8_t COLS = 2;
constexpr uint8_t NUM_LEDS = ROWS * COLS;
constexpr uint8_t LEDS_PER_STRIP = ROWS;

// Set these if one strip is physically mounted in the opposite pixel direction.
constexpr bool LEFT_REVERSED  = false;
constexpr bool RIGHT_REVERSED = false;

constexpr uint16_t LED_POWER_LIMIT_MA = 700;

// ----- Mesh -----
// Every lamp in the test must use the same values.
constexpr char MESH_PREFIX[]   = "CriticalMassMesh";
constexpr char MESH_PASSWORD[] = "CriticalMass2026";
constexpr uint16_t MESH_PORT = 5555;

constexpr uint8_t PROTOCOL_VERSION = 1;

// ----- Locked interaction timing -----
constexpr uint32_t DOUBLE_CLICK_MS = 350;
constexpr uint32_t LONG_PRESS_MS   = 800;
constexpr uint32_t TOUCH_DEBOUNCE_MS = 25;
constexpr uint32_t EMOTE_COOLDOWN_MS = 1000;

// ----- Brightness (locked) -----
constexpr uint8_t BRIGHTNESS_LEVELS_PERCENT[4] = {15, 30, 60, 100};

// ----- Visual test tuning -----
// The functional form is locked, but these numeric values are intentionally still tuning constants.
// Change them freely during bench testing; they are NOT protocol constants.
constexpr float HEARTBEAT_MIN_BPM = 60.0f;
constexpr float HEARTBEAT_MAX_BPM = 120.0f;
constexpr float HEARTBEAT_BPM_PER_EXTRA_NODE = 4.0f;

// Heart-like double pulse ("lub-dub") inside each heartbeat interval.
// These values are visual tuning constants, not protocol constants.
// v8 single-pulse test: slower, flatter rise and fall.
// The primary pulse uses longer cosine-eased ramps so the lamp breathes/pulses
// instead of snapping up and down. The secondary-pulse constants remain only
// for older A/B experiments and are not used by the v8 renderer.
constexpr float HEART_PRIMARY_ATTACK_FRACTION = 0.120f;
constexpr float HEART_PRIMARY_DECAY_FRACTION  = 0.260f;
constexpr float HEART_SECOND_START_FRACTION   = 0.150f;
constexpr float HEART_SECOND_ATTACK_FRACTION  = 0.080f;
constexpr float HEART_SECOND_DECAY_FRACTION   = 0.210f;
constexpr float HEART_SECOND_AMPLITUDE        = 0.84f;
// Kept for compatibility with older pulse experiments; the v7 single-pulse
// renderer uses a smoothstep release instead of exponential decay.
constexpr float HEARTBEAT_DECAY_EXP_K          = 4.6f;

// The v3 test renders heartbeat intensity uniformly across all 16 LEDs.
// These older spatial constants are kept for quick A/B experiments, but are
// not used by the default v3 renderer.
constexpr float HEARTBEAT_RADIUS_GAMMA = 0.55f;
constexpr float HEARTBEAT_EDGE_SOFTNESS = 0.20f;

// Slow fixed animation speeds, within the locked 15-20 s target range.
constexpr uint32_t RAINBOW_CYCLE_MS = 18000;
constexpr uint32_t COLOR_CYCLE_MS   = 18000;

// ----- Greeting (locked) -----
constexpr uint32_t GREETING_BLACK_MS  = 100;
constexpr uint32_t GREETING_NORMAL_MS = 100;
constexpr uint8_t GREETING_REPETITIONS = 3;

// ----- Rendering -----
constexpr uint32_t FRAME_INTERVAL_MS = 16; // ~60 FPS

// ----- Protocol limits -----
constexpr uint8_t MAX_ACTIVE_EMOTES = 8;
constexpr uint8_t MAX_BEAT_TIMES = 32;
constexpr uint32_t BEAT_SCHEDULE_TARGET_US = 6000000UL;

// Buffers for Base64(CBOR(message)). 32 uint64 timestamps fit comfortably.
constexpr size_t MAX_CBOR_BYTES = 512;
constexpr size_t MAX_BASE64_BYTES = 768;

} // namespace cmcfg
