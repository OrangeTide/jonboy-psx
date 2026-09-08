#include "pad.h"
#include "cpu.h"
#include "mc.h"

// SIO0 (controller / memory-card port) in slot 1. The BIOS/game addresses a
// device by asserting select, then clocking a command through the transmit
// register one byte at a time. The addressed device replies a byte per clock and
// pulses its acknowledge line after every byte but the last, which we model as
// the SIO0 interrupt (IRQ_PAD). The first byte selects the device: 0x01 the
// digital pad, 0x81 the memory card (when one is mounted). Slot 2 reports no
// device, so those probes time out.

// The device answering the current transaction.
#define DEV_NONE 0
#define DEV_PAD  1
#define DEV_CARD 2

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
    int device;       // which device is answering (DEV_*)
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
    sio.device = DEV_NONE;
    sio.mode = 0;
    sio.ctrl = 0;
    sio.baud = 0;
}

// Compute the byte the digital pad shifts back for transaction position `pos`
// and say (via *ack) whether it pulses acknowledge after it.
static uint8_t
pad_response(int pos, uint8_t cmd, int *ack)
{
    (void)cmd;
    *ack = 0;

    switch (pos) {
    case 0:
        *ack = 1;
        return 0xFF; // addressed (0x01): presence acknowledge

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
        // A byte was clocked out. A device answers only when slot 1 is selected;
        // otherwise the line floats to 0xFF and nothing acknowledges.
        int selected = (sio.ctrl & CTRL_SELECT) && !(sio.ctrl & CTRL_PORT2);

        if (!selected) {
            sio.rx_data = 0xFF;
            sio.rx_ready = 1;
            break;
        }

        // The first byte of a transaction selects the device.
        if (sio.xfer_pos == 0) {
            if ((uint8_t)value == 0x01) {
                sio.device = DEV_PAD;
            } else if ((uint8_t)value == 0x81 && mc_present()) {
                sio.device = DEV_CARD;
            } else {
                sio.device = DEV_NONE;
            }
        }

        int ack = 0;
        switch (sio.device) {
        case DEV_PAD:
            sio.rx_data = pad_response(sio.xfer_pos, (uint8_t)value, &ack);
            break;
        case DEV_CARD:
            sio.rx_data = mc_transfer(sio.xfer_pos, (uint8_t)value, &ack);
            break;
        default:
            sio.rx_data = 0xFF;
            ack = 0;
            break;
        }
        sio.rx_ready = 1;
        sio.xfer_pos++;

        if (ack) {
            sio.irq = 1;
            cpu.i_stat |= (1u << IRQ_PAD); // device acknowledge -> SIO0 interrupt
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
            sio.device = DEV_NONE;
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
