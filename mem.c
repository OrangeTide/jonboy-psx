#include "cdrom.h"
#include "cpu.h"
#include "dma.h"
#include "gpu.h"
#include "mem.h"
#include "mmio.h"
#include "pad.h"
#include "psio.h"
#include "spu.h"
#include "timer.h"
#include <stdio.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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


    switch (offset) {
    // 2. CONTROLLERS & MEMORY CARDS (0x1F801040)
    case 0x1040:
        return psx_pad_read_data();

    case 0x1044:
        return psx_pad_read_stat();

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

    // 4. TIMERS (0x1F801100 - 0x1F801128)
    case 0x1100:
        return psx_timer_get_value(0);

    case 0x1104:
        return psx_timer_get_mode(0);

    // 5. GRAPHICS PROCESSING UNIT (GPU) (0x1F801810 - 0x1F801814)
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

    switch (offset) {
    // Interrupt Control Register (0x1F801070 / 0x1F801074)
    case 0x1070:
        // psx_acknowledge_interrupts(value);
        cpu.i_stat &= ~value;
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

    // DMA Channels (Example: GPU DMA, CDROM DMA)
    case 0x10A8:
        psx_dma_set_channel_base(0, value);
        break;

    case 0x10AC:
        psx_dma_set_channel_block(0, value);
        break;

    case 0x10B0:
        psx_dma_trigger_channel(0, value);
        break;

    default:
        // printf("Unhandled MMIO Word Write: 0x%08X = 0x%08X\n", physical_addr, value);
        break;
    }
}
