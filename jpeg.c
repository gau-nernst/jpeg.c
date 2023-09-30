#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE 8

// ITU-T.81, Table B.1
const u_int8_t TEM = 0x01;

const u_int8_t SOF0 = 0xC0;
const u_int8_t SOF1 = 0xC1;
const u_int8_t SOF2 = 0xC2;
const u_int8_t SOF3 = 0xC3;

const u_int8_t DHT = 0xC4;

const u_int8_t SOF5 = 0xC5;
const u_int8_t SOF6 = 0xC6;
const u_int8_t SOF7 = 0xC7;

const u_int8_t JPG = 0xC8;
const u_int8_t SOF9 = 0xC9;
const u_int8_t SOF10 = 0xCA;
const u_int8_t SOF11 = 0xCB;

const u_int8_t DAC = 0xCC;

const u_int8_t SOF13 = 0xCD;
const u_int8_t SOF14 = 0xCE;
const u_int8_t SOF15 = 0xCF;

const u_int8_t RST0 = 0xD0;

const u_int8_t SOI = 0xD8;
const u_int8_t EOI = 0xD9;
const u_int8_t SOS = 0xDA;
const u_int8_t DQT = 0xDB;
const u_int8_t DNL = 0xDC;
const u_int8_t DRI = 0xDD;
const u_int8_t DHP = 0xDE;
const u_int8_t EXP = 0xDF;

const u_int8_t APP0 = 0xE0;
const u_int8_t APP1 = 0xE1;

const u_int8_t COM = 0xFE;

struct QuantizationTable {
  uint8_t precision;
  void *data;
};

struct HuffmanTable {
  uint8_t code_lengths[16];
  void *data;
};

struct JPEGState {
  unsigned int width;
  unsigned int height;
  struct QuantizationTable q_tables[4];
  struct HuffmanTable h_tables[4];
};

uint16_t byteswap_16(uint16_t x) { return (x << 8) | (x >> 8); }
uint16_t read_be_16(const uint8_t *buffer) { return (buffer[0] << 8) | buffer[1]; }
uint8_t upper_half(uint8_t x) { return x >> 4; }
uint8_t lower_half(uint8_t x) { return x & 0xF; }

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

int handle_dqt(const uint8_t *payload, struct JPEGState *jpeg_state) {
  uint8_t precision = upper_half(payload[0]);
  uint8_t identifier = lower_half(payload[0]);

  printf("  precision = %d (%d-bit)\n", precision, (precision + 1) * 8);
  printf("  identifier = %d\n", identifier);

  struct QuantizationTable *q_table = &(jpeg_state->q_tables[identifier]);
  q_table->precision = precision;
  q_table->data = malloc((precision + 1) * 64);
  if (jpeg_state->q_tables[identifier].data == NULL) {
    printf("Failed to allocate memory\n");
    return 1;
  }

  if (precision) { // 16-bit
    uint16_t *data = q_table->data;
    for (int i = 0; i < 64; i++)
      data[i] = read_be_16(payload + 1 + i * 2);

    // TODO: make this into a macro to not repeat code?
    for (int i = 0; i < BLOCK_SIZE; i++) {
      printf(" ");
      for (int j = 0; j < BLOCK_SIZE; j++)
        printf(" %3d", data[ZIG_ZAG[i][j]]);
      printf("\n");
    }

  } else {
    uint8_t *data = q_table->data;
    for (int i = 0; i < 64; i++)
      data[i] = payload[1 + i];

    for (int i = 0; i < BLOCK_SIZE; i++) {
      printf(" ");
      for (int j = 0; j < BLOCK_SIZE; j++)
        printf(" %3d", data[ZIG_ZAG[i][j]]);
      printf("\n");
    }
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

int handle_sof0(const uint8_t *payload) {
  // Table B.2
  uint8_t precision = payload[0];
  uint16_t height = read_be_16(payload + 1);
  uint16_t width = read_be_16(payload + 3);
  uint8_t n_channels = payload[5];

  printf("  encoding = Baseline DCT\n");
  printf("  precision = %d-bit\n", precision);
  printf("  image dimension = (%d, %d)\n", width, height);

  for (int i = 0; i < n_channels; i++) {
    uint8_t channel_identifier = payload[6 + i * 3];
    uint8_t x_sampling_factor = upper_half(payload[7 + i * 3]);
    uint8_t y_sampling_factor = lower_half(payload[7 + i * 3]);
    uint8_t q_table_identifier = payload[8 + i * 3];

    printf("  channel %d\n", channel_identifier);
    printf("    sampling_factor = (%d, %d)\n", x_sampling_factor, y_sampling_factor);
    printf("    q_table_identifier = %d\n", q_table_identifier);
  }

  return 0;
}

int main(int argc, char *argv[]) {
  FILE *f = fopen("sample.jpg", "rb");
  if (f == NULL) {
    printf("Failed to open file\n");
    return 1;
  }

  struct JPEGState jpeg_state;
  u_int8_t marker[2];
  u_int16_t length;
  u_int8_t *payload = NULL;

  for (;;) {
    fread(marker, 1, 2, f);
    printf("%X%X ", marker[0], marker[1]);

    if (marker[0] != 0xFF) {
      printf("Not a marker\n");
      return 1;
    }

    if (marker[1] == TEM | marker[1] == SOI | marker[1] == EOI | (marker[1] >= RST0 & marker[1] < RST0 + 8)) {
      length = 0;
    } else {
      if (fread(&length, 1, 2, f) < 2) {
        printf("Failed to read data. Perhaps EOF?\n");
        return 1;
      }
      length = byteswap_16(length);
    }
    if (length) {
      // TODO: re-use payload buffer
      // max buffer size?
      payload = malloc(length - 2);
      if (payload == NULL) {
        printf("Failed to allocate memory\n");
        return 1;
      }
      if (fread(payload, 1, length - 2, f) < length - 2) {
        printf("Failed to read data. Perhaps EOF?\n");
        return 1;
      }
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
      if (handle_sof0(payload))
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
