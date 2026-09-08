#include "pad.h"
#include "cpu.h"
#include "mc.h"
#include <string.h>

// SIO0 (the two controller / memory-card ports). The BIOS/game addresses a
// device by asserting select, then clocking a command through the transmit
// register one byte at a time. The addressed device replies a byte per clock and
// pulses its acknowledge line after every byte but the last, which we model as
// the SIO0 interrupt (IRQ_PAD). The port-select bit (JOY_CTRL bit 13) chooses
// slot 0 or slot 1, and the first byte selects the device: 0x01 the digital pad
// (slot 0 only), 0x81 the memory card in that slot (when one is mounted).
// Anything with no device leaves the line floating, so the probe times out.

// The device answering the current transaction.
#define DEV_NONE 0
#define DEV_PAD  1
#define DEV_CARD 2

// The addressed device pulses its acknowledge a short time after each byte is
// clocked, not on the same cycle. Drivers (and the BIOS) clear the SIO0
// interrupt right after writing a byte and only then wait for this acknowledge,
// so raising it synchronously would let that clear swallow it. This delay, in
// CPU cycles, only needs to outlast the write-then-clear window while staying
// well within the drivers' acknowledge timeouts.
#define SIO_ACK_DELAY_CYCLES 100

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

uint8_t pad_rstick_x = 0x80, pad_rstick_y = 0x80;
uint8_t pad_lstick_x = 0x80, pad_lstick_y = 0x80;

static struct {
    uint8_t rx_data;  // last byte shifted back from the device
    int rx_ready;     // a received byte is waiting to be read
    int irq;          // acknowledge interrupt latched (JOY_STAT bit9)
    int ack_delay;    // cycles until the pending device acknowledge fires (0=none)
    int xfer_pos;     // byte index within the current transaction
    int device;       // which device is answering (DEV_*)
    int card_slot;    // memory-card slot for the current transaction (0 or 1)
    uint16_t mode;
    uint16_t ctrl;
    uint16_t baud;
} sio;

// DualShock controller state. It powers up in digital mode (ID 0x41) and a game
// puts it into analog mode (ID 0x73) through the config-mode command sequence, or
// the player toggles it with the ANALOG button. In config mode the ID is 0xF3.
static struct {
    int analog_mode;  // 0 = digital (0x41), 1 = analog (0x73)
    int config_mode;  // in the escape/config command mode (ID 0xF3)
    uint8_t cmd;      // command byte of the current transaction
    uint8_t id;       // ID byte returned at position 1
    uint8_t buf[8];   // response bytes; buf[0] unused (id is returned instead)
    int buf_len;      // number of response bytes (id plus buf[1..buf_len-1])
} pad;

void
pad_toggle_analog(void)
{
    pad.analog_mode = !pad.analog_mode;
}

int
pad_analog_mode(void)
{
    return pad.analog_mode;
}

// Advance the pending device acknowledge. Called every step from the hardware
// tick so a scheduled /ACK fires a fixed delay after its byte, letting the SIO0
// interrupt survive the driver's clear-then-wait sequence.
void
psx_pad_tick(uint32_t cycles)
{
    if (sio.ack_delay > 0) {
        sio.ack_delay -= (int)cycles;
        if (sio.ack_delay <= 0) {
            sio.ack_delay = 0;
            sio.irq = 1;
            cpu.i_stat |= (1u << IRQ_PAD); // device acknowledge -> SIO0 interrupt
        }
    }
}

void
psx_pad_reset(void)
{
    sio.rx_data = 0xFF;
    sio.rx_ready = 0;
    sio.irq = 0;
    sio.ack_delay = 0;
    sio.xfer_pos = 0;
    sio.device = DEV_NONE;
    sio.mode = 0;
    sio.ctrl = 0;
    sio.baud = 0;

    pad.analog_mode = 0;
    pad.config_mode = 0;
}

// Build the poll (0x42) response: the ID, "data follows" (0x5A), the two button
// bytes and, in analog mode, the four stick axes.
static void
pad_build_poll(void)
{
    pad.buf[1] = 0x5A;
    pad.buf[2] = (uint8_t)(pad_buttons & 0xFF);
    pad.buf[3] = (uint8_t)(pad_buttons >> 8);

    if (pad.analog_mode || pad.config_mode) {
        pad.id = pad.config_mode ? 0xF3 : 0x73;
        pad.buf[4] = pad_rstick_x;
        pad.buf[5] = pad_rstick_y;
        pad.buf[6] = pad_lstick_x;
        pad.buf[7] = pad_lstick_y;
        pad.buf_len = 8;
    } else {
        pad.id = 0x41;
        pad.buf_len = 4;
    }
}

// Build the response for a transaction whose command byte just arrived. Fills
// pad.id, pad.buf[1..], and pad.buf_len. Command constants follow the DualShock
// model in REF/pcsx-redux/src/core/pad.cc.
static void
pad_build(uint8_t cmd)
{
    pad.cmd = cmd;
    memset(pad.buf, 0, sizeof pad.buf);

    if (cmd == 0x42) { // poll
        pad_build_poll();
        return;
    }

    if (cmd == 0x43) { // enter/exit config mode
        if (pad.config_mode) {
            pad.id = 0xF3;
            pad.buf[1] = 0x5A; // buf[2..7] stay zero
            pad.buf_len = 8;
        } else {
            pad_build_poll(); // outside config this reads like a normal poll
        }
        return;
    }

    if (pad.config_mode) {
        pad.id = 0xF3;
        pad.buf[1] = 0x5A;
        pad.buf_len = 8;

        switch (cmd) {
        case 0x44: // set major mode (digital/analog); arg applied at position 3
            break;
        case 0x45: // read controller status/model
            pad.buf[2] = 0x01;
            pad.buf[3] = 0x02;
            pad.buf[4] = pad.analog_mode ? 0x01 : 0x00;
            pad.buf[5] = 0x02;
            pad.buf[6] = 0x01;
            pad.buf[7] = 0x00;
            break;
        case 0x46: // constant capability data; arg selects the variant
            pad.buf[4] = 0x01;
            pad.buf[5] = 0x02;
            pad.buf[6] = 0x00;
            pad.buf[7] = 0x0A;
            break;
        case 0x47: // constant capability data
            pad.buf[4] = 0x02;
            pad.buf[5] = 0x00;
            pad.buf[6] = 0x01;
            pad.buf[7] = 0x00;
            break;
        case 0x4C: // constant capability data; arg sets buf[5]
            break;
        case 0x4D: // configure rumble motor mapping
            pad.buf[2] = 0xFF;
            pad.buf[3] = 0xFF;
            pad.buf[4] = 0xFF;
            pad.buf[5] = 0xFF;
            pad.buf[6] = 0xFF;
            pad.buf[7] = 0xFF;
            break;
        default: // unknown config command: no device response
            pad.id = 0xFF;
            pad.buf_len = 0;
            break;
        }
        return;
    }

    // Unknown command outside config mode: nothing answers.
    pad.id = 0xFF;
    pad.buf_len = 0;
}

// Apply a command's argument byte (transaction position 3). Some commands set
// mode from the argument, others vary their constant reply.
static void
pad_apply_arg(uint8_t arg)
{
    switch (pad.cmd) {
    case 0x43: // enter/exit config: arg 1 enters, 0 exits (effective next frame)
        pad.config_mode = (arg == 1);
        break;
    case 0x44: // set analog (1) or digital (0) mode
        if (pad.config_mode) {
            pad.analog_mode = (arg == 1);
        }
        break;
    case 0x46:
        if (arg == 1) {
            pad.buf[4] = 0x01;
            pad.buf[5] = 0x01;
            pad.buf[6] = 0x01;
            pad.buf[7] = 0x14;
        }
        break;
    case 0x47:
        if (arg != 0) {
            pad.buf[4] = pad.buf[5] = pad.buf[6] = pad.buf[7] = 0x00;
        }
        break;
    case 0x4C:
        if (arg == 0) {
            pad.buf[5] = 0x04;
        } else if (arg == 1) {
            pad.buf[5] = 0x07;
        }
        break;
    default:
        break;
    }
}

// Compute the byte the DualShock shifts back for transaction position `pos` and
// say (via *ack) whether it pulses acknowledge after it. The pad acknowledges
// every byte except the last of the transaction.
static uint8_t
pad_response(int pos, uint8_t val, int *ack)
{
    if (pos == 0) {
        *ack = 1;
        return 0xFF; // addressed (0x01): presence acknowledge
    }

    if (pos == 1) {
        pad_build(val); // val is the command byte
        *ack = (pad.buf_len > 1);
        return pad.id;
    }

    if (pos == 3) {
        pad_apply_arg(val);
    }

    // Positions 2..buf_len return buf[1..buf_len-1]; the last one does not ack.
    if (pos >= 2 && pos <= pad.buf_len) {
        *ack = (pos < pad.buf_len);
        return pad.buf[pos - 1];
    }

    *ack = 0;
    return 0xFF;
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
        // A byte was clocked out. A device answers only when a port is selected;
        // otherwise the line floats to 0xFF and nothing acknowledges.
        int selected = (sio.ctrl & CTRL_SELECT);

        if (!selected) {
            sio.rx_data = 0xFF;
            sio.rx_ready = 1;
            break;
        }

        // The port-select bit picks the controller port: slot 0 or slot 1.
        int slot = (sio.ctrl & CTRL_PORT2) ? 1 : 0;

        // The first byte of a transaction selects the device. The DualShock is
        // in slot 1 (port 0) only; a memory card answers in whichever slot holds
        // one. Anything else leaves the line floating (no device).
        if (sio.xfer_pos == 0) {
            if ((uint8_t)value == 0x01 && slot == 0) {
                sio.device = DEV_PAD;
            } else if ((uint8_t)value == 0x81 && mc_present_slot(slot)) {
                sio.device = DEV_CARD;
                sio.card_slot = slot;
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
            sio.rx_data = mc_transfer_slot(sio.card_slot, sio.xfer_pos,
                                           (uint8_t)value, &ack);
            break;
        default:
            sio.rx_data = 0xFF;
            ack = 0;
            break;
        }
        sio.rx_ready = 1;
        sio.xfer_pos++;

        if (ack) {
            // Schedule the acknowledge to fire a little later, the way real
            // hardware pulses /ACK after the byte completes. psx_pad_tick raises
            // the SIO0 interrupt when the delay elapses.
            sio.ack_delay = SIO_ACK_DELAY_CYCLES;
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
            sio.ack_delay = 0;
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
