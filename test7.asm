.text
.global __start

# Emit instructions exactly as written.
.set noreorder

# GPU semi-transparency self-test. It draws an opaque green rectangle, sets the
# blend function to additive (mode 1), then draws a semi-transparent red
# rectangle on top. Checked by dumping VRAM (run with --vram-out): the overlap
# reads yellow (red + green), the green-only area stays green.

__start:
    lui  $t0, 0x1f80        # I/O base

    # GP1(00): reset the GPU.
    sw   $zero, 0x1814($t0)

    # GP0(E3)/E4/E5: drawing area (0,0)-(319,239), offset (0,0).
    lui  $t1, 0xe300
    sw   $t1, 0x1810($t0)
    lui  $t1, 0xe403
    ori  $t1, $t1, 0xbd3f
    sw   $t1, 0x1810($t0)
    lui  $t1, 0xe500
    sw   $t1, 0x1810($t0)

    # GP0(60): opaque monochrome rectangle, green (0x00FF00), variable size.
    lui  $t1, 0x6000
    ori  $t1, $t1, 0xff00
    sw   $t1, 0x1810($t0)
    lui  $t1, 0x000a           # position (10,10)
    ori  $t1, $t1, 0x000a
    sw   $t1, 0x1810($t0)
    lui  $t1, 0x0032           # size 50x50
    ori  $t1, $t1, 0x0032
    sw   $t1, 0x1810($t0)

    # GP0(E1): draw mode with semi-transparency function 1 (additive, B+F).
    lui  $t1, 0xe100
    ori  $t1, $t1, 0x0020
    sw   $t1, 0x1810($t0)

    # GP0(62): semi-transparent monochrome rectangle, red (0x0000FF).
    lui  $t1, 0x6200
    ori  $t1, $t1, 0x00ff
    sw   $t1, 0x1810($t0)
    lui  $t1, 0x0014           # position (20,20)
    ori  $t1, $t1, 0x0014
    sw   $t1, 0x1810($t0)
    lui  $t1, 0x0014           # size 20x20
    ori  $t1, $t1, 0x0014
    sw   $t1, 0x1810($t0)

    break
