#include "pad.h"
#include "cpu.h"

// SIO0 (controller / memory-card port) with a single digital pad in slot 1.
// The BIOS/game addresses the pad by asserting select, then clocking a command
// through the transmit register one byte at a time. The pad replies a byte per
// clock and pulses its acknowledge line after every byte but the last, which we
// model as the SIO0 interrupt (IRQ_PAD). Slot 2 and the memory-card address
// (0x81) report no device, so those probes time out.

// JOY_STAT bits.
#define STAT_TX_READY_1  0x01 // ready to accept a byte in the transmit register
#define STAT_RX_NONEMPTY 0x02 // a received byte is waiting in the FIFO
#define STAT_TX_READY_2  0x04 // transmit finished
#define STAT_ACK_IRQ     0x200 // bit9: acknowledge interrupt latched

// JOY_CTRL bits.
#define CTRL_SELECT   0x0002 // assert /SEL for the addressed slot
#define CTRL_ACK      0x0010 // write 1 to clear the ack interrupt / error
#define CTRL_RESET    0x0040 // reset the port
#define CTRL_PORT2    0x2000 // 0 = slot 1, 1 = slot 2

// Register offsets within the block.
#define REG_DATA 0x0 // TX (write) / RX (read)
#define REG_STAT 0x4 // status (read-only)
#define REG_MODE 0x8
#define REG_CTRL 0xA
#define REG_BAUD 0xE

uint16_t pad_buttons = 0xFFFF; // active low: 0xFFFF = nothing pressed

static struct {
    uint8_t rx_data;  // last byte shifted back from the device
    int rx_ready;     // a received byte is waiting to be read
    int irq;          // acknowledge interrupt latched (JOY_STAT bit9)
    int xfer_pos;     // byte index within the current transaction
    int dev_active;   // a pad is responding to the current transaction
    uint16_t mode;
    uint16_t ctrl;
    uint16_t baud;
} sio;

void
psx_pad_reset(void)
{
    sio.rx_data = 0xFF;
    sio.rx_ready = 0;
    sio.irq = 0;
    sio.xfer_pos = 0;
    sio.dev_active = 0;
    sio.mode = 0;
    sio.ctrl = 0;
    sio.baud = 0;
}

// Compute the byte the pad shifts back for the current transaction position and
// say (via *ack) whether the pad pulses acknowledge after it.
static uint8_t
pad_response(uint8_t cmd, int *ack)
{
    *ack = 0;

    switch (sio.xfer_pos) {
    case 0:
        // First byte addresses a device: 0x01 selects the controller. Anything
        // else (e.g. 0x81 for a memory card) leaves the slot silent.
        if (cmd == 0x01) {
            sio.dev_active = 1;
            *ack = 1;
            return 0xFF;
        }
        sio.dev_active = 0;
        return 0xFF;

    case 1:
        *ack = 1;
        return 0x41; // digital pad identifier

    case 2:
        *ack = 1;
        return 0x5A; // "data follows"

    case 3:
        *ack = 1;
        return (uint8_t)(pad_buttons & 0xFF); // buttons, active low

    case 4:
        return (uint8_t)(pad_buttons >> 8);   // final byte, no acknowledge

    default:
        return 0xFF;
    }
}

uint32_t
psx_pad_read(uint32_t phys_addr, int size)
{
    (void)size;
    uint32_t reg = phys_addr - PAD_BASE;

    switch (reg) {
    case REG_DATA: {
        uint8_t v = sio.rx_data;
        sio.rx_data = 0xFF;
        sio.rx_ready = 0;
        return v;
    }

    case REG_STAT: {
        uint32_t s = STAT_TX_READY_1 | STAT_TX_READY_2;

        if (sio.rx_ready) {
            s |= STAT_RX_NONEMPTY;
        }

        if (sio.irq) {
            s |= STAT_ACK_IRQ;
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
    case REG_DATA: {
        // A byte was clocked out. A pad answers only when slot 1 is selected;
        // otherwise the line floats to 0xFF and nothing acknowledges.
        int selected = (sio.ctrl & CTRL_SELECT) && !(sio.ctrl & CTRL_PORT2);

        if (!selected) {
            sio.rx_data = 0xFF;
            sio.rx_ready = 1;
            break;
        }

        int ack = 0;
        sio.rx_data = pad_response((uint8_t)value, &ack);
        sio.rx_ready = 1;
        sio.xfer_pos++;

        if (ack && sio.dev_active) {
            sio.irq = 1;
            cpu.i_stat |= (1u << IRQ_PAD); // pad acknowledge -> SIO0 interrupt
        }
        break;
    }

    case REG_MODE:
        sio.mode = (uint16_t)value;
        break;

    case REG_CTRL: {
        uint16_t prev = sio.ctrl;
        sio.ctrl = (uint16_t)value;

        // Acknowledge write clears the latched interrupt.
        if (value & CTRL_ACK) {
            sio.irq = 0;
        }

        // A fresh select (rising edge) or a reset starts a new transaction.
        if ((value & CTRL_RESET) ||
            ((value & CTRL_SELECT) && !(prev & CTRL_SELECT))) {
            sio.xfer_pos = 0;
            sio.dev_active = 0;
            sio.rx_ready = 0;
        }
        break;
    }

    case REG_BAUD:
        sio.baud = (uint16_t)value;
        break;

    default:
        break;
    }
}
