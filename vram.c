void psx_gpu_write_pixel_payload(uint32_t two_pixels_word) {
    // Extract the two sequential 16-bit pixels from the single 32-bit MIPS register stream
    uint16_t pixels[2];
    pixels[0] = two_pixels_word & 0xFFFF;         // Pixel A (Lower 16 bits)
    pixels[1] = (two_pixels_word >> 16) & 0xFFFF;  // Pixel B (Upper 16 bits)

    for (int i = 0; i < 2; i++) {
        // Double check bounds safety against corrupted execution addresses
        uint32_t target_x = vram.transfer_current_x & (VRAM_WIDTH - 1);
        uint32_t target_y = vram.transfer_current_y & (VRAM_HEIGHT - 1);

        // Write pixel straight into our storage grid array map
        vram.canvas[target_x][target_y] = pixels[i];

        // Increment horizontal placement tracking pointer
        vram.transfer_current_x++;

        // Handle Row Wraparound: If we hit the width boundaries of the target texture box
        if (vram.transfer_current_x >= (vram.transfer_start_x + vram.transfer_width)) {
            vram.transfer_current_x = vram.transfer_start_x; // Reset line step
            vram.transfer_current_y++;                       // Step down one row

            // Check if the entire structural square transfer is fully complete
            if (vram.transfer_current_y >= (vram.transfer_start_y + vram.transfer_height)) {
                vram.is_transfer_active = 0; // Terminate active streaming mode
                return;
            }
        }
    }
}
