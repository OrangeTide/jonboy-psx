.text
.global __start

# Emit instructions exactly as written.
.set noreorder

# GPU VRAM-to-CPU readback self-test. It uploads a known pixel (0x1234) to VRAM
# with GP0(A0), requests it back with GP0(C0), then reads GPUREAD. The value
# lands in $s0, which the headless runner prints as RESULT (expect 0x00001234).

__start:
    lui  $t0, 0x1f80        # I/O base

    # GP1(00): reset the GPU.
    sw   $zero, 0x1814($t0)

    # GP0(A0): copy rectangle (CPU to VRAM), destination (0,0), size 1x1.
    lui  $t1, 0xa000
    sw   $t1, 0x1810($t0)
    sw   $zero, 0x1810($t0)    # destination (0,0)
    lui  $t1, 0x0001
    ori  $t1, $t1, 0x0001
    sw   $t1, 0x1810($t0)
    lui  $t1, 0x1234           # pixel value 0x1234
    ori  $t1, $t1, 0x1234
    sw   $t1, 0x1810($t0)

    # GP0(C0): copy rectangle (VRAM to CPU), source (0,0), size 1x1.
    lui  $t1, 0xc000
    sw   $t1, 0x1810($t0)
    sw   $zero, 0x1810($t0)    # source (0,0)
    lui  $t1, 0x0001
    ori  $t1, $t1, 0x0001
    sw   $t1, 0x1810($t0)

    # Read the pixel back through GPUREAD.
    lw   $s0, 0x1810($t0)
    nop

    break
