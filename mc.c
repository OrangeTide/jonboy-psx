#include "mc.h"
#include <stdio.h>
#include <string.h>

// Memory card in slot 1. See mc.h for the image format. The SIO protocol follows
// nocash's psx-spx and the pcsx-redux memory-card model: an 0x81 addressing byte
// selects the card, then a command byte (0x52 read, 0x57 write) drives a fixed
// byte sequence with the card ID (0x5A/0x5D), command acknowledge (0x5C/0x5D),
// the 128-byte sector, a checksum (XOR of the address bytes and the data), and an
// end byte (0x47 'G' good, 0x4E 'N' bad checksum).

// SIO response bytes.
#define RESP_IDLE    0xFF
#define RESP_ID1     0x5A
#define RESP_ID2     0x5D
#define RESP_ACK1    0x5C
#define RESP_ACK2    0x5D
#define RESP_GOOD    0x47
#define RESP_BADCHK  0x4E
#define RESP_BADSEC  0xFF

// FLAG byte. Bit 3 (0x08) means the directory has not been read since the card
// was inserted; it clears after the first successful read or write.
#define FLAG_DIR_UNREAD 0x08
#define FLAG_DIR_READ   0x00

static struct {
    int present;
    char path[1024];
    uint8_t data[MC_SIZE];
    uint8_t flag;

    // Per-transaction state.
    uint8_t cmd;         // current command (0x52 read / 0x57 write)
    uint16_t addr;       // sector (frame) number
    uint8_t chk;         // running checksum
    uint8_t chk_in;      // checksum byte received during a write
    uint8_t temp[MC_FRAME_SIZE]; // staged write data until the checksum verifies
} mc;

// Write byte 127 of a frame as the XOR of bytes 0..126.
static void
frame_checksum(uint8_t *frame)
{
    uint8_t x = 0;
    for (int i = 0; i < MC_FRAME_SIZE - 1; i++) {
        x ^= frame[i];
    }
    frame[MC_FRAME_SIZE - 1] = x;
}

// Lay down a freshly formatted card: the "MC" header frame, 15 free directory
// entries, and a broken-sector list. Everything else stays zero.
static void
format_card(void)
{
    memset(mc.data, 0, sizeof mc.data);

    // Frame 0: header magic "MC" plus its checksum.
    mc.data[0] = 'M';
    mc.data[1] = 'C';
    frame_checksum(&mc.data[0]);

    // Frames 1..15: directory entries, all free.
    for (int f = 1; f <= 15; f++) {
        uint8_t *e = &mc.data[f * MC_FRAME_SIZE];
        e[0] = 0xA0;   // block allocation state: free/formatted
        e[8] = 0xFF;   // next-block pointer: none
        e[9] = 0xFF;
        frame_checksum(e);
    }

    // Frames 16..35: broken-sector list, each entry marked unused.
    for (int f = 16; f <= 35; f++) {
        uint8_t *e = &mc.data[f * MC_FRAME_SIZE];
        memset(e, 0xFF, 4);
        e[8] = 0xFF;
        e[9] = 0xFF;
        frame_checksum(e);
    }
}

int
mc_mount(const char *path)
{
    memset(&mc, 0, sizeof mc);
    snprintf(mc.path, sizeof mc.path, "%s", path);
    mc.flag = FLAG_DIR_UNREAD;

    FILE *f = fopen(path, "rb");
    if (f) {
        size_t got = fread(mc.data, 1, MC_SIZE, f);
        fclose(f);
        if (got != MC_SIZE) {
            fprintf(stderr, "memcard: %s is %zu bytes, expected %d\n",
                    path, got, MC_SIZE);
            return -1;
        }
    } else {
        // No image yet: create a formatted card and save it.
        format_card();
        FILE *w = fopen(path, "wb");
        if (!w) {
            perror(path);
            return -1;
        }
        fwrite(mc.data, 1, MC_SIZE, w);
        fclose(w);
    }

    mc.present = 1;
    return 0;
}

int
mc_present(void)
{
    return mc.present;
}

// Persist one written frame to the backing file.
static void
flush_frame(int frame)
{
    if (mc.path[0] == '\0') {
        return;
    }
    FILE *f = fopen(mc.path, "r+b");
    if (!f) {
        return;
    }
    fseek(f, (long)frame * MC_FRAME_SIZE, SEEK_SET);
    fwrite(&mc.data[frame * MC_FRAME_SIZE], 1, MC_FRAME_SIZE, f);
    fclose(f);
}

static uint8_t
read_command(int pos, uint8_t value, int *ack)
{
    switch (pos) {
    case 4:
        mc.addr = (uint16_t)value << 8;
        return 0x00;
    case 5:
        mc.addr |= value;
        mc.chk = (uint8_t)((mc.addr >> 8) ^ (mc.addr & 0xFF));
        return 0x00;
    case 6:
        return RESP_ACK1;
    case 7:
        return RESP_ACK2;
    case 8:
        return (uint8_t)(mc.addr >> 8);   // confirmed address MSB
    case 9:
        return (uint8_t)(mc.addr & 0xFF); // confirmed address LSB
    case 138:
        return mc.chk;                    // checksum
    case 139:
        *ack = 0;                         // last byte: no acknowledge
        mc.flag = FLAG_DIR_READ;
        return RESP_GOOD;
    default:
        if (pos >= 10 && pos <= 137) {
            int i = pos - 10;
            uint8_t d = 0xFF;
            if (mc.addr < MC_FRAMES) {
                d = mc.data[mc.addr * MC_FRAME_SIZE + i];
            }
            mc.chk ^= d;
            return d;
        }
        *ack = 0;
        return RESP_IDLE;
    }
}

static uint8_t
write_command(int pos, uint8_t value, int *ack)
{
    switch (pos) {
    case 4:
        mc.addr = (uint16_t)value << 8;
        return 0x00;
    case 5:
        mc.addr |= value;
        mc.chk = (uint8_t)((mc.addr >> 8) ^ (mc.addr & 0xFF));
        return 0x00;
    case 134:
        mc.chk_in = value; // checksum from the console
        return 0x00;
    case 135:
        return RESP_ACK1;
    case 136:
        return RESP_ACK2;
    case 137:
        *ack = 0; // last byte: no acknowledge
        if (mc.addr >= MC_FRAMES) {
            return RESP_BADSEC;
        }
        if (mc.chk_in != mc.chk) {
            return RESP_BADCHK;
        }
        memcpy(&mc.data[mc.addr * MC_FRAME_SIZE], mc.temp, MC_FRAME_SIZE);
        flush_frame(mc.addr);
        mc.flag = FLAG_DIR_READ;
        return RESP_GOOD;
    default:
        if (pos >= 6 && pos <= 133) {
            int i = pos - 6;
            mc.temp[i] = value;
            mc.chk ^= value;
            return 0x00;
        }
        *ack = 0;
        return RESP_IDLE;
    }
}

// Get ID (0x53): after the ID and acknowledge bytes, the card returns four
// bytes describing its geometry, 04 00 00 80 for a standard card (0x0400 = 1024
// frames, 0x0080 = 128 bytes per frame).
static uint8_t
id_command(int pos, int *ack)
{
    switch (pos) {
    case 4:
        return RESP_ACK1;
    case 5:
        return RESP_ACK2;
    case 6:
        return 0x04;
    case 7:
        return 0x00;
    case 8:
        return 0x00;
    case 9:
        *ack = 0; // last byte: no acknowledge
        return 0x80;
    default:
        *ack = 0;
        return RESP_IDLE;
    }
}

uint8_t
mc_transfer(int pos, uint8_t value, int *ack)
{
    *ack = 1;

    if (!mc.present) {
        *ack = 0;
        return RESP_IDLE;
    }

    switch (pos) {
    case 0:            // 0x81 addressing byte
        mc.cmd = 0;
        return RESP_IDLE;
    case 1:            // command byte
        mc.cmd = value;
        return mc.flag;
    case 2:
        return RESP_ID1;
    case 3:
        return RESP_ID2;
    }

    if (mc.cmd == 0x52) {
        return read_command(pos, value, ack);
    }
    if (mc.cmd == 0x57) {
        return write_command(pos, value, ack);
    }
    if (mc.cmd == 0x53) {
        return id_command(pos, ack);
    }

    *ack = 0;
    return RESP_IDLE;
}
