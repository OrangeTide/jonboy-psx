#include "rom.h"
#include <stdio.h>

unsigned char rom_data[ROM_MAXSIZE]; // TODO: remove this and copy into system_ram[] instead

// Returns 0 on success, negative on any failure.
int
rom_load(const char *filename)
{
    int result = -1;
    FILE *f = fopen(filename, "rb");

    if (!f) {
        perror(filename);
        return -1;
    }

    if (fseek(f, 0, SEEK_END) == -1) {
        perror(filename);
    } else {
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (fsize < 0) {
            perror(filename);
        } else if (fsize >= ROM_MAXSIZE) {
            fprintf(stderr, "%s: file too large.\n", filename);
        } else {
            size_t readlen = fread(rom_data, 1, (size_t)fsize, f);

            if (readlen != (size_t)fsize) {
                fprintf(stderr, "%s: short read\n", filename);
            } else {
                result = 0; // success!
            }
        }
    }

    fclose(f);

    return result;
}
