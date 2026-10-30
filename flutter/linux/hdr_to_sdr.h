// Shared by the Linux texture bridge and the GLFW consumer. No FFmpeg types:
// decoded P010 planes keep their HDR metadata until the consumer draws them.
#ifndef LYCHEE_FLUTTER_LINUX_HDR_TO_SDR_H_
#define LYCHEE_FLUTTER_LINUX_HDR_TO_SDR_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace lychee_linux {

// PQ-encoded IPT, with Hunt-Pointer-Estevez cone responses, D65 and 4%
// crosstalk. This is the color model used by swscale's relative colorimetric
// mapping (libswscale/cms.c and csputils.c), rather than Windows' ICtCp
// approximation. Matrices are column-major for GLSL.
inline constexpr char kHdrShader[] = R"(
uniform sampler2D gamut_cusps;
const float m1 = 2610.0 / 16384.0;
const float m2 = 2523.0 / 32.0;
const float c1 = 3424.0 / 4096.0;
const float c2 = 2413.0 / 128.0;
const float c3 = 2392.0 / 128.0;
const mat3 bt2020ToLms = mat3(
  0.412036387, 0.166660219, 0.024112359,
  0.523911912, 0.720395213, 0.075474963,
  0.064054982, 0.112946123, 0.900407937);
const mat3 lmsToIpt = mat3(
  0.4, 4.455, 0.8056,
  0.4, -4.851, 0.3572,
  0.2, 0.396, -1.1628);
const mat3 iptToLms = mat3(
  1.0, 1.0, 1.0,
  0.097568931, -0.113876485, 0.032615110,
  0.205226433, 0.133217158, -0.676887183);
const mat3 lmsToBt709 = mat3(
  6.173532658, -1.324031910, -0.011598388,
  -5.320898821, 2.560269770, -0.264921447,
  0.147354885, -0.236238618, 1.276526337);
const mat3 bt709ToBt2020 = mat3(
  0.627404, 0.069097, 0.016391,
  0.329283, 0.919540, 0.088013,
  0.043313, 0.011362, 0.895595);

vec3 pqToNits(vec3 e) {
  vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / m2));
  return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), vec3(1.0 / m1));
}
vec3 nitsToPq(vec3 nits) {
  vec3 y = pow(max(nits, 0.0) / 10000.0, vec3(m1));
  return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}
vec3 iptToNits(vec3 ipt) {
  return lmsToBt709 * pqToNits(iptToLms * ipt);
}
bool inside(vec3 rgb) {
  return all(greaterThanEqual(rgb, vec3(-0.0001))) &&
         all(lessThanEqual(rgb, vec3(203.01)));
}
float hlgScene(float e) {
  return e <= 0.5 ? e * e / 3.0 :
      (exp((e - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
}
vec3 hlgToNits(vec3 e) {
  e = clamp(e, 0.0, 1.0);
  vec3 scene = vec3(hlgScene(e.r), hlgScene(e.g), hlgScene(e.b));
  vec3 weights = color_primaries == 9 ? vec3(0.2627, 0.6780, 0.0593) :
                                      vec3(0.2126, 0.7152, 0.0722);
  return 1000.0 * pow(max(dot(scene, weights), 1e-6), 0.2) * scene;
}
vec3 hdrToSdr(vec3 encoded) {
  // H.273: 16 is PQ, 18 is HLG. SDR retains its existing fast path.
  if (color_transfer != 16 && color_transfer != 18) return encoded;
  vec3 nits = color_transfer == 16 ? pqToNits(encoded) : hlgToNits(encoded);
  // Neutral light needs no gamut mapping. This also avoids the vanishing
  // chroma exponent near white amplifying floating-point noise in IPT.
  if (max(encoded.r, max(encoded.g, encoded.b)) -
      min(encoded.r, min(encoded.g, encoded.b)) < 1e-5) {
    return vec3(pow(clamp(nits.r / 203.0, 0.0, 1.0), 1.0 / 2.4));
  }
  if (color_primaries != 9) nits = bt709ToBt2020 * nits;
  vec3 ipt = lmsToIpt * nitsToPq(bt2020ToLms * nits);
  vec3 rgb = iptToNits(ipt);
  if (!inside(rgb)) {
    // The cusp is the most saturated BT.709 color at this hue. Converging
    // towards its intensity preserves dark colors, while bright colors
    // lose saturation and approach SDR white instead of becoming gray.
    float hue = atan(ipt.z, ipt.y) / 6.28318530718 + 0.5;
#if __VERSION__ >= 300
    vec4 cusp_sample = texture(gamut_cusps, vec2(hue, 0.5));
#else
    vec4 cusp_sample = texture2D(gamut_cusps, vec2(hue, 0.5));
#endif
    vec2 cusp = vec2(dot(cusp_sample.rg, vec2(65280.0, 255.0)),
                     dot(cusp_sample.ba, vec2(65280.0, 255.0))) / 65535.0;
    float chroma = length(ipt.yz);
    float relative = max(ipt.x / cusp.x, 0.0);
    float gamma = 1.8 * relative * relative * relative *
                  min(chroma / cusp.y, 1.0);
    float lo = 0.0;
    float hi = 1.0;
    for (int i = 0; i < 16; ++i) {
      float t = 0.5 * (lo + hi);
      vec3 candidate = vec3(mix(cusp.x, ipt.x, pow(t, gamma)), t * ipt.yz);
      if (inside(iptToNits(candidate))) lo = t; else hi = t;
    }
    rgb = iptToNits(vec3(mix(cusp.x, ipt.x, pow(lo, gamma)), lo * ipt.yz));
  }
  // BT.1886 display encoding, 203-nit SDR white, as swscale's BT.709 output.
  return pow(clamp(rgb / 203.0, 0.0, 1.0), vec3(1.0 / 2.4));
}
)";

inline constexpr int kGamutCuspCount = 256;

// RGBA8 stores two 16-bit values, intensity and chroma. Decoding is linear,
// so ordinary GL_LINEAR sampling also interpolates the unpacked values.
// A power-of-two width permits GL_REPEAT even in an OpenGL ES 2 context.
inline const std::array<uint8_t, static_cast<std::size_t>(kGamutCuspCount) * 4>&
GamutCusps() {
  static const auto table = [] {
    std::array<uint8_t, static_cast<std::size_t>(kGamutCuspCount) * 4> result{};
    const auto pq_to_nits = [](double encoded) {
      const double p = std::pow(std::clamp(encoded, 0.0, 1.0), 32.0 / 2523.0);
      return 10000.0 * std::pow(std::max(p - 3424.0 / 4096.0, 0.0) /
                                    (2413.0 / 128.0 - 2392.0 / 128.0 * p),
                                16384.0 / 2610.0);
    };
    for (int h = 0; h < kGamutCuspCount; ++h) {
      const double hue =
          ((h + 0.5) / kGamutCuspCount - 0.5) * 6.283185307179586;
      const double p = std::cos(hue), t = std::sin(hue);
      const auto boundary = [&](double intensity) {
        double low = 0.0, high = 0.5;
        for (int step = 0; step < 16; ++step) {
          const double c = (low + high) * 0.5;
          const double l =
              pq_to_nits(intensity + c * (0.097568931 * p + 0.205226433 * t));
          const double m =
              pq_to_nits(intensity + c * (-0.113876485 * p + 0.133217158 * t));
          const double s =
              pq_to_nits(intensity + c * (0.032615110 * p - 0.676887183 * t));
          const double r = 6.173532658 * l - 5.320898821 * m + 0.147354885 * s;
          const double g = -1.324031910 * l + 2.560269770 * m - 0.236238618 * s;
          const double b = -0.011598388 * l - 0.264921447 * m + 1.276526337 * s;
          if (std::min({r, g, b}) >= -0.0001 && std::max({r, g, b}) <= 203.01) {
            low = c;
          } else {
            high = c;
          }
        }
        return (low + high) * 0.5;
      };
      // Golden-section search over SDR's PQ intensity range. This runs once
      // per process, rather than searching for every pixel of every frame.
      double low = 0.0, high = 0.580688881;
      for (int step = 0; step < 20; ++step) {
        const double a = low + (high - low) * 0.381966011;
        const double b = low + (high - low) * 0.618033989;
        if (boundary(a) > boundary(b))
          high = b;
        else
          low = a;
      }
      const double intensity = (low + high) * 0.5;
      const std::array<double, 2> cusp = {intensity, boundary(intensity)};
      for (int component = 0; component < 2; ++component) {
        const auto value =
            static_cast<uint16_t>(std::lround(cusp[component] * 65535.0));
        result[h * 4 + component * 2] = static_cast<uint8_t>(value >> 8);
        result[h * 4 + component * 2 + 1] = static_cast<uint8_t>(value & 255);
      }
    }
    return result;
  }();
  return table;
}

}  // namespace lychee_linux

#endif  // LYCHEE_FLUTTER_LINUX_HDR_TO_SDR_H_
