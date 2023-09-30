#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

struct JPEGState {
  unsigned int width;
  unsigned int height;
  struct QuantizationTable dqt[4];
};

uint16_t byteswap_16(uint16_t x) { return (x << 8) | (x >> 8); }
uint16_t read_be_16(uint8_t *buffer) { return (buffer[0] << 8) | buffer[1]; }

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
    printf(" length = %5d ", length);
    if (length) {
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
      // JFIF i.e. JPEG Part 5
      printf("APP0\n");
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
      break;

    case APP1:
      printf("APP1\n");
      printf("  identifier = %s\n", payload);

      if (strcmp((const char *)payload, "Exif") == 0) {
        printf("  Exif detected\n");
      } else
        printf("  Invalid identifier\n");
      break;

    case DQT:
      printf("DQT\n");
      uint8_t precision = payload[0] >> 4;
      uint8_t identifier = payload[0] & 0xF;

      printf("  precision = %d (%d-bit)\n", precision, (precision + 1) * 8);
      printf("  identifier = %d\n", identifier);

      jpeg_state.dqt[identifier].precision = precision;
      jpeg_state.dqt[identifier].data = malloc((precision + 1) * 64);
      if (jpeg_state.dqt[identifier].data == NULL) {
        printf("Failed to allocate memory\n");
        return 1;
      }

      if (precision) { // 16-bit
        uint16_t *dqt = jpeg_state.dqt[identifier].data;
        for (int i = 0; i < 64; i++)
          dqt[i] = read_be_16(payload + 1 + i * 2);
      } else {
        uint8_t *dqt = jpeg_state.dqt[identifier].data;
        for (int i = 0; i < 64; i++)
          dqt[i] = payload[1 + i];
      }

      break;

    default:
      printf("Unknown marker");
      break;
    }

    if (payload)
      free(payload);
    printf("\n");
  }

  return 0;
}
