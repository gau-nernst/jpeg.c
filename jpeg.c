#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE 8
#define MAX_HUFFMAN_CODE_LENGTH 16

#define check(condition, msg)                                                                                          \
  if (condition) {                                                                                                     \
    printf(msg);                                                                                                       \
    return 1;                                                                                                          \
  }

#define try_malloc(ptr, size) check((ptr = malloc(size)) == NULL, "Failed to allocate memory\n")
#define try_fread(ptr, size, n_items, stream)                                                                          \
  check(fread(ptr, size, n_items, stream) < (size * n_items), "Failed to read data. Perhaps EOF?\n")

#define try_free(ptr)                                                                                                  \
  if (ptr == NULL)                                                                                                     \
    free(ptr);

#define print_list(prefix, ptr, length)                                                                                \
  printf(prefix);                                                                                                      \
  for (int i = 0; i < (length); i++)                                                                                   \
    printf(" %3d", (ptr)[i]);                                                                                          \
  printf("\n");

// https://stackoverflow.com/a/2745086
#define ceil_div(x, y) (((x)-1) / (y) + 1)
#define max(x, y) (((x) > (y)) ? (x) : (y))

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

struct HuffmanTable {
  uint8_t *huffsize;
  uint16_t *huffcode;
  uint8_t *huffval;
  uint16_t mincode[MAX_HUFFMAN_CODE_LENGTH];
  int32_t maxcode[MAX_HUFFMAN_CODE_LENGTH];
  uint8_t valptr[MAX_HUFFMAN_CODE_LENGTH];
};

struct Component {
  uint8_t x_sampling_factor;
  uint8_t y_sampling_factor;
  uint8_t q_table_id;
  uint8_t dc_coding_table_id;
  uint8_t ac_coding_table_id;
};

struct JPEGState {
  uint8_t encoding;
  uint16_t width;
  uint16_t height;
  uint8_t n_components;
  uint16_t q_tables[4][BLOCK_SIZE * BLOCK_SIZE];
  struct HuffmanTable h_tables[2][4];
  struct Component *components;
  void *image_buffer;
};

uint16_t read_be_16(const uint8_t *buffer) { return (buffer[0] << 8) | buffer[1]; }
uint8_t upper_half(uint8_t x) { return x >> 4; }
uint8_t lower_half(uint8_t x) { return x & 0xF; }

int decode_jpeg(FILE *);
int handle_app0(const uint8_t *, uint16_t);
int handle_app1(const uint8_t *, uint16_t);
int handle_dqt(const uint8_t *, uint16_t, struct JPEGState *);
int handle_dht(const uint8_t *, uint16_t, struct JPEGState *);
int handle_sof0(const uint8_t *, uint16_t, struct JPEGState *);
int handle_sos(const uint8_t *, uint16_t, struct JPEGState *, FILE *);

int16_t extend(uint16_t, uint16_t);
uint16_t decode(FILE *, struct HuffmanTable *);
uint16_t receive(FILE *, uint16_t);
uint8_t nextbit(FILE *);

void init_dct_matrix();
void idct_2d(uint16_t *, double *);

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
double DCT_MATRIX[BLOCK_SIZE][BLOCK_SIZE];

int main(int argc, char *argv[]) {
  check(argc == 1, "No input\n");

  FILE *f = fopen(argv[1], "rb");
  if (f == NULL) {
    printf("Failed to open %s\n", argv[1]);
    return 1;
  }
  init_dct_matrix();
  return decode_jpeg(f);
}

int decode_jpeg(FILE *f) {
  struct JPEGState jpeg_state;
  uint8_t marker[2];
  uint16_t length;
  uint8_t *payload = NULL;

  uint8_t finished = 0;
  while (finished == 0) {
    try_fread(marker, 1, 2, f);
    printf("%X%X ", marker[0], marker[1]);

    check(marker[0] != 0xFF, "Not a marker\n");

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
      printf("SOI");
      break;

    case APP0:
      printf("APP0 (length = %d)\n", length);
      if (handle_app0(payload, length))
        return 1;
      break;

    case APP1:
      printf("APP1 (length = %d)\n", length);
      if (handle_app1(payload, length))
        return 1;
      break;

    case DQT:
      printf("DQT (length = %d)\n", length);
      if (handle_dqt(payload, length, &jpeg_state))
        return 1;
      break;

    case DHT:
      printf("DHT (length = %d)\n", length);
      if (handle_dht(payload, length, &jpeg_state))
        return 1;
      break;

    case SOF0:
      printf("SOF0 (length = %d)\n", length);
      if (handle_sof0(payload, length, &jpeg_state))
        return 1;
      break;

    case SOS:
      printf("SOS\n");
      if (handle_sos(payload, length, &jpeg_state, f))
        return 1;
      break;

    case EOI:
      printf("EOI\n");
      finished = 1;
      break;

    default:
      printf("Unknown marker (length = %d)\n", length);
      break;
    }

    try_free(payload);
    printf("\n");
  }
  try_free(jpeg_state.image_buffer);
  try_free(jpeg_state.components);

  return 0;
}

// JFIF i.e. JPEG Part 5
int handle_app0(const uint8_t *payload, uint16_t length) {
  printf("  identifier = %.5s\n", payload); // either JFIF or JFXX

  if (strcmp((const char *)payload, "JFIF") == 0) {
    check(length < 14, "Payload is too short\n");
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

int handle_app1(const uint8_t *payload, uint16_t length) {
  printf("  identifier = %s\n", payload);

  if (strcmp((const char *)payload, "Exif") == 0) {
    printf("  Exif detected\n");
  } else
    printf("  Invalid identifier\n");

  return 0;
}

// ITU-T.81 B.2.4.1
// there can be multiple quantization tables within 1 DQT segment
int handle_dqt(const uint8_t *payload, uint16_t length, struct JPEGState *jpeg_state) {
  int offset = 0;
  while (offset < length) {
    uint8_t precision = upper_half(payload[offset]);
    uint8_t identifier = lower_half(payload[offset]);

    check(length < offset + 1 + BLOCK_SIZE * BLOCK_SIZE * (precision + 1), "Payload is too short\n");

    printf("  precision = %d (%d-bit), identifier = %d\n", precision, (precision + 1) * 8, identifier);

    uint16_t *q_table = jpeg_state->q_tables[identifier];
    if (precision) {
      for (int i = 0; i < BLOCK_SIZE * BLOCK_SIZE; i++)
        q_table[i] = read_be_16(payload + offset + 1 + i * 2);
    } else {
      for (int i = 0; i < BLOCK_SIZE * BLOCK_SIZE; i++)
        q_table[i] = payload[offset + 1 + i];
    }

    for (int i = 0; i < BLOCK_SIZE; i++) {
      printf("  ");
      for (int j = 0; j < BLOCK_SIZE; j++)
        printf(" %3d", q_table[ZIG_ZAG[i][j]]);
      printf("\n");
    }

    offset += 1 + BLOCK_SIZE * BLOCK_SIZE * (precision + 1);
  }
  return 0;
}

// ITU-T.81 B.2.4.2
// there can be multiple huffman tables within 1 DHT segment
int handle_dht(const uint8_t *payload, uint16_t length, struct JPEGState *jpeg_state) {
  int offset = 0;
  while (offset < length) {
    uint8_t class = upper_half(payload[offset]);
    uint8_t identifier = lower_half(payload[offset]);

    check(length < offset + 1 + MAX_HUFFMAN_CODE_LENGTH, "Payload is too short\n");

    printf("  class = %d (%s), identifier = %D\n", class, class ? "AC" : "DC", identifier);

    // ITU-T.81 Annex C: create Huffman table
    struct HuffmanTable *h_table = &(jpeg_state->h_tables[class][identifier]);
    int n_codes = 0;
    for (int i = 0; i < MAX_HUFFMAN_CODE_LENGTH; i++)
      n_codes += payload[offset + 1 + i];

    check(length < offset + 1 + MAX_HUFFMAN_CODE_LENGTH + n_codes, "Payload is too short\n");

    try_malloc(h_table->huffsize, n_codes);
    try_malloc(h_table->huffcode, n_codes * 2);
    try_malloc(h_table->huffval, n_codes);

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

    printf("  n_codes = %d\n", n_codes);
    print_list("  BITS     =", payload + offset + 1, MAX_HUFFMAN_CODE_LENGTH);
    print_list("  HUFFSIZE =", h_table->huffsize, n_codes);
    print_list("  HUFFCODE =", h_table->huffcode, n_codes);
    print_list("  HUFFVAL  =", h_table->huffval, n_codes);
    printf("\n");
    print_list("  MINCODE  =", h_table->mincode, MAX_HUFFMAN_CODE_LENGTH);
    print_list("  MAXCODE  =", h_table->maxcode, MAX_HUFFMAN_CODE_LENGTH);
    print_list("  VALPTR   =", h_table->valptr, MAX_HUFFMAN_CODE_LENGTH);
    printf("\n");

    offset += 1 + MAX_HUFFMAN_CODE_LENGTH + n_codes;
  }
  return 0;
}

int handle_sof0(const uint8_t *payload, uint16_t length, struct JPEGState *jpeg_state) {
  jpeg_state->encoding = 0;

  // Table B.2
  check(length < 6, "Payload is too short\n");
  uint8_t precision = payload[0];
  jpeg_state->height = read_be_16(payload + 1);
  jpeg_state->width = read_be_16(payload + 3);
  jpeg_state->n_components = payload[5];

  check(length < 6 + jpeg_state->n_components * 3, "Payload is too short\n");
  try_malloc(jpeg_state->components, sizeof(struct Component) * jpeg_state->n_components);
  try_malloc(jpeg_state->image_buffer,
             precision / 8 * jpeg_state->height * jpeg_state->width * jpeg_state->n_components);

  printf("  encoding = Baseline DCT\n");
  printf("  precision = %d-bit\n", precision);
  printf("  image dimension = (%d, %d)\n", jpeg_state->width, jpeg_state->height);

  for (int i = 0; i < jpeg_state->n_components; i++) {
    uint8_t component_id = payload[6 + i * 3]; // this should be i+1, according to JFIF
    struct Component *component = &jpeg_state->components[component_id - 1];
    component->x_sampling_factor = upper_half(payload[7 + i * 3]);
    component->y_sampling_factor = lower_half(payload[7 + i * 3]);
    component->q_table_id = payload[8 + i * 3];

    printf("  component %d\n", component_id);
    printf("    sampling_factor = (%d, %d)\n", component->x_sampling_factor, component->y_sampling_factor);
    printf("    q_table_identifier = %d\n", component->q_table_id);
  }

  return 0;
}

int handle_sos(const uint8_t *payload, uint16_t length, struct JPEGState *jpeg_state, FILE *f) {
  uint8_t n_components = payload[0];
  printf("  n_components in scan = %d\n", n_components);

  for (int i = 0; i < n_components; i++) {
    struct Component *component = &jpeg_state->components[payload[1 + i * 2] - 1];
    component->dc_coding_table_id = upper_half(payload[2 + i * 2]);
    component->ac_coding_table_id = lower_half(payload[2 + i * 2]);

    printf("  component %d\n", payload[1 + i * 2]);
    printf("    DC coding table = %d\n", component->dc_coding_table_id);
    printf("    AC coding table = %d\n", component->ac_coding_table_id);
  }

  // not used by Baseline DCT
  printf("  ss = %d\n", payload[1 + n_components * 2]);
  printf("  se = %d\n", payload[2 + n_components * 2]);
  printf("  ah = %d\n", payload[3 + n_components * 2]);
  printf("  al = %d\n", payload[4 + n_components * 2]);

  // decode scan
  if (n_components == 1) {
    printf("Decode 1-component scan is not implemented\n");
    return 1;
  }

  // calculate number of MCUs based on chroma-subsampling
  uint16_t max_x_sampling = 0, max_y_sampling = 0;
  for (int i = 0; i < jpeg_state->n_components; i++) {
    struct Component *component = jpeg_state->components + i;
    max_x_sampling = max(max_x_sampling, component->x_sampling_factor);
    max_y_sampling = max(max_y_sampling, component->y_sampling_factor);
  }

  uint16_t n_x_blocks = ceil_div(jpeg_state->width, BLOCK_SIZE);
  uint16_t n_y_blocks = ceil_div(jpeg_state->height, BLOCK_SIZE);

  uint16_t pred = 0;
  uint16_t block[BLOCK_SIZE * BLOCK_SIZE] = {0};
  uint16_t block2[BLOCK_SIZE][BLOCK_SIZE];
  double block_float[BLOCK_SIZE][BLOCK_SIZE];

  // refer to T.81 Table A.2 for MCU packing order
  // F.2.2
  for (int mcu_y = 0; mcu_y < n_y_blocks / max_y_sampling; mcu_y++)
    for (int mcu_x = 0; mcu_x < n_x_blocks / max_x_sampling; mcu_x++) {
      for (int c = 0; c < n_components; c++) {
        struct Component *component = &jpeg_state->components[payload[1 + c * 2] - 1];
        struct HuffmanTable *dc_h_table = &jpeg_state->h_tables[0][component->dc_coding_table_id];
        struct HuffmanTable *ac_h_table = &jpeg_state->h_tables[1][component->ac_coding_table_id];
        uint16_t *q_table = jpeg_state->q_tables[component->q_table_id];

        for (int x = 0; x < component->x_sampling_factor; x++) {
          for (int y = 0; y < component->y_sampling_factor; y++) {
            // decode DC: F.2.2.1
            uint16_t t = decode(f, dc_h_table);
            int16_t diff = extend(receive(f, t), t);
            pred += diff;
            block[0] = pred * q_table[0];

            // decode AC: F.2.2.2
            int k = 1;
            while (k < BLOCK_SIZE * BLOCK_SIZE) {
              uint16_t rs = decode(f, ac_h_table);
              if (rs == 0xF0) // ZRL - zero run length
                k += 16;
              else if (rs == 0x00) // EOB - end of block
                break;
              else {
                uint16_t rrrr = upper_half(rs);
                uint16_t ssss = lower_half(rs);
                k += rrrr;
                check(k >= BLOCK_SIZE * BLOCK_SIZE, "Found invalid code\n");
                block[k] = extend(receive(f, ssss), ssss) * q_table[k];
                k += 1;
              }
            }

            // undo zig-zag
            for (int i = 0; i < BLOCK_SIZE; i++)
              for (int j = 0; j < BLOCK_SIZE; j++)
                block2[i][j] = block[ZIG_ZAG[i][j]];

            idct_2d((uint16_t *)block2, (double *)block_float);
          }
        }
      }
    }
  return 0;
}

// Figure F.12
int16_t extend(uint16_t v, uint16_t t) { return v < (1 << (t - 1)) ? v + (-1 << t) + 1 : v; }

// Figure F.16
uint16_t decode(FILE *f, struct HuffmanTable *h_table) {
  int i = 0;
  uint16_t code = nextbit(f);

  while (code > h_table->maxcode[i]) {
    i++;
    code = (code << 1) + nextbit(f);
  }
  return h_table->huffval[h_table->valptr[i] + code - h_table->mincode[i]];
}

// Figure F.17
uint16_t receive(FILE *f, uint16_t ssss) {
  uint16_t v = 0;
  for (int i = 0; i < ssss; i++)
    v = (v << 1) + nextbit(f);
  return v;
}

// Figure F.18
uint8_t nextbit(FILE *f) {
  // impure function
  static uint8_t b, cnt = 0;

  if (cnt == 0) {
    try_fread(&b, 1, 1, f);
    cnt = 8;

    if (b == 0xFF) {
      uint8_t b2;
      try_fread(&b2, 1, 1, f);
      if (b2 != 0) {
        if (b2 == DNL) {
          printf("DNL marker. Not implemented\n");
          return 1;
        } else {
          printf("Error");
          return 1;
        }
      }
    }
  }
  return (b >> --cnt) & 1;
}

// a(u,v) * cos((v+1/2)*u*pi/N)
void init_dct_matrix() {
  for (int i = 0; i < BLOCK_SIZE; i++)
    for (int j = 0; j < BLOCK_SIZE; j++)
      DCT_MATRIX[i][j] = i == 0 ? 0.5f * M_SQRT1_2 : 0.5f * cos((j + 0.5f) * i * M_PI / BLOCK_SIZE);
}

void idct_1d(uint16_t *x, double *out, size_t offset, size_t stride) {
  for (int i = 0; i < BLOCK_SIZE; i++) {
    double result = 0;
    for (int j = 0; j < BLOCK_SIZE; j++)
      result += x[offset + j * stride] * DCT_MATRIX[i][j];
    out[offset + i * stride] = result;
  }
}

void idct_2d(uint16_t *x, double *out) {
  for (int i = 0; i < BLOCK_SIZE; i++)
    idct_1d(x, out, i * BLOCK_SIZE, 1); // row-wise
  for (int j = 0; j < BLOCK_SIZE; j++)
    idct_1d(x, out, j, BLOCK_SIZE); // column-wise
}
