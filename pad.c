#include "pad.h"

// A minimal SIO0 (controller / memory-card) port that reports no device on
// either slot. The BIOS talks to a pad or card by writing a byte to the
// transmit register and waiting for the received byte, then it checks for the
// device's acknowledge interrupt. With no device attached, each byte still
// shifts through (the data line reads back as 0xFF) but nothing acknowledges,
// so the BIOS times out on its acknowledge wait and treats the slot as empty.

// JOY_STAT bits.
#define STAT_TX_READY_1 0x01 // ready to accept a byte in the transmit register
#define STAT_RX_NONEMPTY 0x02 // a received byte is waiting in the FIFO
#define STAT_TX_READY_2 0x04 // transmit finished

// Register offsets within the block.
#define REG_DATA 0x0 // TX (write) / RX (read)
#define REG_STAT 0x4 // status (read-only)
#define REG_MODE 0x8
#define REG_CTRL 0xA
#define REG_BAUD 0xE

static struct {
    uint8_t rx_data;  // last byte "received" from the (absent) device
    int rx_ready;     // a received byte is waiting to be read
    uint16_t mode;
    uint16_t ctrl;
    uint16_t baud;
} sio;

void
psx_pad_reset(void)
{
    sio.rx_data = 0xFF;
    sio.rx_ready = 0;
    sio.mode = 0;
    sio.ctrl = 0;
    sio.baud = 0;
}

uint32_t
psx_pad_read(uint32_t phys_addr, int size)
{
    (void)size;
    uint32_t reg = phys_addr - PAD_BASE;

    switch (reg) {
    case REG_DATA: {
        // Reading the data register consumes the received byte.
        uint8_t v = sio.rx_data;
        sio.rx_ready = 0;
        return v;
    }

    case REG_STAT: {
        // Transmit is always ready. The acknowledge line is never asserted
        // (no device), so bit7 stays clear and the BIOS acknowledge wait
        // times out.
        uint32_t s = STAT_TX_READY_1 | STAT_TX_READY_2;

        if (sio.rx_ready) {
            s |= STAT_RX_NONEMPTY;
        }

        return s;
    }

    case REG_MODE:
        return sio.mode;

    case REG_CTRL:
        return sio.ctrl;

    case REG_BAUD:
        return sio.baud;

    default:
        return 0;
    }
}

void
psx_pad_write(uint32_t phys_addr, uint32_t value, int size)
{
    (void)size;
    uint32_t reg = phys_addr - PAD_BASE;

    switch (reg) {
    case REG_DATA:
        // A byte was clocked out. With no device on the line, the byte clocked
        // in reads back as 0xFF. Mark it received so the BIOS transfer loop
        // proceeds; no acknowledge interrupt is raised.
        sio.rx_data = 0xFF;
        sio.rx_ready = 1;
        break;

    case REG_MODE:
        sio.mode = (uint16_t)value;
        break;

    case REG_CTRL:
        sio.ctrl = (uint16_t)value;

        // Bit6 resets the port; drop any pending received byte.
        if (value & 0x0040) {
            sio.rx_ready = 0;
        }
        break;

    case REG_BAUD:
        sio.baud = (uint16_t)value;
        break;

    default:
        break;
    }
}
