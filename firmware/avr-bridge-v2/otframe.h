#ifndef OTFRAME_H
#define OTFRAME_H
/* OpenTherm-frame (32 bit):  bit 31 pariteit (even over alles), 30-28 type,
 * 27-24 ongebruikt, 23-16 Data-ID, 15-0 waarde.  Gelijk aan v1. */
#include <stdint.h>

#define MSG_READ_DATA     0
#define MSG_WRITE_DATA    1
#define MSG_READ_ACK      4
#define MSG_WRITE_ACK     5
#define MSG_DATA_INVALID  6
#define MSG_UNKNOWN_ID    7

#define ID_STATUS            0
#define ID_CONTROL_SETPOINT  1
#define ID_FAULT_FLAGS       5
#define ID_SLAVE_CONFIG      3
#define ID_REMOTE_OVERRIDE   9
#define ID_MODULATION       17
#define ID_ROOM_SETPOINT    16
#define ID_ROOM_TEMP        24
#define ID_BOILER_TEMP      25
#define ID_RETURN_TEMP      28
#define ID_OVERRIDE_FUNC   100

static inline uint8_t  ot_type(uint32_t f) { return (uint8_t) ((f >> 28) & 0x07); }
static inline uint8_t  ot_id(uint32_t f)   { return (uint8_t) ((f >> 16) & 0xFF); }
static inline uint16_t ot_val(uint32_t f)  { return (uint16_t) (f & 0xFFFF); }

static inline uint8_t ot_parity_ok(uint32_t f) {
  f ^= f >> 16; f ^= f >> 8; f ^= f >> 4; f ^= f >> 2; f ^= f >> 1;
  return (uint8_t) ((f & 1) == 0);
}
static inline uint32_t ot_with_parity(uint32_t f) {
  uint32_t t;
  f &= 0x7FFFFFFFUL;
  t = f;
  t ^= t >> 16; t ^= t >> 8; t ^= t >> 4; t ^= t >> 2; t ^= t >> 1;
  if (t & 1) f |= 0x80000000UL;
  return f;
}
static inline uint32_t ot_make(uint8_t type, uint8_t id, uint16_t val) {
  return ot_with_parity(((uint32_t) (type & 7) << 28) | ((uint32_t) id << 16) | val);
}
static inline uint32_t ot_replace_value(uint32_t f, uint16_t v) {
  return ot_with_parity((f & 0x7FFF0000UL) | v);
}
static inline uint32_t ot_set_type(uint32_t f, uint8_t t) {
  return ot_with_parity((f & 0x0FFFFFFFUL) | ((uint32_t) (t & 7) << 28));
}

/* f8.8: getekend, 1/256 graad. Wij rekenen in tienden. */
static inline uint16_t tenths_to_f88(int16_t t)  { return (uint16_t) (((int32_t) t * 256L) / 10L); }
static inline int16_t  f88_to_tenths(uint16_t v) { return (int16_t) (((int32_t) (int16_t) v * 10L) / 256L); }
#endif
