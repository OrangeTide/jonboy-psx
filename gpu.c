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

    for (int py = miny; py <= maxy; py++) {
        int w0 = w0_row;
        int w1 = w1_row;
        int w2 = w2_row;

        for (int px = minx; px <= maxx; px++) {
            if ((w0 + bias0) >= 0 && (w1 + bias1) >= 0 && (w2 + bias2) >= 0) {
                float bary0 = w0 * inv_area;
                float bary1 = w1 * inv_area;
                float bary2 = 1.0f - bary0 - bary1;

                int r, g, b;

                if (gouraud) {
                    r = (int)(v0.r * bary0 + v1.r * bary1 + v2.r * bary2 + 0.5f);
                    g = (int)(v0.g * bary0 + v1.g * bary1 + v2.g * bary2 + 0.5f);
                    b = (int)(v0.b * bary0 + v1.b * bary1 + v2.b * bary2 + 0.5f);
                } else {
                    r = v0.r;
                    g = v0.g;
                    b = v0.b;
                }

                if (ps->textured) {
                    int u = (int)(v0.u * bary0 + v1.u * bary1 + v2.u * bary2 + 0.5f);
                    int vv = (int)(v0.v * bary0 + v1.v * bary1 + v2.v * bary2 + 0.5f);
                    int texel = sample_texel(&ps->tex, u, vv);

                    if (texel >= 0) {
                        int tr, tg, tb;
                        unpack_555((uint16_t)texel, &tr, &tg, &tb);

                        if (!ps->raw) {
                            tr = modulate(tr, r);
                            tg = modulate(tg, g);
                            tb = modulate(tb, b);
                        }

                        // Blend only texels whose STP bit is set.
                        int blend = ps->semi && (texel & 0x8000);
                        put_pixel(px, py, rgb_to_555(tr, tg, tb), blend,
                                  ps->semi_mode);
                    }
                } else {
                    put_pixel(px, py, rgb_to_555(r, g, b), ps->semi,
                              ps->semi_mode);
                }
            }

            w0 += a12_step;
            w1 += a20_step;
            w2 += a01_step;
        }

        w0_row += b12_step;
        w1_row += b20_step;
        w2_row += b01_step;
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

    for (int py = y; py <= y1; py++) {
        for (int px = x; px <= x1; px++) {
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

void
execute_completed_gp0_command(uint32_t opcode, uint32_t* buffer)
{
    if (opcode >= 0x20 && opcode <= 0x3F) {
        draw_polygon(opcode, buffer);
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
        break;

    case 0x08: // Display Mode Setup
        // Handle PAL vs NTSC refresh rate selection flags
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
