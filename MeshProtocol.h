#pragma once

#include <Arduino.h>
#include "Config.h"
#include "MiniCbor.h"
#include "Base64Transport.h"

namespace cmproto {

enum MessageType : uint8_t {
  MSG_INVALID = 0,
  MSG_EMOTE = 1,
  MSG_BEAT_SCHEDULE = 2,
};

struct EmoteMessage {
  uint8_t protocolVersion = cmcfg::PROTOCOL_VERSION;
  uint64_t senderId = 0;
  uint32_t sessionId = 0;
  uint32_t sequence = 0;
  uint8_t mode = 0;
};

struct BeatScheduleMessage {
  uint8_t protocolVersion = cmcfg::PROTOCOL_VERSION;
  uint64_t senderId = 0;
  uint32_t sourceSessionId = 0;
  uint64_t scheduleTimestamp = 0;
  uint8_t beatCount = 0;
  uint64_t beatTimes[cmcfg::MAX_BEAT_TIMES] = {};
};

struct DecodedMessage {
  MessageType type = MSG_INVALID;
  EmoteMessage emote;
  BeatScheduleMessage schedule;
};

inline bool encodeEmote(const EmoteMessage& msg, String& base64) {
  uint8_t cbor[cmcfg::MAX_CBOR_BYTES];
  minicbor::Writer w(cbor, sizeof(cbor));

  w.putMap(6);
  w.putUnsigned(0); w.putUnsigned(msg.protocolVersion);
  w.putUnsigned(1); w.putUnsigned(MSG_EMOTE);
  w.putUnsigned(2); w.putUnsigned(msg.senderId);
  w.putUnsigned(3); w.putUnsigned(msg.sessionId);
  w.putUnsigned(4); w.putUnsigned(msg.sequence);
  w.putUnsigned(7); w.putUnsigned(msg.mode);

  if (!w.ok()) return false;
  return cmtransport::encodeBase64(cbor, w.size(), base64);
}

inline bool encodeBeatSchedule(const BeatScheduleMessage& msg, String& base64) {
  if (msg.beatCount < 2 || msg.beatCount > cmcfg::MAX_BEAT_TIMES) return false;

  uint8_t cbor[cmcfg::MAX_CBOR_BYTES];
  minicbor::Writer w(cbor, sizeof(cbor));

  w.putMap(6);
  w.putUnsigned(0); w.putUnsigned(msg.protocolVersion);
  w.putUnsigned(1); w.putUnsigned(MSG_BEAT_SCHEDULE);
  w.putUnsigned(2); w.putUnsigned(msg.senderId);
  w.putUnsigned(3); w.putUnsigned(msg.sourceSessionId);
  w.putUnsigned(5); w.putUnsigned(msg.scheduleTimestamp);
  w.putUnsigned(6); w.putArray(msg.beatCount);
  for (uint8_t i = 0; i < msg.beatCount; ++i) w.putUnsigned(msg.beatTimes[i]);

  if (!w.ok()) return false;
  return cmtransport::encodeBase64(cbor, w.size(), base64);
}

inline bool decodeMessage(const String& base64, DecodedMessage& out) {
  uint8_t cbor[cmcfg::MAX_CBOR_BYTES];
  size_t cborLen = 0;
  if (!cmtransport::decodeBase64(base64, cbor, sizeof(cbor), cborLen)) return false;

  minicbor::Reader r(cbor, cborLen);
  uint64_t mapCount = 0;
  if (!r.readMapSize(mapCount) || mapCount > 32) return false;

  bool hasVersion = false;
  bool hasType = false;
  bool hasSender = false;
  bool hasSession = false;
  bool hasSequence = false;
  bool hasScheduleTimestamp = false;
  bool hasBeatTimes = false;
  bool hasMode = false;

  uint64_t version = 0;
  uint64_t type = 0;
  uint64_t sender = 0;
  uint64_t session = 0;
  uint64_t sequence = 0;
  uint64_t scheduleTimestamp = 0;
  uint64_t mode = 0;
  uint8_t beatCount = 0;
  uint64_t beatTimes[cmcfg::MAX_BEAT_TIMES] = {};

  for (uint64_t i = 0; i < mapCount; ++i) {
    uint64_t key = 0;
    if (!r.readUnsigned(key)) return false;

    switch (key) {
      case 0:
        if (!r.readUnsigned(version)) return false;
        hasVersion = true;
        break;
      case 1:
        if (!r.readUnsigned(type)) return false;
        hasType = true;
        break;
      case 2:
        if (!r.readUnsigned(sender)) return false;
        hasSender = true;
        break;
      case 3:
        if (!r.readUnsigned(session)) return false;
        hasSession = true;
        break;
      case 4:
        if (!r.readUnsigned(sequence)) return false;
        hasSequence = true;
        break;
      case 5:
        if (!r.readUnsigned(scheduleTimestamp)) return false;
        hasScheduleTimestamp = true;
        break;
      case 6: {
        uint64_t count = 0;
        if (!r.readArraySize(count) || count > cmcfg::MAX_BEAT_TIMES) return false;
        beatCount = (uint8_t)count;
        for (uint8_t j = 0; j < beatCount; ++j) {
          if (!r.readUnsigned(beatTimes[j])) return false;
        }
        hasBeatTimes = true;
        break;
      }
      case 7:
        if (!r.readUnsigned(mode)) return false;
        hasMode = true;
        break;
      default:
        if (!r.skipItem()) return false;
        break;
    }
  }

  if (!hasVersion || !hasType || version != cmcfg::PROTOCOL_VERSION) return false;
  if (type > 255) return false;

  if (type == MSG_EMOTE) {
    if (!hasSender || !hasSession || !hasSequence || !hasMode) return false;
    if (sender > 0x0000FFFFFFFFFFFFULL || session > 0xFFFFFFFFULL || sequence > 0xFFFFFFFFULL || mode > 0xFFULL) return false;

    out.type = MSG_EMOTE;
    out.emote.protocolVersion = (uint8_t)version;
    out.emote.senderId = sender;
    out.emote.sessionId = (uint32_t)session;
    out.emote.sequence = (uint32_t)sequence;
    out.emote.mode = (uint8_t)mode;
    return true;
  }

  if (type == MSG_BEAT_SCHEDULE) {
    if (!hasSender || !hasSession || !hasScheduleTimestamp || !hasBeatTimes) return false;
    if (sender > 0x0000FFFFFFFFFFFFULL || session > 0xFFFFFFFFULL || beatCount < 2) return false;

    for (uint8_t i = 1; i < beatCount; ++i) {
      if (beatTimes[i] <= beatTimes[i - 1]) return false;
    }

    out.type = MSG_BEAT_SCHEDULE;
    out.schedule.protocolVersion = (uint8_t)version;
    out.schedule.senderId = sender;
    out.schedule.sourceSessionId = (uint32_t)session;
    out.schedule.scheduleTimestamp = scheduleTimestamp;
    out.schedule.beatCount = beatCount;
    for (uint8_t i = 0; i < beatCount; ++i) out.schedule.beatTimes[i] = beatTimes[i];
    return true;
  }

  return false;
}

inline bool sequenceIsNewer(uint32_t candidate, uint32_t last) {
  const uint32_t delta = candidate - last;
  return delta != 0 && delta < 0x80000000UL;
}

} // namespace cmproto
