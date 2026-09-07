#ifndef PSIO_H_
#define PSIO_H_
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    FILE *sd_card_file;        // Handle to the user's virtual SD card (.img/.bin)
    uint8_t *firmware_rom;     // 512KB allocated buffer holding the PSIO menu/ARM firmware

    // Virtual Registers
    uint32_t status_reg;       // Status flags (see PSIO_STAT_*)
    uint32_t irq_reg;          // Interrupt/ack latch (PSIO_REG_IRQ)

    // Command packet being assembled by writes to the data port. A command is
    // three words (12 bytes): header, LBA, and an XOR checksum.
    uint32_t tx_words[3];
    uint32_t tx_count;

    // Receive staging buffer. Sector data is streamed from the image through
    // here as the guest drains the data port; refilled a chunk at a time.
    uint8_t fifo_buffer[4096];
    uint32_t fifo_ptr;         // read cursor into fifo_buffer
    uint32_t fifo_count;       // unread bytes staged in fifo_buffer
    uint32_t stream_remaining; // bytes of the active read not yet staged
} PsioDevice;

// The image is addressed in fixed-size sectors. This matches a CD-ROM
// Mode1/Mode2-Form1 user-data sector, which is what a booted game reads.
#define PSIO_SECTOR_SIZE 2048

// PSIO menu-mode registers live on the parallel-port bus in Expansion Region 1,
// at the base of the window. The rest of the window is the firmware ROM.
#define PSIO_EXP1_BASE 0x1F000000
#define PSIO_EXP1_SIZE 0x00080000 // 512 KB

#define PSIO_REG_DATA   (PSIO_EXP1_BASE + 0x00) // R: pop RX FIFO; W: push command word
#define PSIO_REG_STATUS (PSIO_EXP1_BASE + 0x04) // R: status flags
#define PSIO_REG_CMD    (PSIO_EXP1_BASE + 0x04) // W: strobe to run the packet
#define PSIO_REG_IRQ    (PSIO_EXP1_BASE + 0x08) // R/W: interrupt / ack latch

// Status register bit flags (read at PSIO_REG_STATUS).
#define PSIO_STAT_RX_READY    (1u << 0) // data is waiting in the FIFO for the PS1
#define PSIO_STAT_TX_FREE     (1u << 1) // the PS1 may write another command word
#define PSIO_STAT_BUSY        (1u << 2) // the cartridge is processing a command
#define PSIO_STAT_SD_INSERTED (1u << 3) // an SD image is mounted

// Command header (tx_words[0]): id in bits 0-7, flags in 8-15, block count in
// 16-31. tx_words[1] is the LBA; tx_words[2] is header XOR LBA.
#define PSIO_CMD_READ_SECTOR 0x20u
#define PSIO_FLAG_DMA        0x01u // read is drained over DMA channel 3, not the FIFO

extern PsioDevice psio;
bool load_virtual_sd_card(const char *filepath);
bool load_psio_firmware(const char *filepath);
void process_psio_command(void);

// Physical-address accessors for the two PSIO windows, called by the memory
// decoder. size is 1, 2, or 4 bytes.
uint32_t psio_read(uint32_t phys_addr, int size);
void psio_write(uint32_t phys_addr, uint32_t value, int size);

#endif
