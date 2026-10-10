#pragma once

#include <Arduino.h>
#include <string.h>

namespace minicbor {

class Writer {
public:
  Writer(uint8_t* data, size_t capacity) : data_(data), capacity_(capacity) {}

  bool ok() const { return ok_; }
  size_t size() const { return pos_; }

  bool putUnsigned(uint64_t value) { return putTypeValue(0, value); }
  bool putArray(size_t count)       { return putTypeValue(4, count); }
  bool putMap(size_t count)         { return putTypeValue(5, count); }

private:
  uint8_t* data_;
  size_t capacity_;
  size_t pos_ = 0;
  bool ok_ = true;

  bool putByte(uint8_t b) {
    if (!ok_ || pos_ >= capacity_) {
      ok_ = false;
      return false;
    }
    data_[pos_++] = b;
    return true;
  }

  bool putBE(uint64_t value, uint8_t bytes) {
    for (int i = bytes - 1; i >= 0; --i) {
      if (!putByte((uint8_t)((value >> (i * 8)) & 0xFF))) return false;
    }
    return true;
  }

  bool putTypeValue(uint8_t major, uint64_t value) {
    if (value < 24) {
      return putByte((major << 5) | (uint8_t)value);
    }
    if (value <= 0xFFULL) {
      return putByte((major << 5) | 24) && putBE(value, 1);
    }
    if (value <= 0xFFFFULL) {
      return putByte((major << 5) | 25) && putBE(value, 2);
    }
    if (value <= 0xFFFFFFFFULL) {
      return putByte((major << 5) | 26) && putBE(value, 4);
    }
    return putByte((major << 5) | 27) && putBE(value, 8);
  }
};

class Reader {
public:
  Reader(const uint8_t* data, size_t length) : data_(data), length_(length) {}

  bool ok() const { return ok_; }
  size_t remaining() const { return pos_ <= length_ ? length_ - pos_ : 0; }

  bool readUnsigned(uint64_t& value) {
    uint8_t major = 0;
    return readHead(major, value) && major == 0;
  }

  bool readMapSize(uint64_t& count) {
    uint8_t major = 0;
    return readHead(major, count) && major == 5;
  }

  bool readArraySize(uint64_t& count) {
    uint8_t major = 0;
    return readHead(major, count) && major == 4;
  }

  // Skip one definite-length CBOR item. Sufficient for forward-compatible unknown keys.
  bool skipItem(uint8_t depth = 0) {
    if (depth > 8) return fail();

    uint8_t major = 0;
    uint64_t value = 0;
    if (!readHead(major, value)) return false;

    switch (major) {
      case 0: // unsigned
      case 1: // negative integer
      case 7: // simple / float head already consumed; floats with payload handled by readHead only partially
        return true;

      case 2: // byte string
      case 3: // text string
        if (value > remaining()) return fail();
        pos_ += (size_t)value;
        return true;

      case 4: // array
        for (uint64_t i = 0; i < value; ++i) {
          if (!skipItem(depth + 1)) return false;
        }
        return true;

      case 5: // map
        for (uint64_t i = 0; i < value; ++i) {
          if (!skipItem(depth + 1)) return false;
          if (!skipItem(depth + 1)) return false;
        }
        return true;

      default:
        return fail();
    }
  }

private:
  const uint8_t* data_;
  size_t length_;
  size_t pos_ = 0;
  bool ok_ = true;

  bool fail() {
    ok_ = false;
    return false;
  }

  bool getByte(uint8_t& out) {
    if (!ok_ || pos_ >= length_) return fail();
    out = data_[pos_++];
    return true;
  }

  bool getBE(uint8_t bytes, uint64_t& out) {
    if (remaining() < bytes) return fail();
    out = 0;
    for (uint8_t i = 0; i < bytes; ++i) {
      out = (out << 8) | data_[pos_++];
    }
    return true;
  }

  bool readHead(uint8_t& major, uint64_t& value) {
    uint8_t initial = 0;
    if (!getByte(initial)) return false;
    major = initial >> 5;
    const uint8_t ai = initial & 0x1F;

    if (ai < 24) {
      value = ai;
      return true;
    }
    if (ai == 24) return getBE(1, value);
    if (ai == 25) return getBE(2, value);
    if (ai == 26) return getBE(4, value);
    if (ai == 27) return getBE(8, value);

    // Indefinite-length items are deliberately not accepted in protocol v1.
    return fail();
  }
};

} // namespace minicbor
