#include "mc.h"
#include <stdio.h>
#include <string.h>

// Memory cards. See mc.h for the image format. The SIO protocol follows nocash's
// psx-spx and the pcsx-redux memory-card model: an 0x81 addressing byte selects
// the card, then a command byte (0x52 read, 0x57 write) drives a fixed byte
// sequence with the card ID (0x5A/0x5D), command acknowledge (0x5C/0x5D), the
// 128-byte sector, a checksum (XOR of the address bytes and the data), and an end
// byte (0x47 'G' good, 0x4E 'N' bad checksum).
//
// Two slots are modeled, one per controller port. pad.c routes a card
// transaction to slot 0 or slot 1 by the SIO port-select bit. The unsuffixed
// functions operate on slot 0.

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

#define MC_SLOTS 2

typedef struct {
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
} Card;

static Card cards[MC_SLOTS];

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
format_card(Card *c)
{
    memset(c->data, 0, sizeof c->data);

    // Frame 0: header magic "MC" plus its checksum.
    c->data[0] = 'M';
    c->data[1] = 'C';
    frame_checksum(&c->data[0]);

    // Frames 1..15: directory entries, all free.
    for (int f = 1; f <= 15; f++) {
        uint8_t *e = &c->data[f * MC_FRAME_SIZE];
        e[0] = 0xA0;   // block allocation state: free/formatted
        e[8] = 0xFF;   // next-block pointer: none
        e[9] = 0xFF;
        frame_checksum(e);
    }

    // Frames 16..35: broken-sector list, each entry marked unused.
    for (int f = 16; f <= 35; f++) {
        uint8_t *e = &c->data[f * MC_FRAME_SIZE];
        memset(e, 0xFF, 4);
        e[8] = 0xFF;
        e[9] = 0xFF;
        frame_checksum(e);
    }
}

int
mc_mount_slot(int slot, const char *path)
{
    if (slot < 0 || slot >= MC_SLOTS) {
        return -1;
    }
    Card *c = &cards[slot];

    memset(c, 0, sizeof *c);
    snprintf(c->path, sizeof c->path, "%s", path);
    c->flag = FLAG_DIR_UNREAD;

    FILE *f = fopen(path, "rb");
    if (f) {
        size_t got = fread(c->data, 1, MC_SIZE, f);
        fclose(f);
        if (got != MC_SIZE) {
            fprintf(stderr, "memcard: %s is %zu bytes, expected %d\n",
                    path, got, MC_SIZE);
            return -1;
        }
    } else {
        // No image yet: create a formatted card and save it.
        format_card(c);
        FILE *w = fopen(path, "wb");
        if (!w) {
            perror(path);
            return -1;
        }
        fwrite(c->data, 1, MC_SIZE, w);
        fclose(w);
    }

    c->present = 1;
    return 0;
}

int
mc_mount(const char *path)
{
    return mc_mount_slot(0, path);
}

int
mc_present_slot(int slot)
{
    return slot >= 0 && slot < MC_SLOTS && cards[slot].present;
}

void
mc_eject_slot(int slot)
{
    if (slot < 0 || slot >= MC_SLOTS) {
        return;
    }
    // Writes already persist to the backing file as they happen, so clearing the
    // slot is enough: the card stops answering the SIO0 protocol.
    memset(&cards[slot], 0, sizeof cards[slot]);
}

int
mc_present(void)
{
    return mc_present_slot(0);
}

// Persist one written frame to the backing file.
static void
flush_frame(Card *c, int frame)
{
    if (c->path[0] == '\0') {
        return;
    }
    FILE *f = fopen(c->path, "r+b");
    if (!f) {
        return;
    }
    fseek(f, (long)frame * MC_FRAME_SIZE, SEEK_SET);
    fwrite(&c->data[frame * MC_FRAME_SIZE], 1, MC_FRAME_SIZE, f);
    fclose(f);
}

static uint8_t
read_command(Card *c, int pos, uint8_t value, int *ack)
{
    switch (pos) {
    case 4:
        c->addr = (uint16_t)value << 8;
        return 0x00;
    case 5:
        c->addr |= value;
        c->chk = (uint8_t)((c->addr >> 8) ^ (c->addr & 0xFF));
        return 0x00;
    case 6:
        return RESP_ACK1;
    case 7:
        return RESP_ACK2;
    case 8:
        return (uint8_t)(c->addr >> 8);   // confirmed address MSB
    case 9:
        return (uint8_t)(c->addr & 0xFF); // confirmed address LSB
    case 138:
        return c->chk;                    // checksum
    case 139:
        *ack = 0;                         // last byte: no acknowledge
        c->flag = FLAG_DIR_READ;
        return RESP_GOOD;
    default:
        if (pos >= 10 && pos <= 137) {
            int i = pos - 10;
            uint8_t d = 0xFF;
            if (c->addr < MC_FRAMES) {
                d = c->data[c->addr * MC_FRAME_SIZE + i];
            }
            c->chk ^= d;
            return d;
        }
        *ack = 0;
        return RESP_IDLE;
    }
}

static uint8_t
write_command(Card *c, int pos, uint8_t value, int *ack)
{
    switch (pos) {
    case 4:
        c->addr = (uint16_t)value << 8;
        return 0x00;
    case 5:
        c->addr |= value;
        c->chk = (uint8_t)((c->addr >> 8) ^ (c->addr & 0xFF));
        return 0x00;
    case 134:
        c->chk_in = value; // checksum from the console
        return 0x00;
    case 135:
        return RESP_ACK1;
    case 136:
        return RESP_ACK2;
    case 137:
        *ack = 0; // last byte: no acknowledge
        if (c->addr >= MC_FRAMES) {
            return RESP_BADSEC;
        }
        if (c->chk_in != c->chk) {
            return RESP_BADCHK;
        }
        memcpy(&c->data[c->addr * MC_FRAME_SIZE], c->temp, MC_FRAME_SIZE);
        flush_frame(c, c->addr);
        c->flag = FLAG_DIR_READ;
        return RESP_GOOD;
    default:
        if (pos >= 6 && pos <= 133) {
            int i = pos - 6;
            c->temp[i] = value;
            c->chk ^= value;
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
mc_transfer_slot(int slot, int pos, uint8_t value, int *ack)
{
    *ack = 1;

    if (slot < 0 || slot >= MC_SLOTS || !cards[slot].present) {
        *ack = 0;
        return RESP_IDLE;
    }
    Card *c = &cards[slot];

    switch (pos) {
    case 0:            // 0x81 addressing byte
        c->cmd = 0;
        return RESP_IDLE;
    case 1:            // command byte
        c->cmd = value;
        return c->flag;
    case 2:
        return RESP_ID1;
    case 3:
        return RESP_ID2;
    }

    if (c->cmd == 0x52) {
        return read_command(c, pos, value, ack);
    }
    if (c->cmd == 0x57) {
        return write_command(c, pos, value, ack);
    }
    if (c->cmd == 0x53) {
        return id_command(pos, ack);
    }

    *ack = 0;
    return RESP_IDLE;
}

uint8_t
mc_transfer(int pos, uint8_t value, int *ack)
{
    return mc_transfer_slot(0, pos, value, ack);
}
