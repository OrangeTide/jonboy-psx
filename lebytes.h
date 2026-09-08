#ifndef LEBYTES_H_
#define LEBYTES_H_

#include <stdint.h>
#include <string.h>

// Little-endian pack and unpack of a 1, 2, or 4 byte value in a byte buffer.
// The caller has already range-checked that off + size fits the buffer.
//
// Both supported build targets (x86-64 and ARM Cortex-A53) are little-endian,
// matching the PS1's own byte order, so a sized memcpy moves the value straight
// through. The compiler lowers each fixed-size memcpy to a single load or store,
// which matters because these run on the interpreter's hottest path (every
// instruction fetch and every RAM access). A per-byte loop here compiles to four
// dependent load/shift/or steps instead.

static inline uint32_t
load_sized(const uint8_t *base, uint32_t off, int size)
{
    switch (size) {
    case 2: {
        uint16_t v;
        memcpy(&v, base + off, 2);
        return v;
    }
    case 4: {
        uint32_t v;
        memcpy(&v, base + off, 4);
        return v;
    }
    default:
        return base[off];
    }
}

static inline void
store_sized(uint8_t *base, uint32_t off, uint32_t value, int size)
{
    switch (size) {
    case 2: {
        uint16_t v = (uint16_t)value;
        memcpy(base + off, &v, 2);
        break;
    }
    case 4:
        memcpy(base + off, &value, 4);
        break;
    default:
        base[off] = (uint8_t)value;
        break;
    }
}

#endif
