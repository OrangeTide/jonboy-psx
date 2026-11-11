// Differential test for the threaded GPU (docs/threaded-gpu.md): a deterministic
// GP0 command stream must produce byte-identical VRAM whether it runs inline
// (synchronous) or through the command ring and a real worker thread. This is the
// correctness gate for the ring, the SPSC atomics, in-order replay, and the
// gpu_sync drain fence. The test replays the same word stream both ways, so any
// difference in the final VRAM is a defect in the threading machinery, not in the
// rasterizer (which is identical code on both paths).

#include "gpu.h"
#include "vram.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Deterministic xorshift32 PRNG so both runs replay the identical stream.
static uint32_t rng_state;

static uint32_t
rng(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

static int
rrange(int lo, int hi)
{
    return lo + (int)(rng() % (uint32_t)(hi - lo + 1));
}

// A vertex / position word with x,y bounded inside VRAM (positive 11-bit fields).
static uint32_t
pos_word(void)
{
    int x = rrange(0, 1000);
    int y = rrange(0, 500);
    return ((uint32_t)(y & 0x7FF) << 16) | (uint32_t)(x & 0x7FF);
}

static uint32_t
cmd_color(uint32_t cmd)
{
    return (cmd << 24) | (rng() & 0xFFFFFF);
}

#define MAXWORDS (1 << 20)
static uint32_t stream[MAXWORDS];
static int stream_n;

static void
emit(uint32_t w)
{
    if (stream_n < MAXWORDS) {
        stream[stream_n++] = w;
    }
}

// Build a varied, deterministic command stream: draw-area setup followed by a mix
// of fills, polygons, CPU-to-VRAM uploads, and VRAM-to-VRAM copies, which between
// them exercise the rasterizer, the payload path, and VRAM reads and writes.
static void
build_stream(void)
{
    stream_n = 0;
    rng_state = 0xC0FFEE11u;

    emit(0xE3000000u);                             // draw area top-left (0,0)
    emit(0xE4000000u | (511u << 10) | 1023u);      // draw area bottom-right
    emit(0xE5000000u);                             // draw offset (0,0)

    for (int i = 0; i < 3000; i++) {
        switch (rng() % 6) {
        case 0: // fill rectangle (0x02): cmd|color, position, size
            emit(cmd_color(0x02));
            emit(pos_word());
            emit(((uint32_t)rrange(1, 64) << 16) | (uint32_t)rrange(1, 64));
            break;
        case 1: // mono triangle (0x20)
            emit(cmd_color(0x20));
            emit(pos_word());
            emit(pos_word());
            emit(pos_word());
            break;
        case 2: // mono quad (0x28)
            emit(cmd_color(0x28));
            emit(pos_word());
            emit(pos_word());
            emit(pos_word());
            emit(pos_word());
            break;
        case 3: // gouraud triangle (0x30): cmd|c0, v0, c1, v1, c2, v2
            emit(cmd_color(0x30));
            emit(pos_word());
            emit(rng() & 0xFFFFFF);
            emit(pos_word());
            emit(rng() & 0xFFFFFF);
            emit(pos_word());
            break;
        case 4: { // CPU-to-VRAM (0xA0): 3 header words then w*h/2 payload words.
            // w kept even so the payload word count is exact (two pixels per word).
            int w = rrange(1, 8) * 2;
            int h = rrange(1, 16);
            emit(0xA0000000u);
            emit(pos_word());                              // dest x,y
            emit(((uint32_t)h << 16) | (uint32_t)w);       // size h,w
            for (int p = 0; p < w * h / 2; p++) {
                emit(rng());                               // two packed pixels
            }
            break;
        }
        default: // VRAM-to-VRAM copy (0x80): cmd, src, dst, size
            emit(0x80000000u);
            emit(pos_word());
            emit(pos_word());
            emit(((uint32_t)rrange(1, 64) << 16) | (uint32_t)rrange(1, 64));
            break;
        }
    }
}

// Worker thread for the threaded run: drain the ring until told to stop.
static atomic_int worker_stop;

static void *
worker_main(void *arg)
{
    (void)arg;

    while (!atomic_load(&worker_stop)) {
        if (!gpu_worker_pop_and_run()) {
            struct timespec ts = { 0, 1000 }; // 1 us idle, not a hot spin
            nanosleep(&ts, NULL);
        }
    }
    while (gpu_worker_pop_and_run()) {
        /* drain anything still queued */
    }
    return NULL;
}

static uint16_t snap_sync[VRAM_WIDTH * VRAM_HEIGHT];
static uint16_t snap_thread[VRAM_WIDTH * VRAM_HEIGHT];

static void
reset_gpu(void)
{
    memset(&vram, 0, sizeof vram);
    psx_gpu_reset();
}

int
main(void)
{
    build_stream();

    // Synchronous: gpu_submit_gp0 runs each word inline.
    reset_gpu();
    gpu_threaded = 0;
    for (int i = 0; i < stream_n; i++) {
        gpu_submit_gp0(stream[i]);
    }
    memcpy(snap_sync, vram.canvas, sizeof snap_sync);

    // Threaded: the same words go through the ring to a real worker thread.
    reset_gpu();
    gpu_threaded = 1;
    atomic_store(&worker_stop, 0);

    pthread_t t;
    if (pthread_create(&t, NULL, worker_main, NULL) != 0) {
        printf("RESULT: FAIL (could not start worker thread)\n");
        return 1;
    }
    for (int i = 0; i < stream_n; i++) {
        gpu_submit_gp0(stream[i]);
    }
    gpu_sync();                    // drain: the worker has run every word
    atomic_store(&worker_stop, 1);
    pthread_join(t, NULL);
    memcpy(snap_thread, vram.canvas, sizeof snap_thread);
    gpu_threaded = 0;

    if (memcmp(snap_sync, snap_thread, sizeof snap_sync) != 0) {
        for (size_t i = 0; i < (size_t)VRAM_WIDTH * VRAM_HEIGHT; i++) {
            if (snap_sync[i] != snap_thread[i]) {
                printf("FAIL: VRAM differs at (x=%zu y=%zu): sync=%04X thread=%04X\n",
                       i / VRAM_HEIGHT, i % VRAM_HEIGHT, snap_sync[i], snap_thread[i]);
                break;
            }
        }
        printf("RESULT: FAIL (%d-word stream)\n", stream_n);
        return 1;
    }

    printf("RESULT: PASS (%d-word stream, threaded VRAM == synchronous VRAM)\n",
           stream_n);
    return 0;
}
