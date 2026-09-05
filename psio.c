#include "psio.h"
#include <stdio.h>

PsioDevice psio;

bool
load_virtual_sd_card(const char *filepath)
{
    FILE *f = fopen(filepath, "rb");

    if (!f) {
        perror(filepath);
        return false;
    }

    // Keep the handle open: DMA and FIFO reads stream from it later. Close any
    // image that was already mounted first.
    if (psio.sd_card_file) {
        fclose(psio.sd_card_file);
    }

    psio.sd_card_file = f;

    return true;
}

void
process_psio_command(void)
{
    // TODO: decode psio.command_reg (mount, seek, read sector, report status)
    // and drive the FIFO / DMA accordingly. For now just acknowledge the
    // command by clearing BUSY so the guest does not stall.
    psio.status_reg &= ~0x01u;
}
