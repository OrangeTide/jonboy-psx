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

// The pregap that precedes LBA 0. An MSF address of 00:02:00 is LBA 0, so a
// disc-relative LBA maps to a file sector index directly.
#define LEAD_IN_FRAMES 150

#define MAX_TRACKS 100
#define MAX_FILES  100

typedef struct {
    uint8_t type;
    uint8_t len;
    uint8_t data[FIFO_MAX];
} Response;

// One track of the mounted disc. start_lba is the absolute (disc) LBA of the
// track's INDEX 01; file_start_lba is the absolute LBA that maps to byte 0 of
// the backing file, so a sector at absolute LBA n lives at file byte
// (n - file_start_lba) * RAW_SECTOR.
typedef struct {
    int num;
    int is_audio;
    int start_lba;
    int file_start_lba;
    int file_idx;
} Track;

// Mounted disc image. Kept apart from the controller state in cd so a reset,
// which clears cd, does not eject the disc.
static struct {
    int has_disc;
    FILE *files[MAX_FILES];
    int num_files;
    Track tracks[MAX_TRACKS];
    int num_tracks;
    int lead_out_lba;       // absolute LBA just past the last track
} disc;

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

    // Read state.
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
    int sector_started;     // the request cursor has been positioned this sector
} cd;

static int
bcd2bin(uint8_t b)
{
    return (b >> 4) * 10 + (b & 0x0F);
}

static uint8_t
bin2bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

void
psx_cdrom_reset(void)
{
    // The disc image lives in its own struct, so clearing the controller state
    // does not eject it.
    memset(&cd, 0, sizeof cd);
}

int
cdrom_has_disc(void)
{
    return disc.has_disc;
}

// The track that contains an absolute LBA, or the last track for an LBA past the
// end. Returns NULL when no disc is mounted.
static Track *
track_at_lba(int lba)
{
    if (disc.num_tracks == 0) {
        return NULL;
    }

    for (int i = disc.num_tracks - 1; i >= 0; i--) {
        if (lba >= disc.tracks[i].start_lba) {
            return &disc.tracks[i];
        }
    }
    return &disc.tracks[0];
}

// Sector count of an open image file.
static long
file_sectors(FILE *f)
{
    long sz;

    if (fseek(f, 0, SEEK_END) != 0) {
        return 0;
    }
    sz = ftell(f);
    return sz > 0 ? sz / RAW_SECTOR : 0;
}

// Parse an "mm:ss:ff" cue address into a frame count, or -1 on failure.
static int
parse_msf(const char *s)
{
    int m = 0, sec = 0, f = 0;

    if (sscanf(s, "%d:%d:%d", &m, &sec, &f) != 3) {
        return -1;
    }
    return (m * 60 + sec) * 75 + f;
}

// Close the mounted image and clear the disc state.
static void
disc_eject(void)
{
    for (int i = 0; i < disc.num_files; i++) {
        if (disc.files[i]) {
            fclose(disc.files[i]);
        }
    }
    memset(&disc, 0, sizeof disc);
}

// Mount a raw single-file image (.bin/.iso) as one data track at LBA 0.
static int
mount_raw(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }

    disc.files[0] = f;
    disc.num_files = 1;

    disc.tracks[0].num = 1;
    disc.tracks[0].is_audio = 0;
    disc.tracks[0].start_lba = 0;
    disc.tracks[0].file_start_lba = 0;
    disc.tracks[0].file_idx = 0;
    disc.num_tracks = 1;

    disc.lead_out_lba = (int)file_sectors(f);
    disc.has_disc = 1;
    return 0;
}

// Parse a .cue sheet into the track table. Handles a single .bin holding every
// track (tracks laid out by their INDEX 01 times) and one .bin per track (files
// laid end to end). INDEX 00 pregaps and PREGAP/POSTGAP are not tracked; INDEX
// 01 gives each track's start.
static int
mount_cue(const char *path)
{
    FILE *cue = fopen(path, "r");
    if (!cue) { perror(path); return -1; }

    const char *slash = strrchr(path, '/');
    int dirlen = slash ? (int)(slash - path + 1) : 0;

    char line[512];
    int cur_file = -1;          // index into disc.files of the current FILE
    long cur_base_lba = 0;      // absolute LBA at byte 0 of the current file
    int track_num = 0;
    int track_audio = 0;
    int have_track = 0;

    while (fgets(line, sizeof line, cue)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }

        if (strncmp(p, "FILE", 4) == 0) {
            char *q1 = strchr(p, '"');
            char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (!q1 || !q2) {
                continue;
            }

            char name[512];
            size_t len = (size_t)(q2 - q1 - 1);
            if (len >= sizeof name) {
                len = sizeof name - 1;
            }
            memcpy(name, q1 + 1, len);
            name[len] = '\0';

            // Resolve the FILE only within the cue's own directory. Reduce the
            // reference to its base name so a hostile cue cannot reach outside
            // that directory with an absolute path or a ".." component.
            char *base = name;
            for (char *s = name; *s; s++) {
                if (*s == '/' || *s == '\\') {
                    base = s + 1;
                }
            }

            char full[1024];
            snprintf(full, sizeof full, "%.*s%s", dirlen, path, base);
            FILE *f = fopen(full, "rb");
            if (!f) { perror(full); fclose(cue); disc_eject(); return -1; }

            // Lay this file after the previous one on the disc timeline.
            if (disc.num_files > 0) {
                cur_base_lba += file_sectors(disc.files[disc.num_files - 1]);
            }
            if (disc.num_files < MAX_FILES) {
                cur_file = disc.num_files;
                disc.files[disc.num_files++] = f;
            } else {
                fclose(f);
            }
        } else if (strncmp(p, "TRACK", 5) == 0) {
            char type[32] = {0};
            track_num = 0;
            sscanf(p + 5, "%d %31s", &track_num, type);
            track_audio = (strncmp(type, "AUDIO", 5) == 0);
            have_track = 1;
        } else if (strncmp(p, "INDEX", 5) == 0 && have_track) {
            int idx = 0;
            char msf[32] = {0};
            if (sscanf(p + 5, "%d %31s", &idx, msf) != 2 || idx != 1) {
                continue;
            }

            int frames = parse_msf(msf);
            if (frames < 0) {
                frames = 0;
            }
            if (disc.num_tracks < MAX_TRACKS && cur_file >= 0) {
                Track *t = &disc.tracks[disc.num_tracks++];
                t->num = track_num;
                t->is_audio = track_audio;
                t->file_idx = cur_file;
                t->file_start_lba = (int)cur_base_lba;
                t->start_lba = (int)cur_base_lba + frames;
            }
            have_track = 0;
        }
    }
    fclose(cue);

    if (disc.num_tracks == 0 || disc.num_files == 0) {
        fprintf(stderr, "%s: no tracks in cue\n", path);
        disc_eject();
        return -1;
    }

    disc.lead_out_lba = (int)(cur_base_lba +
                              file_sectors(disc.files[disc.num_files - 1]));
    disc.has_disc = 1;
    return 0;
}

int
cdrom_mount(const char *path)
{
    size_t n = strlen(path);
    int is_cue = n > 4 && (strcmp(path + n - 4, ".cue") == 0 ||
                           strcmp(path + n - 4, ".CUE") == 0);

    disc_eject();

    return is_cue ? mount_cue(path) : mount_raw(path);
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
    cd.sector_started = 0;

    Track *t = track_at_lba(cd.read_lba);
    if (!t) {
        return;
    }

    FILE *f = disc.files[t->file_idx];
    long pos = (long)(cd.read_lba - t->file_start_lba) * RAW_SECTOR;
    if (fseek(f, pos, SEEK_SET) != 0) {
        return;
    }

    size_t got = fread(raw, 1, RAW_SECTOR, f);
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

    case 0x13: { // GetTN: first and last track numbers (BCD)
        int first = disc.num_tracks ? disc.tracks[0].num : 1;
        int last = disc.num_tracks ? disc.tracks[disc.num_tracks - 1].num : 1;
        uint8_t tn[3] = { status_byte(), bin2bcd(first), bin2bcd(last) };
        enqueue(INT3, tn, 3);
        break;
    }

    case 0x14: { // GetTD: start MSF of a track (BCD); track 0 = lead-out
        int want = cd.param_count > 0 ? bcd2bin(cd.param_fifo[0]) : 0;
        int lba = disc.lead_out_lba;
        for (int i = 0; i < disc.num_tracks; i++) {
            if (disc.tracks[i].num == want) {
                lba = disc.tracks[i].start_lba;
                break;
            }
        }
        int frame = lba + LEAD_IN_FRAMES;
        int m = frame / (60 * 75);
        int s = (frame / 75) % 60;
        uint8_t td[3] = { status_byte(), bin2bcd(m), bin2bcd(s) };
        enqueue(INT3, td, 3);
        break;
    }

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
        if (disc.has_disc) {
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
            // FIFO. The read cursor is positioned only on the first request for a
            // sector; later requests keep it where it is, so a game that drains
            // one sector in several transfers (for example a 12-byte header read
            // followed by a 2048-byte data read) continues rather than restarting.
            // Clearing the bit only stops delivery; it must not discard the sector
            // already loaded, which DMA is about to fetch.
            if (v & 0x80) {
                cd.data_active = 1;
                if (!cd.sector_started) {
                    cd.data_ptr = 0;
                    cd.sector_started = 1;
                }
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
