#include "jpeg.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint16_t tag;
  uint16_t dtype;
  uint32_t count;
  uint32_t value;
} TiffField;

int write_tiff_field(FILE *f, uint16_t tag, uint16_t dtype, uint32_t count, uint32_t value) {
  TiffField field = {tag, dtype, count, value};
  fwrite(&field, sizeof(TiffField), 1, f); // assume system is little-endian
  return 0;
}

int write_tiff(FILE *f, Image8 image) {
  // header. little-endian
  fwrite("II\x2A\0\x8\0\0\0", 1, 8, f);

  uint16_t n_fields = 11;
  int offset = 8 + 2 + n_fields * sizeof(TiffField) + 4;
  fwrite(&n_fields, 2, 1, f);

  write_tiff_field(f, 0x100, 3, 1, image.width);
  write_tiff_field(f, 0x101, 3, 1, image.height);
  write_tiff_field(f, 0x103, 3, 1, 1); // compression -> no
  if (image.n_channels == 1) {
    write_tiff_field(f, 0x102, 3, 1, 8); // bits per sample
    write_tiff_field(f, 0x106, 3, 1, 1); // photometric interpretation
  } else if (image.n_channels == 3) {
    write_tiff_field(f, 0x102, 3, 3, offset); // bits per sample
    write_tiff_field(f, 0x106, 3, 1, 2);      // photometric interpretation
  } else
    return 1;
  write_tiff_field(f, 0x111, 3, 1, offset + 2 * image.n_channels + 8 + 8);         // strip offset
  write_tiff_field(f, 0x115, 3, 1, image.n_channels);                              // samples per pixel
  write_tiff_field(f, 0x116, 3, 1, image.height);                                  // rows per strip (only 1 strip)
  write_tiff_field(f, 0x117, 4, 1, image.width * image.height * image.n_channels); // strip byte counts
  write_tiff_field(f, 0x11A, 5, 1, offset + 2 * image.n_channels);                 // x resolution
  write_tiff_field(f, 0x11B, 5, 1, offset + 2 * image.n_channels + 8);             // y resolution
  fwrite("\0\0\0\0", 1, 4, f);

  for (int i = 0; i < image.n_channels; i++)
    fwrite("\x8\0", 2, 1, f);             // bits per sample = 8. redundant when n_channels == 1
  fwrite("\x48\0\0\0\x1\0\0\0", 4, 2, f); // x resolution
  fwrite("\x48\0\0\0\x1\0\0\0", 4, 2, f); // y resolution

  fwrite(image.data, 1, image.width * image.height * image.n_channels, f);
  return 0;
}

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
  int decode_result = decode_jpeg(f, &image);
  fclose(f);

  if (image.data == 0) {
    fprintf(stderr, "No image\n");
    return 1;
  }

  size_t input_length = strlen(argv[1]);
  char *filename = malloc(input_length + 6);
  strcpy(filename, argv[1]);
  strcat(filename, ".tiff");

  f = fopen(filename, "w");
  if (f == NULL) {
    fprintf(stderr, "Failed to open %s to write", filename);
    return 1;
  }

  write_tiff(f, image);
  return decode_result;
}
