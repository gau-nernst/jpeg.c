#include <stdint.h>
#include <stdio.h>

typedef struct {
  int width;
  int height;
  int n_channels;
  uint8_t *data;
} Image8;

void init_dct_matrix();
int decode_jpeg(FILE *, Image8 *);
