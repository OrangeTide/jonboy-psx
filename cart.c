#include "cart.h"
#include <stdio.h>

unsigned char cart_data[CART_MAXSIZE];

int cart_load(const char *filename)
{
    int result = 0;
    FILE *f = fopen(filename, "rb");
    if (f) {
        int ofs = fseek(f, 0, SEEK_END);
        if (ofs == -1) {
            perror(filename);
            // result = 0;
        } else {
            int fsize = ftell(f);
            fseek(f, 0, SEEK_SET);

            if (fsize < CART_MAXSIZE) {
                int readlen = fread(cart_data, 1, fsize, f);
                if (readlen == -1) {
                    perror("filename");
                } else if (readlen < fsize) {
                    fprintf(stderr, "%s:short read\n", filename);
                } else {
                    result = 1; // success!
                }
            } else {
                fprintf(stderr, "%s:file too large.\n", filename);
            }
        }
    }
    fclose(f);
    return result;
}

