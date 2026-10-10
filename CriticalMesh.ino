#include <Arduino.h>
#include <ESP.h>
#include <esp_system.h>
#include <Preferences.h>
#include <FastLED.h>
#include <painlessMesh.h>

#include "Config.h"
#include "MeshProtocol.h"
#include "Timing.h"
#include "Rendering.h"
#include "TouchButton.h"

// -----------------------------------------------------------------------------
// Critical Mass Mesh Light - bench-test firmware
// Based on firmware/mesh specs v16.
// -----------------------------------------------------------------------------

Scheduler userScheduler;
painlessMesh mesh;
Preferences preferences;

CRGB leftLeds[cmcfg::LEDS_PER_STRIP];
CRGB rightLeds[cmcfg::LEDS_PER_STRIP];
CRGB logicalFrame[cmcfg::NUM_LEDS];

TouchButton touchButton(cmcfg::PIN_TOUCH);
BeatScheduleState beatSchedule;
TimingEngine timingEngine(beatSchedule);
MeshClock64Sender scheduleSenderClock;
cmrender::EmoteMixer emoteMixer;
cmrender::GreetingLayer greetingLayer;

uint64_t senderId = 0;
uint32_t sessionId = 0;
uint32_t nextSequence = 1;

uint8_t localMode = 1;
uint8_t brightnessIndex = 3;
uint8_t localFargmyllerResolvedMode = 1;

size_t knownNodeCount = 1;
bool topologyDirty = true;
uint32_t lastTopologyPollMs = 0;
uint32_t lastFrameMs = 0;

bool emoteSentOnce = false;
uint32_t lastEmoteSentMs = 0;

String retransmitPayload;
bool retransmitPending = false;
uint32_t retransmitAtMs = 0;

// ----- Duplicate/stale cache for sequence-ordered messages -----
struct SeenSequenceEntry {
  bool used = false;
  uint64_t senderId = 0;
  uint32_t sessionId = 0;
  uint32_t lastSequence = 0;
  uint32_t touchedMs = 0;
};

constexpr uint8_t SEEN_CACHE_SIZE = 24;
SeenSequenceEntry seenCache[SEEN_CACHE_SIZE];

bool acceptSequence(uint64_t remoteSender, uint32_t remoteSession, uint32_t sequence, uint32_t nowMs) {
  for (uint8_t i = 0; i < SEEN_CACHE_SIZE; ++i) {
    SeenSequenceEntry& e = seenCache[i];
    if (e.used && e.senderId == remoteSender && e.sessionId == remoteSession) {
      if (!cmproto::sequenceIsNewer(sequence, e.lastSequence)) return false;
      e.lastSequence = sequence;
      e.touchedMs = nowMs;
      return true;
    }
  }

  int target = -1;
  for (uint8_t i = 0; i < SEEN_CACHE_SIZE; ++i) {
    if (!seenCache[i].used) {
      target = i;
      break;
    }
  }
  if (target < 0) {
    target = 0;
    for (uint8_t i = 1; i < SEEN_CACHE_SIZE; ++i) {
      if ((uint32_t)(nowMs - seenCache[i].touchedMs) >
          (uint32_t)(nowMs - seenCache[target].touchedMs)) {
        target = i;
      }
    }
  }

  seenCache[target].used = true;
  seenCache[target].senderId = remoteSender;
  seenCache[target].sessionId = remoteSession;
  seenCache[target].lastSequence = sequence;
  seenCache[target].touchedMs = nowMs;
  return true;
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

const char* modeName(uint8_t mode) {
  switch (mode) {
    case 1: return "White";
    case 2: return "Red";
    case 3: return "Yellow";
    case 4: return "Green";
    case 5: return "Purple";
    case 6: return "Rainbow";
    case 7: return "Color Cycle";
    case 8: return "Fargmyller";
    case 9: return "Off / Black";
    default: return "Invalid";
  }
}

uint8_t brightnessPercent() {
  return cmcfg::BRIGHTNESS_LEVELS_PERCENT[brightnessIndex];
}

void saveLocalSettings() {
  preferences.putUChar("mode", localMode);
  preferences.putUChar("bright", brightnessIndex);
}

void setLocalMode(uint8_t mode, bool persist = true) {
  if (mode < 1 || mode > 9) return;
  localMode = mode;
  if (localMode == 8) localFargmyllerResolvedMode = cmrender::randomMode1to7();
  if (persist) saveLocalSettings();
  Serial.printf("Local mode -> %u (%s)\n", localMode, modeName(localMode));
}

void nextLocalMode() {
  uint8_t next = localMode + 1;
  if (next > 9) next = 1;
  setLocalMode(next);
}

void nextBrightness() {
  brightnessIndex = (brightnessIndex + 1) % 4;
  saveLocalSettings();
  Serial.printf("Brightness -> %u%%\n", brightnessPercent());
}

void printSenderId(uint64_t id) {
  Serial.printf("%012llX", (unsigned long long)(id & 0x0000FFFFFFFFFFFFULL));
}

void printStatus() {
  const uint32_t meshLow = mesh.getNodeTime();
  const size_t nodeCount = mesh.getNodeList().size() + 1;
  Serial.println("\n--- Critical Mass status ---");
  Serial.print("senderId: 0x"); printSenderId(senderId); Serial.println();
  Serial.printf("sessionId: 0x%08lX\n", (unsigned long)sessionId);
  Serial.printf("next sequence: %lu\n", (unsigned long)nextSequence);
  Serial.printf("mode: %u (%s)\n", localMode, modeName(localMode));
  if (localMode == 8) Serial.printf("fargmyller resolved mode now: %u (%s)\n", localFargmyllerResolvedMode, modeName(localFargmyllerResolvedMode));
  Serial.printf("brightness: %u%%\n", brightnessPercent());
  Serial.printf("mesh nodes including self: %u\n", (unsigned)nodeCount);
  Serial.printf("normal heartbeat test BPM: %.1f\n", TimingEngine::normalBpm(nodeCount));
  Serial.printf("painlessMesh getNodeTime(): %lu us (uint32)\n", (unsigned long)meshLow);
  Serial.printf("beat schedule stored: %s\n", beatSchedule.valid() ? "yes" : "no");
  Serial.printf("visible emotes: %u\n", emoteMixer.visibleCount());
  Serial.println("----------------------------\n");
}

void printHelp() {
  Serial.println("\nSerial test commands:");
  Serial.println("  1..9  set local mode directly");
  Serial.println("  m     next local mode (same as single click)");
  Serial.println("  b     next brightness (same as long press)");
  Serial.println("  e     broadcast current mode as EMOTE (same as double click)");
  Serial.println("  t     broadcast a demo 120 BPM BEAT_SCHEDULE for ~6 s");
  Serial.println("  c     clear local beat schedule (debug only, not a wire command)");
  Serial.println("  s     print status");
  Serial.println("  h     print this help\n");
}

// -----------------------------------------------------------------------------
// Mesh protocol send/receive
// -----------------------------------------------------------------------------

void sendCurrentEmote() {
  const uint32_t nowMs = millis();

  if (localMode == 9) {
    Serial.println("EMOTE blocked: local mode 9 (Off/Black) is local-only.");
    return;
  }

  if (emoteSentOnce && (uint32_t)(nowMs - lastEmoteSentMs) < cmcfg::EMOTE_COOLDOWN_MS) {
    Serial.println("EMOTE blocked: 1000 ms local cooldown active.");
    return;
  }

  cmproto::EmoteMessage msg;
  msg.senderId = senderId;
  msg.sessionId = sessionId;
  msg.sequence = nextSequence++;
  msg.mode = localMode;

  String payload;
  if (!cmproto::encodeEmote(msg, payload)) {
    Serial.println("ERROR: could not encode EMOTE.");
    return;
  }

  if (mesh.sendBroadcast(payload)) {
    emoteSentOnce = true;
    lastEmoteSentMs = nowMs;
    Serial.printf("TX EMOTE seq=%lu mode=%u (%s), Base64 bytes=%u\n",
                  (unsigned long)msg.sequence, msg.mode, modeName(msg.mode), (unsigned)payload.length());
  } else {
    Serial.println("ERROR: mesh.sendBroadcast(EMOTE) failed.");
  }
}

void handleBeatSchedule(const cmproto::BeatScheduleMessage& msg, bool localSource = false) {
  String reason;
  const uint32_t lowNow = mesh.getNodeTime();
  const bool accepted = beatSchedule.ingest(msg, lowNow, reason);

  Serial.print(localSource ? "LOCAL " : "RX ");
  Serial.print("BEAT_SCHEDULE session=0x");
  Serial.print(msg.sourceSessionId, HEX);
  Serial.printf(" count=%u ts=%llu -> %s\n",
                msg.beatCount,
                (unsigned long long)msg.scheduleTimestamp,
                reason.c_str());

  if (!accepted && reason == "different sourceSessionId owns timing") {
    Serial.println("  (another beat-control session currently owns timing)");
  }
}

void sendDemoBeatSchedule() {
  // This is only a test source. A future music analyzer will call the same protocol
  // encoder with its calculated absolute beat timestamps instead of a fixed 500 ms grid.
  const uint32_t lowNow = mesh.getNodeTime();
  const uint64_t now64 = scheduleSenderClock.update(lowNow);
  constexpr uint64_t periodUs = 500000ULL; // demo only: 120 BPM

  cmproto::BeatScheduleMessage msg;
  msg.senderId = senderId;
  msg.sourceSessionId = sessionId;
  msg.scheduleTimestamp = now64;

  const uint64_t currentBeat = (now64 / periodUs) * periodUs;
  uint64_t t = currentBeat >= periodUs ? currentBeat - periodUs : currentBeat;
  const uint64_t horizon = now64 + cmcfg::BEAT_SCHEDULE_TARGET_US;

  while (msg.beatCount < cmcfg::MAX_BEAT_TIMES && t <= horizon + periodUs) {
    msg.beatTimes[msg.beatCount++] = t;
    t += periodUs;
  }

  if (msg.beatCount < 2) {
    Serial.println("ERROR: demo schedule too short.");
    return;
  }

  String payload;
  if (!cmproto::encodeBeatSchedule(msg, payload)) {
    Serial.println("ERROR: could not encode BEAT_SCHEDULE.");
    return;
  }

  // Broadcast excludes self by default, so apply the same schedule locally.
  handleBeatSchedule(msg, true);

  if (mesh.sendBroadcast(payload)) {
    Serial.printf("TX BEAT_SCHEDULE count=%u, Base64 bytes=%u\n", msg.beatCount, (unsigned)payload.length());

    // Protocol permits exact retransmission with the same scheduleTimestamp.
    retransmitPayload = payload;
    retransmitPending = true;
    retransmitAtMs = millis() + 120;
  } else {
    Serial.println("ERROR: mesh.sendBroadcast(BEAT_SCHEDULE) failed.");
  }
}

void receivedCallback(uint32_t from, String& payload) {
  cmproto::DecodedMessage decoded;
  if (!cmproto::decodeMessage(payload, decoded)) {
    Serial.printf("RX from painless node %lu: ignored (not valid protocol-v1 Base64/CBOR)\n", (unsigned long)from);
    return;
  }

  const uint32_t nowMs = millis();

  if (decoded.type == cmproto::MSG_EMOTE) {
    const cmproto::EmoteMessage& msg = decoded.emote;
    if (msg.mode < 1 || msg.mode > 8) {
      Serial.printf("RX EMOTE ignored: invalid wire mode %u\n", msg.mode);
      return;
    }

    if (!acceptSequence(msg.senderId, msg.sessionId, msg.sequence, nowMs)) {
      Serial.printf("RX EMOTE ignored: duplicate/stale seq=%lu\n", (unsigned long)msg.sequence);
      return;
    }

    emoteMixer.add(msg.senderId, msg.sessionId, msg.sequence, msg.mode, nowMs);
    Serial.print("RX EMOTE sender=0x"); printSenderId(msg.senderId);
    Serial.printf(" session=0x%08lX seq=%lu mode=%u (%s)\n",
                  (unsigned long)msg.sessionId,
                  (unsigned long)msg.sequence,
                  msg.mode,
                  modeName(msg.mode));
    return;
  }

  if (decoded.type == cmproto::MSG_BEAT_SCHEDULE) {
    handleBeatSchedule(decoded.schedule, false);
    return;
  }
}

void newConnectionCallback(uint32_t nodeId) {
  Serial.printf("New direct mesh connection: painless nodeId=%lu\n", (unsigned long)nodeId);
  topologyDirty = true;
}

void changedConnectionCallback() {
  Serial.println("Mesh topology changed.");
  topologyDirty = true;
}

void nodeTimeAdjustedCallback(int32_t offset) {
  Serial.printf("Mesh time adjusted: low=%lu us, offset=%ld us\n",
                (unsigned long)mesh.getNodeTime(), (long)offset);
}

// -----------------------------------------------------------------------------
// Input, topology and rendering
// -----------------------------------------------------------------------------

void handleTouchEvent(TouchEvent event) {
  switch (event) {
    case TouchEvent::SingleClick:
      Serial.println("Touch: single click");
      nextLocalMode();
      break;
    case TouchEvent::DoubleClick:
      Serial.println("Touch: double click");
      sendCurrentEmote();
      break;
    case TouchEvent::LongPress:
      Serial.println("Touch: long press");
      nextBrightness();
      break;
    default:
      break;
  }
}

void updateTopology(uint32_t nowMs) {
  if (!topologyDirty && (uint32_t)(nowMs - lastTopologyPollMs) < 500) return;
  topologyDirty = false;
  lastTopologyPollMs = nowMs;

  const size_t current = mesh.getNodeList().size() + 1;
  if (current > knownNodeCount) {
    Serial.printf("Node count increased %u -> %u: greeting triggered.\n",
                  (unsigned)knownNodeCount, (unsigned)current);
    greetingLayer.trigger(nowMs);
  }
  knownNodeCount = current;
}

void handlePulseBoundary() {
  if (localMode == 8) {
    localFargmyllerResolvedMode = cmrender::randomMode1to7();
    Serial.printf("Fargmyller local choice -> %u (%s)\n",
                  localFargmyllerResolvedMode, modeName(localFargmyllerResolvedMode));
  }
  emoteMixer.onPulseBoundary();
}

void buildBaseFrame(uint32_t nowMs) {
  if (emoteMixer.hasVisibleEmotes()) {
    emoteMixer.render(nowMs, logicalFrame);
    return;
  }

  uint8_t renderMode = localMode;
  if (localMode == 8) renderMode = localFargmyllerResolvedMode;
  cmrender::renderModeFrame(renderMode, nowMs, logicalFrame);
}

void pushLogicalFrameToHardware() {
  for (uint8_t row = 0; row < cmcfg::ROWS; ++row) {
    const uint8_t leftIndex = cmcfg::LEFT_REVERSED ? (cmcfg::ROWS - 1 - row) : row;
    const uint8_t rightIndex = cmcfg::RIGHT_REVERSED ? (cmcfg::ROWS - 1 - row) : row;
    leftLeds[leftIndex] = logicalFrame[row];
    rightLeds[rightIndex] = logicalFrame[cmcfg::ROWS + row];
  }
  FastLED.show();
}

void renderFrame(uint32_t nowMs) {
  const uint32_t meshLow = mesh.getNodeTime();
  const TimingSample timing = timingEngine.sample(meshLow, knownNodeCount);

  if (timing.pulseBoundary) handlePulseBoundary();

  buildBaseFrame(nowMs);
  cmrender::applyHeartbeatAndBrightness(logicalFrame, timing.phase, brightnessPercent());

  if (greetingLayer.blackNow(nowMs)) {
    for (uint8_t i = 0; i < cmcfg::NUM_LEDS; ++i) logicalFrame[i] = CRGB::Black;
  }

  pushLogicalFrameToHardware();
}

void handleSerial() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n' || c == ' ') continue;

    if (c >= '1' && c <= '9') {
      setLocalMode((uint8_t)(c - '0'));
      continue;
    }

    switch (c) {
      case 'm': case 'M': nextLocalMode(); break;
      case 'b': case 'B': nextBrightness(); break;
      case 'e': case 'E': sendCurrentEmote(); break;
      case 't': case 'T': sendDemoBeatSchedule(); break;
      case 'c': case 'C': beatSchedule.clear(); Serial.println("Local beat schedule cleared (debug only)."); break;
      case 's': case 'S': printStatus(); break;
      case 'h': case 'H': printHelp(); break;
      default: Serial.printf("Unknown command '%c'. Press h for help.\n", c); break;
    }
  }
}

// -----------------------------------------------------------------------------
// Arduino setup/loop
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println("\nCritical Mass Mesh Light - v16/v2 heart-pulse bench firmware");

  // Stable hardware identity: direct eFuse MAC, no hash.
  senderId = ESP.getEfuseMac() & 0x0000FFFFFFFFFFFFULL;
  sessionId = esp_random();

  Serial.print("senderId (eFuse MAC): 0x"); printSenderId(senderId); Serial.println();
  Serial.printf("sessionId (esp_random): 0x%08lX\n", (unsigned long)sessionId);

  preferences.begin("critmass", false);
  localMode = preferences.getUChar("mode", 1);
  brightnessIndex = preferences.getUChar("bright", 3);
  if (localMode < 1 || localMode > 9) localMode = 1;
  if (brightnessIndex > 3) brightnessIndex = 3;
  localFargmyllerResolvedMode = cmrender::randomMode1to7();

  touchButton.begin();

  FastLED.addLeds<WS2812B, cmcfg::PIN_LED_LEFT, GRB>(leftLeds, cmcfg::LEDS_PER_STRIP);
  FastLED.addLeds<WS2812B, cmcfg::PIN_LED_RIGHT, GRB>(rightLeds, cmcfg::LEDS_PER_STRIP);
  FastLED.setBrightness(255); // local brightness is applied in the renderer
  FastLED.setMaxPowerInVoltsAndMilliamps(5, cmcfg::LED_POWER_LIMIT_MA);
  FastLED.clear(true);

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(cmcfg::MESH_PREFIX, cmcfg::MESH_PASSWORD, &userScheduler, cmcfg::MESH_PORT);
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);

  knownNodeCount = mesh.getNodeList().size() + 1;

  Serial.printf("Restored mode=%u (%s), brightness=%u%%\n", localMode, modeName(localMode), brightnessPercent());
  printHelp();
}

void loop() {
  mesh.update();

  // Keep the sender's 64-bit extension observing every 32-bit wrap.
  scheduleSenderClock.update(mesh.getNodeTime());

  const uint32_t nowMs = millis();
  handleTouchEvent(touchButton.update(nowMs));
  handleSerial();
  updateTopology(nowMs);

  if (retransmitPending && (int32_t)(nowMs - retransmitAtMs) >= 0) {
    retransmitPending = false;
    if (mesh.sendBroadcast(retransmitPayload)) {
      Serial.println("TX BEAT_SCHEDULE exact retransmission sent.");
    }
    retransmitPayload = "";
  }

  if ((uint32_t)(nowMs - lastFrameMs) >= cmcfg::FRAME_INTERVAL_MS) {
    lastFrameMs = nowMs;
    renderFrame(nowMs);
  }
}
