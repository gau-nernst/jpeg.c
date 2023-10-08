#include "jpeg.h"
#include <stdint.h>
#include <stdio.h>

int to_ppm(const char *, uint8_t *, unsigned int, unsigned int);

int main(int argc, char *argv[]) {
  if (argc == 1) {
    fprintf(stderr, "No input\n");
    return 1;
  }

  FILE *f = fopen(argv[1], "rb");
  if (f == NULL) {
    fprintf(stderr, "Failed to open %s\n", argv[1]);
    return 1;
  }

  struct JPEGState jpeg_state;
  init_dct_matrix();
  decode_jpeg(f, &jpeg_state);

  to_ppm("output.ppm", jpeg_state.image_buffer, jpeg_state.width, jpeg_state.height);
}

int to_ppm(const char *filename, uint8_t *image_buffer, unsigned int width, unsigned int height) {
  FILE *f = fopen(filename, "w");
  if (f == NULL) {
    fprintf(stderr, "Failed to open %s to write", filename);
    return 1;
  }

  fprintf(f, "P3\n%d %d\n255\n", width, height);
  for (int j = 0; j < height; j++)
    for (int i = 0; i < width; i++) {
      for (int c = 0; c < 3; c++)
        fprintf(f, "%d ", image_buffer[(j * width + i) * 3 + c]);
      fprintf(f, "\n");
    }

  return 0;
}
