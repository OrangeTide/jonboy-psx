.text
.global __start

# Emit instructions exactly as written.
.set noreorder

# GPU line self-test. It draws one flat horizontal line, then a monochrome
# poly-line with two segments (horizontal then vertical). Checked by dumping
# VRAM (run with --vram-out): points on the lines carry the drawn color and
# points off them stay black.

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

    # GP0(40): flat line, green (0x00FF00), from (10,10) to (50,10).
    lui  $t1, 0x4000
    ori  $t1, $t1, 0xff00
    sw   $t1, 0x1810($t0)
    lui  $t1, 0x000a           # (10,10)
    ori  $t1, $t1, 0x000a
    sw   $t1, 0x1810($t0)
    lui  $t1, 0x000a           # (50,10)
    ori  $t1, $t1, 0x0032
    sw   $t1, 0x1810($t0)

    # GP0(48): monochrome poly-line, red (0x0000FF).
    lui  $t1, 0x4800
    ori  $t1, $t1, 0x00ff
    sw   $t1, 0x1810($t0)
    lui  $t1, 0x0032           # (10,50)
    ori  $t1, $t1, 0x000a
    sw   $t1, 0x1810($t0)
    lui  $t1, 0x0032           # (50,50)
    ori  $t1, $t1, 0x0032
    sw   $t1, 0x1810($t0)
    lui  $t1, 0x005a           # (50,90)
    ori  $t1, $t1, 0x0032
    sw   $t1, 0x1810($t0)
    lui  $t1, 0x5555           # termination code
    ori  $t1, $t1, 0x5555
    sw   $t1, 0x1810($t0)

    break
