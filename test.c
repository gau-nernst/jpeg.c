#include "jpeg.h"
#include <stdint.h>
#include <stdio.h>

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

  Image8 image = {0};
  init_dct_matrix();
  int decode_result = decode_jpeg(f, &image);
  fclose(f);

  if (image.data == 0) {
    fprintf(stderr, "No image\n");
    return 1;
  }

  char *filename = "output.ppm";
  f = fopen(filename, "w");
  if (f == NULL) {
    fprintf(stderr, "Failed to open %s to write", filename);
    return 1;
  }

  fprintf(f, "P%d\n%d %d\n255\n", image.n_channels == 1 ? 2 : 3, image.width, image.height);
  for (int j = 0; j < image.height; j++)
    for (int i = 0; i < image.width; i++) {
      for (int c = 0; c < image.n_channels; c++)
        fprintf(f, "%d ", image.data[(j * image.width + i) * image.n_channels + c]);
      fprintf(f, "\n");
    }

  return decode_result;
}
