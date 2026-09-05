#ifndef ROM_H_
#define ROM_H_
#define ROM_MAXSIZE (1024 * 1024)

extern unsigned char rom_data[];
int rom_load(const char *filename);
#endif
