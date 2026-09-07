// Wrap a flat binary (linked at 0x80010000) into a PS-EXE the emulator can load.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define LOAD_ADDR 0x80010000u
#define SP_ADDR   0x801FFF00u

static void put32(uint8_t *p, uint32_t v)
{
    p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s in.bin out.exe\n", argv[0]); return 2; }

    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror(argv[1]); return 1; }
    fseek(in, 0, SEEK_END);
    long sz = ftell(in);
    fseek(in, 0, SEEK_SET);

    long tsize = (sz + 0x7FF) & ~0x7FFL; // round text size up to 2 KB
    uint8_t *buf = calloc(1, 0x800 + tsize);
    if (!buf) { fprintf(stderr, "oom\n"); return 1; }

    if (fread(buf + 0x800, 1, sz, in) != (size_t)sz) { perror("read"); return 1; }
    fclose(in);

    memcpy(buf, "PS-X EXE", 8);
    put32(buf + 0x10, LOAD_ADDR);   // entry PC
    put32(buf + 0x14, 0);           // gp
    put32(buf + 0x18, LOAD_ADDR);   // load address
    put32(buf + 0x1C, (uint32_t)tsize); // text size
    put32(buf + 0x30, SP_ADDR);     // stack base
    put32(buf + 0x34, 0);           // stack size

    FILE *out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }
    fwrite(buf, 1, 0x800 + tsize, out);
    fclose(out);
    free(buf);
    return 0;
}
