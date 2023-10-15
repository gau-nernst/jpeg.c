#include "jpeg.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define _USE_MATH_DEFINES // math constants for MSVC
#include <math.h>

#define DATA_UNIT_SIZE 8
#define MAX_HUFFMAN_CODE_LENGTH 16
#define MAX_COMPONENTS 256

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
  int x_sampling;
  int y_sampling;
  int q_table_id;
} Component;

typedef struct {
  uint8_t encoding;
  uint16_t restart_interval;
  uint16_t q_tables[4][DATA_UNIT_SIZE * DATA_UNIT_SIZE];
  HuffmanTable h_tables[2][4];
  Component components[MAX_COMPONENTS];
  int max_x_sampling;
  int max_y_sampling;
  int dc_preds[MAX_COMPONENTS];
  int is_restart;
  Image8 *image;
} DecoderState;

static uint16_t read_be_16(const uint8_t *buffer) { return (buffer[0] << 8) | buffer[1]; }
static uint8_t upper_half(uint8_t x) { return x >> 4; }
static uint8_t lower_half(uint8_t x) { return x & 0xF; }

static int handle_app0(const uint8_t *, uint16_t);
static int handle_dqt(const uint8_t *, uint16_t, DecoderState *);
static int handle_dht(const uint8_t *, uint16_t, DecoderState *);
static int handle_sof0(const uint8_t *, uint16_t, DecoderState *);
static int handle_sos(const uint8_t *, uint16_t, DecoderState *, FILE *);

static int sof0_decode_data_unit(FILE *, uint8_t[DATA_UNIT_SIZE][DATA_UNIT_SIZE], DecoderState *, int, int, int);

static void idct_2d_(double *);
static void ycbcr_to_rgb_(uint8_t *);

// clang-format off
// ITU T.81 Figure A.6
const uint8_t ZIG_ZAG[DATA_UNIT_SIZE][DATA_UNIT_SIZE] = {
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
double DCT_MATRIX[DATA_UNIT_SIZE][DATA_UNIT_SIZE];

int decode_jpeg(FILE *f, Image8 *image) {
  uint8_t marker[2];
  uint16_t length;
  uint8_t *payload = NULL;
  DecoderState decoder_state;
  decoder_state.image = image;

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
      if ((APP0 < marker[1]) & (marker[1] <= APP0 + 15)) {
        fprintf(stderr, "APP%d (length = %d)\n", marker[1] - APP0, length);
        fprintf(stderr, "  identifier = %s\n", payload);
      } else
        fprintf(stderr, "Unknown marker (length = %d)\n", length);
      break;
    }

    try_free(payload);
    fprintf(stderr, "\n");
  }

  // TODO: free decoder_state
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

// ITU-T.81 B.2.4.1
// there can be multiple quantization tables within 1 DQT segment
int handle_dqt(const uint8_t *payload, uint16_t length, DecoderState *decoder_state) {
  int offset = 0;
  while (offset < length) {
    uint8_t precision = upper_half(payload[offset]);
    uint8_t identifier = lower_half(payload[offset]);
    fprintf(stderr, "  precision = %d (%d-bit), identifier = %d\n", precision, (precision + 1) * 8, identifier);
    assert(length >= offset + 1 + DATA_UNIT_SIZE * DATA_UNIT_SIZE * (precision + 1), "Payload is too short");

    uint16_t *q_table = decoder_state->q_tables[identifier];
    if (precision) {
      for (int i = 0; i < DATA_UNIT_SIZE * DATA_UNIT_SIZE; i++)
        q_table[i] = read_be_16(payload + offset + 1 + i * 2);
    } else {
      for (int i = 0; i < DATA_UNIT_SIZE * DATA_UNIT_SIZE; i++)
        q_table[i] = payload[offset + 1 + i];
    }

    for (int i = 0; i < DATA_UNIT_SIZE; i++) {
      fprintf(stderr, "  ");
      for (int j = 0; j < DATA_UNIT_SIZE; j++)
        fprintf(stderr, " %3d", q_table[ZIG_ZAG[i][j]]);
      fprintf(stderr, "\n");
    }

    offset += 1 + DATA_UNIT_SIZE * DATA_UNIT_SIZE * (precision + 1);
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
  Image8 *image = decoder_state->image;
  decoder_state->encoding = SOF0;

  // Table B.2
  assert(length >= 6, "Payload is too short");
  uint8_t precision = payload[0];
  image->height = read_be_16(payload + 1);
  image->width = read_be_16(payload + 3);
  image->n_channels = payload[5];

  fprintf(stderr, "  encoding = Baseline DCT\n");
  fprintf(stderr, "  precision = %d-bit\n", precision);
  fprintf(stderr, "  image dimension = (%d, %d)\n", image->width, image->height);

  // TODO: check if image->data is allocated?
  assert(precision == 8, "Only 8-bit image is supported");
  assert((payload[5] == 1) | (payload[5] == 3), "Only 1 or 3 channels are supported");
  assert(length >= 6 + image->n_channels * 3, "Payload is too short");
  try_malloc(image->data, image->height * image->width * image->n_channels);

  decoder_state->max_x_sampling = 0;
  decoder_state->max_y_sampling = 0;
  for (int i = 0; i < image->n_channels; i++) {
    uint8_t component_id = payload[6 + i * 3];
    Component *component = &decoder_state->components[component_id];
    component->x_sampling = upper_half(payload[7 + i * 3]);
    component->y_sampling = lower_half(payload[7 + i * 3]);
    component->q_table_id = payload[8 + i * 3];

    decoder_state->max_x_sampling = max(decoder_state->max_x_sampling, component->x_sampling);
    decoder_state->max_y_sampling = max(decoder_state->max_y_sampling, component->y_sampling);

    fprintf(stderr, "  component %d: sampling_factor = (%d, %d)  q_table_id = %d\n", component_id,
            component->x_sampling, component->y_sampling, component->q_table_id);
  }

  return 0;
}

int handle_sos(const uint8_t *payload, uint16_t length, DecoderState *decoder_state, FILE *f) {
  assert(decoder_state->encoding == SOF0, "Only Baseline JPEG is support");
  Image8 *image = decoder_state->image;

  uint8_t n_components = payload[0];
  fprintf(stderr, "  n_components in scan = %d\n", n_components);
  assert(n_components <= image->n_channels, "Scan contains more channels than declared");

  for (int i = 0; i < n_components; i++)
    fprintf(stderr, "  component %d: DC coding table = %d  AC coding table = %d\n", payload[1 + i * 2],
            upper_half(payload[2 + i * 2]), lower_half(payload[2 + i * 2]));

  // not used by Baseline DCT
  fprintf(stderr, "  ss = %d\n", payload[1 + n_components * 2]);
  fprintf(stderr, "  se = %d\n", payload[2 + n_components * 2]);
  fprintf(stderr, "  ah = %d\n", payload[3 + n_components * 2]);
  fprintf(stderr, "  al = %d\n", payload[4 + n_components * 2]);

  if (n_components == 1) {
    // Non-interleaved order. A.2.2
    int component_id = payload[1];
    int dc_table_id = upper_half(payload[2]);
    int ac_table_id = lower_half(payload[2]);

    decoder_state->dc_preds[component_id] = 0;
    decoder_state->is_restart = 0;

    // TODO: take into account sampling factor
    int nx_blocks = ceil_div(image->width, DATA_UNIT_SIZE);
    int ny_blocks = ceil_div(image->height, DATA_UNIT_SIZE);
    fprintf(stderr, "%d %d\n", nx_blocks, ny_blocks);

    for (int mcu_idx = 0; mcu_idx < ny_blocks * nx_blocks;) {
      uint8_t block_u8[DATA_UNIT_SIZE][DATA_UNIT_SIZE];
      check(sof0_decode_data_unit(f, block_u8, decoder_state, dc_table_id, ac_table_id, component_id));

      // E.2.4
      // When restart marker is received, ignore current MCU. Reset decoder state and move on to the next MCU
      // NOTE: we don't check for consecutive restart markers
      if (decoder_state->is_restart) {
        decoder_state->dc_preds[component_id] = 0;
        decoder_state->is_restart = 0;
        mcu_idx = ceil_div(mcu_idx, decoder_state->restart_interval) * decoder_state->restart_interval;
        continue;
      }

      // place mcu to image buffer
      int y = mcu_idx / nx_blocks;
      int x = mcu_idx % nx_blocks;
      for (int j = 0; j < min(DATA_UNIT_SIZE, image->height - y * DATA_UNIT_SIZE); j++) {
        int row_idx = y * DATA_UNIT_SIZE + j;
        for (int i = 0; i < min(DATA_UNIT_SIZE, image->width - x * DATA_UNIT_SIZE); i++) {
          int col_idx = x * DATA_UNIT_SIZE + i;
          image->data[(row_idx * image->width + col_idx) * image->n_channels + component_id] = block_u8[j][i];
        }
      }
      mcu_idx++;
    }
    return 0;
  }

  // Interleaved order. A.2.3
  // calculate number of MCUs based on chroma-subsampling
  int mcu_width = DATA_UNIT_SIZE * decoder_state->max_x_sampling;
  int mcu_height = DATA_UNIT_SIZE * decoder_state->max_y_sampling;
  int nx_mcu = ceil_div(image->width, mcu_width);
  int ny_mcu = ceil_div(image->height, mcu_height);

  for (int i = 0; i < MAX_COMPONENTS; i++)
    decoder_state->dc_preds[i] = 0;
  decoder_state->is_restart = 0;
  uint8_t *mcu;
  try_malloc(mcu, mcu_width * mcu_height * n_components);

  for (int mcu_y = 0; mcu_y < ny_mcu; mcu_y++)
    for (int mcu_x = 0; mcu_x < nx_mcu; mcu_x++) {
      for (int c = 0; c < n_components; c++) {
        int component_id = payload[1 + c * 2];
        int dc_table_id = upper_half(payload[2 + c * 2]);
        int ac_table_id = lower_half(payload[2 + c * 2]);
        Component *component = &decoder_state->components[component_id];

        for (int y = 0; y < component->y_sampling; y++)
          for (int x = 0; x < component->x_sampling; x++) {
            uint8_t block_u8[DATA_UNIT_SIZE][DATA_UNIT_SIZE];
            check(sof0_decode_data_unit(f, block_u8, decoder_state, dc_table_id, ac_table_id, component_id));

            // place to mcu. A.2.3 and JFIF p.4
            int n_repeat_y = decoder_state->max_y_sampling / component->y_sampling;
            int n_repeat_x = decoder_state->max_x_sampling / component->x_sampling;
            for (int j = 0; j < DATA_UNIT_SIZE * n_repeat_y; j++) {
              int row_idx = y * DATA_UNIT_SIZE + j;
              for (int i = 0; i < DATA_UNIT_SIZE * n_repeat_x; i++) {
                int col_idx = x * DATA_UNIT_SIZE + i;
                mcu[(row_idx * mcu_width + col_idx) * n_components + c] = block_u8[j / n_repeat_y][i / n_repeat_x];
              }
            }
          }
      }

      for (int j = 0; j < min(mcu_height, image->height - mcu_y * mcu_height); j++) {
        int row_idx = mcu_y * mcu_height + j;
        for (int i = 0; i < min(mcu_width, image->width - mcu_x * mcu_width); i++) {
          int col_idx = mcu_x * mcu_width + i;
          ycbcr_to_rgb_(mcu + (j * mcu_width + i) * n_components);
          for (int c = 0; c < n_components; c++) {
            int channel_id = payload[1 + c * 2] - 1;
            image->data[(row_idx * image->width + col_idx) * image->n_channels + channel_id] =
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
int nextbit(FILE *f, uint16_t *out, DecoderState *decoder_state) {
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
          fprintf(stderr, "Encounter RST%d marker\n", b2 - RST0);
          cnt = 0;
          decoder_state->is_restart = 1;
          return 0;
        } else if (b2 == DNL) {
          fprintf(stderr, "DNL marker. Not implemented\n");
          return 1;
        } else {
          fprintf(stderr, "Found marker %X in scan. Decode error?", b2);
          return 1;
        }
      }
    }
  }
  *out = (b >> --cnt) & 1;
  return 0;
}

// Figure F.17
int receive(FILE *f, uint16_t ssss, uint16_t *out, DecoderState *decoder_state) {
  uint16_t v = 0, temp;
  for (int i = 0; i < ssss; i++) {
    check(nextbit(f, &temp, decoder_state));
    if (decoder_state->is_restart)
      return 0;
    v = (v << 1) + temp;
  }
  *out = v;
  return 0;
}

// Figure F.16
int decode(FILE *f, HuffmanTable *h_table, uint16_t *out, DecoderState *decoder_state) {
  int i = -1;
  uint16_t code, temp;
  check(nextbit(f, &code, decoder_state));
  if (decoder_state->is_restart)
    return 0;

  while (code > h_table->maxcode[++i]) {
    check(nextbit(f, &temp, decoder_state));
    if (decoder_state->is_restart)
      return 0;
    code = (code << 1) + temp;
  }
  *out = h_table->huffval[h_table->valptr[i] + code - h_table->mincode[i]];
  return 0;
}

int sof0_decode_data_unit(FILE *f, uint8_t block_u8[DATA_UNIT_SIZE][DATA_UNIT_SIZE], DecoderState *decoder_state,
                          int dc_table_id, int ac_table_id, int component_id) {
  HuffmanTable *dc_table = &decoder_state->h_tables[0][dc_table_id];
  HuffmanTable *ac_table = &decoder_state->h_tables[1][ac_table_id];
  uint16_t *q_table = decoder_state->q_tables[decoder_state->components[component_id].q_table_id];

  // NOTE: block can be negative
  // NOTE: dequantized value can be out-of-range
  int32_t block[DATA_UNIT_SIZE * DATA_UNIT_SIZE] = {0};
  double block_f64[DATA_UNIT_SIZE][DATA_UNIT_SIZE];

  // decode DC: F.2.2.1
  uint16_t n_bits, value;
  check(decode(f, dc_table, &n_bits, decoder_state));
  if (decoder_state->is_restart)
    return 0;
  check(receive(f, n_bits, &value, decoder_state));
  if (decoder_state->is_restart)
    return 0;
  int32_t diff = extend(value, n_bits);

  decoder_state->dc_preds[component_id] += diff;
  block[0] = decoder_state->dc_preds[component_id] * q_table[0];

  // decode AC: F.2.2.2
  for (int k = 1; k < DATA_UNIT_SIZE * DATA_UNIT_SIZE;) {
    uint16_t rs;
    check(decode(f, ac_table, &rs, decoder_state));
    if (decoder_state->is_restart)
      return 0;
    if (rs == ZRL)
      k += 16;
    else if (rs == EOB)
      break;
    else {
      int rrrr = upper_half(rs);
      int ssss = lower_half(rs);
      k += rrrr;
      assert(k < DATA_UNIT_SIZE * DATA_UNIT_SIZE, "Encounter invalid code");
      check(receive(f, ssss, &value, decoder_state));
      if (decoder_state->is_restart)
        return 0;
      block[k] = extend(value, ssss) * q_table[k];
      k += 1;
    }
  }

  // undo zig-zag
  for (int i = 0; i < DATA_UNIT_SIZE; i++)
    for (int j = 0; j < DATA_UNIT_SIZE; j++)
      block_f64[i][j] = block[ZIG_ZAG[i][j]];

  idct_2d_((double *)block_f64);

  // level shift and rounding. A.3.1
  for (int i = 0; i < DATA_UNIT_SIZE; i++)
    for (int j = 0; j < DATA_UNIT_SIZE; j++)
      block_u8[i][j] = clip(round(block_f64[i][j]) + 128, 0, 255);

  return 0;
}

// a(u,v) * cos((v+1/2)*u*pi/N)
void init_dct_matrix() {
  for (int i = 0; i < DATA_UNIT_SIZE; i++)
    for (int j = 0; j < DATA_UNIT_SIZE; j++)
      DCT_MATRIX[i][j] = i == 0 ? 0.5 * M_SQRT1_2 : 0.5 * cos((j + 0.5) * i * M_PI / DATA_UNIT_SIZE);
}

// TODO: benchmark. use single-precision instead of double-precision?
void idct_1d(double *x, double *out, size_t offset, size_t stride) {
  for (int i = 0; i < DATA_UNIT_SIZE; i++) {
    double result = 0;
    for (int j = 0; j < DATA_UNIT_SIZE; j++)
      result += x[offset + j * stride] * DCT_MATRIX[j][i]; // DCT transposed
    out[offset + i * stride] = result;
  }
}

void idct_2d_(double *x) {
  double temp[DATA_UNIT_SIZE][DATA_UNIT_SIZE];
  for (int i = 0; i < DATA_UNIT_SIZE; i++)
    idct_1d(x, (double *)temp, i * DATA_UNIT_SIZE, 1); // row-wise
  for (int j = 0; j < DATA_UNIT_SIZE; j++)
    idct_1d((double *)temp, x, j, DATA_UNIT_SIZE); // column-wise
}

// JFIF p.3
void ycbcr_to_rgb_(uint8_t *x) {
  // clang-format off
  float r = x[0]                           + 1.402f   * (x[2] - 128);
  float g = x[0] - 0.34414f * (x[1] - 128) - 0.71414f * (x[2] - 128);
  float b = x[0] + 1.772f   * (x[1] - 128);
  // clang-format on
  x[0] = clip(round(r), 0, 255);
  x[1] = clip(round(g), 0, 255);
  x[2] = clip(round(b), 0, 255);
}
