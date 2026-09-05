#ifndef CART_H_
#define CART_H_
#define CART_MAXSIZE (1024 * 1024)

extern unsigned char cart_data[];
int cart_load(const char *filename);
#endif
