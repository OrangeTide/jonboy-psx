#include "gpu.h"
#include "vram.h"

PSX_GPU gpu;

uint32_t
calculate_gp0_packet_length(uint32_t primary_word)
{
    uint32_t opcode = primary_word >> 24;

    switch (opcode) {
    case 0x00:
        return 1; // NOP

    case 0x01:
        return 1; // Clear Cache

    case 0x02:
        return 3; // Fill VRAM Rectangle

    case 0x20:
        return 4; // Monochrome 3-point Polygon (Opaque)

    case 0x28:
        return 5; // Monochrome 4-point Polygon (Opaque)

    case 0x30:
        return 6; // Shaded 3-point Polygon (Opaque)

    case 0x38:
        return 8; // Shaded 4-point Polygon (Opaque)

    case 0xA0:
        return 3; // Copy Image Data CPU-to-VRAM (Followed by pixels length calculation)

    case 0xE1:
        return 1; // Draw Mode Configuration

    case 0xE2:
        return 1; // Texture Window Setting

    case 0xE3:
        return 1; // Set Drawing Area Top-Left

    case 0xE4:
        return 1; // Set Drawing Area Bottom-Right

    case 0xE5:
        return 1; // Set Drawing Offset

    default:
        // Standard hardware fallback for unhandled / obscure drawing codes
        return 1;
    }
}

void
execute_completed_gp0_command(uint32_t opcode, uint32_t* buffer)
{
    switch (opcode) {
    case 0x20: // Draw Monochrome Triangle
        // Vertices are parsed out of buffer[1], buffer[2], buffer[3]
        // psx_rasterize_triangle(buffer);
        break;

    case 0xA0: { // CPU to VRAM Block Transfer Setup
        uint32_t width  = buffer[2] & 0xFFFF;
        uint32_t height = (buffer[2] >> 16) & 0xFFFF;
        uint32_t total_pixels = width * height;
        // Scale remaining expected inputs for raw stream payload processing
        uint32_t target_words = (total_pixels + 1) / 2;

        // Keep the engine open to consume subsequent payload blocks
        gpu.current_packet_length += target_words;
        return;
    }
    break;

    case 0xE5: // Drawing Offset
        // Adjust internal canvas offsets for frame coordinate shifts
        break;

    case 0x40: // Command 0xA0: CPU-to-VRAM Block Transfer Setup
        // Parameter Word 1: Coordinates (X is bottom 16 bits, Y is top 16 bits)
        vram.transfer_start_x   = buffer[1] & 0xFFFF;
        vram.transfer_start_y   = (buffer[1] >> 16) & 0xFFFF;

        // Parameter Word 2: Dimensions (Width is bottom 16 bits, Height is top 16 bits)
        vram.transfer_width     = buffer[2] & 0xFFFF;
        vram.transfer_height    = (buffer[2] >> 16) & 0xFFFF;

        // Initialize our streaming runtime pointers
        vram.transfer_current_x = vram.transfer_start_x;
        vram.transfer_current_y = vram.transfer_start_y;

        // Flag our system that subsequent GP0 words are raw pixel payloads
        vram.is_transfer_active = 1;

        // Clean out command buffers to cleanly receive data streams
        gpu.fifo_words_count = 0;
        gpu.current_packet_length = 0;
        break;

    }

    // Clear buffer count to await the next standalone operation
    gpu.fifo_words_count = 0;
    gpu.current_packet_length = 0;
}

void
psx_gpu_send_command_gp0(uint32_t value)
{
    // Catch active payload streams first!
    if (vram.is_transfer_active) {
        psx_gpu_write_pixel_payload(value);
        return;
    }

    // If we are currently starting a completely new command packet stream
    if (gpu.fifo_words_count == 0) {
        gpu.current_packet_length = calculate_gp0_packet_length(value);
    }

    // Append current stream parameter into processing FIFO
    if (gpu.fifo_words_count < GPU_FIFO_MAX) {
        gpu.command_fifo[gpu.fifo_words_count++] = value;
    }

    // Fire the command dispatcher if the buffer depth condition is fulfilled
    if (gpu.fifo_words_count >= gpu.current_packet_length) {
        uint32_t master_opcode = gpu.command_fifo[0] >> 24;
        execute_completed_gp0_command(master_opcode, gpu.command_fifo);
    }
}

void
psx_gpu_send_command_gp1(uint32_t value)
{
    uint32_t cmd = value >> 24;

    switch (cmd) {
    case 0x00: // Reset GPU Subsystem
        gpu.gpu_status = 0x14802000; // Reset status register to default hardware safe flags
        gpu.fifo_words_count = 0;
        gpu.current_packet_length = 0;
        break;

    case 0x01: // Clear Command FIFO Buffer
        gpu.fifo_words_count = 0;
        gpu.current_packet_length = 0;
        break;

    case 0x08: // Display Mode Setup
        // Handle PAL vs NTSC refresh rate selection flags
        break;
    }
}

// Returns data back to the CPU during a VRAM-to-CPU transfer. Default to safe NOP data.
uint32_t
psx_gpu_read_vram_or_gp0()
{
    return 0x00000000;
}

uint32_t
psx_gpu_get_status()
{
    // Return the hardware status register.
    // To fool basic games into thinking the GPU is always ready to draw:
    // Bit 26: Ready to receive Command Word (Always set to 1)
    // Bit 27: Ready to receive DMA block (Always set to 1)
    // Bit 28: Ready to send VRAM data to CPU (Always set to 1)
    return gpu.gpu_status | (1 << 26) | (1 << 27) | (1 << 28);
}
