#pragma once
//
// app/base64.h — standard Base64 encoding (RFC 4648), header-only and PURE.
//
// M22: the Shoggoth's POV snapshot is encoded to PNG in memory, then to Base64 for the
// KEEL vision request's `image_url` data URI. Encode-only (the client never decodes);
// deterministic, no allocation surprises, no dependency. Unit-tested against the RFC
// 4648 vectors so the wire format is exactly what the vision endpoint expects.
//
#include <cstddef>
#include <cstdint>
#include <string>

namespace br::app {

inline std::string base64_encode(const uint8_t* data, size_t n) {
    static const char* kT =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        const uint32_t w = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           static_cast<uint32_t>(data[i + 2]);
        out.push_back(kT[(w >> 18) & 0x3f]);
        out.push_back(kT[(w >> 12) & 0x3f]);
        out.push_back(kT[(w >> 6) & 0x3f]);
        out.push_back(kT[w & 0x3f]);
    }
    const size_t rem = n - i;  // 0, 1, or 2 trailing bytes
    if (rem == 1) {
        const uint32_t w = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kT[(w >> 18) & 0x3f]);
        out.push_back(kT[(w >> 12) & 0x3f]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const uint32_t w = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kT[(w >> 18) & 0x3f]);
        out.push_back(kT[(w >> 12) & 0x3f]);
        out.push_back(kT[(w >> 6) & 0x3f]);
        out.push_back('=');
    }
    return out;
}

}  // namespace br::app
