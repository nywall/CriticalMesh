#pragma once

#include <Arduino.h>
#include <math.h>
#include "Config.h"
#include "MeshProtocol.h"

struct TimingSample {
  float phase = 0.0f;          // 0..1
  bool pulseBoundary = false;  // true exactly when a new pulse interval is observed locally
  bool usingSchedule = false;
};

// Local extender for a sender creating uint64 schedule timestamps from painlessMesh's
// uint32 microsecond clock. It is valid for the lifetime of this boot session.
class MeshClock64Sender {
public:
  uint64_t update(uint32_t low) {
    if (!initialized_) {
      initialized_ = true;
      lastLow_ = low;
      high_ = 0;
      return (high_ | low);
    }

    // Detect a true 32-bit wrap, not a small SNTP correction.
    if (low < lastLow_ && (uint32_t)(lastLow_ - low) > 0x80000000UL) {
      high_ += (1ULL << 32);
    }
    lastLow_ = low;
    return high_ | low;
  }

private:
  bool initialized_ = false;
  uint32_t lastLow_ = 0;
  uint64_t high_ = 0;
};

// Expand the current 32-bit painlessMesh time into the 64-bit epoch nearest a
// received schedule reference timestamp. Because schedules cover only ~6 s,
// choosing the nearest 2^32 epoch is unambiguous.
inline uint64_t expandMeshTimeNear(uint32_t low, uint64_t reference) {
  const uint64_t epoch = reference & 0xFFFFFFFF00000000ULL;
  uint64_t candidate = epoch | (uint64_t)low;

  if (candidate + 0x80000000ULL < reference) {
    candidate += (1ULL << 32);
  } else if (candidate > reference + 0x80000000ULL && candidate >= (1ULL << 32)) {
    candidate -= (1ULL << 32);
  }
  return candidate;
}

class BeatScheduleState {
public:
  bool ingest(const cmproto::BeatScheduleMessage& incoming, uint32_t meshLowNow, String& reason) {
    const uint64_t now = expandMeshTimeNear(meshLowNow, incoming.scheduleTimestamp);

    if (incoming.beatCount < 2 || incoming.beatCount > cmcfg::MAX_BEAT_TIMES) {
      reason = "invalid beat count";
      return false;
    }
    if (incoming.beatTimes[incoming.beatCount - 1] <= now) {
      reason = "schedule expired";
      return false;
    }

    // Active ownership blocks other controller sessions until current final beat passes.
    if (valid_) {
      const uint64_t activeNow = expandMeshTimeNear(meshLowNow, schedule_.scheduleTimestamp);
      const bool stillOwned = activeNow < schedule_.beatTimes[schedule_.beatCount - 1];
      const bool sameOwner = incoming.sourceSessionId == schedule_.sourceSessionId;
      if (stillOwned && !sameOwner) {
        reason = "different sourceSessionId owns timing";
        return false;
      }

      if (sameOwner) {
        if (incoming.scheduleTimestamp == schedule_.scheduleTimestamp) {
          reason = "duplicate schedule";
          return false;
        }
        if (incoming.scheduleTimestamp < schedule_.scheduleTimestamp) {
          reason = "stale schedule";
          return false;
        }
      }
    }

    schedule_ = incoming;
    valid_ = true;
    reason = "accepted";
    return true;
  }

  bool phaseAt(uint32_t meshLowNow, float& phase, uint64_t& intervalStart) {
    if (!valid_) return false;

    const uint64_t now = expandMeshTimeNear(meshLowNow, schedule_.scheduleTimestamp);
    const uint64_t last = schedule_.beatTimes[schedule_.beatCount - 1];
    if (now >= last) {
      valid_ = false;
      return false;
    }

    if (now < schedule_.beatTimes[0]) {
      // Ownership is active, but visual timing remains normal until first defined interval begins.
      return false;
    }

    for (uint8_t i = 0; i + 1 < schedule_.beatCount; ++i) {
      const uint64_t a = schedule_.beatTimes[i];
      const uint64_t b = schedule_.beatTimes[i + 1];
      if (a <= now && now < b) {
        phase = (float)(now - a) / (float)(b - a);
        intervalStart = a;
        return true;
      }
    }
    return false;
  }

  bool ownershipActive(uint32_t meshLowNow) const {
    if (!valid_) return false;
    const uint64_t now = expandMeshTimeNear(meshLowNow, schedule_.scheduleTimestamp);
    return now < schedule_.beatTimes[schedule_.beatCount - 1];
  }

  void clear() { valid_ = false; }
  bool valid() const { return valid_; }
  uint32_t sourceSessionId() const { return schedule_.sourceSessionId; }
  uint64_t scheduleTimestamp() const { return schedule_.scheduleTimestamp; }

private:
  bool valid_ = false;
  cmproto::BeatScheduleMessage schedule_;
};

class TimingEngine {
public:
  TimingEngine(BeatScheduleState& schedule) : schedule_(schedule) {}

  TimingSample sample(uint32_t meshLowNow, size_t nodeCount) {
    TimingSample out;

    float schedulePhase = 0.0f;
    uint64_t intervalStart = 0;
    if (schedule_.phaseAt(meshLowNow, schedulePhase, intervalStart)) {
      out.phase = constrain(schedulePhase, 0.0f, 0.999999f);
      out.usingSchedule = true;

      if (!wasSchedule_) {
        // Entering in the middle of a scheduled interval must not fabricate a boundary.
        lastScheduleIntervalStart_ = intervalStart;
        out.pulseBoundary = false;
      } else if (intervalStart != lastScheduleIntervalStart_) {
        out.pulseBoundary = true;
        lastScheduleIntervalStart_ = intervalStart;
      }
      wasSchedule_ = true;
      normalInitialized_ = false;
      return out;
    }

    wasSchedule_ = false;

    const float bpm = normalBpm(nodeCount);
    uint32_t periodUs = (uint32_t)(60000000.0f / bpm);
    if (periodUs < 100000UL) periodUs = 100000UL;

    const uint32_t remainder = meshLowNow % periodUs;
    out.phase = (float)remainder / (float)periodUs;
    out.usingSchedule = false;

    if (!normalInitialized_ || periodUs != lastNormalPeriodUs_) {
      normalInitialized_ = true;
      lastNormalPhase_ = out.phase;
      lastNormalPeriodUs_ = periodUs;
      out.pulseBoundary = false;
    } else {
      // Phase wrap = new heartbeat. A 0.5 guard avoids treating tiny time adjustments as a pulse.
      out.pulseBoundary = (out.phase + 0.5f < lastNormalPhase_);
      lastNormalPhase_ = out.phase;
    }

    return out;
  }

  static float normalBpm(size_t nodeCount) {
    if (nodeCount < 1) nodeCount = 1;
    const float raw = cmcfg::HEARTBEAT_MIN_BPM +
                      cmcfg::HEARTBEAT_BPM_PER_EXTRA_NODE * (float)(nodeCount - 1);
    return constrain(raw, cmcfg::HEARTBEAT_MIN_BPM, cmcfg::HEARTBEAT_MAX_BPM);
  }

private:
  BeatScheduleState& schedule_;
  bool wasSchedule_ = false;
  uint64_t lastScheduleIntervalStart_ = 0;

  bool normalInitialized_ = false;
  float lastNormalPhase_ = 0.0f;
  uint32_t lastNormalPeriodUs_ = 0;
};
