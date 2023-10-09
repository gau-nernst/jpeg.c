#include <stdint.h>
#include <stdio.h>

typedef struct HuffmanTable {
  uint8_t *huffsize;
  uint16_t *huffcode;
  uint8_t *huffval;
  uint16_t mincode[16];
  int32_t maxcode[16];
  uint8_t valptr[16];
} HuffmanTable;

typedef struct Component {
  uint8_t x_sampling_factor;
  uint8_t y_sampling_factor;
  uint8_t q_table_id;
} Component;

typedef struct JPEGState {
  uint8_t encoding;
  uint16_t width;
  uint16_t height;
  uint8_t n_components;
  uint16_t q_tables[4][8 * 8];
  struct HuffmanTable h_tables[2][4];
  struct Component *components;
  uint8_t *image_buffer;
} JPEGState;

void init_dct_matrix();
int decode_jpeg(FILE *, JPEGState *);
