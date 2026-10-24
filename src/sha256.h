// A small SHA-256 (FIPS 180-4), for viron-usd's per-entity offset and colour.
// Strata's own SHA-256 lives in modules/save, which builds only with OpenUSD,
// so the client carries this one instead (RFC 0030, section 3).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace viron_strata {

std::array<uint8_t, 32> sha256(const void* data, size_t size);
inline std::array<uint8_t, 32> sha256(const std::string& s) { return sha256(s.data(), s.size()); }
std::string toHex(const std::array<uint8_t, 32>& digest);

}  // namespace viron_strata
