#pragma once

#include <cstdint>

// Network byte order (big-endian) <-> the CPU's (little-endian on x86-64).
// Headers on the wire are big-endian: convert every multi-byte field when
// reading or writing one.
namespace Net {

constexpr uint16_t hton16(uint16_t v) { return __builtin_bswap16(v); }
constexpr uint16_t ntoh16(uint16_t v) { return __builtin_bswap16(v); }
constexpr uint32_t hton32(uint32_t v) { return __builtin_bswap32(v); }
constexpr uint32_t ntoh32(uint32_t v) { return __builtin_bswap32(v); }

} // namespace Net
