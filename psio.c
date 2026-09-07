#include "psio.h"
#include "lebytes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

PsioDevice psio;

bool
load_psio_firmware(const char *filepath)
{
    FILE *f = fopen(filepath, "rb");

    if (!f) {
        perror(filepath);
        return false;
    }

    if (!psio.firmware_rom) {
        psio.firmware_rom = malloc(PSIO_EXP1_SIZE);

        if (!psio.firmware_rom) {
            fprintf(stderr, "psio firmware: out of memory\n");
            fclose(f);
            return false;
        }
    }

    // Pad the whole window with zero so bytes past the image read as 0.
    memset(psio.firmware_rom, 0, PSIO_EXP1_SIZE);

    size_t got = fread(psio.firmware_rom, 1, PSIO_EXP1_SIZE, f);
    fclose(f);

    if (got == 0) {
        fprintf(stderr, "%s: empty or unreadable firmware\n", filepath);
        return false;
    }

    return true;
}

bool
load_virtual_sd_card(const char *filepath)
{
    FILE *f = fopen(filepath, "rb");

    if (!f) {
        perror(filepath);
        return false;
    }

    // Keep the handle open for later bulk reads. Close any prior image first.
    if (psio.sd_card_file) {
        fclose(psio.sd_card_file);
    }

    psio.sd_card_file = f;

    return true;
}

uint32_t
psio_read(uint32_t phys_addr, int size)
{
    // The EXP1 window presents the cartridge ROM. The BIOS reads the boot-hook
    // signature here and executes from the vector next to it, so instruction
    // fetches land in this same path.
    if (phys_addr >= PSIO_EXP1_BASE && phys_addr < PSIO_EXP1_BASE + PSIO_EXP1_SIZE) {
        if (!psio.firmware_rom) {
            return 0xFFFFFFFF; // no cartridge: an unmapped ROM reads as all-ones
        }

        return load_sized(psio.firmware_rom, phys_addr - PSIO_EXP1_BASE, size);
    }

    return 0;
}

void
psio_write(uint32_t phys_addr, uint32_t value, int size)
{
    (void)phys_addr;
    (void)value;
    (void)size;

    // The EXP1 window is presented as ROM to the PS1. Writes (flash programming,
    // MCU command registers) are not modeled yet; ignore them.
}

// ============================================================================
// LOADER BACKDOOR
// ============================================================================
// An invented control interface (not part of any real cartridge) that lets the
// guest menu enumerate host image files and request a boot. The heavy lifting
// (directory scan, file load) happens here on the host; the guest only browses
// names and picks one.

void
loader_set_games_dir(const char *dir)
{
    snprintf(psio.games_dir, sizeof psio.games_dir, "%s", dir ? dir : "");
}

// True if name ends with ext, case-insensitively.
static int
has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);

    if (n < e) {
        return 0;
    }

    const char *tail = name + (n - e);

    for (size_t i = 0; i < e; i++) {
        char a = tail[i], b = ext[i];

        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }

    return 1;
}

static int
name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

// Write "<games_dir>/<name>" into dst (LDR_MAX_PATH bytes). Returns 0 on success
// or -1 if it would not fit. memcpy is used rather than snprintf so the bound is
// an explicit runtime check the compiler can see is safe.
static int
build_path(char *dst, const char *name)
{
    size_t dlen = strlen(psio.games_dir);
    size_t nlen = strlen(name);

    if (dlen + 1 + nlen + 1 > LDR_MAX_PATH) {
        return -1;
    }

    memcpy(dst, psio.games_dir, dlen);
    dst[dlen] = '/';
    memcpy(dst + dlen + 1, name, nlen + 1);
    return 0;
}

// Rescan the games directory for bootable images. For now only PS-EXE files are
// indexed; CD image formats come with the game-execution path later.
static void
loader_scan(void)
{
    psio.entry_count = 0;
    psio.sel_index = 0;

    if (psio.games_dir[0] == '\0') {
        return;
    }

    DIR *d = opendir(psio.games_dir);

    if (!d) {
        return;
    }

    struct dirent *ent;

    while ((ent = readdir(d)) != NULL && psio.entry_count < LDR_MAX_ENTRIES) {
        size_t nlen = strlen(ent->d_name);

        if (!has_ext(ent->d_name, ".exe") || nlen + 1 > LDR_MAX_NAME) {
            continue;
        }

        if (build_path(psio.entry_path[psio.entry_count], ent->d_name) != 0) {
            continue; // full path too long
        }

        memcpy(psio.entry_name[psio.entry_count], ent->d_name, nlen + 1);
        psio.entry_count++;
    }

    closedir(d);

    // Sort by display name so the menu order is stable, then rederive each path
    // to keep it aligned with the sorted name.
    qsort(psio.entry_name, psio.entry_count, LDR_MAX_NAME, name_cmp);

    for (int i = 0; i < psio.entry_count; i++) {
        build_path(psio.entry_path[i], psio.entry_name[i]);
    }
}

// Copy the selected entry's name into the FIFO so the guest can read it out.
static void
loader_stage_name(void)
{
    psio.name_ptr = 0;
    psio.name_len = 0;
    psio.status &= ~LDR_STAT_DATA;

    if (psio.sel_index < 0 || psio.sel_index >= psio.entry_count) {
        return;
    }

    const char *name = psio.entry_name[psio.sel_index];
    size_t n = strlen(name);

    if (n > sizeof psio.name_fifo) {
        n = sizeof psio.name_fifo;
    }

    memcpy(psio.name_fifo, name, n);
    psio.name_len = (uint32_t)n;
    psio.status |= LDR_STAT_DATA;
}

uint32_t
loader_read(uint32_t phys_addr, int size)
{
    (void)size;

    switch (phys_addr) {
    case LDR_REG_CMD:
        return psio.status | LDR_STAT_READY;

    case LDR_REG_INDEX:
        return (uint32_t)psio.entry_count;

    case LDR_REG_DATA:
        if (psio.name_ptr < psio.name_len) {
            uint32_t b = psio.name_fifo[psio.name_ptr++];

            if (psio.name_ptr >= psio.name_len) {
                psio.status &= ~LDR_STAT_DATA; // name fully read
            }

            return b;
        }

        return 0;

    default:
        return 0;
    }
}

void
loader_write(uint32_t phys_addr, uint32_t value, int size)
{
    (void)size;

    switch (phys_addr) {
    case LDR_REG_CMD:
        switch (value) {
        case LDR_CMD_REFRESH:
            loader_scan();
            break;

        case LDR_CMD_NAME:
            loader_stage_name();
            break;

        case LDR_CMD_BOOT:
            if (psio.sel_index >= 0 && psio.sel_index < psio.entry_count) {
                psio.boot_pending = 1;
            }
            break;
        }
        break;

    case LDR_REG_INDEX:
        psio.sel_index = (int)value;
        break;

    default:
        break;
    }
}

int
loader_boot_pending(void)
{
    return psio.boot_pending;
}

const char *
loader_boot_path(void)
{
    if (psio.sel_index < 0 || psio.sel_index >= psio.entry_count) {
        return NULL;
    }

    return psio.entry_path[psio.sel_index];
}

void
loader_clear_boot(void)
{
    psio.boot_pending = 0;
}
