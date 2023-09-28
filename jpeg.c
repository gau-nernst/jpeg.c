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

// assume system is little-endian
uint16_t parse_2_bytes(const uint8_t *buffer) { return (buffer[0] << 8) | buffer[1]; }

void handle_sof(const u_int8_t *payload) { printf("SOI"); }

// JFIF i.e. JPEG Part 5
void handle_app0(const u_int8_t *payload) {
  printf("APP0\n");
  printf("  identifier = %.5s\n", payload); // either JFIF or JFXX

  if (strcmp((const char *)payload, "JFIF") == 0) {
    printf("  version = %d.%d\n", payload[5], payload[6]);
    printf("  units = %d\n", payload[7]);
    printf("  density = (%d, %d)\n", parse_2_bytes(payload + 8), parse_2_bytes(payload + 10));
    printf("  thumbnail = (%d, %d)\n", payload[12], payload[13]);
  } else if (strcmp((const char *)payload, "JFXX") == 0) {
    printf("  extension_code = %X\n", payload[5]);
  } else
    printf("  Invalid identifier\n");
}

// exif
void handle_app1(const u_int8_t *payload) {
  printf("APP1\n");
  printf("  identifier = %s\n", payload);

  if (strcmp((const char *)payload, "Exif") == 0) {
    printf("  Exif detected\n");
  } else
    printf("  Invalid identifier\n");
}

void handle_dqt(const u_int8_t *payload) {
  printf("DQT\n");
  int precision = payload[0] >> 4;
  int identifier = payload[0] & 0xF;

  printf("  precision = %d\n", precision);
  printf("  identifier = %d\n", identifier);
}

void handle_unknown(const u_int8_t *payload) { printf("Unknown marker"); }

int main(int argc, char *argv[]) {
  FILE *f = fopen("sample.jpg", "rb");
  if (f == NULL) {
    printf("Error");
    return 1;
  }

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
      fread(&length, 1, 2, f);
      length = parse_2_bytes((const uint8_t *)&length);
    }
    printf(" length = %5d ", length);
    if (length) {
      payload = malloc(length - 2);
      if (payload == NULL) {
        printf("Failed to allocate memory\n");
        return 1;
      }
      fread(payload, 1, length - 2, f);
    }

    switch (marker[1]) {
    case SOI:
      handle_sof(payload);
      break;

    case APP0:
      handle_app0(payload);
      break;

    case APP1:
      handle_app1(payload);
      break;

    case DQT:
      handle_dqt(payload);
      break;

    default:
      handle_unknown(payload);
      break;
    }

    if (payload)
      free(payload);
    printf("\n");
  }
}
