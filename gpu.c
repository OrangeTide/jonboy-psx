#include "gpu.h"
#include "vram.h"

PSX_GPU gpu;

void
psx_gpu_reset(void)
{
    gpu.fifo_words_count = 0;
    gpu.current_packet_length = 0;
    gpu.gpu_status = 0x14802000;
    gpu.draw_offset_x = 0;
    gpu.draw_offset_y = 0;
    gpu.draw_x0 = 0;
    gpu.draw_y0 = 0;
    gpu.draw_x1 = VRAM_WIDTH - 1;
    gpu.draw_y1 = VRAM_HEIGHT - 1;
    gpu.tex_page_x = 0;
    gpu.tex_page_y = 0;
    gpu.tex_depth = 0;
    gpu.tex_semi_mode = 0;
    gpu.tw_mask_x = 0;
    gpu.tw_mask_y = 0;
    gpu.tw_off_x = 0;
    gpu.tw_off_y = 0;
    gpu.pl_active = 0;
    gpu.disp_x = 0;
    gpu.disp_y = 0;
    gpu.disp_w = 320;
    gpu.disp_h = 240;
    gpu.disp_blank = 1; // the display is disabled until software enables it
    gpu.disp_24bpp = 0;
    gpu.disp_range_y1 = 16;  // the standard NTSC 240-line active window
    gpu.disp_range_y2 = 256;
}

// Visible height from the GP1(07) vertical range. In interlaced (480-line)
// modes each programmed scanline covers two VRAM rows, so the count doubles. The
// result never exceeds the mode height and falls back to it if the range is
// empty or unset.
int
psx_gpu_active_height(void)
{
    int lines = gpu.disp_range_y2 - gpu.disp_range_y1;

    if (lines <= 0) {
        return gpu.disp_h;
    }

    if (gpu.disp_h > 240) {
        lines *= 2;
    }

    return lines < gpu.disp_h ? lines : gpu.disp_h;
}

// Decode a GP1(08) display-mode word into the on-screen resolution.
static void
set_display_mode(uint32_t v)
{
    static const int hres[4] = { 256, 320, 512, 640 };

    gpu.disp_w = (v & 0x40) ? 368 : hres[v & 3];
    gpu.disp_h = (v & 0x04) ? 480 : 240;
    gpu.disp_24bpp = (v >> 4) & 1;
}

// ---------------------------------------------------------------------------
// Rasterization helpers
// ---------------------------------------------------------------------------

typedef struct {
    int x, y;    // screen position, already offset
    int r, g, b; // vertex color (0-255)
    int u, v;    // texture coordinates (0-255 within the page)
} RVertex;

// Decoded texture parameters for a single primitive.
typedef struct {
    int page_x, page_y; // texture page base, in VRAM pixels
    int depth;          // 0 = 4-bit CLUT, 1 = 8-bit CLUT, 2/3 = 15-bit
    int clut_x, clut_y; // palette base, in VRAM pixels
} TexInfo;

// Per-primitive rendering state passed to the rasterizer.
typedef struct {
    int textured;
    int raw;       // raw texture: draw the texel without color modulation
    int semi;      // semi-transparency enabled (the primitive's ABE bit)
    int semi_mode; // semi-transparency blend function (0-3)
    TexInfo tex;
} PrimState;

static uint16_t
rgb_to_555(int r, int g, int b)
{
    // Clamp each channel to 0-255 first. An out-of-range value (from gouraud
    // interpolation overshooting at an edge, or a modulated texel) would
    // otherwise let r>>3 exceed 5 bits and corrupt the neighbouring channel,
    // showing up as rainbow speckle.
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;

    return (uint16_t)(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
}

// Expand a 5-bit VRAM color channel to the full 8-bit range.
static int
expand5(int c)
{
    return (c << 3) | (c >> 2);
}

// Unpack a BGR555 texel into 8-bit components.
static void
unpack_555(uint16_t px, int *r, int *g, int *b)
{
    *r = expand5(px & 0x1F);
    *g = expand5((px >> 5) & 0x1F);
    *b = expand5((px >> 10) & 0x1F);
}

// Modulate a texel channel by a vertex color channel. A vertex value of 128
// leaves the texel unchanged, matching the PSX texture-blend formula.
static int
modulate(int tex, int col)
{
    int v = tex * col / 128;
    return v > 255 ? 255 : v;
}

// Decode a texpage field (GP0 0xE1, or the upper half of a textured
// primitive's texcoord word) into the persistent texture-page state.
static void
set_texpage_bits(uint32_t tp)
{
    gpu.tex_page_x = (tp & 0x0F) * 64;
    gpu.tex_page_y = ((tp >> 4) & 1) * 256;
    gpu.tex_semi_mode = (tp >> 5) & 3;
    gpu.tex_depth = (tp >> 7) & 3;
}

// Blend a foreground pixel over the background already in VRAM, using one of
// the four PSX semi-transparency functions. Values are BGR555.
static uint16_t
blend_555(uint16_t back, uint16_t fore, int mode)
{
    int br = back & 0x1F, bg = (back >> 5) & 0x1F, bb = (back >> 10) & 0x1F;
    int fr = fore & 0x1F, fg = (fore >> 5) & 0x1F, fb = (fore >> 10) & 0x1F;
    int r, g, b;

    switch (mode) {
    case 0: // B/2 + F/2
        r = (br + fr) / 2; g = (bg + fg) / 2; b = (bb + fb) / 2;
        break;
    case 1: // B + F
        r = br + fr; g = bg + fg; b = bb + fb;
        break;
    case 2: // B - F
        r = br - fr; g = bg - fg; b = bb - fb;
        break;
    default: // B + F/4
        r = br + fr / 4; g = bg + fg / 4; b = bb + fb / 4;
        break;
    }

    if (r < 0) r = 0;
    if (r > 31) r = 31;
    if (g < 0) g = 0;
    if (g > 31) g = 31;
    if (b < 0) b = 0;
    if (b > 31) b = 31;

    return (uint16_t)((b << 10) | (g << 5) | r);
}

// Write one pixel to VRAM, blending against the current contents when the
// primitive is semi-transparent.
static void
put_pixel(int x, int y, uint16_t fore, int blend, int mode)
{
    if (blend) {
        vram.canvas[x][y] = blend_555(vram.canvas[x][y], fore, mode);
    } else {
        vram.canvas[x][y] = fore;
    }
}

// Sample a texel with affine (non-perspective) coordinates. Returns the raw
// BGR555 value, or -1 when the texel is fully transparent (all bits zero).
static int
sample_texel(const TexInfo *t, int u, int v)
{
    uint16_t texel;

    u &= 0xFF;
    v &= 0xFF;

    // Texture window: repeat a sub-region of the page. Mask and offset are in
    // 8-texel units. With all fields zero this leaves the coordinate untouched.
    u = (u & ~(gpu.tw_mask_x * 8)) | ((gpu.tw_off_x & gpu.tw_mask_x) * 8);
    v = (v & ~(gpu.tw_mask_y * 8)) | ((gpu.tw_off_y & gpu.tw_mask_y) * 8);

    if (t->depth == 0) {        // 4-bit CLUT: four texels per VRAM word
        uint16_t word = vram.canvas[(t->page_x + (u >> 2)) & (VRAM_WIDTH - 1)]
                                   [(t->page_y + v) & (VRAM_HEIGHT - 1)];
        int idx = (word >> ((u & 3) * 4)) & 0xF;
        texel = vram.canvas[(t->clut_x + idx) & (VRAM_WIDTH - 1)]
                           [t->clut_y & (VRAM_HEIGHT - 1)];
    } else if (t->depth == 1) { // 8-bit CLUT: two texels per VRAM word
        uint16_t word = vram.canvas[(t->page_x + (u >> 1)) & (VRAM_WIDTH - 1)]
                                   [(t->page_y + v) & (VRAM_HEIGHT - 1)];
        int idx = (word >> ((u & 1) * 8)) & 0xFF;
        texel = vram.canvas[(t->clut_x + idx) & (VRAM_WIDTH - 1)]
                           [t->clut_y & (VRAM_HEIGHT - 1)];
    } else {                    // 15-bit direct
        texel = vram.canvas[(t->page_x + u) & (VRAM_WIDTH - 1)]
                           [(t->page_y + v) & (VRAM_HEIGHT - 1)];
    }

    return texel == 0 ? -1 : texel;
}

// Sign-extend the 11-bit vertex coordinates and apply the drawing offset.
static int
vertex_x(uint32_t word)
{
    int v = word & 0x7FF;

    if (v & 0x400) {
        v |= ~0x7FF;
    }

    return v + gpu.draw_offset_x;
}

static int
vertex_y(uint32_t word)
{
    int v = (word >> 16) & 0x7FF;

    if (v & 0x400) {
        v |= ~0x7FF;
    }

    return v + gpu.draw_offset_y;
}

static int
min3(int a, int b, int c)
{
    int m = a < b ? a : b;
    return m < c ? m : c;
}

static int
max3(int a, int b, int c)
{
    int m = a > b ? a : b;
    return m > c ? m : c;
}

// Rasterize a triangle with the half-space edge-function method. Coordinates
// carry 4 fractional bits for subpixel accuracy, edge functions are stepped
// incrementally across each row, and a top-left fill rule keeps shared edges
// from being drawn twice. Color is flat (v0) or Gouraud-interpolated; texture
// coordinates are interpolated affinely, as the real PSX GPU does.
static void
draw_triangle(RVertex v0, RVertex v1, RVertex v2,
              int gouraud, const PrimState *ps)
{
    int x0 = v0.x << 4, y0 = v0.y << 4;
    int x1 = v1.x << 4, y1 = v1.y << 4;
    int x2 = v2.x << 4, y2 = v2.y << 4;

    int area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);

    if (area == 0) {
        return; // degenerate
    }

    // Normalize winding so the inside test is a set of >= 0 checks. Swapping
    // whole vertices keeps colors and texture coordinates in step.
    if (area < 0) {
        RVertex t = v1;
        v1 = v2;
        v2 = t;
        int tx = x1; x1 = x2; x2 = tx;
        int ty = y1; y1 = y2; y2 = ty;
        area = -area;
    }

    float inv_area = 1.0f / (float)area;

    int minx = (min3(x0, x1, x2) + 15) >> 4;
    int miny = (min3(y0, y1, y2) + 15) >> 4;
    int maxx = max3(x0, x1, x2) >> 4;
    int maxy = max3(y0, y1, y2) >> 4;

    // Clip to the drawing area, then to the VRAM bounds.
    if (minx < gpu.draw_x0) minx = gpu.draw_x0;
    if (miny < gpu.draw_y0) miny = gpu.draw_y0;
    if (maxx > gpu.draw_x1) maxx = gpu.draw_x1;
    if (maxy > gpu.draw_y1) maxy = gpu.draw_y1;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > VRAM_WIDTH - 1) maxx = VRAM_WIDTH - 1;
    if (maxy > VRAM_HEIGHT - 1) maxy = VRAM_HEIGHT - 1;

    // Edge-function coefficients. Each w corresponds to the weight of the
    // opposite vertex: w0 -> v0, w1 -> v1, w2 -> v2.
    int a01 = y0 - y1, b01 = x1 - x0;
    int a12 = y1 - y2, b12 = x2 - x1;
    int a20 = y2 - y0, b20 = x0 - x2;

    int px0 = (minx << 4) + 8; // sample at pixel centers
    int py0 = (miny << 4) + 8;

    int w0_row = a12 * (px0 - x1) + b12 * (py0 - y1);
    int w1_row = a20 * (px0 - x2) + b20 * (py0 - y2);
    int w2_row = a01 * (px0 - x0) + b01 * (py0 - y0);

    // Top-left fill rule: exclude pixels exactly on a right or bottom edge.
    int bias0 = (a12 > 0 || (a12 == 0 && b12 > 0)) ? 0 : -1;
    int bias1 = (a20 > 0 || (a20 == 0 && b20 > 0)) ? 0 : -1;
    int bias2 = (a01 > 0 || (a01 == 0 && b01 > 0)) ? 0 : -1;

    int a12_step = a12 << 4, b12_step = b12 << 4;
    int a20_step = a20 << 4, b20_step = b20 << 4;
    int a01_step = a01 << 4, b01_step = b01 << 4;

    // Every interpolated attribute A is affine across the triangle, so instead
    // of three float products per pixel (A0*b0 + A1*b1 + A2*b2) it is stepped by
    // a constant per pixel. A = A2 + (A0-A2)*b0 + (A1-A2)*b1, so dA/dx and dA/dy
    // fall straight out of the barycentric steps. Only the attributes the
    // primitive actually uses are carried.
    //
    // The nest walks column by column, py innermost. VRAM is column-major
    // (canvas[x][y]), so a step in py is the contiguous direction; stepping px
    // in the inner loop would stride a whole column (1 KB) per pixel and miss
    // the cache on nearly every write.
    int interp_rgb = gouraud;
    int interp_uv = ps->textured;

    float b0_dx = a12_step * inv_area, b0_dy = b12_step * inv_area;
    float b1_dx = a20_step * inv_area, b1_dy = b20_step * inv_area;
    float b0_col = w0_row * inv_area;
    float b1_col = w1_row * inv_area;

    float r_dy = 0, g_dy = 0, b_dy = 0;
    float u_dy = 0, v_dy = 0;
    float r_col = 0, g_col = 0, b_col = 0, u_col = 0, v_col = 0;

    if (interp_rgb) {
        r_dy = (v0.r - v2.r) * b0_dy + (v1.r - v2.r) * b1_dy;
        g_dy = (v0.g - v2.g) * b0_dy + (v1.g - v2.g) * b1_dy;
        b_dy = (v0.b - v2.b) * b0_dy + (v1.b - v2.b) * b1_dy;
    }
    if (interp_uv) {
        u_dy = (v0.u - v2.u) * b0_dy + (v1.u - v2.u) * b1_dy;
        v_dy = (v0.v - v2.v) * b0_dy + (v1.v - v2.v) * b1_dy;
    }

    for (int px = minx; px <= maxx; px++) {
        int w0 = w0_row;
        int w1 = w1_row;
        int w2 = w2_row;

        if (interp_rgb) {
            r_col = v2.r + (v0.r - v2.r) * b0_col + (v1.r - v2.r) * b1_col;
            g_col = v2.g + (v0.g - v2.g) * b0_col + (v1.g - v2.g) * b1_col;
            b_col = v2.b + (v0.b - v2.b) * b0_col + (v1.b - v2.b) * b1_col;
        }
        if (interp_uv) {
            u_col = v2.u + (v0.u - v2.u) * b0_col + (v1.u - v2.u) * b1_col;
            v_col = v2.v + (v0.v - v2.v) * b0_col + (v1.v - v2.v) * b1_col;
        }

        float r = r_col, g = g_col, b = b_col, u = u_col, vv = v_col;

        for (int py = miny; py <= maxy; py++) {
            if ((w0 + bias0) >= 0 && (w1 + bias1) >= 0 && (w2 + bias2) >= 0) {
                int cr, cg, cb;

                if (gouraud) {
                    cr = (int)(r + 0.5f);
                    cg = (int)(g + 0.5f);
                    cb = (int)(b + 0.5f);
                } else {
                    cr = v0.r;
                    cg = v0.g;
                    cb = v0.b;
                }

                if (ps->textured) {
                    int texel = sample_texel(&ps->tex, (int)(u + 0.5f),
                                             (int)(vv + 0.5f));

                    if (texel >= 0) {
                        int tr, tg, tb;
                        unpack_555((uint16_t)texel, &tr, &tg, &tb);

                        if (!ps->raw) {
                            tr = modulate(tr, cr);
                            tg = modulate(tg, cg);
                            tb = modulate(tb, cb);
                        }

                        // Blend only texels whose STP bit is set.
                        int blend = ps->semi && (texel & 0x8000);
                        put_pixel(px, py, rgb_to_555(tr, tg, tb), blend,
                                  ps->semi_mode);
                    }
                } else {
                    put_pixel(px, py, rgb_to_555(cr, cg, cb), ps->semi,
                              ps->semi_mode);
                }
            }

            w0 += b12_step;
            w1 += b20_step;
            w2 += b01_step;
            if (interp_rgb) {
                r += r_dy;
                g += g_dy;
                b += b_dy;
            }
            if (interp_uv) {
                u += u_dy;
                vv += v_dy;
            }
        }

        w0_row += a12_step;
        w1_row += a20_step;
        w2_row += a01_step;
        b0_col += b0_dx;
        b1_col += b1_dx;
    }
}

// Fill an axis-aligned rectangle (sprite) with a solid color or a texture,
// clipped to the drawing area. Textured sprites map texels affinely from the
// (u0,v0) origin, one texel per pixel.
static void
draw_rectangle(int x, int y, int w, int h, int r, int g, int b,
               const PrimState *ps, int u0, int v0)
{
    int ox = x, oy = y;
    int x1 = x + w - 1;
    int y1 = y + h - 1;

    if (x < gpu.draw_x0) x = gpu.draw_x0;
    if (y < gpu.draw_y0) y = gpu.draw_y0;
    if (x1 > gpu.draw_x1) x1 = gpu.draw_x1;
    if (y1 > gpu.draw_y1) y1 = gpu.draw_y1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > VRAM_WIDTH - 1) x1 = VRAM_WIDTH - 1;
    if (y1 > VRAM_HEIGHT - 1) y1 = VRAM_HEIGHT - 1;

    uint16_t solid = rgb_to_555(r, g, b);

    // Iterate py innermost: VRAM is column-major (canvas[x][y]), so this walks
    // contiguous memory instead of striding a full column per pixel.
    for (int px = x; px <= x1; px++) {
        for (int py = y; py <= y1; py++) {
            if (ps->textured) {
                int texel = sample_texel(&ps->tex, u0 + (px - ox),
                                         v0 + (py - oy));

                if (texel >= 0) {
                    int tr, tg, tb;
                    unpack_555((uint16_t)texel, &tr, &tg, &tb);

                    if (!ps->raw) {
                        tr = modulate(tr, r);
                        tg = modulate(tg, g);
                        tb = modulate(tb, b);
                    }

                    int blend = ps->semi && (texel & 0x8000);
                    put_pixel(px, py, rgb_to_555(tr, tg, tb), blend,
                              ps->semi_mode);
                }
            } else {
                put_pixel(px, py, solid, ps->semi, ps->semi_mode);
            }
        }
    }
}

static int
iabs(int v)
{
    return v < 0 ? -v : v;
}

// Draw a line between two vertices with Bresenham's algorithm, clipped to the
// drawing area. Color is flat (v0) or interpolated along the span; both flat
// and shaded lines honor semi-transparency.
static void
draw_line(RVertex a, RVertex b, int shaded, int semi, int mode)
{
    int x0 = a.x, y0 = a.y, x1 = b.x, y1 = b.y;

    // The GPU rejects lines whose span exceeds the coordinate range.
    if (iabs(x1 - x0) > 1023 || iabs(y1 - y0) > 511) {
        return;
    }

    int dx = iabs(x1 - x0);
    int dy = iabs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    int steps = dx > dy ? dx : dy;
    int i = 0;

    for (;;) {
        int r = a.r, g = a.g, bl = a.b;

        if (shaded && steps > 0) {
            float t = (float)i / (float)steps;
            r = (int)(a.r + (b.r - a.r) * t + 0.5f);
            g = (int)(a.g + (b.g - a.g) * t + 0.5f);
            bl = (int)(a.b + (b.b - a.b) * t + 0.5f);
        }

        if (x0 >= gpu.draw_x0 && x0 <= gpu.draw_x1 &&
            y0 >= gpu.draw_y0 && y0 <= gpu.draw_y1 &&
            x0 >= 0 && x0 < VRAM_WIDTH && y0 >= 0 && y0 < VRAM_HEIGHT) {
            put_pixel(x0, y0, rgb_to_555(r, g, bl), semi, mode);
        }

        if (x0 == x1 && y0 == y1) {
            break;
        }

        int e2 = 2 * err;

        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }

        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }

        i++;
    }
}

// ---------------------------------------------------------------------------
// GP0 command handling
// ---------------------------------------------------------------------------

uint32_t
calculate_gp0_packet_length(uint32_t primary_word)
{
    uint32_t opcode = primary_word >> 24;

    // Polygons (0x20-0x3F): bit 3 selects quad, bit 4 Gouraud, bit 2 textured.
    if (opcode >= 0x20 && opcode <= 0x3F) {
        int verts = (opcode & 0x08) ? 4 : 3;
        int gouraud = opcode & 0x10;
        int textured = opcode & 0x04;
        uint32_t words = 1; // command word carries the first color

        for (int i = 0; i < verts; i++) {
            if (gouraud && i > 0) {
                words++; // per-vertex color
            }

            words++; // vertex position

            if (textured) {
                words++; // texture coord / palette / page
            }
        }

        return words;
    }

    // Lines (0x40-0x5F): only the two-point forms are sized precisely here.
    if (opcode >= 0x40 && opcode <= 0x5F) {
        return (opcode & 0x10) ? 4 : 3;
    }

    // Rectangles / sprites (0x60-0x7F).
    if (opcode >= 0x60 && opcode <= 0x7F) {
        uint32_t words = 2; // command+color, then position
        int textured = opcode & 0x04;
        int size = (opcode >> 3) & 0x03;

        if (textured) {
            words++; // texture coord / palette
        }

        if (size == 0) {
            words++; // width/height word for the variable-size form
        }

        return words;
    }

    switch (opcode) {
    case 0x00:
        return 1; // NOP

    case 0x01:
        return 1; // Clear Cache

    case 0x02:
        return 3; // Fill VRAM Rectangle

    case 0x80:
        return 4; // VRAM-to-VRAM copy

    case 0xA0:
        return 3; // CPU-to-VRAM copy (then streams pixel data)

    case 0xC0:
        return 3; // VRAM-to-CPU copy

    case 0xE1:
    case 0xE2:
    case 0xE3:
    case 0xE4:
    case 0xE5:
    case 0xE6:
        return 1; // Rendering environment setup

    default:
        return 1;
    }
}

// Parse and draw a polygon primitive (0x20-0x3F).
static void
draw_polygon(uint32_t opcode, uint32_t *buffer)
{
    int quad = opcode & 0x08;
    int gouraud = opcode & 0x10;
    int nverts = quad ? 4 : 3;

    PrimState ps = {0};
    ps.textured = opcode & 0x04;
    ps.raw = opcode & 0x01;
    ps.semi = opcode & 0x02;

    RVertex v[4];
    uint32_t color0 = buffer[0] & 0xFFFFFF;
    int idx = 1;

    for (int i = 0; i < nverts; i++) {
        uint32_t color = color0;

        if (gouraud && i > 0) {
            color = buffer[idx++] & 0xFFFFFF;
        }

        uint32_t pos = buffer[idx++];
        uint32_t tc = 0;

        if (ps.textured) {
            tc = buffer[idx++];

            // Vertex 0 carries the CLUT, vertex 1 the texture page. Both are
            // in the upper half of the texcoord word.
            if (i == 0) {
                uint32_t clut = (tc >> 16) & 0xFFFF;
                ps.tex.clut_x = (clut & 0x3F) * 16;
                ps.tex.clut_y = (clut >> 6) & 0x1FF;
            } else if (i == 1) {
                set_texpage_bits((tc >> 16) & 0xFFFF);
            }
        }

        v[i].x = vertex_x(pos);
        v[i].y = vertex_y(pos);
        v[i].r = color & 0xFF;
        v[i].g = (color >> 8) & 0xFF;
        v[i].b = (color >> 16) & 0xFF;
        v[i].u = tc & 0xFF;
        v[i].v = (tc >> 8) & 0xFF;
    }

    if (ps.textured) {
        ps.tex.page_x = gpu.tex_page_x;
        ps.tex.page_y = gpu.tex_page_y;
        ps.tex.depth = gpu.tex_depth;
    }

    // Semi-transparency mode is taken from the current draw mode, updated by
    // this primitive's own texpage word above when it is textured.
    ps.semi_mode = gpu.tex_semi_mode;

    draw_triangle(v[0], v[1], v[2], gouraud, &ps);

    if (quad) {
        draw_triangle(v[1], v[2], v[3], gouraud, &ps);
    }
}

// Parse and draw a rectangle / sprite primitive (0x60-0x7F).
static void
draw_rect_command(uint32_t opcode, uint32_t *buffer)
{
    int size = (opcode >> 3) & 0x03;

    PrimState ps = {0};
    ps.textured = opcode & 0x04;
    ps.raw = opcode & 0x01;
    ps.semi = opcode & 0x02;
    ps.semi_mode = gpu.tex_semi_mode;

    uint32_t color = buffer[0] & 0xFFFFFF;
    uint32_t pos = buffer[1];
    int idx = 2;

    int u0 = 0, v0 = 0;

    if (ps.textured) {
        uint32_t tc = buffer[idx++];
        u0 = tc & 0xFF;
        v0 = (tc >> 8) & 0xFF;

        uint32_t clut = (tc >> 16) & 0xFFFF;
        ps.tex.clut_x = (clut & 0x3F) * 16;
        ps.tex.clut_y = (clut >> 6) & 0x1FF;

        // Sprites take the texture page from the current draw mode (GP0 0xE1).
        ps.tex.page_x = gpu.tex_page_x;
        ps.tex.page_y = gpu.tex_page_y;
        ps.tex.depth = gpu.tex_depth;
    }

    int w, h;

    if (size == 0) {
        uint32_t dim = buffer[idx++];
        w = dim & 0xFFFF;
        h = (dim >> 16) & 0xFFFF;
    } else if (size == 1) {
        w = h = 1;
    } else if (size == 2) {
        w = h = 8;
    } else {
        w = h = 16;
    }

    draw_rectangle(vertex_x(pos), vertex_y(pos), w, h,
                   color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF,
                   &ps, u0, v0);
}

// Draw a single line primitive (0x40-0x47, 0x50-0x57).
static void
draw_line_command(uint32_t opcode, uint32_t *buffer)
{
    int shaded = opcode & 0x10;
    int semi = opcode & 0x02;

    RVertex a, b;
    uint32_t c0 = buffer[0] & 0xFFFFFF;
    int idx = 1;

    uint32_t p1 = buffer[idx++];
    a.x = vertex_x(p1);
    a.y = vertex_y(p1);
    a.r = c0 & 0xFF;
    a.g = (c0 >> 8) & 0xFF;
    a.b = (c0 >> 16) & 0xFF;

    uint32_t c1 = shaded ? (buffer[idx++] & 0xFFFFFF) : c0;
    uint32_t p2 = buffer[idx++];
    b.x = vertex_x(p2);
    b.y = vertex_y(p2);
    b.r = c1 & 0xFF;
    b.g = (c1 >> 8) & 0xFF;
    b.b = (c1 >> 16) & 0xFF;

    draw_line(a, b, shaded, semi, gpu.tex_semi_mode);
}

// Begin a poly-line (0x48-0x4F, 0x58-0x5F). The command word carries the color
// of the first point; vertices then stream in through poly_line_feed.
static void
poly_line_start(uint32_t cmd)
{
    uint32_t op = cmd >> 24;

    gpu.pl_active = 1;
    gpu.pl_shaded = op & 0x10;
    gpu.pl_semi = op & 0x02;
    gpu.pl_semi_mode = gpu.tex_semi_mode;
    gpu.pl_count = 0;
    gpu.pl_expect_color = 0; // the first point's color came with the command
    gpu.pl_cur_r = cmd & 0xFF;
    gpu.pl_cur_g = (cmd >> 8) & 0xFF;
    gpu.pl_cur_b = (cmd >> 16) & 0xFF;
}

// Consume one poly-line word: a color, a vertex, or the termination code. Each
// vertex after the first completes a segment, which is drawn immediately.
static void
poly_line_feed(uint32_t word)
{
    // Once two points exist, a word of the form 0x5xxx5xxx ends the poly-line.
    if (gpu.pl_count >= 2 && (word & 0xF000F000) == 0x50005000) {
        gpu.pl_active = 0;
        return;
    }

    if (gpu.pl_shaded && gpu.pl_expect_color) {
        gpu.pl_cur_r = word & 0xFF;
        gpu.pl_cur_g = (word >> 8) & 0xFF;
        gpu.pl_cur_b = (word >> 16) & 0xFF;
        gpu.pl_expect_color = 0;
        return;
    }

    int x = vertex_x(word);
    int y = vertex_y(word);

    if (gpu.pl_count > 0) {
        RVertex a, b;
        a.x = gpu.pl_prev_x;
        a.y = gpu.pl_prev_y;
        a.r = gpu.pl_prev_r;
        a.g = gpu.pl_prev_g;
        a.b = gpu.pl_prev_b;
        b.x = x;
        b.y = y;
        b.r = gpu.pl_cur_r;
        b.g = gpu.pl_cur_g;
        b.b = gpu.pl_cur_b;
        draw_line(a, b, gpu.pl_shaded, gpu.pl_semi, gpu.pl_semi_mode);
    }

    gpu.pl_prev_x = x;
    gpu.pl_prev_y = y;
    gpu.pl_prev_r = gpu.pl_cur_r;
    gpu.pl_prev_g = gpu.pl_cur_g;
    gpu.pl_prev_b = gpu.pl_cur_b;
    gpu.pl_count++;

    if (gpu.pl_shaded) {
        gpu.pl_expect_color = 1; // the next point needs its own color first
    }
}

void
execute_completed_gp0_command(uint32_t opcode, uint32_t* buffer)
{
    if (opcode >= 0x20 && opcode <= 0x3F) {
        draw_polygon(opcode, buffer);
    } else if (opcode >= 0x40 && opcode <= 0x5F) {
        draw_line_command(opcode, buffer);
    } else if (opcode >= 0x60 && opcode <= 0x7F) {
        draw_rect_command(opcode, buffer);
    } else {
        switch (opcode) {
        case 0x02: { // Fill Rectangle in VRAM
            uint32_t color24 = buffer[0] & 0xFFFFFF;
            uint32_t r = (color24 >> 3) & 0x1F;
            uint32_t g = (color24 >> 11) & 0x1F;
            uint32_t b = (color24 >> 19) & 0x1F;
            uint16_t px = (uint16_t)((b << 10) | (g << 5) | r);

            uint32_t x = buffer[1] & 0xFFFF;
            uint32_t y = (buffer[1] >> 16) & 0xFFFF;
            uint32_t w = buffer[2] & 0xFFFF;
            uint32_t h = (buffer[2] >> 16) & 0xFFFF;

            for (uint32_t dy = 0; dy < h; dy++) {
                for (uint32_t dx = 0; dx < w; dx++) {
                    vram.canvas[(x + dx) & (VRAM_WIDTH - 1)][(y + dy) & (VRAM_HEIGHT - 1)] = px;
                }
            }
        }
        break;

        case 0x80: { // Copy Rectangle (VRAM to VRAM)
            int sx = buffer[1] & 0x3FF;
            int sy = (buffer[1] >> 16) & 0x1FF;
            int dx = buffer[2] & 0x3FF;
            int dy = (buffer[2] >> 16) & 0x1FF;
            int w = buffer[3] & 0xFFFF;
            int h = (buffer[3] >> 16) & 0xFFFF;

            // A size field of zero means the maximum extent.
            w = ((w - 1) & 0x3FF) + 1;
            h = ((h - 1) & 0x1FF) + 1;

            // Copy a row at a time through a temporary buffer so overlapping
            // source and destination do not corrupt the move: the row buffer
            // covers horizontal overlap, and iterating away from the
            // destination covers vertical overlap.
            uint16_t row[VRAM_WIDTH];
            int start = 0, end = h, step = 1;

            if (dy > sy) {
                start = h - 1;
                end = -1;
                step = -1;
            }

            for (int r = start; r != end; r += step) {
                int syy = (sy + r) & (VRAM_HEIGHT - 1);
                int dyy = (dy + r) & (VRAM_HEIGHT - 1);

                for (int c = 0; c < w; c++) {
                    row[c] = vram.canvas[(sx + c) & (VRAM_WIDTH - 1)][syy];
                }

                for (int c = 0; c < w; c++) {
                    vram.canvas[(dx + c) & (VRAM_WIDTH - 1)][dyy] = row[c];
                }
            }
        }
        break;

        case 0xA0: // Copy Rectangle (CPU to VRAM): set up the streaming transfer
            vram.transfer_start_x = buffer[1] & 0xFFFF;
            vram.transfer_start_y = (buffer[1] >> 16) & 0xFFFF;
            vram.transfer_width  = buffer[2] & 0xFFFF;
            vram.transfer_height = (buffer[2] >> 16) & 0xFFFF;
            vram.transfer_current_x = vram.transfer_start_x;
            vram.transfer_current_y = vram.transfer_start_y;
            vram.is_transfer_active = 1;
            gpu.fifo_words_count = 0;
            gpu.current_packet_length = 0;
            return; // stay open to consume the pixel stream

        case 0xC0: // Copy Rectangle (VRAM to CPU): set up the read stream
            vram.rd_start_x = buffer[1] & 0xFFFF;
            vram.rd_start_y = (buffer[1] >> 16) & 0xFFFF;
            vram.rd_width  = buffer[2] & 0xFFFF;
            vram.rd_height = (buffer[2] >> 16) & 0xFFFF;

            if (vram.rd_width == 0) vram.rd_width = VRAM_WIDTH;
            if (vram.rd_height == 0) vram.rd_height = VRAM_HEIGHT;

            vram.rd_current_x = vram.rd_start_x;
            vram.rd_current_y = vram.rd_start_y;
            vram.is_read_active = 1;
            break;

        case 0xE1: // Set draw mode (texture page, color depth, etc.)
            set_texpage_bits(buffer[0] & 0x1FF);
            break;

        case 0xE2: // Set texture window (mask and offset, in 8-texel units)
            gpu.tw_mask_x = buffer[0] & 0x1F;
            gpu.tw_mask_y = (buffer[0] >> 5) & 0x1F;
            gpu.tw_off_x = (buffer[0] >> 10) & 0x1F;
            gpu.tw_off_y = (buffer[0] >> 15) & 0x1F;
            break;

        case 0xE3: // Set drawing area top-left
            gpu.draw_x0 = buffer[0] & 0x3FF;
            gpu.draw_y0 = (buffer[0] >> 10) & 0x3FF;
            break;

        case 0xE4: // Set drawing area bottom-right
            gpu.draw_x1 = buffer[0] & 0x3FF;
            gpu.draw_y1 = (buffer[0] >> 10) & 0x3FF;
            break;

        case 0xE5: { // Set drawing offset (11-bit signed X and Y)
            int ox = buffer[0] & 0x7FF;
            int oy = (buffer[0] >> 11) & 0x7FF;

            if (ox & 0x400) ox |= ~0x7FF;
            if (oy & 0x400) oy |= ~0x7FF;

            gpu.draw_offset_x = ox;
            gpu.draw_offset_y = oy;
        }
        break;

        default:
            // NOP, cache, other environment and unimplemented commands.
            break;
        }
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

    // Poly-lines stream vertices with no fixed length; parse them separately.
    if (gpu.pl_active) {
        poly_line_feed(value);
        return;
    }

    if (gpu.fifo_words_count == 0) {
        uint32_t op = value >> 24;

        if (op >= 0x40 && op <= 0x5F && (op & 0x08)) {
            poly_line_start(value);
            return;
        }
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
        psx_gpu_reset();
        break;

    case 0x01: // Clear Command FIFO Buffer
        gpu.fifo_words_count = 0;
        gpu.current_packet_length = 0;
        gpu.pl_active = 0;
        break;

    case 0x03: // Display Enable (bit 0: 1 = disabled/blank)
        gpu.disp_blank = value & 1;
        break;

    case 0x05: // Start of the display area in VRAM
        gpu.disp_x = value & 0x3FF;
        gpu.disp_y = (value >> 10) & 0x1FF;
        break;

    case 0x06: // Horizontal display range (screen timing, not output size)
        break;

    case 0x07: // Vertical display range: the scanlines the picture occupies.
        gpu.disp_range_y1 = value & 0x3FF;
        gpu.disp_range_y2 = (value >> 10) & 0x3FF;
        break;

    case 0x08: // Display mode (resolution and color depth)
        set_display_mode(value);
        break;
    }
}

// Returns data back to the CPU. During a VRAM-to-CPU transfer this drains the
// read stream; otherwise there is no GPUREAD result to report.
uint32_t
psx_gpu_read_vram_or_gp0()
{
    if (vram.is_read_active) {
        return psx_gpu_read_pixel_payload();
    }

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
