#include "jpeg.h"
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA_UNIT_SIZE 8
#define MAX_HUFFMAN_CODE_LENGTH 16
#define MAX_COMPONENTS 4

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

static bool ERROR = false;
static const char *ERROR_MSG = NULL;

static bool assert(bool condition, const char *msg) {
  if (!condition) {
    ERROR = true;
    ERROR_MSG = msg;
    fprintf(stderr, "%s", msg);
    raise(SIGABRT);
  }
  return condition;
}

static void *try_malloc(size_t size) {
  void *ptr = malloc(size);
  assert(ptr != NULL, "Failed to allocate memory");
  return ptr;
}

static bool try_fread(void *buffer, size_t size, size_t count, FILE *stream) {
  size_t n_elems = fread(buffer, size, count, stream);
  assert(n_elems == count, "Failed to read data. Perhaps EOF?");
  return n_elems;
}

typedef struct HuffmanTable {
  uint8_t *huffsize;
  uint16_t *huffcode;
  uint8_t *huffval;
  uint16_t mincode[16];
  int32_t maxcode[16];
  uint8_t valptr[16];
} HuffmanTable;

typedef struct Component {
  int x_sampling;
  int y_sampling;
  int q_table_id;
} Component;

typedef struct Decoder {
  uint8_t encoding;
  uint16_t restart_interval;
  uint16_t q_tables[4][DATA_UNIT_SIZE * DATA_UNIT_SIZE];
  HuffmanTable h_tables[2][4];
  Component components[MAX_COMPONENTS];
  int max_x_sampling;
  int max_y_sampling;
  int dc_preds[MAX_COMPONENTS];
  int is_restart;
  uint8_t *image;
  int width;
  int height;
  int n_channels;
} Decoder;

static uint16_t read_be_16(const uint8_t *buffer) { return (buffer[0] << 8) | buffer[1]; }
static uint8_t upper_half(uint8_t x) { return x >> 4; }
static uint8_t lower_half(uint8_t x) { return x & 0xF; }

static void handle_app0(const uint8_t *buffer, uint16_t buflen);
static int handle_dqt(Decoder *decoder, const uint8_t *buffer, uint16_t buflen);
static int handle_dht(Decoder *decoder, const uint8_t *buffer, uint16_t buflen);
static int handle_sof0(Decoder *decoder, const uint8_t *buffer, uint16_t buflen);
static int handle_sos(Decoder *decoder, const uint8_t *buffer, uint16_t buflen, FILE *f);

static int sof0_decode_data_unit(Decoder *decoder, FILE *f, uint8_t block[DATA_UNIT_SIZE][DATA_UNIT_SIZE], int, int,
                                 int);

static void idct_2d_(double *);
static void ycbcr_to_rgb_(uint8_t *);

// ITU T.81 Figure A.6
// clang-format off
static const uint8_t ZIG_ZAG[DATA_UNIT_SIZE][DATA_UNIT_SIZE] = {
  { 0,  1,  5,  6, 14, 15, 27, 28},
  { 2,  4,  7, 13, 16, 26, 29, 42},
  { 3,  8, 12, 17, 25, 30, 41, 43},
  { 9, 11, 18, 24, 31, 40, 44, 53},
  {10, 19, 23, 32, 39, 45, 52, 54},
  {20, 22, 33, 38, 46, 51, 55, 60},
  {21, 34, 37, 47, 50, 56, 59, 61},
  {35, 36, 48, 49, 57, 58, 62, 63},
};

static const double DCT_TABLE[] = {
   0.5000000000000000,  0.4903926402016152,  0.4619397662556434,  0.4157348061512726,
   0.3535533905932738,  0.2777851165098011,  0.1913417161825449,  0.0975451610080642,
   0.0000000000000000, -0.0975451610080641, -0.1913417161825449, -0.2777851165098010,
  -0.3535533905932737, -0.4157348061512727, -0.4619397662556434, -0.4903926402016152,
  -0.5000000000000000, -0.4903926402016152, -0.4619397662556434, -0.4157348061512726,
  -0.3535533905932738, -0.2777851165098011, -0.1913417161825449, -0.0975451610080642,
  -0.0000000000000000,  0.0975451610080641,  0.1913417161825449,  0.2777851165098010,
   0.3535533905932737,  0.4157348061512727,  0.4619397662556434,  0.4903926402016152,
};
// clang-format on

uint8_t *decode_jpeg(FILE *f, int *width, int *height, int *n_channels) {
  uint8_t marker[2];
  uint16_t buflen;
  uint8_t *buffer = NULL;
  Decoder decoder;

  bool finished = false;
  while (!finished) {
    try_fread(marker, 1, 2, f);
    fprintf(stderr, "%X%X ", marker[0], marker[1]);

    assert(marker[0] == 0xFF, "Not a marker");

    if (marker[1] == TEM | marker[1] == SOI | marker[1] == EOI | (marker[1] >= RST0 & marker[1] < RST0 + 8)) {
      buflen = 0;
    } else {
      try_fread(&buflen, 1, 2, f);
      buflen = read_be_16((uint8_t *)&buflen) - 2;

      // TODO: re-use payload buffer
      buffer = try_malloc(buflen);
      try_fread(buffer, 1, buflen, f);
    }

    switch (marker[1]) {
    case SOI:
      fprintf(stderr, "SOI");
      break;

    case APP0:
      fprintf(stderr, "APP0 (length = %d)\n", buflen);
      handle_app0(buffer, buflen);
      break;

    case DQT:
      fprintf(stderr, "DQT (length = %d)\n", buflen);
      if (handle_dqt(&decoder, buffer, buflen))
        return 0;
      break;

    case DHT:
      fprintf(stderr, "DHT (length = %d)\n", buflen);
      if (handle_dht(&decoder, buffer, buflen))
        return 0;
      break;

    case SOF0:
      fprintf(stderr, "SOF0 (length = %d)\n", buflen);
      if (handle_sof0(&decoder, buffer, buflen))
        return 0;
      break;

    case DRI:
      fprintf(stderr, "DRI (length = %d)\n", buflen);
      assert(buflen >= 2, "Payload not long enough");
      decoder.restart_interval = read_be_16(buffer);
      fprintf(stderr, "  restart interval = %d\n", decoder.restart_interval);
      break;

    case SOS:
      fprintf(stderr, "SOS\n");
      if (handle_sos(&decoder, buffer, buflen, f))
        return 0;
      break;

    case EOI:
      fprintf(stderr, "EOI\n");
      finished = true;
      break;

    default:
      if ((APP0 < marker[1]) & (marker[1] <= APP0 + 15)) {
        fprintf(stderr, "APP%d (length = %d)\n", marker[1] - APP0, buflen);
        fprintf(stderr, "  identifier = %s\n", buffer);
      } else
        fprintf(stderr, "Unknown marker (length = %d)\n", buflen);
      break;
    }

    try_free(buffer);
    fprintf(stderr, "\n");
  }

  // TODO: free decoder

  if (width != NULL)
    *width = decoder.width;
  if (height != NULL)
    *height = decoder.height;
  if (n_channels != NULL)
    *n_channels = decoder.n_channels;
  return decoder.image;
}

// JFIF i.e. JPEG Part 5
void handle_app0(const uint8_t *buffer, uint16_t buflen) {
  fprintf(stderr, "  identifier = %.5s\n", buffer); // either JFIF or JFXX

  if (strcmp((const char *)buffer, "JFIF") == 0) {
    assert(buflen >= 14, "Payload is too short");
    fprintf(stderr, "  version = %d.%d\n", buffer[5], buffer[6]);
    fprintf(stderr, "  units = %d\n", buffer[7]);
    fprintf(stderr, "  density = (%d, %d)\n", read_be_16(buffer + 8), read_be_16(buffer + 10));
    fprintf(stderr, "  thumbnail = (%d, %d)\n", buffer[12], buffer[13]);
  } else if (strcmp((const char *)buffer, "JFXX") == 0) {
    fprintf(stderr, "  extension_code = %X\n", buffer[5]);
  } else
    fprintf(stderr, "  Invalid identifier\n");
}

// ITU-T.81 B.2.4.1
// there can be multiple quantization tables within 1 DQT segment
int handle_dqt(Decoder *decoder, const uint8_t *buffer, uint16_t buflen) {
  int offset = 0;
  while (offset < buflen) {
    uint8_t precision = upper_half(buffer[offset]);
    uint8_t identifier = lower_half(buffer[offset]);
    fprintf(stderr, "  precision = %d (%d-bit), identifier = %d\n", precision, (precision + 1) * 8, identifier);
    assert(buflen >= offset + 1 + DATA_UNIT_SIZE * DATA_UNIT_SIZE * (precision + 1), "Payload is too short");

    uint16_t *q_table = decoder->q_tables[identifier];
    if (precision) {
      for (int i = 0; i < DATA_UNIT_SIZE * DATA_UNIT_SIZE; i++)
        q_table[i] = read_be_16(buffer + offset + 1 + i * 2);
    } else {
      for (int i = 0; i < DATA_UNIT_SIZE * DATA_UNIT_SIZE; i++)
        q_table[i] = buffer[offset + 1 + i];
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
int handle_dht(Decoder *decoder, const uint8_t *buffer, uint16_t buflen) {
  int offset = 0;
  while (offset < buflen) {
    uint8_t class = upper_half(buffer[offset]);
    uint8_t identifier = lower_half(buffer[offset]);
    fprintf(stderr, "  class = %d (%s), identifier = %d\n", class, class ? "AC" : "DC", identifier);
    assert(buflen >= offset + 1 + MAX_HUFFMAN_CODE_LENGTH, "Payload is too short");

    // ITU-T.81 Annex C: create Huffman table
    HuffmanTable *h_table = &decoder->h_tables[class][identifier];
    int n_codes = 0;
    for (int i = 0; i < MAX_HUFFMAN_CODE_LENGTH; i++)
      n_codes += buffer[offset + 1 + i];
    assert(buflen >= offset + 1 + MAX_HUFFMAN_CODE_LENGTH + n_codes, "Payload is too short");

    h_table->huffsize = try_malloc(n_codes * sizeof(*h_table->huffsize));
    h_table->huffcode = try_malloc(n_codes * sizeof(*h_table->huffcode));
    h_table->huffval = try_malloc(n_codes * sizeof(*h_table->huffval));

    // Figure C.1 and C.2
    for (int i = 0, k = 0, code = 0; i < MAX_HUFFMAN_CODE_LENGTH; i++) {
      for (int j = 0; j < buffer[offset + 1 + i]; j++, k++, code++) {
        h_table->huffsize[k] = i;
        h_table->huffcode[k] = code;
        h_table->huffval[k] = buffer[offset + 1 + MAX_HUFFMAN_CODE_LENGTH + k];
      }
      code = code << 1;
    }

    // Figure F.16
    for (int i = 0, j = 0; i < MAX_HUFFMAN_CODE_LENGTH; i++)
      if (buffer[offset + 1 + i]) {
        h_table->valptr[i] = j;
        h_table->mincode[i] = h_table->huffcode[j];
        h_table->maxcode[i] = h_table->huffcode[j + buffer[offset + 1 + i] - 1];
        j += buffer[offset + 1 + i];
      } else
        h_table->maxcode[i] = -1;

    fprintf(stderr, "  n_codes = %d\n", n_codes);
    print_list("  BITS     =", buffer + offset + 1, MAX_HUFFMAN_CODE_LENGTH, " %3d");
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

int handle_sof0(Decoder *decoder, const uint8_t *buffer, uint16_t buflen) {
  decoder->encoding = SOF0;

  // Table B.2
  assert(buflen >= 6, "Payload is too short");
  uint8_t precision = buffer[0];
  decoder->height = read_be_16(buffer + 1);
  decoder->width = read_be_16(buffer + 3);
  decoder->n_channels = buffer[5];

  fprintf(stderr, "  encoding = Baseline DCT\n");
  fprintf(stderr, "  precision = %d-bit\n", precision);
  fprintf(stderr, "  image dimension = (%d, %d)\n", decoder->width, decoder->height);

  // TODO: check if image->data is allocated?
  assert(precision == 8, "Only 8-bit image is supported");
  assert((buffer[5] == 1) | (buffer[5] == 3), "Only 1 or 3 channels are supported");
  assert(buflen >= 6 + decoder->n_channels * 3, "Payload is too short");
  decoder->image = try_malloc(decoder->height * decoder->width * decoder->n_channels);

  decoder->max_x_sampling = 0;
  decoder->max_y_sampling = 0;
  for (int i = 0; i < decoder->n_channels; i++) {
    uint8_t component_id = buffer[6 + i * 3];
    Component *component = &decoder->components[component_id];
    component->x_sampling = upper_half(buffer[7 + i * 3]);
    component->y_sampling = lower_half(buffer[7 + i * 3]);
    component->q_table_id = buffer[8 + i * 3];

    decoder->max_x_sampling = max(decoder->max_x_sampling, component->x_sampling);
    decoder->max_y_sampling = max(decoder->max_y_sampling, component->y_sampling);

    fprintf(stderr, "  component %d: sampling_factor = (%d, %d)  q_table_id = %d\n", component_id,
            component->x_sampling, component->y_sampling, component->q_table_id);
  }

  return 0;
}

int handle_sos(Decoder *decoder, const uint8_t *payload, uint16_t length, FILE *f) {
  assert(decoder->encoding == SOF0, "Only Baseline JPEG is support");

  uint8_t n_components = payload[0];
  fprintf(stderr, "  n_components in scan = %d\n", n_components);
  assert(n_components <= decoder->n_channels, "Scan contains more channels than declared");

  for (int i = 0; i < n_components; i++)
    fprintf(stderr, "  component %d: DC coding table = %d  AC coding table = %d\n", payload[1 + i * 2],
            upper_half(payload[2 + i * 2]), lower_half(payload[2 + i * 2]));

  // not used by Baseline DCT
  fprintf(stderr, "  ss = %d\n", payload[1 + n_components * 2]);
  fprintf(stderr, "  se = %d\n", payload[2 + n_components * 2]);
  fprintf(stderr, "  ah = %d\n", upper_half(payload[3 + n_components * 2]));
  fprintf(stderr, "  al = %d\n", lower_half(payload[3 + n_components * 2]));

  if (n_components == 1) {
    // Non-interleaved order. A.2.2
    int component_id = payload[1];
    int dc_table_id = upper_half(payload[2]);
    int ac_table_id = lower_half(payload[2]);

    decoder->dc_preds[component_id] = 0;
    decoder->is_restart = 0;

    // TODO: take into account sampling factor
    int nx_blocks = ceil_div(decoder->width, DATA_UNIT_SIZE);
    int ny_blocks = ceil_div(decoder->height, DATA_UNIT_SIZE);
    fprintf(stderr, "%d %d\n", nx_blocks, ny_blocks);

    for (int mcu_idx = 0; mcu_idx < ny_blocks * nx_blocks;) {
      uint8_t block_u8[DATA_UNIT_SIZE][DATA_UNIT_SIZE];
      assert(sof0_decode_data_unit(decoder, f, block_u8, dc_table_id, ac_table_id, component_id) == 0, "Error");

      // E.2.4
      // When restart marker is received, ignore current MCU. Reset decoder state and move on to the next MCU
      // NOTE: we don't check for consecutive restart markers
      if (decoder->is_restart) {
        decoder->dc_preds[component_id] = 0;
        decoder->is_restart = 0;
        mcu_idx = ceil_div(mcu_idx, decoder->restart_interval) * decoder->restart_interval;
        continue;
      }

      // place mcu to image buffer
      int y = mcu_idx / nx_blocks;
      int x = mcu_idx % nx_blocks;
      for (int j = 0; j < min(DATA_UNIT_SIZE, decoder->height - y * DATA_UNIT_SIZE); j++) {
        int row_idx = y * DATA_UNIT_SIZE + j;
        for (int i = 0; i < min(DATA_UNIT_SIZE, decoder->width - x * DATA_UNIT_SIZE); i++) {
          int col_idx = x * DATA_UNIT_SIZE + i;
          decoder->image[(row_idx * decoder->width + col_idx) * decoder->n_channels + component_id] = block_u8[j][i];
        }
      }
      mcu_idx++;
    }
    return 0;
  }

  // Interleaved order. A.2.3
  // calculate number of MCUs based on chroma-subsampling
  // TODO: handle restart markers for 3-channel
  int mcu_width = DATA_UNIT_SIZE * decoder->max_x_sampling;
  int mcu_height = DATA_UNIT_SIZE * decoder->max_y_sampling;
  int nx_mcu = ceil_div(decoder->width, mcu_width);
  int ny_mcu = ceil_div(decoder->height, mcu_height);

  for (int i = 0; i < MAX_COMPONENTS; i++)
    decoder->dc_preds[i] = 0;
  decoder->is_restart = 0;
  uint8_t *mcu = try_malloc(mcu_width * mcu_height * n_components);

  for (int mcu_y = 0; mcu_y < ny_mcu; mcu_y++)
    for (int mcu_x = 0; mcu_x < nx_mcu; mcu_x++) {
      for (int c = 0; c < n_components; c++) {
        int component_id = payload[1 + c * 2];
        int dc_table_id = upper_half(payload[2 + c * 2]);
        int ac_table_id = lower_half(payload[2 + c * 2]);
        Component *component = &decoder->components[component_id];

        for (int y = 0; y < component->y_sampling; y++)
          for (int x = 0; x < component->x_sampling; x++) {
            uint8_t block_u8[DATA_UNIT_SIZE][DATA_UNIT_SIZE];
            assert(sof0_decode_data_unit(decoder, f, block_u8, dc_table_id, ac_table_id, component_id) == 0, "Error");

            // place to mcu. A.2.3 and JFIF p.4
            // NOTE: assume order in the scan is YCbCr
            int n_repeat_y = decoder->max_y_sampling / component->y_sampling;
            int n_repeat_x = decoder->max_x_sampling / component->x_sampling;
            for (int j = 0; j < DATA_UNIT_SIZE * n_repeat_y; j++) {
              int row_idx = y * DATA_UNIT_SIZE + j;
              for (int i = 0; i < DATA_UNIT_SIZE * n_repeat_x; i++) {
                int col_idx = x * DATA_UNIT_SIZE + i;
                mcu[(row_idx * mcu_width + col_idx) * n_components + c] = block_u8[j / n_repeat_y][i / n_repeat_x];
              }
            }
          }
      }

      for (int j = 0; j < min(mcu_height, decoder->height - mcu_y * mcu_height); j++) {
        int row_idx = mcu_y * mcu_height + j;
        for (int i = 0; i < min(mcu_width, decoder->width - mcu_x * mcu_width); i++) {
          int col_idx = mcu_x * mcu_width + i;
          ycbcr_to_rgb_(mcu + (j * mcu_width + i) * n_components);
          for (int c = 0; c < n_components; c++) {
            int channel_id = payload[1 + c * 2] - 1;
            decoder->image[(row_idx * decoder->width + col_idx) * decoder->n_channels + channel_id] =
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
int nextbit(FILE *f, uint16_t *out, Decoder *decoder_state) {
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
int receive(FILE *f, uint16_t ssss, uint16_t *out, Decoder *decoder_state) {
  uint16_t v = 0, temp;
  for (int i = 0; i < ssss; i++) {
    assert(nextbit(f, &temp, decoder_state) == 0, "Error");
    if (decoder_state->is_restart)
      return 0;
    v = (v << 1) + temp;
  }
  *out = v;
  return 0;
}

// Figure F.16
int decode(FILE *f, HuffmanTable *h_table, uint16_t *out, Decoder *decoder_state) {
  int i = -1;
  uint16_t code, temp;
  assert(nextbit(f, &code, decoder_state) == 0, "Error");
  if (decoder_state->is_restart)
    return 0;

  while (code > h_table->maxcode[++i]) {
    assert(nextbit(f, &temp, decoder_state) == 0, "Error");
    if (decoder_state->is_restart)
      return 0;
    code = (code << 1) + temp;
  }
  *out = h_table->huffval[h_table->valptr[i] + code - h_table->mincode[i]];
  return 0;
}

int sof0_decode_data_unit(Decoder *decoder, FILE *f, uint8_t block_u8[DATA_UNIT_SIZE][DATA_UNIT_SIZE], int dc_table_id,
                          int ac_table_id, int component_id) {
  HuffmanTable *dc_table = &decoder->h_tables[0][dc_table_id];
  HuffmanTable *ac_table = &decoder->h_tables[1][ac_table_id];
  uint16_t *q_table = decoder->q_tables[decoder->components[component_id].q_table_id];

  // NOTE: block can be negative
  // NOTE: dequantized value can be out-of-range
  int32_t block[DATA_UNIT_SIZE * DATA_UNIT_SIZE] = {0};
  double block_f64[DATA_UNIT_SIZE][DATA_UNIT_SIZE];

  // decode DC: F.2.2.1
  uint16_t n_bits, value;
  assert(decode(f, dc_table, &n_bits, decoder) == 0, "Error");
  if (decoder->is_restart)
    return 0;
  assert(receive(f, n_bits, &value, decoder) == 0, "Error");
  if (decoder->is_restart)
    return 0;
  int32_t diff = extend(value, n_bits);

  decoder->dc_preds[component_id] += diff;
  block[0] = decoder->dc_preds[component_id] * q_table[0];

  // decode AC: F.2.2.2
  for (int k = 1; k < DATA_UNIT_SIZE * DATA_UNIT_SIZE;) {
    uint16_t rs;
    assert(decode(f, ac_table, &rs, decoder) == 0, "Error");
    if (decoder->is_restart)
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
      assert(receive(f, ssss, &value, decoder) == 0, "Error");
      if (decoder->is_restart)
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

void idct_1d(double *x, double *out, size_t offset, size_t stride) {
  for (int k = 0; k < DATA_UNIT_SIZE; k++) {
    double result = x[offset] * 0.3535533905932738; // 1/sqrt(8)
    for (int n = 1; n < DATA_UNIT_SIZE; n++)
      result += x[offset + n * stride] * DCT_TABLE[((2 * k + 1) * n) % 32];
    out[offset + k * stride] = result;
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
