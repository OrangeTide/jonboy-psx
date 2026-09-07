#include "cdrom.h"
#include "cpu.h"
#include "mem.h"
#include <stdio.h>
#include <string.h>

// CD-ROM controller. It implements the index/status register, the parameter and
// response FIFOs, the interrupt flag/enable registers, and enough commands to
// let the BIOS boot a disc image: seek, read, and a licensed GetID. Sector data
// is served through the data FIFO and DMA channel 3.
//
// Command flow: the CPU writes parameters to the parameter FIFO, writes a
// command byte, and waits for a CD-ROM interrupt (IRQ 2). The controller answers
// with responses tagged by interrupt type (INT3 acknowledge, INT2 complete, INT1
// data-ready, INT5 error). The CPU reads the response FIFO, acknowledges, and
// waits for the next response.

// Response interrupt types.
#define INT1 1 // a data sector is ready
#define INT2 2 // second response / command complete
#define INT3 3 // first response / acknowledge
#define INT5 5 // error

// Status byte bits reported in responses.
#define STAT_MOTOR 0x02 // spindle motor on
#define STAT_READ  0x20 // reading / seeking

// Cycles to wait before delivering a queued response, so an interrupt does not
// fire in the same instruction that writes the command, and between sectors.
#define CDROM_RESP_DELAY 2000
#define CDROM_READ_DELAY 6000

#define FIFO_MAX 16
#define QUEUE_MAX 8
#define RAW_SECTOR 2352

typedef struct {
    uint8_t type;
    uint8_t len;
    uint8_t data[FIFO_MAX];
} Response;

static struct {
    uint8_t index;

    uint8_t param_fifo[FIFO_MAX];
    int param_count;

    uint8_t resp_fifo[FIFO_MAX];
    int resp_count;
    int resp_read;

    uint8_t irq_enable;
    uint8_t irq_flags;

    Response queue[QUEUE_MAX];
    int queue_head;
    int queue_count;

    int busy;
    int deliver_delay;

    // Disc image and read state.
    FILE *disc;
    long track_offset;      // byte offset of track 1 in the file
    int has_disc;
    uint8_t mode;           // Setmode value (bit5 = whole 2340-byte sector)
    int seek_lba;           // target set by Setloc
    int read_lba;           // next sector a read will deliver
    int reading;            // a ReadN/ReadS is in progress
    int read_delay;         // cycles until the next sector

    // Data FIFO: the user portion of the current sector, drained by the data
    // port or DMA channel 3.
    uint8_t data_buf[RAW_SECTOR];
    int data_len;
    int data_ptr;
    int data_active;        // the CPU asked for the sector (request bit)
} cd;

static int
bcd2bin(uint8_t b)
{
    return (b >> 4) * 10 + (b & 0x0F);
}

void
psx_cdrom_reset(void)
{
    FILE *disc = cd.disc;
    long off = cd.track_offset;
    int has = cd.has_disc;

    memset(&cd, 0, sizeof cd);

    // A reset must not eject the mounted image.
    cd.disc = disc;
    cd.track_offset = off;
    cd.has_disc = has;
}

int
cdrom_has_disc(void)
{
    return cd.has_disc;
}

int
cdrom_mount(const char *path)
{
    char binpath[1024];
    const char *toopen = path;
    size_t n = strlen(path);

    // A .cue names its .bin; open that instead (single-track images only).
    if (n > 4 && (strcmp(path + n - 4, ".cue") == 0 ||
                  strcmp(path + n - 4, ".CUE") == 0)) {
        FILE *cue = fopen(path, "r");
        if (!cue) { perror(path); return -1; }

        char line[512];
        char binname[512] = {0};
        while (fgets(line, sizeof line, cue)) {
            char *q1 = strchr(line, '"');
            char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q1 && q2) {
                size_t len = (size_t)(q2 - q1 - 1);
                if (len < sizeof binname) {
                    memcpy(binname, q1 + 1, len);
                    binname[len] = '\0';
                }
                break;
            }
        }
        fclose(cue);

        if (binname[0] == '\0') {
            fprintf(stderr, "%s: no FILE entry in cue\n", path);
            return -1;
        }

        // Resolve the .bin relative to the .cue's directory.
        const char *slash = strrchr(path, '/');
        int dirlen = slash ? (int)(slash - path + 1) : 0;
        snprintf(binpath, sizeof binpath, "%.*s%s", dirlen, path, binname);
        toopen = binpath;
    }

    FILE *f = fopen(toopen, "rb");
    if (!f) { perror(toopen); return -1; }

    if (cd.disc) {
        fclose(cd.disc);
    }

    cd.disc = f;
    cd.track_offset = 0;
    cd.has_disc = 1;
    return 0;
}

static uint8_t
status_byte(void)
{
    uint8_t s = STAT_MOTOR;
    if (cd.reading) {
        s |= STAT_READ;
    }
    return s;
}

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

// Load the sector at read_lba into the data buffer, honoring the mode's sector
// size. Mode2/Form1 user data is 2048 bytes at offset 24; a whole-sector read is
// 2340 bytes at offset 12 (after the 12-byte sync).
static void
load_sector(void)
{
    uint8_t raw[RAW_SECTOR];

    cd.data_len = 0;
    cd.data_ptr = 0;

    if (!cd.disc) {
        return;
    }

    long pos = cd.track_offset + (long)cd.read_lba * RAW_SECTOR;
    if (fseek(cd.disc, pos, SEEK_SET) != 0) {
        return;
    }

    size_t got = fread(raw, 1, RAW_SECTOR, cd.disc);
    if (got < RAW_SECTOR) {
        memset(raw + got, 0, RAW_SECTOR - got);
    }

    if (cd.mode & 0x20) {
        cd.data_len = 2340;
        memcpy(cd.data_buf, raw + 12, 2340);
    } else {
        cd.data_len = 2048;
        memcpy(cd.data_buf, raw + 24, 2048);
    }
}

static void
run_command(uint8_t cmd)
{
    uint8_t stat[1] = { status_byte() };

    switch (cmd) {
    case 0x01: // Getstat
        enqueue(INT3, stat, 1);
        break;

    case 0x02: // Setloc (amm, ass, asect in BCD)
        if (cd.param_count >= 3) {
            int m = bcd2bin(cd.param_fifo[0]);
            int s = bcd2bin(cd.param_fifo[1]);
            int f = bcd2bin(cd.param_fifo[2]);
            cd.seek_lba = (m * 60 + s) * 75 + f - 150;
            if (cd.seek_lba < 0) {
                cd.seek_lba = 0;
            }
        }
        enqueue(INT3, stat, 1);
        break;

    case 0x0E: // Setmode
        if (cd.param_count >= 1) {
            cd.mode = cd.param_fifo[0];
        }
        enqueue(INT3, stat, 1);
        break;

    case 0x15: // SeekL
    case 0x16: // SeekP
        cd.read_lba = cd.seek_lba;
        enqueue(INT3, stat, 1);
        enqueue(INT2, stat, 1);
        break;

    case 0x06: // ReadN
    case 0x1B: // ReadS
        cd.read_lba = cd.seek_lba;
        cd.reading = 1;
        cd.read_delay = CDROM_READ_DELAY;
        stat[0] = status_byte();
        enqueue(INT3, stat, 1);
        break;

    case 0x09: // Pause
        cd.reading = 0;
        enqueue(INT3, stat, 1);
        stat[0] = status_byte();
        enqueue(INT2, stat, 1);
        break;

    case 0x08: // Stop
        cd.reading = 0;
        enqueue(INT3, stat, 1);
        enqueue(INT2, stat, 1);
        break;

    case 0x0A: // Init
        cd.mode = 0;
        cd.reading = 0;
        enqueue(INT3, stat, 1);
        enqueue(INT2, stat, 1);
        break;

    case 0x0C: // Demute
    case 0x0B: // Mute
    case 0x0D: // Setfilter
    case 0x03: // Play
    case 0x1E: // GetToc
        enqueue(INT3, stat, 1);
        break;

    case 0x19: { // Test
        uint8_t sub = (cd.param_count > 0) ? cd.param_fifo[0] : 0;
        if (sub == 0x20) {
            uint8_t ver[4] = { 0x94, 0x09, 0x19, 0xC0 };
            enqueue(INT3, ver, 4);
        } else {
            enqueue(INT3, stat, 1);
        }
        break;
    }

    case 0x1A: // GetID
        enqueue(INT3, stat, 1);
        if (cd.has_disc) {
            // Licensed disc: flags 0, type 0x20 (mode2 data), region "SCEA".
            uint8_t id[8] = { STAT_MOTOR, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A' };
            enqueue(INT2, id, 8);
        } else {
            uint8_t nodisc[8] = { 0x08, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            enqueue(INT5, nodisc, 8);
        }
        break;

    default:
        enqueue(INT3, stat, 1);
        break;
    }

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

    // Sector read pump: once any pending command interrupt is acknowledged and
    // the queue is drained, deliver the next sector as an INT1.
    if (cd.reading && cd.irq_flags == 0 && cd.queue_count == 0) {
        cd.read_delay -= (int)cycles;
        if (cd.read_delay <= 0) {
            uint8_t stat[1];
            load_sector();
            cd.read_lba++;
            stat[0] = status_byte();
            enqueue(INT1, stat, 1);
            deliver_next();
            cd.read_delay = CDROM_READ_DELAY;
        }
    }
}

void
cdrom_dma_read(uint32_t madr, uint32_t words)
{
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = 0;
        for (int b = 0; b < 4; b++) {
            uint8_t v = (cd.data_ptr < cd.data_len) ? cd.data_buf[cd.data_ptr++] : 0;
            w |= (uint32_t)v << (b * 8);
        }
        MEM_WRITE_32((madr + i * 4) & RAM_MASK, w);
    }
}

uint32_t
psx_cdrom_read(uint32_t phys_addr, int size)
{
    (void)size;
    uint32_t reg = (phys_addr - CDROM_BASE) & 3;

    if (reg == 0) {
        uint8_t s = cd.index & 3;

        if (cd.param_count == 0) {
            s |= 0x08; // PRMEMPT
        }
        s |= 0x10;     // PRMWRDY
        if (cd.resp_read < cd.resp_count) {
            s |= 0x20; // RSLRRDY
        }
        if (cd.data_active && cd.data_ptr < cd.data_len) {
            s |= 0x40; // DRQSTS: data FIFO has data
        }
        if (cd.busy) {
            s |= 0x80; // BUSYSTS
        }
        return s;
    }

    if (reg == 1) {
        uint8_t v = 0;
        if (cd.resp_read < cd.resp_count) {
            v = cd.resp_fifo[cd.resp_read++];
        }
        return v;
    }

    if (reg == 2) {
        // Data FIFO (byte access).
        if (cd.data_active && cd.data_ptr < cd.data_len) {
            return cd.data_buf[cd.data_ptr++];
        }
        return 0;
    }

    if (reg == 3) {
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

    switch (cd.index) {
    case 0:
        if (reg == 1) {
            run_command(v);
        } else if (reg == 2) {
            if (cd.param_count < FIFO_MAX) {
                cd.param_fifo[cd.param_count++] = v;
            }
        } else if (reg == 3) {
            // Request register: bit7 (BFRD) exposes the loaded sector on the data
            // FIFO. Clearing it only stops delivery; it must not discard the
            // sector already loaded by the read, which DMA is about to fetch.
            if (v & 0x80) {
                cd.data_active = 1;
                cd.data_ptr = 0;
            } else {
                cd.data_active = 0;
            }
        }
        break;

    case 1:
        if (reg == 2) {
            cd.irq_enable = v & 0x1F;
            if (cd.irq_flags & cd.irq_enable) {
                cpu.i_stat |= (1u << IRQ_CDROM);
            }
        } else if (reg == 3) {
            cd.irq_flags &= ~(v & 0x07);
            if (v & 0x40) {
                cd.param_count = 0;
            }
            if (cd.irq_flags == 0 && cd.queue_count > 0 && cd.deliver_delay <= 0) {
                cd.deliver_delay = CDROM_RESP_DELAY;
            }
        }
        break;

    default:
        break;
    }
}
