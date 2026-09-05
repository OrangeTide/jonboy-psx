.text
.global __start

# Emit instructions exactly as written.
.set noreorder

# GPU affine-texturing self-test. It uploads a 1x1 green texel into VRAM, points
# the texture page at it, then draws a textured triangle. Checked by dumping
# VRAM (run with --vram-out): a point inside the triangle is green (0x03E0),
# proving the textured path samples VRAM and modulates by the vertex color.

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

    # GP0(A0): copy rectangle (CPU to VRAM), destination = (640,0).
    lui  $t1, 0xa000
    sw   $t1, 0x1810($t0)
    ori  $t1, $zero, 0x0280    # dest Y=0, X=640
    sw   $t1, 0x1810($t0)
    lui  $t1, 0x0001           # size H=1, W=1
    ori  $t1, $t1, 0x0001
    sw   $t1, 0x1810($t0)
    lui  $t1, 0x03e0           # one green texel (second half unused)
    ori  $t1, $t1, 0x03e0
    sw   $t1, 0x1810($t0)

    # GP0(E1): draw mode. Texture page X base = 10*64 = 640, 15-bit color depth.
    lui  $t1, 0xe100
    ori  $t1, $t1, 0x010a
    sw   $t1, 0x1810($t0)

    # GP0(24): flat textured triangle, color 0x808080 (unmodified texel).
    lui  $t1, 0x2480
    ori  $t1, $t1, 0x8080
    sw   $t1, 0x1810($t0)

    # Vertex 0 = (10,10), texcoord (0,0), CLUT unused.
    lui  $t1, 0x000a
    ori  $t1, $t1, 0x000a
    sw   $t1, 0x1810($t0)
    sw   $zero, 0x1810($t0)

    # Vertex 1 = (100,10), texcoord (0,0), texpage 0x010a in upper half.
    lui  $t1, 0x000a
    ori  $t1, $t1, 0x0064
    sw   $t1, 0x1810($t0)
    lui  $t1, 0x010a
    sw   $t1, 0x1810($t0)

    # Vertex 2 = (10,100), texcoord (0,0).
    lui  $t1, 0x0064
    ori  $t1, $t1, 0x000a
    sw   $t1, 0x1810($t0)
    sw   $zero, 0x1810($t0)

    break
