#ifndef CRC32_H
#define CRC32_H
#include <stdint.h>
/* zlib's CRC-32: crc = crc32_update(0xFFFFFFFF, ...) then ~crc at the end. */
uint32_t crc32_update(uint32_t crc, uint8_t byte);
#endif
