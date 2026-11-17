#include "XwaCrc.h"

#if defined(ESP_PLATFORM)
#include <esp_rom_crc.h>
#endif

namespace xwa {

namespace {
// Reflected polynomial 0xEDB88320, four bits at a time: a 64-byte table.
constexpr uint32_t NIBBLE[16] = {0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
                                 0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
                                 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};

uint32_t softCrc32(uint32_t crc, const uint8_t* p, size_t len) {
  crc = ~crc;
  while (len--) {
    crc ^= *p++;
    crc = (crc >> 4) ^ NIBBLE[crc & 15];
    crc = (crc >> 4) ^ NIBBLE[crc & 15];
  }
  return ~crc;
}

#if defined(ESP_PLATFORM)
// The ROM routine is far faster; use it only once it has proven to compute
// this CRC (check value 0xCBF43926), so a ROM with other conventions cannot
// make every archive look damaged.
bool romMatches() {
  static const bool ok = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u;
  return ok;
}
#endif
}  // namespace

uint32_t crc32(uint32_t crc, const void* data, size_t len) {
  const auto* p = static_cast<const uint8_t*>(data);
#if defined(ESP_PLATFORM)
  if (romMatches()) return esp_rom_crc32_le(crc, p, static_cast<uint32_t>(len));
#endif
  return softCrc32(crc, p, len);
}

}  // namespace xwa
