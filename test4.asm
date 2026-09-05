.text
.global __start

# Emit instructions exactly as written.
.set noreorder

# DMA linked-list self-test. It builds a GPU command list in RAM whose single
# node fills a 16x16 rectangle at (0,0) with red, then kicks channel 2 in
# SyncMode 2. The result is checked by dumping VRAM (run with --vram-out).

__start:
    # Build the command list at 0x00001000:
    #   +0x0 header : count=3, next=0xFFFFFF (end)   -> 0x03FFFFFF
    #   +0x4 GP0 $02 fill, colour 0x0000FF (red)     -> 0x020000FF
    #   +0x8 position (Y<<16)|X = 0                   -> 0x00000000
    #   +0xC size (H<<16)|W = 16x16                   -> 0x00100010
    ori  $t0, $zero, 0x1000

    lui  $t1, 0x03ff
    ori  $t1, $t1, 0xffff
    sw   $t1, 0($t0)

    lui  $t1, 0x0200
    ori  $t1, $t1, 0x00ff
    sw   $t1, 4($t0)

    sw   $zero, 8($t0)

    lui  $t1, 0x0010
    ori  $t1, $t1, 0x0010
    sw   $t1, 12($t0)

    # Program DMA channel 2 (GPU): MADR = 0x1F8010A0, CHCR = 0x1F8010A8.
    lui  $t0, 0x1f80
    ori  $t1, $zero, 0x1000
    sw   $t1, 0x10A0($t0)   # MADR = list base

    # CHCR = start (bit 24) | SyncMode 2 (bits 9-10) | from RAM (bit 0).
    lui  $t1, 0x0100
    ori  $t1, $t1, 0x0401
    sw   $t1, 0x10A8($t0)   # writing the start bit runs the transfer

    break
