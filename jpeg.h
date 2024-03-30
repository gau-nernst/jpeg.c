#include <stdint.h>
#include <stdio.h>

void jpeg_enable_debug_print();
void jpeg_disable_debug_print();
uint8_t *decode_jpeg(FILE *, int *width, int *height, int *n_channels);
