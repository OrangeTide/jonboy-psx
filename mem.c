#include "cdrom.h"
#include "savestate.h"
#include "cpu.h"
#include "dma.h"
#include "gpu.h"
#include "mdec.h"
#include "mem.h"
#include "mmio.h"
#include "pad.h"
#include "psio.h"
#include "spu.h"
#include "cdrom.h"
#include "timer.h"
#include "sched.h"
#include "lebytes.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

uint8_t system_ram[MEM_MAX_SIZE];
uint8_t bios_rom[BIOS_SIZE];
uint8_t scratchpad[SCRATCH_SIZE];

uint32_t read_hardware_word(uint32_t physical_addr);
void write_hardware_word(uint32_t physical_addr, uint32_t value);

// Little-endian read/write of a 1, 2, or 4 byte value inside a backing array.
// True if a physical address falls in the PSIO cartridge window in Expansion
// Region 1. That window holds both the menu-mode device registers (at its base)
// and the firmware ROM; psio_read/psio_write decode the registers first.
static int
in_psio_region(uint32_t a)
{
    return a >= PSIO_EXP1_BASE && a < PSIO_EXP1_BASE + PSIO_EXP1_SIZE;
}

// The loader backdoor register window in Expansion Region 2. Checked before the
// general MMIO block, which also covers this address range.
static int
in_loader_region(uint32_t a)
{
    return a >= LDR_REG_BASE && a < LDR_REG_BASE + LDR_REG_SIZE;
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

    if (in_loader_region(a)) {
        return loader_read(a, size);
    }

    // Everything below is a timing device (SPU, CD-ROM, pad, or the MMIO block).
    // Bring the hardware clock current so the register reads the right time, then
    // reschedule afterward since a read can have side effects (a FIFO pop) that
    // move the next event.
    psx_hardware_sync();

    uint32_t val = 0;

    if (a >= SPU_BASE && a < SPU_BASE + SPU_SIZE) {
        val = psx_spu_read(a, size);
    } else if (a >= CDROM_BASE && a < CDROM_BASE + CDROM_SIZE) {
        val = psx_cdrom_read(a, size);
    } else if (a >= PAD_BASE && a < PAD_BASE + PAD_SIZE) {
        val = psx_pad_read(a, size);
    } else if (a >= MMIO_BASE && a < (MMIO_BASE + MMIO_LENGTH)) {
        uint32_t word = read_hardware_word(a & ~3u);

        if (size == 4) {
            val = word;
        } else {
            uint32_t shift = (a & 3) * 8;
            uint32_t mask = (size == 1) ? 0xFF : 0xFFFF;
            val = (word >> shift) & mask;
        }
    }

    psx_hardware_resched();
    return val;
}

// Decode a physical address to its region and write a sized value. Sub-word
// MMIO writes merge into the addressed byte lane of the containing word.
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

    if (in_loader_region(a)) {
        loader_write(a, value, size);
        return;
    }

    // Everything below is a timing device (SPU, CD-ROM, pad, or the MMIO block).
    // Sync first so the device sees the right time, then reschedule afterward so
    // an event this write arms, cancels, or moves is folded into the deadline.
    psx_hardware_sync();

    if (a >= SPU_BASE && a < SPU_BASE + SPU_SIZE) {
        psx_spu_write(a, value, size);
    } else if (a >= CDROM_BASE && a < CDROM_BASE + CDROM_SIZE) {
        psx_cdrom_write(a, value, size);
    } else if (a >= PAD_BASE && a < PAD_BASE + PAD_SIZE) {
        psx_pad_write(a, value, size);
    } else if (a >= MMIO_BASE && a < (MMIO_BASE + MMIO_LENGTH)) {
        uint32_t aligned = a & ~3u;

        if (size == 4) {
            write_hardware_word(aligned, value);
        } else {
            // Sub-word write: merge the written bytes into their lane of the
            // aligned word, leaving the rest intact. Games do byte and halfword
            // writes to individual register fields (for example the DMA
            // interrupt register's enable byte at 0x1F8010F6), so the value has
            // to land in the addressed lane, not the low bytes of the word.
            uint32_t shift = (a & 3u) * 8;
            uint32_t mask  = ((size == 1) ? 0xFFu : 0xFFFFu) << shift;
            uint32_t word  = read_hardware_word(aligned);
            word = (word & ~mask) | ((value << shift) & mask);
            write_hardware_word(aligned, word);
        }
    }

    // BIOS is read-only; anything else is unmapped and ignored.

    psx_hardware_resched();
}

// Debugger watchpoint hook. NULL unless the gdb server has active watchpoints.
void (*mem_watch_hook)(uint32_t addr, int size, int is_write) = NULL;

// Fast path for the common case: an aligned word in main RAM. Instruction
// fetches and most loads/stores land here, so skip the mem_read/mem_write call
// and its region chain and go straight to the backing array. The bounds and
// mirror mask match the RAM case in mem_read/mem_write; anything else falls
// through to the full decoder. RAM has no MMIO side effects, so nothing else is
// needed. (RAM_END and RAM_MASK are kept in sync with mem_read by hand.)
uint32_t
mem_fetch_word(uint32_t v_addr)
{
    uint32_t a = TO_PHYSICAL(v_addr); // no watch hook: this is an instruction fetch

    if (a < RAM_END) {
        return load_sized(system_ram, a & RAM_MASK, 4);
    }

    return mem_read(v_addr, 4);
}

uint32_t
memory_read_word(uint32_t v_addr)
{
    if (mem_watch_hook) {
        mem_watch_hook(v_addr, 4, 0);
    }

    uint32_t a = TO_PHYSICAL(v_addr);

    if (a < RAM_END) {
        return load_sized(system_ram, a & RAM_MASK, 4);
    }

    return mem_read(v_addr, 4);
}

uint32_t
memory_read_halfword(uint32_t v_addr)
{
    if (mem_watch_hook) {
        mem_watch_hook(v_addr, 2, 0);
    }

    return mem_read(v_addr, 2);
}

uint32_t
memory_read_byte(uint32_t v_addr)
{
    if (mem_watch_hook) {
        mem_watch_hook(v_addr, 1, 0);
    }

    return mem_read(v_addr, 1);
}

void
memory_write_word(uint32_t v_addr, uint32_t value)
{
    if (mem_watch_hook) {
        mem_watch_hook(v_addr, 4, 1);
    }

    uint32_t a = TO_PHYSICAL(v_addr);

    if (a < RAM_END) {
        store_sized(system_ram, a & RAM_MASK, value, 4);
        return;
    }

    mem_write(v_addr, value, 4);
}

void
memory_write_halfword(uint32_t v_addr, uint32_t value)
{
    if (mem_watch_hook) {
        mem_watch_hook(v_addr, 2, 1);
    }

    mem_write(v_addr, value, 2);
}

void
memory_write_byte(uint32_t v_addr, uint32_t value)
{
    if (mem_watch_hook) {
        mem_watch_hook(v_addr, 1, 1);
    }

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
        case 0x0:
            return psx_timer_get_value(timer);

        case 0x4:
            return psx_timer_get_mode(timer);

        case 0x8:
            return psx_timer_get_target(timer);
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

    // MACROBLOCK DECODER (MDEC) (0x1F801820 data, 0x1F801824 control/status)
    case 0x1820:
    case 0x1824:
        return mdec_read(physical_addr);

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
        case 0x0:
            psx_timer_set_value(timer, value);
            return;

        case 0x4:
            psx_timer_set_mode(timer, value);
            return;

        case 0x8:
            psx_timer_set_target(timer, value);
            return;
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

    // DMA control register (0x1F8010F0): channel priorities/enables, R/W.
    case 0x10F0:
        dma_control = value;
        break;

    // DMA interrupt register (0x1F8010F4). Bits 0-23 (force flag, per-channel
    // enables, master enable) are read/write. The per-channel flag bits 24-30
    // are acknowledged by writing a 1, so keep any not being cleared.
    case 0x10F4: {
        uint32_t flags = dma_interrupt & 0x7F000000;
        flags &= ~(value & 0x7F000000);
        dma_interrupt = (value & 0x00FFFFFF) | flags;
        break;
    }

    // GPU Commands (GP0 / GP1)
    case 0x1810:
        gpu_submit_gp0(value);
        break;

    case 0x1814:
        psx_gpu_send_command_gp1(value);
        break;

    // MDEC command/data (0x1820) and control (0x1824).
    case 0x1820:
    case 0x1824:
        mdec_write(physical_addr, value);
        break;

    // DMA per-channel registers (0x1080-0x10E8) are handled by the generic
    // decoder block above, which also triggers a transfer on the start bit.

    default:
        // printf("Unhandled MMIO Word Write: 0x%08X = 0x%08X\n", physical_addr, value);
        break;
    }
}


int
mem_state_save(FILE *f)
{
    return (SS_WR(f, system_ram, RAM_SIZE)
            && SS_WR(f, scratchpad, SCRATCH_SIZE)) ? 0 : -1;
}

int
mem_state_load(FILE *f)
{
    return (SS_RD(f, system_ram, RAM_SIZE)
            && SS_RD(f, scratchpad, SCRATCH_SIZE)) ? 0 : -1;
}
