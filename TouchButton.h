#pragma once

#include <Arduino.h>
#include "Config.h"

enum class TouchEvent : uint8_t {
  None,
  SingleClick,
  DoubleClick,
  LongPress,
};

class TouchButton {
public:
  explicit TouchButton(uint8_t pin) : pin_(pin) {}

  void begin() {
    pinMode(pin_, INPUT_PULLDOWN);
    rawState_ = digitalRead(pin_);
    stableState_ = rawState_;
    lastRawChangeMs_ = millis();
  }

  TouchEvent update(uint32_t nowMs) {
    const bool raw = digitalRead(pin_) == HIGH;

    if (raw != rawState_) {
      rawState_ = raw;
      lastRawChangeMs_ = nowMs;
    }

    if (rawState_ != stableState_ && (uint32_t)(nowMs - lastRawChangeMs_) >= cmcfg::TOUCH_DEBOUNCE_MS) {
      stableState_ = rawState_;
      if (stableState_) {
        pressStartedMs_ = nowMs;
        longFired_ = false;
      } else {
        if (longFired_) {
          clickCount_ = 0;
        } else {
          ++clickCount_;
          lastReleaseMs_ = nowMs;
          if (clickCount_ >= 2) {
            clickCount_ = 0;
            return TouchEvent::DoubleClick;
          }
        }
      }
    }

    if (stableState_ && !longFired_ &&
        (uint32_t)(nowMs - pressStartedMs_) >= cmcfg::LONG_PRESS_MS) {
      longFired_ = true;
      clickCount_ = 0;
      return TouchEvent::LongPress;
    }

    if (!stableState_ && clickCount_ == 1 &&
        (uint32_t)(nowMs - lastReleaseMs_) >= cmcfg::DOUBLE_CLICK_MS) {
      clickCount_ = 0;
      return TouchEvent::SingleClick;
    }

    return TouchEvent::None;
  }

  bool pressed() const { return stableState_; }

private:
  uint8_t pin_;
  bool rawState_ = false;
  bool stableState_ = false;
  bool longFired_ = false;
  uint8_t clickCount_ = 0;
  uint32_t lastRawChangeMs_ = 0;
  uint32_t pressStartedMs_ = 0;
  uint32_t lastReleaseMs_ = 0;
};
