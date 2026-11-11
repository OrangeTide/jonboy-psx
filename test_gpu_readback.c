// Readback-fence stress for the threaded GPU (docs/threaded-gpu.md). A game that
// issues GP0 0xC0 (VRAM-to-CPU) and then reads the result through GPUREAD must see
// every draw queued before the 0xC0. With the worker, those draws and the 0xC0
// itself run asynchronously, so psx_gpu_read_vram_or_gp0 drains the queue (gpu_sync)
// before it reads. This test exercises that fence directly: it queues a heavy draw
// stream plus a 0xC0 setup, then immediately reads the region back while the worker
// is still rasterizing, and checks the bytes match the synchronous reference. The
// draw stream is deliberately large so the worker lags and the fence actually
// blocks. Looping many times shakes out any timing-dependent hole in the fence.
//
// The differential test (test_gpu_thread.c) covers VRAM-write equivalence but emits
// no 0xC0, so it never waits on the readback fence; this test is that missing case.

#include "gpu.h"
#include "vram.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Deterministic xorshift32 PRNG so every run replays the identical stream.
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

static uint32_t
pos_word(void)
{
    int x = rrange(0, 500);
    int y = rrange(0, 480);
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

// The region read back through GPU 0xC0. Kept inside the drawn area so the pixels
// are meaningful, and even width so the two-pixels-per-word packing is exact.
#define RD_X 0
#define RD_Y 0
#define RD_W 256
#define RD_H 200
#define RD_WORDS (RD_W * RD_H / 2)

static void
emit(uint32_t w)
{
    if (stream_n < MAXWORDS) {
        stream[stream_n++] = w;
    }
}

// Build a heavy draw stream, then a GP0 0xC0 that sets up the readback region. The
// draw count is large so the worker is still busy when the first readback word is
// requested, which is what makes the fence block rather than find the queue empty.
static void
build_stream(void)
{
    stream_n = 0;
    rng_state = 0x5EED1234u;

    emit(0xE3000000u);                             // draw area top-left (0,0)
    emit(0xE4000000u | (511u << 10) | 1023u);      // draw area bottom-right
    emit(0xE5000000u);                             // draw offset (0,0)

    for (int i = 0; i < 1500; i++) {
        switch (rng() % 5) {
        case 0: // fill rectangle (0x02)
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
        case 2: // gouraud triangle (0x30)
            emit(cmd_color(0x30));
            emit(pos_word());
            emit(rng() & 0xFFFFFF);
            emit(pos_word());
            emit(rng() & 0xFFFFFF);
            emit(pos_word());
            break;
        case 3: { // CPU-to-VRAM upload (0xA0) into the readback region
            int w = rrange(1, 8) * 2;
            int h = rrange(1, 16);
            emit(0xA0000000u);
            emit(((uint32_t)rrange(0, RD_H - 1) << 16) | (uint32_t)rrange(0, RD_W - 1));
            emit(((uint32_t)h << 16) | (uint32_t)w);
            for (int p = 0; p < w * h / 2; p++) {
                emit(rng());
            }
            break;
        }
        default: // mono quad (0x28)
            emit(cmd_color(0x28));
            emit(pos_word());
            emit(pos_word());
            emit(pos_word());
            emit(pos_word());
            break;
        }
    }

    // Set up the VRAM-to-CPU read. This goes through the ring too, so the worker
    // owns is_read_active and rd_*; the fence must run it before the read below.
    emit(0xC0000000u);
    emit(((uint32_t)RD_Y << 16) | (uint32_t)RD_X);
    emit(((uint32_t)RD_H << 16) | (uint32_t)RD_W);
}

// Worker thread: the same park/wake loop the windowed build runs.
static void *
worker_main(void *arg)
{
    (void)arg;

    while (!gpu_worker_should_stop()) {
        if (!gpu_worker_pop_and_run()) {
            gpu_worker_wait();
        }
    }
    while (gpu_worker_pop_and_run()) {
        /* drain anything still queued */
    }
    return NULL;
}

static uint32_t rd_sync[RD_WORDS];
static uint32_t rd_thread[RD_WORDS];
static uint16_t vram_sync[VRAM_WIDTH * VRAM_HEIGHT];

static void
reset_gpu(void)
{
    memset(&vram, 0, sizeof vram);
    psx_gpu_reset();
}

// Submit the whole stream, then drain the readback into out[]. Each GPUREAD call
// fences on its own; the first pays the drain, the rest find the queue empty.
static void
run_stream(uint32_t *out)
{
    for (int i = 0; i < stream_n; i++) {
        gpu_submit_gp0(stream[i]);
    }
    for (int i = 0; i < RD_WORDS; i++) {
        out[i] = psx_gpu_read_vram_or_gp0();
    }
}

int
main(void)
{
    build_stream();

    // Synchronous reference: everything runs inline on this thread.
    reset_gpu();
    gpu_threaded = 0;
    run_stream(rd_sync);
    memcpy(vram_sync, vram.canvas, sizeof vram_sync);

    if (vram.is_read_active) {
        printf("RESULT: FAIL (read not fully drained in sync run)\n");
        return 1;
    }

    // Threaded: the same stream through the ring to a real worker, with the readback
    // issued immediately so the fence has to block on the lagging worker. Repeated so
    // a timing-dependent fence hole has many chances to show.
    gpu_threaded = 1;
    gpu_worker_reset();

    pthread_t t;
    if (pthread_create(&t, NULL, worker_main, NULL) != 0) {
        printf("RESULT: FAIL (could not start worker thread)\n");
        return 1;
    }

    const int iterations = 60;
    int ok = 1;

    for (int it = 0; it < iterations && ok; it++) {
        reset_gpu();           // safe: the previous iteration drained the ring
        run_stream(rd_thread);

        if (memcmp(rd_thread, rd_sync, sizeof rd_sync) != 0) {
            printf("RESULT: FAIL (readback differs on iteration %d: the fence let "
                   "the CPU read VRAM before the worker finished)\n", it);
            ok = 0;
            break;
        }
        if (memcmp(vram.canvas, vram_sync, sizeof vram_sync) != 0) {
            printf("RESULT: FAIL (final VRAM differs on iteration %d)\n", it);
            ok = 0;
            break;
        }
    }

    gpu_worker_request_stop();
    pthread_join(t, NULL);
    gpu_threaded = 0;

    if (ok) {
        printf("RESULT: PASS (%d words drawn, %d-word readback, %d iterations, "
               "threaded readback == synchronous through the fence)\n",
               stream_n, RD_WORDS, iterations);
    }
    return ok ? 0 : 1;
}
