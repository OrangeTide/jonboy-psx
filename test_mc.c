#include "mc.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Host unit test for the memory card (mc.c). It drives the SIO protocol through
// mc_transfer the way pad.c does, exercising read, write, persistence to the
// backing file, the bad-checksum path, and the FLAG byte.

static int failures;

static void
check(const char *what, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got 0x%02X want 0x%02X\n", what, got, want);
        failures++;
    }
}

// Run one transaction of n bytes, feeding send[] and recording recv[].
static void
run(const uint8_t *send, uint8_t *recv, int n)
{
    for (int pos = 0; pos < n; pos++) {
        int ack = 0;
        recv[pos] = mc_transfer(pos, send[pos], &ack);
    }
}

// Perform a read of a frame, returning the 128 data bytes and the end byte.
static uint8_t
do_read(int frame, uint8_t data[128])
{
    uint8_t send[140] = { 0 };
    uint8_t recv[140] = { 0 };
    send[0] = 0x81;
    send[1] = 0x52;
    send[4] = (uint8_t)(frame >> 8);
    send[5] = (uint8_t)(frame & 0xFF);
    run(send, recv, 140);

    check("read ID1", recv[2], 0x5A);
    check("read ID2", recv[3], 0x5D);
    check("read ack1", recv[6], 0x5C);
    check("read ack2", recv[7], 0x5D);
    check("read confirm MSB", recv[8], (frame >> 8) & 0xFF);
    check("read confirm LSB", recv[9], frame & 0xFF);

    uint8_t chk = (uint8_t)((frame >> 8) ^ (frame & 0xFF));
    for (int i = 0; i < 128; i++) {
        data[i] = recv[10 + i];
        chk ^= data[i];
    }
    check("read checksum", recv[138], chk);
    return recv[139];
}

// Perform a write of a frame, returning the end byte. If corrupt is set, send a
// wrong checksum.
static uint8_t
do_write(int frame, const uint8_t data[128], int corrupt)
{
    uint8_t send[138] = { 0 };
    uint8_t recv[138] = { 0 };
    send[0] = 0x81;
    send[1] = 0x57;
    send[4] = (uint8_t)(frame >> 8);
    send[5] = (uint8_t)(frame & 0xFF);

    uint8_t chk = (uint8_t)((frame >> 8) ^ (frame & 0xFF));
    for (int i = 0; i < 128; i++) {
        send[6 + i] = data[i];
        chk ^= data[i];
    }
    send[134] = corrupt ? (uint8_t)(chk ^ 0xFF) : chk;
    run(send, recv, 138);

    check("write ack1", recv[135], 0x5C);
    check("write ack2", recv[136], 0x5D);
    return recv[137];
}

int
main(void)
{
    const char *path = "_out/_test.mcr";
    remove(path);

    // Mount creates a fresh formatted card.
    if (mc_mount(path) != 0) {
        fprintf(stderr, "FAIL: mount\n");
        return 1;
    }
    check("present", mc_present(), 1);

    // FLAG is 0x08 (directory unread) on the first command.
    {
        int ack = 0;
        mc_transfer(0, 0x81, &ack);
        uint8_t flag = mc_transfer(1, 0x52, &ack);
        check("initial FLAG", flag, 0x08);
        // finish the transaction so state is clean
        for (int pos = 2; pos < 140; pos++) {
            mc_transfer(pos, 0, &ack);
        }
    }

    // Frame 0 holds the "MC" header of a formatted card.
    uint8_t data[128];
    check("read header end byte", do_read(0, data), 0x47);
    check("header 'M'", data[0], 'M');
    check("header 'C'", data[1], 'C');

    // After a read the FLAG clears to 0x00.
    {
        int ack = 0;
        mc_transfer(0, 0x81, &ack);
        uint8_t flag = mc_transfer(1, 0x52, &ack);
        check("FLAG after read", flag, 0x00);
        for (int pos = 2; pos < 140; pos++) {
            mc_transfer(pos, 0, &ack);
        }
    }

    // Write a known pattern to frame 16 and read it back.
    uint8_t pattern[128];
    for (int i = 0; i < 128; i++) {
        pattern[i] = (uint8_t)(i * 7 + 3);
    }
    check("write end byte", do_write(16, pattern, 0), 0x47);

    uint8_t back[128];
    check("read-back end byte", do_read(16, back), 0x47);
    check("read-back matches", memcmp(pattern, back, 128) == 0, 1);

    // Persistence: re-mount from the file and confirm the data survived.
    if (mc_mount(path) != 0) {
        fprintf(stderr, "FAIL: remount\n");
        return 1;
    }
    check("persisted end byte", do_read(16, back), 0x47);
    check("persisted matches", memcmp(pattern, back, 128) == 0, 1);

    // A write with a bad checksum is rejected and leaves the frame unchanged.
    uint8_t other[128];
    memset(other, 0xEE, sizeof other);
    check("bad-checksum end byte", do_write(16, other, 1), 0x4E);
    check("bad write end byte", do_read(16, back), 0x47);
    check("bad write left data intact", memcmp(pattern, back, 128) == 0, 1);

    remove(path);

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASS: memory card read/write/persist",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
