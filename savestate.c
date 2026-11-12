#include "savestate.h"

#include "cpu.h"
#include "gpu.h"
#include "gte.h"
#include "vram.h"

#include <string.h>

// Save-state file format: a 4-byte magic, a version, then each subsystem's state in
// a fixed order. There are no per-section tags; the version gates compatibility, so
// any change to a subsystem's serialized layout bumps SS_VERSION. See savestate.h
// for what is and is not captured.
static const char SS_MAGIC[4] = { 'J', 'B', 'S', 'S' };
#define SS_VERSION 1u

int
savestate_save(const char *path)
{
    // Drain the GPU worker so VRAM and the GPU registers reflect every queued draw
    // before they are snapshotted. A no-op when the threaded GPU is off.
    gpu_sync();

    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }

    uint32_t version = SS_VERSION;
    int ok = SS_WR(f, SS_MAGIC, sizeof SS_MAGIC)
             && SS_WR(f, &version, sizeof version)
             && SS_WR(f, &cpu, sizeof cpu)
             && SS_WR(f, &gte, sizeof gte)
             && SS_WR(f, &gpu, sizeof gpu)
             && SS_WR(f, &vram, sizeof vram)
             && mem_state_save(f) == 0
             && dma_state_save(f) == 0
             && spu_state_save(f) == 0
             && cdrom_state_save(f) == 0
             && xa_state_save(f) == 0
             && pad_state_save(f) == 0
             && mdec_state_save(f) == 0
             && timer_state_save(f) == 0
             && sched_state_save(f) == 0;

    fclose(f);
    return ok ? 0 : -1;
}

int
savestate_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }

    // Validate the header before touching any state, so a wrong or corrupt file
    // leaves the running machine unchanged.
    char magic[4];
    uint32_t version = 0;
    if (!SS_RD(f, magic, sizeof magic) || memcmp(magic, SS_MAGIC, 4) != 0
        || !SS_RD(f, &version, sizeof version) || version != SS_VERSION) {
        fclose(f);
        return -1;
    }

    // Drain the worker so no in-flight draw writes VRAM after we overwrite it.
    gpu_sync();

    int ok = SS_RD(f, &cpu, sizeof cpu)
             && SS_RD(f, &gte, sizeof gte)
             && SS_RD(f, &gpu, sizeof gpu)
             && SS_RD(f, &vram, sizeof vram)
             && mem_state_load(f) == 0
             && dma_state_load(f) == 0
             && spu_state_load(f) == 0
             && cdrom_state_load(f) == 0
             && xa_state_load(f) == 0
             && pad_state_load(f) == 0
             && mdec_state_load(f) == 0
             && timer_state_load(f) == 0
             && sched_state_load(f) == 0;

    fclose(f);
    return ok ? 0 : -1;
}
