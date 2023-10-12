#include "jpeg.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define _USE_MATH_DEFINES // math constants for MSVC
#include <math.h>

#define BLOCK_SIZE 8
#define MAX_HUFFMAN_CODE_LENGTH 16

#define assert(condition, msg)                                                                                         \
  if (!(condition)) {                                                                                                  \
    fprintf(stderr, msg);                                                                                              \
    fprintf(stderr, "\n");                                                                                             \
    return 1;                                                                                                          \
  }
#define check(condition)                                                                                               \
  if (condition)                                                                                                       \
    return 1;

#define try_malloc(ptr, size) assert((ptr = malloc(size)) != NULL, "Failed to allocate memory")
#define try_fread(ptr, size, n_items, stream)                                                                          \
  assert(fread(ptr, size, n_items, stream) == (size * n_items), "Failed to read data. Perhaps EOF?")

#define try_free(ptr)                                                                                                  \
  if (ptr != NULL) {                                                                                                   \
    free(ptr);                                                                                                         \
    ptr = NULL;                                                                                                        \
  }

#define print_list(prefix, ptr, length, fmt)                                                                           \
  {                                                                                                                    \
    fprintf(stderr, prefix);                                                                                           \
    for (int _i = 0; _i < (length); _i++)                                                                              \
      fprintf(stderr, fmt, (ptr)[_i]);                                                                                 \
    fprintf(stderr, "\n");                                                                                             \
  }

// https://stackoverflow.com/a/2745086
#define ceil_div(x, y) (((x)-1) / (y) + 1)
#define max(x, y) (((x) > (y)) ? (x) : (y))
#define min(x, y) (((x) < (y)) ? (x) : (y))
#define clip(x, lower, higher) min(max(x, lower), higher)

enum MARKER {
  // ITU-T.81 F.1.2.2.3
  EOB = 0x00,
  ZRL = 0xF0,

  // ITU-T.81 Table B.1
  TEM = 0x01,

  SOF0 = 0xC0,
  SOF1 = 0xC1,
  SOF2 = 0xC2,
  SOF3 = 0xC3,

  DHT = 0xC4,

  SOF5 = 0xC5,
  SOF6 = 0xC6,
  SOF7 = 0xC7,

  JPG = 0xC8,
  SOF9 = 0xC9,
  SOF10 = 0xCA,
  SOF11 = 0xCB,

  DAC = 0xCC,

  SOF13 = 0xC,
  SOF14 = 0xC,
  SOF15 = 0xC,

  RST0 = 0xD0,

  SOI = 0xD8,
  EOI = 0xD9,
  SOS = 0xDA,
  DQT = 0xDB,
  DNL = 0xDC,
  DRI = 0xDD,
  DHP = 0xDE,
  EXP = 0xDF,

  APP0 = 0xE0,
  APP1 = 0xE1,

  COM = 0xFE,
};

typedef struct {
  uint8_t *huffsize;
  uint16_t *huffcode;
  uint8_t *huffval;
  uint16_t mincode[16];
  int32_t maxcode[16];
  uint8_t valptr[16];
} HuffmanTable;

typedef struct {
  int x_sampling_factor;
  int y_sampling_factor;
  int q_table_id;
} Component;

typedef struct {
  uint8_t encoding;
  uint16_t width;
  uint16_t height;
  uint8_t n_components;
  uint16_t restart_interval;
  uint16_t q_tables[4][8 * 8];
  HuffmanTable h_tables[2][4];
  Component *components;
  uint8_t *image_buffer;
} DecoderState;

static uint16_t read_be_16(const uint8_t *buffer) { return (buffer[0] << 8) | buffer[1]; }
static uint8_t upper_half(uint8_t x) { return x >> 4; }
static uint8_t lower_half(uint8_t x) { return x & 0xF; }

static int handle_app0(const uint8_t *, uint16_t);
static int handle_app1(const uint8_t *, uint16_t);
static int handle_dqt(const uint8_t *, uint16_t, DecoderState *);
static int handle_dht(const uint8_t *, uint16_t, DecoderState *);
static int handle_sof0(const uint8_t *, uint16_t, DecoderState *);
static int handle_sos(const uint8_t *, uint16_t, DecoderState *, FILE *);

int is_rst = 0;

static int sof0_decode_block(uint8_t block_u8[BLOCK_SIZE][BLOCK_SIZE], int *dc_coef, FILE *f, HuffmanTable *dc_h_table,
                             HuffmanTable *ac_h_table, uint16_t *q_table);

static void idct_2d_(double *);
static void ycbcr_to_rgb_(uint8_t *);

// clang-format off
// ITU T.81 Figure A.6
const uint8_t ZIG_ZAG[BLOCK_SIZE][BLOCK_SIZE] = {
  { 0,  1,  5,  6, 14, 15, 27, 28},
  { 2,  4,  7, 13, 16, 26, 29, 42},
  { 3,  8, 12, 17, 25, 30, 41, 43},
  { 9, 11, 18, 24, 31, 40, 44, 53},
  {10, 19, 23, 32, 39, 45, 52, 54},
  {20, 22, 33, 38, 46, 51, 55, 60},
  {21, 34, 37, 47, 50, 56, 59, 61},
  {35, 36, 48, 49, 57, 58, 62, 63},
};
// clang-format on
double DCT_MATRIX[BLOCK_SIZE][BLOCK_SIZE];

int decode_jpeg(FILE *f, Image8 *image) {
  uint8_t marker[2];
  uint16_t length;
  uint8_t *payload = NULL;
  DecoderState decoder_state;

  uint8_t finished = 0;
  while (finished == 0) {
    try_fread(marker, 1, 2, f);
    fprintf(stderr, "%X%X ", marker[0], marker[1]);

    assert(marker[0] == 0xFF, "Not a marker");

    if (marker[1] == TEM | marker[1] == SOI | marker[1] == EOI | (marker[1] >= RST0 & marker[1] < RST0 + 8)) {
      length = 0;
    } else {
      try_fread(&length, 1, 2, f);
      length = read_be_16((const uint8_t *)&length) - 2;
    }
    if (length) {
      // TODO: re-use payload buffer
      // max buffer size?
      try_malloc(payload, length);
      try_fread(payload, 1, length, f);
    }

    switch (marker[1]) {
    case SOI:
      fprintf(stderr, "SOI");
      break;

    case APP0:
      fprintf(stderr, "APP0 (length = %d)\n", length);
      if (handle_app0(payload, length))
        return 1;
      break;

    case APP1:
      fprintf(stderr, "APP1 (length = %d)\n", length);
      if (handle_app1(payload, length))
        return 1;
      break;

    case DQT:
      fprintf(stderr, "DQT (length = %d)\n", length);
      if (handle_dqt(payload, length, &decoder_state))
        return 1;
      break;

    case DHT:
      fprintf(stderr, "DHT (length = %d)\n", length);
      if (handle_dht(payload, length, &decoder_state))
        return 1;
      break;

    case SOF0:
      fprintf(stderr, "SOF0 (length = %d)\n", length);
      if (handle_sof0(payload, length, &decoder_state))
        return 1;
      break;

    case DRI:
      fprintf(stderr, "DRI (length = %d)\n", length);
      assert(length >= 2, "Payload not long enough");
      decoder_state.restart_interval = read_be_16(payload);
      fprintf(stderr, "  restart interval = %d\n", decoder_state.restart_interval);
      break;

    case SOS:
      fprintf(stderr, "SOS\n");
      if (handle_sos(payload, length, &decoder_state, f))
        return 1;
      break;

    case EOI:
      fprintf(stderr, "EOI\n");
      finished = 1;
      break;

    default:
      fprintf(stderr, "Unknown marker (length = %d)\n", length);
      break;
    }

    try_free(payload);
    fprintf(stderr, "\n");
  }

  image->width = decoder_state.width;
  image->height = decoder_state.height;
  image->n_channels = decoder_state.n_components;
  image->data = decoder_state.image_buffer;
  return 0;
}

// JFIF i.e. JPEG Part 5
int handle_app0(const uint8_t *payload, uint16_t length) {
  fprintf(stderr, "  identifier = %.5s\n", payload); // either JFIF or JFXX

  if (strcmp((const char *)payload, "JFIF") == 0) {
    assert(length >= 14, "Payload is too short");
    fprintf(stderr, "  version = %d.%d\n", payload[5], payload[6]);
    fprintf(stderr, "  units = %d\n", payload[7]);
    fprintf(stderr, "  density = (%d, %d)\n", read_be_16(payload + 8), read_be_16(payload + 10));
    fprintf(stderr, "  thumbnail = (%d, %d)\n", payload[12], payload[13]);
  } else if (strcmp((const char *)payload, "JFXX") == 0) {
    fprintf(stderr, "  extension_code = %X\n", payload[5]);
  } else
    fprintf(stderr, "  Invalid identifier\n");

  return 0;
}

int handle_app1(const uint8_t *payload, uint16_t length) {
  fprintf(stderr, "  identifier = %s\n", payload);

  if (strcmp((const char *)payload, "Exif") == 0) {
    fprintf(stderr, "  Exif detected\n");
  } else
    fprintf(stderr, "  Invalid identifier\n");

  return 0;
}

// ITU-T.81 B.2.4.1
// there can be multiple quantization tables within 1 DQT segment
int handle_dqt(const uint8_t *payload, uint16_t length, DecoderState *decoder_state) {
  int offset = 0;
  while (offset < length) {
    uint8_t precision = upper_half(payload[offset]);
    uint8_t identifier = lower_half(payload[offset]);
    fprintf(stderr, "  precision = %d (%d-bit), identifier = %d\n", precision, (precision + 1) * 8, identifier);
    assert(length >= offset + 1 + BLOCK_SIZE * BLOCK_SIZE * (precision + 1), "Payload is too short");

    uint16_t *q_table;
    q_table = decoder_state->q_tables[identifier];
    if (precision) {
      for (int i = 0; i < BLOCK_SIZE * BLOCK_SIZE; i++)
        q_table[i] = read_be_16(payload + offset + 1 + i * 2);
    } else {
      for (int i = 0; i < BLOCK_SIZE * BLOCK_SIZE; i++)
        q_table[i] = payload[offset + 1 + i];
    }

    for (int i = 0; i < BLOCK_SIZE; i++) {
      fprintf(stderr, "  ");
      for (int j = 0; j < BLOCK_SIZE; j++)
        fprintf(stderr, " %3d", q_table[ZIG_ZAG[i][j]]);
      fprintf(stderr, "\n");
    }

    offset += 1 + BLOCK_SIZE * BLOCK_SIZE * (precision + 1);
  }
  return 0;
}

// ITU-T.81 B.2.4.2
// there can be multiple huffman tables within 1 DHT segment
int handle_dht(const uint8_t *payload, uint16_t length, DecoderState *decoder_state) {
  int offset = 0;
  while (offset < length) {
    uint8_t class = upper_half(payload[offset]);
    uint8_t identifier = lower_half(payload[offset]);
    fprintf(stderr, "  class = %d (%s), identifier = %d\n", class, class ? "AC" : "DC", identifier);
    assert(length >= offset + 1 + MAX_HUFFMAN_CODE_LENGTH, "Payload is too short");

    // ITU-T.81 Annex C: create Huffman table
    HuffmanTable *h_table = &decoder_state->h_tables[class][identifier];
    int n_codes = 0;
    for (int i = 0; i < MAX_HUFFMAN_CODE_LENGTH; i++)
      n_codes += payload[offset + 1 + i];
    assert(length >= offset + 1 + MAX_HUFFMAN_CODE_LENGTH + n_codes, "Payload is too short");

    try_malloc(h_table->huffsize, n_codes * sizeof(*h_table->huffsize));
    try_malloc(h_table->huffcode, n_codes * sizeof(*h_table->huffcode));
    try_malloc(h_table->huffval, n_codes * sizeof(*h_table->huffval));

    // Figure C.1 and C.2
    for (int i = 0, k = 0, code = 0; i < MAX_HUFFMAN_CODE_LENGTH; i++) {
      for (int j = 0; j < payload[offset + 1 + i]; j++, k++, code++) {
        h_table->huffsize[k] = i;
        h_table->huffcode[k] = code;
        h_table->huffval[k] = payload[offset + 1 + MAX_HUFFMAN_CODE_LENGTH + k];
      }
      code = code << 1;
    }

    // Figure F.16
    for (int i = 0, j = 0; i < MAX_HUFFMAN_CODE_LENGTH; i++)
      if (payload[offset + 1 + i]) {
        h_table->valptr[i] = j;
        h_table->mincode[i] = h_table->huffcode[j];
        h_table->maxcode[i] = h_table->huffcode[j + payload[offset + 1 + i] - 1];
        j += payload[offset + 1 + i];
      } else
        h_table->maxcode[i] = -1;

    fprintf(stderr, "  n_codes = %d\n", n_codes);
    print_list("  BITS     =", payload + offset + 1, MAX_HUFFMAN_CODE_LENGTH, " %3d");
    print_list("  HUFFSIZE =", h_table->huffsize, n_codes, " %3d");
    print_list("  HUFFCODE =", h_table->huffcode, n_codes, " %3d");
    print_list("  HUFFVAL  =", h_table->huffval, n_codes, " %3d");
    fprintf(stderr, "\n");
    print_list("  MINCODE  =", h_table->mincode, MAX_HUFFMAN_CODE_LENGTH, " %3d");
    print_list("  MAXCODE  =", h_table->maxcode, MAX_HUFFMAN_CODE_LENGTH, " %3d");
    print_list("  VALPTR   =", h_table->valptr, MAX_HUFFMAN_CODE_LENGTH, " %3d");
    fprintf(stderr, "\n");

    offset += 1 + MAX_HUFFMAN_CODE_LENGTH + n_codes;
  }
  return 0;
}

int handle_sof0(const uint8_t *payload, uint16_t length, DecoderState *decoder_state) {
  decoder_state->encoding = SOF0;

  // Table B.2
  assert(length >= 6, "Payload is too short");
  uint8_t precision = payload[0];
  decoder_state->height = read_be_16(payload + 1);
  decoder_state->width = read_be_16(payload + 3);
  decoder_state->n_components = payload[5];

  fprintf(stderr, "  encoding = Baseline DCT\n");
  fprintf(stderr, "  precision = %d-bit\n", precision);
  fprintf(stderr, "  image dimension = (%d, %d)\n", decoder_state->width, decoder_state->height);

  assert(precision == 8, "Only 8-bit image is supported");
  assert((payload[5] == 1) | (payload[5] == 3), "Only 1 or 3 channels are supported");
  assert(length >= 6 + decoder_state->n_components * 3, "Payload is too short");
  try_malloc(decoder_state->components, sizeof(Component) * decoder_state->n_components);
  try_malloc(decoder_state->image_buffer, decoder_state->height * decoder_state->width * decoder_state->n_components);

  for (int i = 0; i < decoder_state->n_components; i++) {
    uint8_t component_id = payload[6 + i * 3]; // this should be i+1, according to JFIF
    Component *component = &decoder_state->components[component_id - 1];
    component->x_sampling_factor = upper_half(payload[7 + i * 3]);
    component->y_sampling_factor = lower_half(payload[7 + i * 3]);
    component->q_table_id = payload[8 + i * 3];

    fprintf(stderr, "  component %d: sampling_factor = (%d, %d)  q_table_id = %d\n", component_id,
            component->x_sampling_factor, component->y_sampling_factor, component->q_table_id);
  }

  return 0;
}

int handle_sos(const uint8_t *payload, uint16_t length, DecoderState *decoder_state, FILE *f) {
  assert(decoder_state->encoding == SOF0, "Only Baseline JPEG is support");

  uint8_t n_components = payload[0];
  fprintf(stderr, "  n_components in scan = %d\n", n_components);

  for (int i = 0; i < n_components; i++)
    fprintf(stderr, "  component %d: DC coding table = %d  AC coding table = %d\n", payload[1 + i * 2],
            upper_half(payload[2 + i * 2]), lower_half(payload[2 + i * 2]));

  // not used by Baseline DCT
  fprintf(stderr, "  ss = %d\n", payload[1 + n_components * 2]);
  fprintf(stderr, "  se = %d\n", payload[2 + n_components * 2]);
  fprintf(stderr, "  ah = %d\n", payload[3 + n_components * 2]);
  fprintf(stderr, "  al = %d\n", payload[4 + n_components * 2]);

  int n_x_blocks = ceil_div(decoder_state->width, BLOCK_SIZE);
  int n_y_blocks = ceil_div(decoder_state->height, BLOCK_SIZE);

  // A.2.2
  if (n_components == 1) {
    // fprintf(stderr, "Decode 1-component scan is not implemented\n");
    // ignore sampling factor
    int channel_id = payload[1] - 1;
    Component *component = &decoder_state->components[channel_id];
    HuffmanTable *dc_h_table = &decoder_state->h_tables[0][upper_half(payload[2])];
    HuffmanTable *ac_h_table = &decoder_state->h_tables[1][lower_half(payload[2])];
    uint16_t *q_table = decoder_state->q_tables[component->q_table_id];
    int dc_coef = 0;

    for (int y = 0; y < n_y_blocks; y++)
      for (int x = 0; x < n_x_blocks; x++) {
        uint8_t block_u8[BLOCK_SIZE][BLOCK_SIZE];
        // E.2.4
        check(sof0_decode_block(block_u8, &dc_coef, f, dc_h_table, ac_h_table, q_table));
        if (is_rst) {
          dc_coef = 0;
          is_rst = 0;
        }

        // place to image buffer
        for (int j = 0; j < BLOCK_SIZE; j++) {
          int row_idx = y * BLOCK_SIZE + j;
          if (row_idx >= decoder_state->height)
            break;

          for (int i = 0; i < BLOCK_SIZE; i++) {
            int col_idx = x * BLOCK_SIZE + i;
            if (col_idx >= decoder_state->width)
              break;

            decoder_state
                ->image_buffer[(row_idx * decoder_state->width + col_idx) * decoder_state->n_components + channel_id] =
                block_u8[j][i];
          }
        }
      }
    return 0;
  }

  // A.2.3
  // calculate number of MCUs based on chroma-subsampling
  int max_x_sampling = 0, max_y_sampling = 0;
  for (int i = 0; i < decoder_state->n_components; i++) {
    Component *component = decoder_state->components + i;
    max_x_sampling = max(max_x_sampling, component->x_sampling_factor);
    max_y_sampling = max(max_y_sampling, component->y_sampling_factor);
  }
  int mcu_width = BLOCK_SIZE * max_x_sampling;
  int mcu_height = BLOCK_SIZE * max_y_sampling;

  int dc_coefs[3] = {0};
  uint8_t *mcu;
  try_malloc(mcu, mcu_width * mcu_height * n_components);

  // refer to T.81 Table A.2 for MCU packing order
  // A.2.3
  for (int mcu_y = 0; mcu_y < n_y_blocks / max_y_sampling; mcu_y++)
    for (int mcu_x = 0; mcu_x < n_x_blocks / max_x_sampling; mcu_x++) {
      for (int c = 0; c < n_components; c++) {
        int channel_id = payload[1 + c * 2] - 1;
        Component *component = &decoder_state->components[channel_id];
        HuffmanTable *dc_h_table = &decoder_state->h_tables[0][upper_half(payload[2 + c * 2])];
        HuffmanTable *ac_h_table = &decoder_state->h_tables[1][lower_half(payload[2 + c * 2])];
        uint16_t *q_table = decoder_state->q_tables[component->q_table_id];

        for (int y = 0; y < component->y_sampling_factor; y++)
          for (int x = 0; x < component->x_sampling_factor; x++) {
            uint8_t block_u8[BLOCK_SIZE][BLOCK_SIZE];
            check(sof0_decode_block(block_u8, dc_coefs + c, f, dc_h_table, ac_h_table, q_table));

            // place to mcu. A.2.3
            // JFIF p.4
            int n_repeat_y = max_y_sampling / component->y_sampling_factor;
            int n_repeat_x = max_x_sampling / component->x_sampling_factor;
            for (int j = 0; j < BLOCK_SIZE * n_repeat_y; j++) {
              int row_idx = y * BLOCK_SIZE + j;
              for (int i = 0; i < BLOCK_SIZE * n_repeat_x; i++) {
                int col_idx = x * BLOCK_SIZE + i;
                mcu[(row_idx * mcu_width + col_idx) * n_components + c] = block_u8[j / n_repeat_y][i / n_repeat_x];
              }
            }
          }
      }

      for (int j = 0; j < mcu_height; j++) {
        int row_idx = mcu_y * mcu_height + j;
        if (row_idx >= decoder_state->height)
          break;

        for (int i = 0; i < mcu_width; i++) {
          int col_idx = mcu_x * mcu_width + i;
          if (col_idx >= decoder_state->width)
            break;

          ycbcr_to_rgb_(mcu + (j * mcu_width + i) * n_components);
          for (int c = 0; c < n_components; c++) {
            int channel_id = payload[1 + c * 2] - 1;
            decoder_state
                ->image_buffer[(row_idx * decoder_state->width + col_idx) * decoder_state->n_components + channel_id] =
                mcu[(j * mcu_width + i) * n_components + c];
          }
        }
      }
    }
  return 0;
}

// Figure F.12
int32_t extend(uint16_t value, uint16_t n_bits) {
  return value < (1 << (n_bits - 1)) ? value + (-1 << n_bits) + 1 : value;
}

// Figure F.18
int nextbit(FILE *f, uint16_t *out) {
  // impure function
  static uint8_t b, cnt = 0;

  while (cnt == 0) {
    try_fread(&b, 1, 1, f);
    cnt = 8;

    // byte stuffing: ITU T.81 F.1.2.3
    if (b == 0xFF) {
      uint8_t b2;
      try_fread(&b2, 1, 1, f);
      if (b2 != 0) {
        if ((RST0 <= b2) & (b2 < RST0 + 8)) {
          fprintf(stderr, "Encounter RST marker %d\n", b2 - RST0);
          cnt = 0;
          is_rst = 1;
          continue;
        } else if (b2 == DNL) {
          fprintf(stderr, "DNL marker. Not implemented\n");
          return 1;
        } else {
          fprintf(stderr, "Found a marker in scan %X. Decode error?", b2);
          return 1;
        }
      }
    }
  }
  *out = (b >> --cnt) & 1;
  return 0;
}

// Figure F.17
int receive(FILE *f, uint16_t ssss, uint16_t *out) {
  uint16_t v = 0, temp;
  for (int i = 0; i < ssss; i++) {
    check(nextbit(f, &temp));
    v = (v << 1) + temp;
  }
  *out = v;
  return 0;
}

// Figure F.16
int decode(FILE *f, HuffmanTable *h_table, uint16_t *out) {
  int i = -1;
  uint16_t code, temp;
  check(nextbit(f, &code));

  while (code > h_table->maxcode[++i]) {
    check(nextbit(f, &temp));
    code = (code << 1) + temp;
  }
  *out = h_table->huffval[h_table->valptr[i] + code - h_table->mincode[i]];
  return 0;
}

int sof0_decode_block(uint8_t block_u8[BLOCK_SIZE][BLOCK_SIZE], int *dc_coef, FILE *f, HuffmanTable *dc_h_table,
                      HuffmanTable *ac_h_table, uint16_t *q_table) {
  // NOTE: block can be negative
  // NOTE: dequantized value can be out-of-range
  int32_t block[BLOCK_SIZE * BLOCK_SIZE] = {0};
  double block_f64[BLOCK_SIZE][BLOCK_SIZE];

  // decode DC: F.2.2.1
  uint16_t n_bits, value;
  check(decode(f, dc_h_table, &n_bits));
  check(receive(f, n_bits, &value));
  int32_t diff = extend(value, n_bits);

  dc_coef[0] += diff;
  block[0] = dc_coef[0] * q_table[0];

  // decode AC: F.2.2.2
  for (int k = 1; k < BLOCK_SIZE * BLOCK_SIZE;) {
    uint16_t rs;
    check(decode(f, ac_h_table, &rs));
    if (rs == ZRL)
      k += 16;
    else if (rs == EOB)
      break;
    else {
      int rrrr = upper_half(rs);
      int ssss = lower_half(rs);
      k += rrrr;
      assert(k < BLOCK_SIZE * BLOCK_SIZE, "Found invalid code");
      check(receive(f, ssss, &value));
      block[k] = extend(value, ssss) * q_table[k];
      k += 1;
    }
  }

  // undo zig-zag
  for (int i = 0; i < BLOCK_SIZE; i++)
    for (int j = 0; j < BLOCK_SIZE; j++)
      block_f64[i][j] = block[ZIG_ZAG[i][j]];

  idct_2d_((double *)block_f64);

  // level shift and rounding. A.3.1
  for (int i = 0; i < BLOCK_SIZE; i++)
    for (int j = 0; j < BLOCK_SIZE; j++)
      block_u8[i][j] = clip(round(block_f64[i][j]) + 128, 0, 255);

  return 0;
}

// a(u,v) * cos((v+1/2)*u*pi/N)
void init_dct_matrix() {
  for (int i = 0; i < BLOCK_SIZE; i++)
    for (int j = 0; j < BLOCK_SIZE; j++)
      DCT_MATRIX[i][j] = i == 0 ? 0.5 * M_SQRT1_2 : 0.5 * cos((j + 0.5) * i * M_PI / BLOCK_SIZE);
}

void idct_1d(double *x, double *out, size_t offset, size_t stride) {
  for (int i = 0; i < BLOCK_SIZE; i++) {
    double result = 0;
    for (int j = 0; j < BLOCK_SIZE; j++)
      result += x[offset + j * stride] * DCT_MATRIX[j][i]; // DCT transposed
    out[offset + i * stride] = result;
  }
}

void idct_2d_(double *x) {
  double temp[BLOCK_SIZE][BLOCK_SIZE];
  for (int i = 0; i < BLOCK_SIZE; i++)
    idct_1d(x, (double *)temp, i * BLOCK_SIZE, 1); // row-wise
  for (int j = 0; j < BLOCK_SIZE; j++)
    idct_1d((double *)temp, x, j, BLOCK_SIZE); // column-wise
}

// JFIF p.3
void ycbcr_to_rgb_(uint8_t *x) {
  // clang-format off
  double r = x[0]                          + 1.402   * (x[2] - 128);
  double g = x[0] - 0.34414 * (x[1] - 128) - 0.71414 * (x[2] - 128);
  double b = x[0] + 1.772   * (x[1] - 128);
  // clang-format on
  x[0] = clip(round(r), 0, 255);
  x[1] = clip(round(g), 0, 255);
  x[2] = clip(round(b), 0, 255);
}
