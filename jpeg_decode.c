#include "jpeg_decode.h"
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int DEBUG_PRINT = 0;

void jpeg_enable_debug_print() { DEBUG_PRINT = 1; }
void jpeg_disable_debug_print() { DEBUG_PRINT = 0; }

#define PRINT(...)                                                                                                     \
  if (DEBUG_PRINT)                                                                                                     \
    printf(__VA_ARGS__);

#define BLOCK_SIZE 8
#define MAX_HUFFMAN_CODE_LENGTH 16
#define MAX_COMPONENTS 3

#define ASSERT(condition, ...)                                                                                         \
  if (!(condition)) {                                                                                                  \
    fprintf(stderr, "Line %d: ", __LINE__);                                                                            \
    fprintf(stderr, __VA_ARGS__);                                                                                      \
    fprintf(stderr, "\n");                                                                                             \
    raise(SIGABRT);                                                                                                    \
  }

#define _MALLOC(ptr, size)                                                                                             \
  ptr = malloc(size);                                                                                                  \
  ASSERT(ptr != NULL, "Failed to allocate memory");

#define _FREAD(buffer, size, count, stream)                                                                            \
  {                                                                                                                    \
    size_t n_elems = fread(buffer, size, count, stream);                                                               \
    ASSERT(n_elems == count, "Failed to read data. Perhaps EOF?");                                                     \
  }

#define _FREE(ptr)                                                                                                     \
  if (ptr != NULL) {                                                                                                   \
    free(ptr);                                                                                                         \
    ptr = NULL;                                                                                                        \
  }

#define PRINT_LIST(prefix, ptr, length, fmt)                                                                           \
  {                                                                                                                    \
    PRINT(prefix);                                                                                                     \
    for (int _i = 0; _i < (length); _i++)                                                                              \
      PRINT(fmt, (ptr)[_i]);                                                                                           \
    PRINT("\n");                                                                                                       \
  }

#define CDIV(x, y) (((x) + (y)-1) / (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define CLAMP(x, lo, hi) MIN(MAX(x, lo), hi)

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
  SOF13 = 0xCD,
  SOF14 = 0xCE,
  SOF15 = 0xCF,

  RST0 = 0xD0,
  RST1 = 0xD1,
  RST2 = 0xD2,
  RST3 = 0xD3,
  RST4 = 0xD4,
  RST5 = 0xD5,
  RST6 = 0xD6,
  RST7 = 0xD7,

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
  uint16_t q_tables[4][BLOCK_SIZE * BLOCK_SIZE];
  HuffmanTable h_tables[2][4];
  Component components[MAX_COMPONENTS];
  int min_component;
  int max_x_sampling;
  int max_y_sampling;
  int dc_preds[MAX_COMPONENTS];
  uint8_t *image;
  int width;
  int height;
  int n_channels;
} Decoder;

static uint16_t read_be_16(const uint8_t *buffer) { return (buffer[0] << 8) | buffer[1]; }
static uint8_t upper_half(uint8_t x) { return x >> 4; }
static uint8_t lower_half(uint8_t x) { return x & 0xF; }

static void handle_app0(const uint8_t *buffer, uint16_t buflen);
static void handle_dqt(Decoder *decoder, const uint8_t *buffer, uint16_t buflen);
static void handle_dht(Decoder *decoder, const uint8_t *buffer, uint16_t buflen);
static void handle_sof0(Decoder *decoder, const uint8_t *buffer, uint16_t buflen);
static void handle_sos(Decoder *decoder, const uint8_t *buffer, uint16_t buflen, FILE *f);

static void decode_block_sof0(Decoder *decoder, FILE *f, uint8_t block[BLOCK_SIZE][BLOCK_SIZE], int, int, int);

static void idct_2d_(double *);
static void ycbcr_to_rgb_(uint8_t *);

static jmp_buf RST_JMP_BUF;

// ITU T.81 Figure A.6
static const uint8_t ZIG_ZAG[BLOCK_SIZE][BLOCK_SIZE] = {
    { 0,  1,  5,  6, 14, 15, 27, 28}, //
    { 2,  4,  7, 13, 16, 26, 29, 42}, //
    { 3,  8, 12, 17, 25, 30, 41, 43}, //
    { 9, 11, 18, 24, 31, 40, 44, 53}, //
    {10, 19, 23, 32, 39, 45, 52, 54}, //
    {20, 22, 33, 38, 46, 51, 55, 60}, //
    {21, 34, 37, 47, 50, 56, 59, 61}, //
    {35, 36, 48, 49, 57, 58, 62, 63},
};

static const double DCT_TABLE[] = {
    0.5000000000000000,  0.4903926402016152,  0.4619397662556434,  0.4157348061512726,  //
    0.3535533905932738,  0.2777851165098011,  0.1913417161825449,  0.0975451610080642,  //
    0.0000000000000000,  -0.0975451610080641, -0.1913417161825449, -0.2777851165098010, //
    -0.3535533905932737, -0.4157348061512727, -0.4619397662556434, -0.4903926402016152, //
    -0.5000000000000000, -0.4903926402016152, -0.4619397662556434, -0.4157348061512726, //
    -0.3535533905932738, -0.2777851165098011, -0.1913417161825449, -0.0975451610080642, //
    -0.0000000000000000, 0.0975451610080641,  0.1913417161825449,  0.2777851165098010,  //
    0.3535533905932737,  0.4157348061512727,  0.4619397662556434,  0.4903926402016152,  //
};

uint8_t *decode_jpeg(FILE *f, int *width, int *height, int *n_channels) {
  uint8_t marker[2];
  uint16_t buflen;
  uint8_t *buffer = NULL;
  Decoder decoder = {0};

  bool finished = false;
  while (!finished) {
    _FREAD(marker, 1, 2, f);
    PRINT("%X%X ", marker[0], marker[1]);

    ASSERT(marker[0] == 0xFF, "Not a marker");

    if (marker[1] == TEM || marker[1] == SOI || marker[1] == EOI || (marker[1] >= RST0 && marker[1] <= RST7)) {
      buflen = 0;
    } else {
      _FREAD(&buflen, 1, 2, f);
      buflen = read_be_16((uint8_t *)&buflen) - 2;

      // TODO: re-use payload buffer
      _MALLOC(buffer, buflen);
      _FREAD(buffer, 1, buflen, f);
    }

    switch (marker[1]) {
    case SOI:
      PRINT("SOI");
      break;

    case APP0:
      handle_app0(buffer, buflen);
      break;

    case DQT:
      handle_dqt(&decoder, buffer, buflen);
      break;

    case DHT:
      handle_dht(&decoder, buffer, buflen);
      break;

    case SOF0:
      handle_sof0(&decoder, buffer, buflen);
      break;

    case SOS:
      handle_sos(&decoder, buffer, buflen, f);
      break;

    case DRI:
      PRINT("DRI (length = %d)\n", buflen);
      ASSERT(buflen >= 2, "Payload not long enough");
      decoder.restart_interval = read_be_16(buffer);
      PRINT("  restart interval = %d\n", decoder.restart_interval);
      break;

    case EOI:
      PRINT("EOI\n");
      finished = true;
      break;

    default:
      if ((APP0 < marker[1]) && (marker[1] <= APP0 + 15)) {
        PRINT("APP%d (length = %d)\n", marker[1] - APP0, buflen);
        PRINT("  identifier = %.*s\n", buflen, buffer);
      } else
        PRINT("Unknown marker (length = %d)\n", buflen);
      break;
    }

    _FREE(buffer);
    PRINT("\n");
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
  PRINT("APP0 (length = %d)\n", buflen);
  PRINT("  identifier = %.5s\n", buffer); // either JFIF or JFXX

  if (strcmp((const char *)buffer, "JFIF") == 0) {
    ASSERT(buflen >= 14, "Payload is too short");
    PRINT("  version = %d.%d\n", buffer[5], buffer[6]);
    PRINT("  units = %d\n", buffer[7]);
    PRINT("  density = (%d, %d)\n", read_be_16(buffer + 8), read_be_16(buffer + 10));
    PRINT("  thumbnail = (%d, %d)\n", buffer[12], buffer[13]);
  } else if (strcmp((const char *)buffer, "JFXX") == 0) {
    PRINT("  extension_code = %X\n", buffer[5]);
  } else
    PRINT("  Invalid identifier\n");
}

// ITU-T.81 B.2.4.1
// there can be multiple quantization tables within 1 DQT segment
void handle_dqt(Decoder *decoder, const uint8_t *buffer, uint16_t buflen) {
  PRINT("DQT (length = %d)\n", buflen);

  int offset = 0;
  while (offset < buflen) {
    uint8_t precision = upper_half(buffer[offset]);
    uint8_t identifier = lower_half(buffer[offset]);
    int table_size = 1 + BLOCK_SIZE * BLOCK_SIZE * (precision + 1);

    PRINT("  precision = %d (%d-bit), identifier = %d\n", precision, (precision + 1) * 8, identifier);
    ASSERT(buflen >= offset + table_size, "Payload is too short");

    uint16_t *q_table = decoder->q_tables[identifier];
    if (precision) {
      for (int i = 0; i < BLOCK_SIZE * BLOCK_SIZE; i++)
        q_table[i] = read_be_16(buffer + offset + 1 + i * 2);
    } else {
      for (int i = 0; i < BLOCK_SIZE * BLOCK_SIZE; i++)
        q_table[i] = buffer[offset + 1 + i];
    }

    for (int i = 0; i < BLOCK_SIZE; i++) {
      PRINT("  ");
      for (int j = 0; j < BLOCK_SIZE; j++)
        PRINT(" %3d", q_table[ZIG_ZAG[i][j]]);
      PRINT("\n");
    }

    offset += table_size;
  }
}

// ITU-T.81 B.2.4.2
// there can be multiple huffman tables within 1 DHT segment
void handle_dht(Decoder *decoder, const uint8_t *buffer, uint16_t buflen) {
  PRINT("DHT (length = %d)\n", buflen);

  int offset = 0;
  while (offset < buflen) {
    uint8_t class = upper_half(buffer[offset]);
    uint8_t identifier = lower_half(buffer[offset]);
    PRINT("  class = %d (%s), identifier = %d\n", class, class ? "AC" : "DC", identifier);
    ASSERT(buflen >= offset + 1 + MAX_HUFFMAN_CODE_LENGTH, "Payload is too short");

    // ITU-T.81 Annex C: create Huffman table
    HuffmanTable *h_table = &decoder->h_tables[class][identifier];
    int n_codes = 0;
    for (int i = 0; i < MAX_HUFFMAN_CODE_LENGTH; i++)
      n_codes += buffer[offset + 1 + i];
    int table_size = 1 + MAX_HUFFMAN_CODE_LENGTH + n_codes;
    ASSERT(buflen >= offset + table_size, "Payload is too short");

    _MALLOC(h_table->huffsize, n_codes * sizeof(*h_table->huffsize));
    _MALLOC(h_table->huffcode, n_codes * sizeof(*h_table->huffcode));
    _MALLOC(h_table->huffval, n_codes * sizeof(*h_table->huffval));

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

    PRINT("  n_codes = %d\n", n_codes);
    PRINT_LIST("  BITS     =", buffer + offset + 1, MAX_HUFFMAN_CODE_LENGTH, " %3d");
    PRINT_LIST("  HUFFSIZE =", h_table->huffsize, n_codes, " %3d");
    PRINT_LIST("  HUFFCODE =", h_table->huffcode, n_codes, " %3d");
    PRINT_LIST("  HUFFVAL  =", h_table->huffval, n_codes, " %3d");
    PRINT("\n");
    PRINT_LIST("  MINCODE  =", h_table->mincode, MAX_HUFFMAN_CODE_LENGTH, " %3d");
    PRINT_LIST("  MAXCODE  =", h_table->maxcode, MAX_HUFFMAN_CODE_LENGTH, " %3d");
    PRINT_LIST("  VALPTR   =", h_table->valptr, MAX_HUFFMAN_CODE_LENGTH, " %3d");
    PRINT("\n");

    offset += table_size;
  }
}

void handle_sof0(Decoder *decoder, const uint8_t *buffer, uint16_t buflen) {
  PRINT("SOF0 (length = %d)\n", buflen);

  decoder->encoding = SOF0;

  // Table B.2
  ASSERT(buflen >= 6, "Payload is too short");
  uint8_t precision = buffer[0];
  decoder->height = read_be_16(buffer + 1);
  decoder->width = read_be_16(buffer + 3);
  decoder->n_channels = buffer[5];

  PRINT("  encoding = Baseline DCT\n");
  PRINT("  precision = %d-bit\n", precision);
  PRINT("  image dimension = (%d, %d)\n", decoder->width, decoder->height);

  ASSERT(precision == 8, "Only 8-bit image is supported");
  ASSERT((buffer[5] == 1) || (buffer[5] == 3), "Only 1 or 3 channels are supported");
  ASSERT(buflen >= 6 + decoder->n_channels * 3, "Payload is too short");
  _MALLOC(decoder->image, decoder->height * decoder->width * decoder->n_channels);

  // we need to do this since component_id is not consistent. it can be 1, 2, 3 or 0, 1, 2
  decoder->min_component = buffer[6];
  for (int i = 1; i < decoder->n_channels; i++)
    decoder->min_component = MIN(decoder->min_component, buffer[6 + i * 3]);

  decoder->max_x_sampling = 0;
  decoder->max_y_sampling = 0;
  for (int i = 0; i < decoder->n_channels; i++) {
    uint8_t component_id = buffer[6 + i * 3];
    Component *component = &decoder->components[component_id - decoder->min_component];
    component->x_sampling = upper_half(buffer[7 + i * 3]);
    component->y_sampling = lower_half(buffer[7 + i * 3]);
    component->q_table_id = buffer[8 + i * 3];

    decoder->max_x_sampling = MAX(decoder->max_x_sampling, component->x_sampling);
    decoder->max_y_sampling = MAX(decoder->max_y_sampling, component->y_sampling);

    PRINT("  component %d: sampling_factor = (%d, %d) q_table_id = %d\n", component_id, component->x_sampling,
          component->y_sampling, component->q_table_id);
  }
}

void handle_sos(Decoder *decoder, const uint8_t *payload, uint16_t length, FILE *f) {
  PRINT("SOS\n");

  ASSERT(decoder->encoding == SOF0, "Only Baseline JPEG is support");

  uint8_t n_components = payload[0];
  PRINT("  n_components in scan = %d\n", n_components);
  ASSERT(n_components <= decoder->n_channels, "Scan contains more channels than declared in SOF");

  for (int i = 0; i < n_components; i++) {
    PRINT("  component %d: DC coding table = %d  AC coding table = %d\n", payload[1 + i * 2],
          upper_half(payload[2 + i * 2]), lower_half(payload[2 + i * 2]));
    ASSERT(payload[1 + i * 2] - decoder->min_component < decoder->n_channels, "Encounter invalid component_id");
  }

  // not used by Baseline DCT
  PRINT("  ss = %d\n", payload[1 + n_components * 2]);
  PRINT("  se = %d\n", payload[2 + n_components * 2]);
  PRINT("  ah = %d\n", upper_half(payload[3 + n_components * 2]));
  PRINT("  al = %d\n", lower_half(payload[3 + n_components * 2]));

  if (n_components == 1) {
    // Non-interleaved order. A.2.2
    int component_id = payload[1] - decoder->min_component;
    ASSERT(component_id < decoder->n_channels, "Encounter invalid component_id");
    int dc_table_id = upper_half(payload[2]);
    int ac_table_id = lower_half(payload[2]);

    decoder->dc_preds[component_id] = 0;

    // TODO: take into account sampling factor
    int nx_blocks = CDIV(decoder->width, BLOCK_SIZE);
    int ny_blocks = CDIV(decoder->height, BLOCK_SIZE);

    int interval_idx = 0;

    for (int mcu_idx = 0; mcu_idx < ny_blocks * nx_blocks;) {
      uint8_t block_u8[BLOCK_SIZE][BLOCK_SIZE];
      if (!setjmp(RST_JMP_BUF)) {
        // normal flow. decode_block may encounter RSTx marker
        decode_block_sof0(decoder, f, block_u8, dc_table_id, ac_table_id, component_id);

        // place mcu to image buffer
        int mcu_y = mcu_idx / nx_blocks;
        int mcu_x = mcu_idx % nx_blocks;
        for (int j = 0; j < MIN(BLOCK_SIZE, decoder->height - mcu_y * BLOCK_SIZE); j++) {
          int row_idx = mcu_y * BLOCK_SIZE + j;
          for (int i = 0; i < MIN(BLOCK_SIZE, decoder->width - mcu_x * BLOCK_SIZE); i++) {
            int col_idx = mcu_x * BLOCK_SIZE + i;
            decoder->image[(row_idx * decoder->width + col_idx) * decoder->n_channels + component_id] = block_u8[j][i];
          }
        }
        mcu_idx++;
      } else {
        // encounter restart interval (E.2.4)
        // ignore current MCU, reset decoder state, move to the next interval
        // NOTE: we don't check for consecutive restart markers
        decoder->dc_preds[component_id] = 0;
        mcu_idx = (++interval_idx) * decoder->restart_interval;
      }
    }
    return;
  }

  // Interleaved order. A.2.3
  // calculate number of MCUs based on chroma-subsampling
  // TODO: handle restart markers for 3-channel
  int mcu_width = BLOCK_SIZE * decoder->max_x_sampling;
  int mcu_height = BLOCK_SIZE * decoder->max_y_sampling;
  int nx_mcu = CDIV(decoder->width, mcu_width);
  int ny_mcu = CDIV(decoder->height, mcu_height);

  for (int i = 0; i < MAX_COMPONENTS; i++)
    decoder->dc_preds[i] = 0;
  uint8_t *mcu;
  _MALLOC(mcu, mcu_width * mcu_height * n_components);

  for (int mcu_y = 0; mcu_y < ny_mcu; mcu_y++)
    for (int mcu_x = 0; mcu_x < nx_mcu; mcu_x++) {
      for (int c = 0; c < n_components; c++) {
        int component_id = payload[1 + c * 2] - decoder->min_component;
        int dc_table_id = upper_half(payload[2 + c * 2]);
        int ac_table_id = lower_half(payload[2 + c * 2]);
        Component *component = &decoder->components[component_id];

        for (int y = 0; y < component->y_sampling; y++)
          for (int x = 0; x < component->x_sampling; x++) {
            uint8_t block_u8[BLOCK_SIZE][BLOCK_SIZE];
            decode_block_sof0(decoder, f, block_u8, dc_table_id, ac_table_id, component_id);

            // place to mcu. A.2.3 and JFIF p.4
            // NOTE: assume order in the scan is YCbCr
            // TODO: use better upsampling algorithm e.g. bilinear
            int n_repeat_y = decoder->max_y_sampling / component->y_sampling;
            int n_repeat_x = decoder->max_x_sampling / component->x_sampling;
            for (int j = 0; j < BLOCK_SIZE * n_repeat_y; j++) {
              int row_idx = y * BLOCK_SIZE + j;
              for (int i = 0; i < BLOCK_SIZE * n_repeat_x; i++) {
                int col_idx = x * BLOCK_SIZE + i;
                mcu[(row_idx * mcu_width + col_idx) * n_components + c] = block_u8[j / n_repeat_y][i / n_repeat_x];
              }
            }
          }
      }

      for (int j = 0; j < MIN(mcu_height, decoder->height - mcu_y * mcu_height); j++) {
        int row_idx = mcu_y * mcu_height + j;
        for (int i = 0; i < MIN(mcu_width, decoder->width - mcu_x * mcu_width); i++) {
          int col_idx = mcu_x * mcu_width + i;
          ycbcr_to_rgb_(mcu + (j * mcu_width + i) * n_components);
          for (int c = 0; c < n_components; c++) {
            int component_id = payload[1 + c * 2] - decoder->min_component;
            decoder->image[(row_idx * decoder->width + col_idx) * decoder->n_channels + component_id] =
                mcu[(j * mcu_width + i) * n_components + c];
          }
        }
      }
    }
}

// Figure F.12
int32_t extend(uint16_t value, uint16_t n_bits) {
  return value < (1 << (n_bits - 1)) ? value + (-1 << n_bits) + 1 : value;
}

// Figure F.18
uint8_t nextbit(FILE *f) {
  // impure function
  static uint8_t CNT = 0, B;

  if (CNT == 0) {
    _FREAD(&B, 1, 1, f);
    CNT = 8;

    // potential marker. need to read next byte
    // if next byte is 0x00, ignore this byte (byte stuffing: ITU T.81 F.1.2.3)
    if (B == 0xFF) {
      uint8_t B2;
      _FREAD(&B2, 1, 1, f);

      if (B2 != 0) {
        if ((RST0 <= B2) && (B2 <= RST7)) {
          PRINT("Encounter RST%d marker\n", B2 - RST0);
          CNT = 0;
          longjmp(RST_JMP_BUF, 1);
        } else if (B2 == DNL) {
          ASSERT(false, "DNL marker. Not implemented");
        } else {
          ASSERT(false, "Found marker %X in scan. Decode error?", B2);
        }
      }
    }
  }

  uint8_t BIT = B >> 7;
  CNT--;
  B = B << 1;
  return BIT;
}

// Figure F.17
uint16_t receive(FILE *f, uint16_t n_bits) {
  uint16_t V = 0;
  for (int i = 0; i < n_bits; i++)
    V = (V << 1) + nextbit(f);
  return V;
}

// Figure F.16
uint16_t decode(FILE *f, HuffmanTable *h_table) {
  int i = 0;
  uint16_t CODE = nextbit(f);

  for (; CODE > h_table->maxcode[i]; i++)
    CODE = (CODE << 1) + nextbit(f);

  return h_table->huffval[h_table->valptr[i] + CODE - h_table->mincode[i]];
}

void decode_block_sof0(Decoder *decoder, FILE *f, uint8_t block_u8[BLOCK_SIZE][BLOCK_SIZE], int dc_table_id,
                       int ac_table_id, int component_id) {
  HuffmanTable *dc_table = &decoder->h_tables[0][dc_table_id];
  HuffmanTable *ac_table = &decoder->h_tables[1][ac_table_id];
  uint16_t *q_table = decoder->q_tables[decoder->components[component_id].q_table_id];

  // NOTE: block can be negative, dequantized value can be out-of-range
  int16_t block[BLOCK_SIZE * BLOCK_SIZE] = {0};
  double block_f64[BLOCK_SIZE][BLOCK_SIZE];

  // decode DC: F.2.2.1
  uint16_t n_bits = decode(f, dc_table);
  uint16_t value = receive(f, n_bits);
  int32_t diff = extend(value, n_bits);

  decoder->dc_preds[component_id] += diff;
  block[0] = decoder->dc_preds[component_id] * q_table[0];

  // decode AC: F.2.2.2
  for (int k = 1; k < BLOCK_SIZE * BLOCK_SIZE;) {
    uint16_t rs = decode(f, ac_table);
    if (rs == ZRL)
      k += 16;
    else if (rs == EOB)
      break;
    else {
      int rrrr = upper_half(rs);
      int ssss = lower_half(rs);
      k += rrrr;
      ASSERT(k < BLOCK_SIZE * BLOCK_SIZE, "Encounter invalid code");
      value = receive(f, ssss);
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
      block_u8[i][j] = CLAMP(round(block_f64[i][j]) + 128, 0, 255);
}

void idct_1d(double *x, double *out, size_t offset, size_t stride) {
  for (int k = 0; k < BLOCK_SIZE; k++) {
    double result = x[offset] * 0.3535533905932738; // 1/sqrt(8)
    for (int n = 1; n < BLOCK_SIZE; n++)
      result += x[offset + n * stride] * DCT_TABLE[((2 * k + 1) * n) % 32];
    out[offset + k * stride] = result;
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
  float r = x[0]                           + 1.402f   * (x[2] - 128);
  float g = x[0] - 0.34414f * (x[1] - 128) - 0.71414f * (x[2] - 128);
  float b = x[0] + 1.772f   * (x[1] - 128);
  // clang-format on
  x[0] = CLAMP(round(r), 0, 255);
  x[1] = CLAMP(round(g), 0, 255);
  x[2] = CLAMP(round(b), 0, 255);
}
