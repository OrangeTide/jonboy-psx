#include "cdrom.h"
#include "cpu.h"

// A minimal CD-ROM controller that always reports "no disc". It implements the
// index/status register, the parameter and response FIFOs, and the interrupt
// flag/enable registers, which is enough for the BIOS shell to run its drive
// probe and then give up and show its menu.
//
// Command flow on real hardware: the CPU writes parameters to the parameter
// FIFO, writes a command byte, and waits for a CD-ROM interrupt (IRQ 2). The
// controller answers with one or more responses, each tagged with an interrupt
// type (INT3 acknowledge, INT2 complete, INT5 error). The CPU reads the
// response FIFO, acknowledges the interrupt, then waits for the next response
// if the command produces more than one.

// Response interrupt types.
#define INT2 2 // second response / command complete
#define INT3 3 // first response / acknowledge
#define INT5 5 // error

// Status byte reported in responses. Bit1 = spindle motor on. The drive claims
// to be spun up and idle; the "no disc" condition is signalled by the GetID
// error response rather than by this byte.
#define STAT_IDLE 0x02

// Cycles to wait before delivering a queued response, so the interrupt does not
// fire in the same instruction that writes the command.
#define CDROM_RESP_DELAY 2000

#define FIFO_MAX 16
#define QUEUE_MAX 4

typedef struct {
    uint8_t type;           // INT2 / INT3 / INT5
    uint8_t len;
    uint8_t data[FIFO_MAX];
} Response;

static struct {
    uint8_t index;          // selects the meaning of ports 1-3

    uint8_t param_fifo[FIFO_MAX];
    int param_count;

    uint8_t resp_fifo[FIFO_MAX];
    int resp_count;
    int resp_read;

    uint8_t irq_enable;     // IE: which interrupt types reach the CPU
    uint8_t irq_flags;      // IF: the interrupt type currently pending (0 = none)

    Response queue[QUEUE_MAX]; // responses waiting to be delivered one at a time
    int queue_head;
    int queue_count;

    int busy;               // a command is being processed
    int deliver_delay;      // cycles until the next response is delivered
} cd;

void
psx_cdrom_reset(void)
{
    cd.index = 0;
    cd.param_count = 0;
    cd.resp_count = 0;
    cd.resp_read = 0;
    cd.irq_enable = 0;
    cd.irq_flags = 0;
    cd.queue_head = 0;
    cd.queue_count = 0;
    cd.busy = 0;
    cd.deliver_delay = 0;
}

// Append a response to the delivery queue.
static void
enqueue(uint8_t type, const uint8_t *data, int len)
{
    if (cd.queue_count >= QUEUE_MAX) {
        return;
    }

    int slot = (cd.queue_head + cd.queue_count) % QUEUE_MAX;
    cd.queue[slot].type = type;
    cd.queue[slot].len = (uint8_t)len;

    for (int i = 0; i < len && i < FIFO_MAX; i++) {
        cd.queue[slot].data[i] = data[i];
    }

    cd.queue_count++;
}

// Move the next queued response into the response FIFO and raise the CD-ROM
// interrupt. Only runs when no earlier interrupt is still waiting to be
// acknowledged, so responses are seen one at a time.
static void
deliver_next(void)
{
    if (cd.queue_count == 0 || cd.irq_flags != 0) {
        return;
    }

    Response *r = &cd.queue[cd.queue_head];
    cd.resp_count = r->len;
    cd.resp_read = 0;

    for (int i = 0; i < r->len; i++) {
        cd.resp_fifo[i] = r->data[i];
    }

    cd.irq_flags = r->type;
    cd.queue_head = (cd.queue_head + 1) % QUEUE_MAX;
    cd.queue_count--;
    cd.busy = 0;

    if (cd.irq_flags & cd.irq_enable) {
        cpu.i_stat |= (1u << IRQ_CDROM);
    }
}

// Decode and answer a command byte using whatever is in the parameter FIFO.
static void
run_command(uint8_t cmd)
{
    uint8_t stat[1] = { STAT_IDLE };

    switch (cmd) {
    case 0x01: // Getstat
        enqueue(INT3, stat, 1);
        break;

    case 0x0A: // Init
    case 0x08: // Stop
    case 0x09: // Pause
        enqueue(INT3, stat, 1);
        enqueue(INT2, stat, 1);
        break;

    case 0x19: { // Test
        uint8_t sub = (cd.param_count > 0) ? cd.param_fifo[0] : 0;

        if (sub == 0x20) {
            // Get controller ROM version/date. A plausible fixed value.
            uint8_t ver[4] = { 0x94, 0x09, 0x19, 0xC0 };
            enqueue(INT3, ver, 4);
        } else {
            enqueue(INT3, stat, 1);
        }
        break;
    }

    case 0x1A: { // GetID
        // Acknowledge, then report "no disc" as an error response.
        uint8_t nodisc[8] = { 0x08, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        enqueue(INT3, stat, 1);
        enqueue(INT5, nodisc, 8);
        break;
    }

    default:
        // Unknown or unsupported: just acknowledge so the caller does not hang.
        enqueue(INT3, stat, 1);
        break;
    }

    // The parameters have been consumed by this command.
    cd.param_count = 0;
    cd.busy = 1;

    if (cd.deliver_delay <= 0) {
        cd.deliver_delay = CDROM_RESP_DELAY;
    }
}

void
psx_cdrom_tick(uint32_t cycles)
{
    if (cd.deliver_delay > 0) {
        cd.deliver_delay -= (int)cycles;

        if (cd.deliver_delay <= 0) {
            cd.deliver_delay = 0;
            deliver_next();
        }
    }
}

uint32_t
psx_cdrom_read(uint32_t phys_addr, int size)
{
    (void)size;
    uint32_t reg = (phys_addr - CDROM_BASE) & 3;

    if (reg == 0) {
        // Status register.
        uint8_t s = cd.index & 3;

        if (cd.param_count == 0) {
            s |= 0x08; // PRMEMPT: parameter FIFO empty
        }

        s |= 0x10; // PRMWRDY: parameter FIFO not full (always room here)

        if (cd.resp_read < cd.resp_count) {
            s |= 0x20; // RSLRRDY: response FIFO has data
        }

        if (cd.busy) {
            s |= 0x80; // BUSYSTS: command in progress
        }

        return s;
    }

    if (reg == 1) {
        // Response FIFO (all indexes read the response FIFO here).
        uint8_t v = 0;

        if (cd.resp_read < cd.resp_count) {
            v = cd.resp_fifo[cd.resp_read++];
        }

        return v;
    }

    if (reg == 3) {
        // Interrupt registers. Index 0/2 read the enable, index 1/3 the flags.
        // The upper bits read back as 1 on hardware.
        if ((cd.index & 1) == 0) {
            return cd.irq_enable | 0xE0;
        }

        return cd.irq_flags | 0xE0;
    }

    return 0;
}

void
psx_cdrom_write(uint32_t phys_addr, uint32_t value, int size)
{
    (void)size;
    uint32_t reg = (phys_addr - CDROM_BASE) & 3;
    uint8_t v = (uint8_t)value;

    if (reg == 0) {
        cd.index = v & 3;
        return;
    }

    // Ports 1-3 change meaning with the index.
    switch (cd.index) {
    case 0:
        if (reg == 1) {
            run_command(v); // command register
        } else if (reg == 2) {
            if (cd.param_count < FIFO_MAX) {
                cd.param_fifo[cd.param_count++] = v; // parameter FIFO
            }
        }
        // reg == 3: request register (data FIFO control). No data to serve.
        break;

    case 1:
        if (reg == 2) {
            cd.irq_enable = v & 0x1F; // interrupt enable

            // Enabling an interrupt while a flag is already pending lets it
            // reach the CPU now.
            if (cd.irq_flags & cd.irq_enable) {
                cpu.i_stat |= (1u << IRQ_CDROM);
            }
        } else if (reg == 3) {
            // Interrupt flag register: writing 1 to bits 0-2 acknowledges the
            // pending interrupt; bit6 resets the parameter FIFO.
            cd.irq_flags &= ~(v & 0x07);

            if (v & 0x40) {
                cd.param_count = 0;
            }

            // Once acknowledged, deliver the next queued response, if any.
            if (cd.irq_flags == 0 && cd.queue_count > 0 && cd.deliver_delay <= 0) {
                cd.deliver_delay = CDROM_RESP_DELAY;
            }
        }
        break;

    default:
        // Indexes 2 and 3 mirror audio-volume registers we do not emulate.
        break;
    }
}
