// Host-side unit test for CD-DA playback in cdrom.c. It builds a synthetic
// two-track disc (a data track and an audio track backed by a known byte
// pattern), drives the CD-ROM registers to issue Play, pumps the controller,
// and checks that the decoded PCM handed to the SPU matches the disc bytes and
// that playback advances sector by sector.
//
// cdrom.c is compiled on its own for this test, so the few globals it refers to
// are provided here: the cpu interrupt-status word, main RAM (only touched by
// the DMA path, unused here), and the SPU CD-audio sink, which we capture.

#include "cdrom.h"
#include "cpu.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

R3051_CPU cpu;
uint8_t system_ram[2 * 1024 * 1024];

#define RAW_SECTOR 2352
#define CDDA_FRAMES 588

// Captured CD-audio output.
static int16_t cap_pcm[CDDA_FRAMES * 2];
static int cap_have;
static int cap_calls;
// First left sample of each spu_cd_audio call, so the XA filter test can tell
// which sectors reached the SPU.
static int16_t cap_first_left[64];

void
spu_cd_audio(const int16_t *frames, int nframes)
{
    if (cap_calls == 0 && nframes == CDDA_FRAMES) {
        memcpy(cap_pcm, frames, sizeof cap_pcm);
        cap_have = 1;
    }
    if (cap_calls < 64 && nframes > 0) {
        cap_first_left[cap_calls] = frames[0];
    }
    cap_calls++;
}

// Deterministic byte pattern for the disc image.
static uint8_t
pat(long x)
{
    return (uint8_t)(((uint32_t)x * 2654435761u) >> 24);
}

// Write one Mode2 XA audio sector: a subheader marking it audio (form2) with the
// given file and channel, 4-bit stereo 37800 Hz coding, and filter-0 ADPCM data
// (every header byte 0, every data byte 0x71) so the decode is a constant left
// 0x1000 and right 0x7000.
static void
write_xa_sector(FILE *f, uint8_t file, uint8_t chan)
{
    uint8_t s[RAW_SECTOR];
    memset(s, 0, sizeof s);

    s[16] = file;   // subheader: file number
    s[17] = chan;   // channel number
    s[18] = 0x24;   // submode: audio (0x04) + form2 (0x20)
    s[19] = 0x01;   // coding: 4-bit, 37800 Hz, stereo
    s[20] = file;   // subheader copy
    s[21] = chan;
    s[22] = 0x24;
    s[23] = 0x01;

    for (int g = 0; g < 18; g++) {
        memset(s + 24 + g * 128 + 16, 0x71, 112); // headers stay 0
    }

    fwrite(s, 1, RAW_SECTOR, f);
}

static void cd_index(uint8_t i) { psx_cdrom_write(CDROM_BASE + 0, i, 1); }
static void cd_cmd(uint8_t c)   { cd_index(0); psx_cdrom_write(CDROM_BASE + 1, c, 1); }
static void cd_param(uint8_t p) { cd_index(0); psx_cdrom_write(CDROM_BASE + 2, p, 1); }
static void cd_ack(void)        { cd_index(1); psx_cdrom_write(CDROM_BASE + 3, 0x07, 1); }

int
main(void)
{
    const char *bin = "_out/_cdda.bin";
    const char *cue = "_out/_cdda.cue";
    const int total_sectors = 150;
    const int audio_lba = 75;   // track 2 starts here (file-relative = 00:01:00)

    // Write the synthetic image.
    FILE *f = fopen(bin, "wb");
    if (!f) { perror(bin); return 1; }
    for (long i = 0; i < (long)total_sectors * RAW_SECTOR; i++) {
        uint8_t b = pat(i);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);

    f = fopen(cue, "w");
    if (!f) { perror(cue); return 1; }
    fprintf(f, "FILE \"_cdda.bin\" BINARY\n");
    fprintf(f, "  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
    // audio_lba 75 frames = 00:01:00 file-relative.
    fprintf(f, "  TRACK 02 AUDIO\n    INDEX 01 00:01:00\n");
    fclose(f);

    psx_cdrom_reset();
    if (cdrom_mount(cue) != 0) {
        fprintf(stderr, "mount failed\n");
        return 1;
    }

    // Play track 2.
    cd_param(0x02);
    cd_cmd(0x03);

    // Deliver the Play acknowledge, then ack it so the play pump can run.
    psx_cdrom_tick(4000);
    cd_ack();

    // Pump several sectors; report mode is off, so nothing needs acking between
    // them. Each tick past the sector delay streams one sector.
    for (int i = 0; i < 6; i++) {
        psx_cdrom_tick(500000);
    }

    if (!cap_have) {
        fprintf(stderr, "FAIL: no CD audio was produced\n");
        return 1;
    }

    // The first audio sector is at audio_lba; verify the decode.
    long off = (long)audio_lba * RAW_SECTOR;
    int bad = 0;
    for (int i = 0; i < CDDA_FRAMES * 2; i++) {
        int16_t want = (int16_t)(pat(off + i * 2) | (pat(off + i * 2 + 1) << 8));
        if (cap_pcm[i] != want) {
            bad++;
            if (bad <= 3) {
                fprintf(stderr, "  sample %d: got %d want %d\n", i, cap_pcm[i], want);
            }
        }
    }
    if (bad) {
        fprintf(stderr, "FAIL: %d/%d PCM samples wrong\n", bad, CDDA_FRAMES * 2);
        return 1;
    }

    if (cap_calls < 2) {
        fprintf(stderr, "FAIL: playback did not advance (only %d sector)\n", cap_calls);
        return 1;
    }

    printf("PASS: CD-DA decode correct, %d sectors streamed\n", cap_calls);

    // Muted playback streams silence.
    cap_have = 0;
    cap_calls = 0;
    memset(cap_pcm, 0xFF, sizeof cap_pcm);

    cd_cmd(0x0B);           // Mute
    psx_cdrom_tick(4000);
    cd_ack();
    cd_param(0x02);
    cd_cmd(0x03);           // Play track 2
    psx_cdrom_tick(4000);
    cd_ack();
    for (int i = 0; i < 4; i++) {
        psx_cdrom_tick(500000);
    }

    if (!cap_have) {
        fprintf(stderr, "FAIL: muted play produced nothing\n");
        return 1;
    }
    for (int i = 0; i < CDDA_FRAMES * 2; i++) {
        if (cap_pcm[i] != 0) {
            fprintf(stderr, "FAIL: muted play not silent (sample %d = %d)\n",
                    i, cap_pcm[i]);
            return 1;
        }
    }
    printf("PASS: muted CD-DA is silent\n");

    remove(bin);
    remove(cue);

    // ---- XA filter (Setfilter file/channel selection) ------------------------
    // Build a data track of four XA audio sectors with different file/channel
    // subheaders, enable XA + the XA filter, select one file/channel, read, and
    // check that only the matching sectors reached the SPU.
    const char *xbin = "_out/_xa.bin";
    const char *xcue = "_out/_xa.cue";

    struct { uint8_t file, chan; } secs[4] = {
        { 1, 0 },  // match
        { 1, 1 },  // wrong channel
        { 1, 0 },  // match
        { 2, 0 },  // wrong file
    };

    f = fopen(xbin, "wb");
    if (!f) { perror(xbin); return 1; }
    for (int i = 0; i < 4; i++) {
        write_xa_sector(f, secs[i].file, secs[i].chan);
    }
    fclose(f);

    f = fopen(xcue, "w");
    if (!f) { perror(xcue); return 1; }
    fprintf(f, "FILE \"_xa.bin\" BINARY\n");
    fprintf(f, "  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
    fclose(f);

    psx_cdrom_reset();
    if (cdrom_mount(xcue) != 0) {
        fprintf(stderr, "mount failed\n");
        return 1;
    }

    cap_calls = 0;
    memset(cap_first_left, 0, sizeof cap_first_left);

    cd_param(0x00); cd_param(0x02); cd_param(0x00); // Setloc 00:02:00 = LBA 0
    cd_cmd(0x02);
    psx_cdrom_tick(4000);
    cd_ack();

    cd_param(0x40 | 0x08);   // Setmode: XA-ADPCM + XA-filter
    cd_cmd(0x0E);
    psx_cdrom_tick(4000);
    cd_ack();

    cd_param(0x01); cd_param(0x00); // Setfilter file 1, channel 0
    cd_cmd(0x0D);
    psx_cdrom_tick(4000);
    cd_ack();

    cd_cmd(0x06);            // ReadN from LBA 0
    psx_cdrom_tick(4000);
    cd_ack();

    // Stream past all four sectors (real disc rate under XA mode).
    for (int i = 0; i < 6; i++) {
        psx_cdrom_tick(500000);
    }

    if (cap_calls != 2) {
        fprintf(stderr, "FAIL: XA filter passed %d sectors, expected 2\n", cap_calls);
        return 1;
    }
    if (cap_first_left[0] != 0x1000 || cap_first_left[1] != 0x1000) {
        fprintf(stderr, "FAIL: XA filtered audio decoded wrong (%d, %d)\n",
                cap_first_left[0], cap_first_left[1]);
        return 1;
    }
    printf("PASS: XA filter played only the 2 matching sectors\n");

    // With the filter off, all four audio sectors should play.
    psx_cdrom_reset();
    if (cdrom_mount(xcue) != 0) {
        fprintf(stderr, "mount failed\n");
        return 1;
    }
    cap_calls = 0;

    cd_param(0x00); cd_param(0x02); cd_param(0x00);
    cd_cmd(0x02);
    psx_cdrom_tick(4000);
    cd_ack();

    cd_param(0x40);          // Setmode: XA-ADPCM, no filter
    cd_cmd(0x0E);
    psx_cdrom_tick(4000);
    cd_ack();

    cd_cmd(0x06);            // ReadN from LBA 0
    psx_cdrom_tick(4000);
    cd_ack();

    for (int i = 0; i < 6; i++) {
        psx_cdrom_tick(500000);
    }

    if (cap_calls != 4) {
        fprintf(stderr, "FAIL: filter off played %d sectors, expected 4\n", cap_calls);
        return 1;
    }
    printf("PASS: XA filter off plays all audio sectors\n");

    remove(xbin);
    remove(xcue);
    return 0;
}
