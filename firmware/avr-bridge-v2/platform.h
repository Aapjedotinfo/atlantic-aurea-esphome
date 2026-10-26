#ifndef PLATFORM_H
#define PLATFORM_H
/* Gedeelde kern (proto.c, tabel.c, brug.c) is gewone C zonder Arduino of hardware.
 * Dezelfde bestanden worden voor de AVR gebouwd (in de .ino-map) en native op de PC
 * getest (test/). Hier staat het enige verschil: PROGMEM. */
#include <stdint.h>
#include <string.h>

#ifdef __AVR__
  #include <avr/pgmspace.h>
  #define RD8(p) pgm_read_byte(p)
#else
  #ifndef PROGMEM
    #define PROGMEM
  #endif
  #define RD8(p) (*(const uint8_t *)(p))
#endif

#ifdef __cplusplus
  #define KERN_BEGIN extern "C" {
  #define KERN_END   }
#else
  #define KERN_BEGIN
  #define KERN_END
#endif
#endif
