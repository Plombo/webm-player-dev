#ifndef YUV_H
#define YUV_H

void yuv_init(int bits);

void yuv_to_rgb(unsigned char *lum, unsigned char *cr,
			        unsigned char *cb, unsigned char *out,
			        int rows, int cols, int mod);

#endif
