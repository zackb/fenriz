#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fenriz {

    // sRGB transfer function (IEC 61966-2-1).
    inline float srgb_to_linear(uint32_t b) {
        const float c = b / 255.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }

    inline uint32_t linear_to_srgb(float v) {
        v = std::clamp(v, 0.0f, 1.0f);
        const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        return (uint32_t)(s * 255.0f + 0.5f);
    }

    // Blend two 0xRRGGBBAA colors, t in [0,1]. RGB mixes in linear light
    inline uint32_t u32_lerp(uint32_t a, uint32_t b, float t) {
        const float aa = (float)(a & 0xff), ab = (float)(b & 0xff);
        uint32_t out = (uint32_t)(aa + (ab - aa) * t + 0.5f) & 0xff;
        for (int shift = 8; shift < 32; shift += 8) {
            const float la = srgb_to_linear((a >> shift) & 0xff);
            const float lb = srgb_to_linear((b >> shift) & 0xff);
            out |= linear_to_srgb(la + (lb - la) * t) << shift;
        }
        return out;
    }

    inline uint32_t u32_mix(uint32_t a, uint32_t b) { return u32_lerp(a, b, 0.5f); }

    // gradient ramp so it lingers near its endpoints.
    inline float ramp_ease(float t, float amount) { return t + amount * ((3.0f - 2.0f * t) * t * t - t); }

} // namespace fenriz
