#include "cdrom.h"
#include "cpu.h"
#include "dma.h"
#include "gpu.h"
#include "mem.h"
#include "mmio.h"
#include "pad.h"
#include "psio.h"
#include "spu.h"
#include "cdrom.h"
#include "timer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

uint8_t system_ram[MEM_MAX_SIZE];
uint8_t bios_rom[BIOS_SIZE];
uint8_t scratchpad[SCRATCH_SIZE];

uint32_t read_hardware_word(uint32_t physical_addr);
void write_hardware_word(uint32_t physical_addr, uint32_t value);

// Little-endian read/write of a 1, 2, or 4 byte value inside a backing array.
// The caller has already range-checked that off + size fits.
static uint32_t
load_sized(const uint8_t *base, uint32_t off, int size)
{
    uint32_t v = 0;

    for (int i = 0; i < size; i++) {
        v |= (uint32_t)base[off + i] << (i * 8);
    }

    return v;
}

static void
store_sized(uint8_t *base, uint32_t off, uint32_t value, int size)
{
    for (int i = 0; i < size; i++) {
        base[off + i] = (uint8_t)(value >> (i * 8));
    }
}

// True if a physical address falls in the PSIO cartridge window in Expansion
// Region 1. That window holds both the menu-mode device registers (at its base)
// and the firmware ROM; psio_read/psio_write decode the registers first.
static int
in_psio_region(uint32_t a)
{
    return a >= PSIO_EXP1_BASE && a < PSIO_EXP1_BASE + PSIO_EXP1_SIZE;
}

// Decode a physical address to its region and read a sized value. Sub-word
// reads of MMIO extract the requested bytes from the containing word.
static uint32_t
mem_read(uint32_t v_addr, int size)
{
    uint32_t a = TO_PHYSICAL(v_addr);

    if (a < RAM_END) {
        return load_sized(system_ram, a & RAM_MASK, size);
    }

    if (a >= BIOS_BASE && a + (uint32_t)size <= BIOS_BASE + BIOS_SIZE) {
        return load_sized(bios_rom, a - BIOS_BASE, size);
    }

    if (a >= SCRATCH_BASE && a + (uint32_t)size <= SCRATCH_BASE + SCRATCH_SIZE) {
        return load_sized(scratchpad, a - SCRATCH_BASE, size);
    }

    if (in_psio_region(a)) {
        return psio_read(a, size);
    }

    if (a >= SPU_BASE && a < SPU_BASE + SPU_SIZE) {
        return psx_spu_read(a, size);
    }

    if (a >= CDROM_BASE && a < CDROM_BASE + CDROM_SIZE) {
        return psx_cdrom_read(a, size);
    }

    if (a >= PAD_BASE && a < PAD_BASE + PAD_SIZE) {
        return psx_pad_read(a, size);
    }

    if (a >= MMIO_BASE && a < (MMIO_BASE + MMIO_LENGTH)) {
        uint32_t word = read_hardware_word(a & ~3u);

        if (size == 4) {
            return word;
        }

        uint32_t shift = (a & 3) * 8;
        uint32_t mask = (size == 1) ? 0xFF : 0xFFFF;
        return (word >> shift) & mask;
    }

    return 0;
}

// Decode a physical address to its region and write a sized value. Sub-word
// MMIO writes are approximated for now; the CPU-facing sub-word registers used
// so far take full words.
static void
mem_write(uint32_t v_addr, uint32_t value, int size)
{
    uint32_t a = TO_PHYSICAL(v_addr);

    if (a < RAM_END) {
        store_sized(system_ram, a & RAM_MASK, value, size);
        return;
    }

    if (a >= SCRATCH_BASE && a + (uint32_t)size <= SCRATCH_BASE + SCRATCH_SIZE) {
        store_sized(scratchpad, a - SCRATCH_BASE, value, size);
        return;
    }

    if (in_psio_region(a)) {
        psio_write(a, value, size);
        return;
    }

    if (a >= SPU_BASE && a < SPU_BASE + SPU_SIZE) {
        psx_spu_write(a, value, size);
        return;
    }

    if (a >= CDROM_BASE && a < CDROM_BASE + CDROM_SIZE) {
        psx_cdrom_write(a, value, size);
        return;
    }

    if (a >= PAD_BASE && a < PAD_BASE + PAD_SIZE) {
        psx_pad_write(a, value, size);
        return;
    }

    if (a >= MMIO_BASE && a < (MMIO_BASE + MMIO_LENGTH)) {
        write_hardware_word(a & ~3u, value);
        return;
    }

    // BIOS is read-only; anything else is unmapped. Ignore.
}

uint32_t
memory_read_word(uint32_t v_addr)
{
    return mem_read(v_addr, 4);
}

uint32_t
memory_read_halfword(uint32_t v_addr)
{
    return mem_read(v_addr, 2);
}

uint32_t
memory_read_byte(uint32_t v_addr)
{
    return mem_read(v_addr, 1);
}

void
memory_write_word(uint32_t v_addr, uint32_t value)
{
    mem_write(v_addr, value, 4);
}

void
memory_write_halfword(uint32_t v_addr, uint32_t value)
{
    mem_write(v_addr, value, 2);
}

void
memory_write_byte(uint32_t v_addr, uint32_t value)
{
    mem_write(v_addr, value, 1);
}

// Little-endian 32-bit read from a byte buffer.
static uint32_t
rd32(const uint8_t *b, int off)
{
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8)
           | ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

int
mem_load_exe(const char *filename, uint32_t *out_pc, uint32_t *out_gp, uint32_t *out_sp)
{
    FILE *f = fopen(filename, "rb");

    if (!f) {
        perror(filename);
        return -1;
    }

    // The 2 KB PS-EXE header carries the entry PC, GP, load address/size, and
    // the initial stack pointer. The program image follows it.
    uint8_t hdr[0x800];

    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || memcmp(hdr, "PS-X EXE", 8) != 0) {
        fprintf(stderr, "%s: not a PS-EXE\n", filename);
        fclose(f);
        return -1;
    }

    uint32_t pc     = rd32(hdr, 0x10);
    uint32_t gp     = rd32(hdr, 0x14);
    uint32_t t_addr = rd32(hdr, 0x18);
    uint32_t t_size = rd32(hdr, 0x1C);
    uint32_t s_addr = rd32(hdr, 0x30);
    uint32_t s_size = rd32(hdr, 0x34);

    uint32_t phys = TO_PHYSICAL(t_addr);

    if ((uint64_t)phys + t_size > RAM_SIZE) {
        fprintf(stderr, "%s: image does not fit in RAM (addr 0x%08X size 0x%X)\n",
                filename, t_addr, t_size);
        fclose(f);
        return -1;
    }

    size_t readlen = fread(system_ram + phys, 1, t_size, f);
    fclose(f);

    if (readlen != t_size) {
        fprintf(stderr, "%s: short read (%zu of %u bytes)\n", filename, readlen, t_size);
        return -1;
    }

    *out_pc = pc;
    *out_gp = gp;
    *out_sp = s_addr ? (s_addr + s_size) : 0x801FFF00;
    return 0;
}

int
mem_load_bios(const char *filename)
{
    FILE *f = fopen(filename, "rb");

    if (!f) {
        perror(filename);
        return -1;
    }

    size_t readlen = fread(bios_rom, 1, BIOS_SIZE, f);
    fclose(f);

    if (readlen == 0) {
        fprintf(stderr, "%s: empty or unreadable BIOS\n", filename);
        return -1;
    }

    return 0;
}

int
mem_load_file(const char *filename, uint32_t phys_addr)
{
    FILE *f = fopen(filename, "rb");

    if (!f) {
        perror(filename);
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        perror(filename);
        fclose(f);
        return -1;
    }

    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize < 0) {
        perror(filename);
        fclose(f);
        return -1;
    }

    if ((uint64_t)phys_addr + (uint64_t)fsize > MEM_MAX_SIZE) {
        fprintf(stderr, "%s: does not fit in RAM at 0x%08X\n", filename, phys_addr);
        fclose(f);
        return -1;
    }

    size_t readlen = fread(system_ram + phys_addr, 1, (size_t)fsize, f);
    fclose(f);

    if (readlen != (size_t)fsize) {
        fprintf(stderr, "%s: short read\n", filename);
        return -1;
    }

    return 0;
}

uint32_t
read_hardware_word(uint32_t physical_addr)
{
    uint32_t offset = physical_addr - MMIO_BASE;

    // 1. Core Interconnect & Memory Control Registers (0x1F801000 - 0x1F801020)
    if (offset >= 0x1000 && offset <= 0x1020) {
        // Return memory timings / expansion parameters
        return 0x00000000;
    }

    // used inside MMIO dispatcher handlers:
    if (offset >= 0x1080 && offset <= 0x10E8) {
        uint32_t channel = (offset - 0x1080) / 0x10;
        uint32_t reg     = (offset - 0x1080) & 0x0F;

        if (reg == 0x0) return dma_channels[channel].madr;

        if (reg == 0x4) return dma_channels[channel].bcr;

        if (reg == 0x8) return dma_channels[channel].chcr;
    }

    // Root counters (0x1F801100 - 0x1F801128): three timers, each with a
    // value (reg 0), mode (reg 4), and target (reg 8) register.
    if (offset >= 0x1100 && offset < 0x1130) {
        int timer = (offset - 0x1100) / 0x10;

        switch (offset & 0xF) {
        case 0x0: return psx_timer_get_value(timer);
        case 0x4: return psx_timer_get_mode(timer);
        case 0x8: return psx_timer_get_target(timer);
        }
    }


    switch (offset) {
    // 3. INTERRUPT CONTROLLER (0x1F801070 - 0x1F801074)
    case 0x1070:
        return cpu.i_stat;

    case 0x1074:
        return cpu.i_mask;

    // 3. DMA CONTROLLER (0x1F8010F0 - 0x1F8010F4)
    case 0x10F0:
        return dma_control;

    case 0x10F4:
        return dma_interrupt;

    // GRAPHICS PROCESSING UNIT (GPU) (0x1F801810 - 0x1F801814)
    case 0x1810:
        return psx_gpu_read_vram_or_gp0();

    case 0x1814:
        return psx_gpu_get_status(); // Vital for frame synchronization games

    default:
        // Fail silently or log unhandled hardware registers
        // printf("Unhandled MMIO Word Read: 0x%08X\n", physical_addr);
        return 0;
    }
}

void
write_hardware_word(uint32_t physical_addr, uint32_t value)
{
    uint32_t offset = physical_addr - MMIO_BASE;

    if (offset >= 0x1080 && offset <= 0x10E8) {
        uint32_t channel = (offset - 0x1080) / 0x10;
        uint32_t reg     = (offset - 0x1080) & 0x0F;

        if (reg == 0x0) dma_channels[channel].madr = value;

        if (reg == 0x4) dma_channels[channel].bcr  = value;

        if (reg == 0x8) {
            dma_channels[channel].chcr = value;

            // If bit 24 is set, a DMA transfer has been triggered!
            if ((value & 0x01000000) != 0) {
                run_dma_transfer(channel);
            }
        }
    }

    // Root counters (0x1F801100 - 0x1F801128).
    if (offset >= 0x1100 && offset < 0x1130) {
        int timer = (offset - 0x1100) / 0x10;

        switch (offset & 0xF) {
        case 0x0: psx_timer_set_value(timer, value); return;
        case 0x4: psx_timer_set_mode(timer, value); return;
        case 0x8: psx_timer_set_target(timer, value); return;
        }
    }

    switch (offset) {
    // Interrupt Control Register (0x1F801070 / 0x1F801074)
    case 0x1070:
        // Acknowledge interrupts: a written 0 clears the corresponding I_STAT
        // bit, a written 1 leaves it unchanged. This is an AND with the written
        // value, not with its complement.
        cpu.i_stat &= value;
        break;

    case 0x1074:
        // psx_set_interrupt_mask(value);
        cpu.i_mask = value;
        break;

    // GPU Commands (GP0 / GP1)
    case 0x1810:
        psx_gpu_send_command_gp0(value);
        break;

    case 0x1814:
        psx_gpu_send_command_gp1(value);
        break;

    // DMA per-channel registers (0x1080-0x10E8) are handled by the generic
    // decoder block above, which also triggers a transfer on the start bit.

    default:
        // printf("Unhandled MMIO Word Write: 0x%08X = 0x%08X\n", physical_addr, value);
        break;
    }
}

