#pragma once

#include <cstddef>
#include <cstdint>

namespace xwa {

// CRC-32 as in zlib and PNG (spec 2): crc32(0, data, len) is the checksum of
// data; pass the previous result to continue over more bytes.
uint32_t crc32(uint32_t crc, const void* data, size_t len);

}  // namespace xwa
