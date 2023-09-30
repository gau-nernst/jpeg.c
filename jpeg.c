#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE 8

#define try_malloc(ptr, size)                                                                                          \
  if ((ptr = malloc(size)) == NULL) {                                                                                  \
    printf("Failed to allocate memory");                                                                               \
    return 1;                                                                                                          \
  }

#define try_fread(ptr, size, n_items, stream)                                                                          \
  if (fread(ptr, size, n_items, stream) < (size * n_items)) {                                                          \
    printf("Failed to read data. Perhaps EOF?\n");                                                                     \
    return 1;                                                                                                          \
  }

#define print_q_table(table)                                                                                           \
  for (int i = 0; i < BLOCK_SIZE; i++) {                                                                               \
    printf(" ");                                                                                                       \
    for (int j = 0; j < BLOCK_SIZE; j++)                                                                               \
      printf(" %3d", table[ZIG_ZAG[i][j]]);                                                                            \
    printf("\n");                                                                                                      \
  }

// https://stackoverflow.com/a/2745086
#define ceil_div(x, y) (((x)-1) / (y) + 1)

// ITU-T.81, Table B.1
#define TEM 0x01

#define SOF0 0xC0
#define SOF1 0xC1
#define SOF2 0xC2
#define SOF3 0xC3

#define DHT 0xC4

#define SOF5 0xC5
#define SOF6 0xC6
#define SOF7 0xC7

#define JPG 0xC8
#define SOF9 0xC9
#define SOF10 0xCA
#define SOF11 0xCB

#define DAC 0xCC

#define SOF13 0xC
#define SOF14 0xC
#define SOF15 0xC

#define RST0 0xD0

#define SOI 0xD8
#define EOI 0xD9
#define SOS 0xDA
#define DQT 0xDB
#define DNL 0xDC
#define DRI 0xDD
#define DHP 0xDE
#define EXP 0xDF

#define APP0 0xE0
#define APP1 0xE1

#define COM 0xFE

struct QuantizationTable {
  uint8_t precision;
  void *data;
};

struct HuffmanTable {
  uint8_t code_lengths[16];
  void *data;
};

struct JPEGState {
  uint8_t encoding;
  uint16_t width;
  uint16_t height;
  uint8_t n_components;
  struct QuantizationTable q_tables[4];
  struct HuffmanTable h_tables[4];
  void *image_buffer;
};

uint16_t read_be_16(const uint8_t *buffer) { return (buffer[0] << 8) | buffer[1]; }
uint8_t upper_half(uint8_t x) { return x >> 4; }
uint8_t lower_half(uint8_t x) { return x & 0xF; }

int decode_jpeg(FILE *);
int handle_app0(const uint8_t *);
int handle_app1(const uint8_t *);
int handle_dqt(const uint8_t *, struct JPEGState *);
int handle_dht(const uint8_t *, struct JPEGState *);
int handle_sof0(const uint8_t *, struct JPEGState *);
int handle_sos(const uint8_t *, struct JPEGState *, FILE *);

// clang-format off
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

int main(int argc, char *argv[]) {
  if (argc == 1) {
    printf("No input\n");
    return 1;
  }

  FILE *f = fopen(argv[1], "rb");
  if (f == NULL) {
    printf("Failed to open %s\n", argv[1]);
    return 1;
  }
  return decode_jpeg(f);
}

int decode_jpeg(FILE *f) {
  struct JPEGState jpeg_state;
  uint8_t marker[2];
  uint16_t length;
  uint8_t *payload = NULL;

  for (;;) {
    try_fread(marker, 1, 2, f);
    printf("%X%X ", marker[0], marker[1]);

    if (marker[0] != 0xFF) {
      printf("Not a marker\n");
      return 1;
    }

    if (marker[1] == TEM | marker[1] == SOI | marker[1] == EOI | (marker[1] >= RST0 & marker[1] < RST0 + 8)) {
      length = 0;
    } else {
      try_fread(&length, 1, 2, f);
      length = read_be_16((const uint8_t *)&length);
    }
    if (length) {
      // TODO: re-use payload buffer
      // max buffer size?
      try_malloc(payload, length - 2);
      try_fread(payload, 1, length - 2, f);
    }

    switch (marker[1]) {
    case SOI:
      printf("SOI");
      break;

    case APP0:
      printf("APP0 (length = %d)\n", length);
      if (handle_app0(payload))
        return 1;
      break;

    case APP1:
      printf("APP1 (length = %d)\n", length);
      if (handle_app1(payload))
        return 1;
      break;

    case DQT:
      printf("DQT (length = %d)\n", length);
      if (handle_dqt(payload, &jpeg_state))
        return 1;
      break;

    case DHT:
      printf("DHT (length = %d)\n", length);
      if (handle_dht(payload, &jpeg_state))
        return 1;
      break;

    case SOF0:
      printf("SOF0 (length = %d)\n", length);
      if (handle_sof0(payload, &jpeg_state))
        return 1;
      break;

    case SOS:
      printf("SOS\n");
      if (handle_sos(payload, &jpeg_state, f))
        return 1;
      break;

    default:
      printf("Unknown marker (length = %d)\n", length);
      break;
    }

    if (payload)
      free(payload);
    printf("\n");
  }

  return 0;
}

// JFIF i.e. JPEG Part 5
int handle_app0(const uint8_t *payload) {
  printf("  identifier = %.5s\n", payload); // either JFIF or JFXX

  if (strcmp((const char *)payload, "JFIF") == 0) {
    printf("  version = %d.%d\n", payload[5], payload[6]);
    printf("  units = %d\n", payload[7]);
    printf("  density = (%d, %d)\n", read_be_16(payload + 8), read_be_16(payload + 10));
    printf("  thumbnail = (%d, %d)\n", payload[12], payload[13]);
  } else if (strcmp((const char *)payload, "JFXX") == 0) {
    printf("  extension_code = %X\n", payload[5]);
  } else
    printf("  Invalid identifier\n");

  return 0;
}

int handle_app1(const uint8_t *payload) {
  printf("  identifier = %s\n", payload);

  if (strcmp((const char *)payload, "Exif") == 0) {
    printf("  Exif detected\n");
  } else
    printf("  Invalid identifier\n");

  return 0;
}

int handle_dqt(const uint8_t *payload, struct JPEGState *jpeg_state) {
  uint8_t precision = upper_half(payload[0]);
  uint8_t identifier = lower_half(payload[0]);

  printf("  precision = %d (%d-bit)\n", precision, (precision + 1) * 8);
  printf("  identifier = %d\n", identifier);

  struct QuantizationTable *q_table = &(jpeg_state->q_tables[identifier]);
  q_table->precision = precision;
  try_malloc(q_table->data, (precision + 1) * 64);

  if (precision) { // 16-bit
    uint16_t *data = q_table->data;
    for (int i = 0; i < 64; i++)
      data[i] = read_be_16(payload + 1 + i * 2);
    print_q_table(data);
  } else {
    uint8_t *data = q_table->data;
    for (int i = 0; i < 64; i++)
      data[i] = payload[1 + i];
    print_q_table(data);
  }

  return 0;
}

int handle_dht(const uint8_t *payload, struct JPEGState *jpeg_state) {
  uint8_t class = upper_half(payload[0]);
  uint8_t identifier = lower_half(payload[0]);

  printf("  class = %d (%s)\n", class, class ? "AC" : "DC");
  printf("  identifier = %d\n", identifier);

  uint8_t *lengths = jpeg_state->h_tables[identifier].code_lengths;

  int n_codes = 0;
  for (int i = 0; i < 16; i++)
    n_codes += (lengths[i] = payload[1 + i]);

  uint8_t offset = 17;
  for (int i = 0; i < 16; i++) {
    if (lengths[i] == 0)
      continue;

    printf("  code length %d:", i);
    for (int j = 0; j < lengths[i]; j++)
      printf(" %d", payload[offset++]);
    printf("\n");
  }

  return 0;
}

int handle_sof0(const uint8_t *payload, struct JPEGState *jpeg_state) {
  jpeg_state->encoding = 0;

  // Table B.2
  uint8_t precision = payload[0];
  jpeg_state->height = read_be_16(payload + 1);
  jpeg_state->width = read_be_16(payload + 3);
  jpeg_state->n_components = payload[5];

  try_malloc(jpeg_state->image_buffer,
             precision / 8 * jpeg_state->height * jpeg_state->width * jpeg_state->n_components);

  printf("  encoding = Baseline DCT\n");
  printf("  precision = %d-bit\n", precision);
  printf("  image dimension = (%d, %d)\n", jpeg_state->width, jpeg_state->height);

  for (int i = 0; i < jpeg_state->n_components; i++) {
    uint8_t component = payload[6 + i * 3];
    uint8_t x_sampling_factor = upper_half(payload[7 + i * 3]);
    uint8_t y_sampling_factor = lower_half(payload[7 + i * 3]);
    uint8_t q_table_identifier = payload[8 + i * 3];

    printf("  component %d\n", component);
    printf("    sampling_factor = (%d, %d)\n", x_sampling_factor, y_sampling_factor);
    printf("    q_table_identifier = %d\n", q_table_identifier);
  }

  return 0;
}

int handle_sos(const uint8_t *payload, struct JPEGState *jpeg_state, FILE *f) {
  uint8_t n_components = payload[0];
  printf("  n_components in scan = %d\n", n_components);

  for (int i = 0; i < n_components; i++) {
    uint8_t component = payload[1 + i * 2];
    uint8_t dc_coding_table = upper_half(payload[2 + i * 2]);
    uint8_t ac_coding_table = lower_half(payload[2 + i * 2]);

    printf("  component %d\n", component);
    printf("    DC coding table = %d\n", dc_coding_table);
    printf("    AC coding table = %d\n", ac_coding_table);
  }

  // not used by Baseline DCT
  uint8_t ss = payload[1 + n_components * 2];
  uint8_t se = payload[2 + n_components * 2];
  uint8_t ah = payload[3 + n_components * 2];
  uint8_t al = payload[4 + n_components * 2];

  printf("  ss = %d\n", ss);
  printf("  se = %d\n", se);
  printf("  ah = %d\n", ah);
  printf("  al = %d\n", al);

  // decode scan
  // don't support 16-bit image for now
  uint8_t pred = 0;
  uint8_t decode;
  uint8_t diff;

  if (n_components == 1) {
    printf("Decode 1-component scan is not implemented\n");
    return 1;
  }

  // assume 4:2:0 chroma-subsampling
  uint16_t n_mcus = ceil_div(jpeg_state->width, BLOCK_SIZE) * ceil_div(jpeg_state->height, BLOCK_SIZE) * 3 / 2;

  // refer to T.81 Table A.2 for MCU packing order
  for (int i = 0; i < n_mcus; i++) {
    // decode DC
    try_fread(&decode, 1, 1, f);

    // decode AC
  }

  return 0;
}
