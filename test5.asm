.text
.global __start

# Emit instructions exactly as written.
.set noreorder

# GPU rasterization self-test. It sets the drawing area, then draws a flat red
# triangle with vertices (10,10), (100,10), (10,100). Checked by dumping VRAM
# (run with --vram-out): a point inside the triangle is red, one outside black.

__start:
    lui  $t0, 0x1f80        # I/O base; GP0 = 0x1F801810, GP1 = 0x1F801814

    # GP1(00): reset the GPU.
    sw   $zero, 0x1814($t0)

    # GP0(E3): drawing area top-left = (0,0).
    lui  $t1, 0xe300
    sw   $t1, 0x1810($t0)

    # GP0(E4): drawing area bottom-right = (319,239).
    lui  $t1, 0xe403
    ori  $t1, $t1, 0xbd3f
    sw   $t1, 0x1810($t0)

    # GP0(E5): drawing offset = (0,0).
    lui  $t1, 0xe500
    sw   $t1, 0x1810($t0)

    # GP0(20): flat triangle, colour red (0x0000FF).
    lui  $t1, 0x2000
    ori  $t1, $t1, 0x00ff
    sw   $t1, 0x1810($t0)

    # Vertex 0 = (10,10).
    lui  $t1, 0x000a
    ori  $t1, $t1, 0x000a
    sw   $t1, 0x1810($t0)

    # Vertex 1 = (100,10).
    lui  $t1, 0x000a
    ori  $t1, $t1, 0x0064
    sw   $t1, 0x1810($t0)

    # Vertex 2 = (10,100).
    lui  $t1, 0x0064
    ori  $t1, $t1, 0x000a
    sw   $t1, 0x1810($t0)

    break
