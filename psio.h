#ifndef PSIO_H_
#define PSIO_H_
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

// Limits for the loader game index.
#define LDR_MAX_ENTRIES 512
#define LDR_MAX_NAME    128
#define LDR_MAX_PATH    1024

typedef struct {
    FILE *sd_card_file;        // Handle to the user's virtual SD card (.img/.bin)
    uint8_t *firmware_rom;     // EXP1 ROM image (our loader boot ROM)

    // Loader game index: host image files the guest menu can browse and boot.
    char games_dir[LDR_MAX_PATH];
    char entry_name[LDR_MAX_ENTRIES][LDR_MAX_NAME]; // display names
    char entry_path[LDR_MAX_ENTRIES][LDR_MAX_PATH]; // full host paths
    int  entry_count;
    int  sel_index;                                 // entry the guest selected

    // Name FIFO: the selected entry's name, drained a byte at a time.
    uint8_t  name_fifo[LDR_MAX_NAME];
    uint32_t name_ptr;
    uint32_t name_len;

    uint32_t status;           // loader status flags (LDR_STAT_*)
    int      boot_pending;     // set by LDR_CMD_BOOT, consumed by the run loop
} PsioDevice;

// --- EXP1 boot ROM ----------------------------------------------------------
// The cartridge ROM lives in Expansion Region 1. The BIOS pre/post boot check
// looks here for a signature and, on a match, calls the vector next to it, which
// is how our loader takes control.
#define PSIO_EXP1_BASE 0x1F000000
#define PSIO_EXP1_SIZE 0x00080000 // 512 KB

#define PSIO_POST_VECTOR (PSIO_EXP1_BASE + 0x00)
#define PSIO_POST_SIG    (PSIO_EXP1_BASE + 0x04)
#define PSIO_PRE_VECTOR  (PSIO_EXP1_BASE + 0x80)
#define PSIO_PRE_SIG     (PSIO_EXP1_BASE + 0x84)
#define PSIO_BOOT_SIGNATURE "Licensed by Sony Computer Entertainment Inc."

// --- Loader backdoor registers ----------------------------------------------
// Our own invented control interface (Cybdyn PSIO does not use this). It sits in
// Expansion Region 2 so it never collides with the EXP1 boot ROM. The guest menu
// drives it to enumerate images and boot a selection.
#define LDR_REG_BASE  0x1F802000
#define LDR_REG_SIZE  0x00000010
#define LDR_REG_CMD   (LDR_REG_BASE + 0x00) // W: command (LDR_CMD_*); R: status
#define LDR_REG_INDEX (LDR_REG_BASE + 0x04) // W: select entry; R: entry count
#define LDR_REG_DATA  (LDR_REG_BASE + 0x08) // R: pop a byte of the selected name

#define LDR_CMD_REFRESH 0x01u // rescan the games directory
#define LDR_CMD_NAME    0x02u // load the selected entry's name into the FIFO
#define LDR_CMD_BOOT    0x03u // request boot of the selected entry

#define LDR_STAT_READY 0x01u // idle, ready for a command
#define LDR_STAT_DATA  0x02u // the name FIFO holds unread bytes

extern PsioDevice psio;
bool load_virtual_sd_card(const char *filepath);
bool load_psio_firmware(const char *filepath);

// EXP1 ROM accessors, called by the memory decoder. size is 1, 2, or 4 bytes.
uint32_t psio_read(uint32_t phys_addr, int size);
void psio_write(uint32_t phys_addr, uint32_t value, int size);

// Loader backdoor accessors (EXP2 register window) and host-side controls.
uint32_t loader_read(uint32_t phys_addr, int size);
void loader_write(uint32_t phys_addr, uint32_t value, int size);
void loader_set_games_dir(const char *dir);
int loader_boot_pending(void);
const char *loader_boot_path(void);
void loader_clear_boot(void);

#endif
