#ifndef LEBYTES_H_
#define LEBYTES_H_

#include <stdint.h>

// Little-endian pack and unpack of a 1, 2, or 4 byte value in a byte buffer.
// The caller has already range-checked that off + size fits the buffer.

static inline uint32_t
load_sized(const uint8_t *base, uint32_t off, int size)
{
    uint32_t v = 0;

    for (int i = 0; i < size; i++) {
        v |= (uint32_t)base[off + i] << (i * 8);
    }

    return v;
}

static inline void
store_sized(uint8_t *base, uint32_t off, uint32_t value, int size)
{
    for (int i = 0; i < size; i++) {
        base[off + i] = (uint8_t)(value >> (i * 8));
    }
}

#endif
