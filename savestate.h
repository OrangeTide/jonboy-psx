#ifndef SAVESTATE_H_
#define SAVESTATE_H_

#include <stdio.h>

// Save-state serialization (docs/menu-overlay.md, phase 4). A snapshot captures all
// mutable console state so a game can be resumed exactly. It does not capture the
// BIOS ROM, the mounted disc image, or the memory-card files (those are external
// media reloaded by path), nor the host-side worker threads. Windowed build only;
// the menu drives it while the emulation thread is paused and the GPU worker drained.

// Write the whole machine to a file, or read it back. Return 0 on success. A load
// that fails a magic or version check leaves the machine as it was.
int savestate_save(const char *path);
int savestate_load(const char *path);

// Small checked block I/O, shared by the per-subsystem state functions below.
#define SS_WR(f, p, n) (fwrite((p), (n), 1, (f)) == 1)
#define SS_RD(f, p, n) (fread((p), (n), 1, (f)) == 1)

// Per-subsystem serializers, defined where the state lives. Each returns 0 on
// success, non-zero on an I/O error.
int mem_state_save(FILE *f);
int mem_state_load(FILE *f);
int dma_state_save(FILE *f);
int dma_state_load(FILE *f);
int spu_state_save(FILE *f);
int spu_state_load(FILE *f);
int cdrom_state_save(FILE *f);
int cdrom_state_load(FILE *f);
int xa_state_save(FILE *f);
int xa_state_load(FILE *f);
int pad_state_save(FILE *f);
int pad_state_load(FILE *f);
int mdec_state_save(FILE *f);
int mdec_state_load(FILE *f);
int timer_state_save(FILE *f);
int timer_state_load(FILE *f);
int sched_state_save(FILE *f);
int sched_state_load(FILE *f);

#endif
