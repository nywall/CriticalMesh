#pragma once

#include <Arduino.h>
#include <mbedtls/base64.h>
#include "Config.h"

namespace cmtransport {

inline bool encodeBase64(const uint8_t* input, size_t inputLen, String& output) {
  unsigned char buffer[cmcfg::MAX_BASE64_BYTES + 1];
  size_t written = 0;
  const int rc = mbedtls_base64_encode(
      buffer,
      cmcfg::MAX_BASE64_BYTES,
      &written,
      input,
      inputLen);

  if (rc != 0 || written > cmcfg::MAX_BASE64_BYTES) return false;
  buffer[written] = '\0';
  output = String(reinterpret_cast<const char*>(buffer));
  return true;
}

inline bool decodeBase64(const String& input, uint8_t* output, size_t outputCapacity, size_t& outputLen) {
  outputLen = 0;
  const int rc = mbedtls_base64_decode(
      output,
      outputCapacity,
      &outputLen,
      reinterpret_cast<const unsigned char*>(input.c_str()),
      input.length());
  return rc == 0;
}

} // namespace cmtransport
