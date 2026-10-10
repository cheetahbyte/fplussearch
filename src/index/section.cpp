#include "index/section.hpp"

#include "storage/fields.hpp"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace fplussearch {

size_t unpack_hex(const char* p, char* out, size_t& len) {
  len = uint8_t(p[0]);
  const uint8_t* src = reinterpret_cast<const uint8_t*>(p + 1);
  const size_t packed = (len + 1) / 2;
#if defined(__ARM_NEON)
  static const uint8_t digits[16] = {'0', '1', '2', '3', '4', '5', '6', '7',
                                     '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  const uint8x16_t table = vld1q_u8(digits);
  for (size_t i = 0; i < packed; i += 16) {
    uint8x16_t v = vld1q_u8(src + i);
    uint8x16_t hi = vqtbl1q_u8(table, vshrq_n_u8(v, 4));
    uint8x16_t lo = vqtbl1q_u8(table, vandq_u8(v, vdupq_n_u8(0x0f)));
    vst1q_u8(reinterpret_cast<uint8_t*>(out) + 2 * i, vzip1q_u8(hi, lo));
    vst1q_u8(reinterpret_cast<uint8_t*>(out) + 2 * i + 16, vzip2q_u8(hi, lo));
  }
#else
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < packed; ++i) {
    out[2 * i] = digits[src[i] >> 4];
    out[2 * i + 1] = digits[src[i] & 15];
  }
#endif
  return 1 + packed;
}

[[gnu::noinline, gnu::cold]]
uint32_t Section::count_slow(uint32_t name) const {
  const Overflow* o = find_key(overflow, name);
  return o ? uint32_t(o->value) : 255;
}

}  // namespace fplussearch
