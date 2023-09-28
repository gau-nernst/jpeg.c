#include "jpeg.h"
#include <stdio.h>
#include <stdlib.h>

// ITU-T.81, Table B.1
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

unsigned int read_2_bytes(FILE *f) {
  u_int8_t buffer[2];
  fread(buffer, 1, 2, f);
  return (buffer[0] << 8) | buffer[1];
}

void handle_sof(u_int8_t *payload) { printf("SOI"); }

void handle_app0(u_int8_t *payload) { printf("APP0"); }

// exif
void handle_app1(u_int8_t *payload) { printf("APP1"); }

void handle_unknown(u_int8_t *payload) { printf("Unknown marker"); }

int main(int argc, char *argv[]) {
  FILE *f = fopen("sample.jpg", "rb");
  if (f == NULL) {
    printf("Error");
    return 1;
  }

  u_int8_t marker[2];
  unsigned int length;
  u_int8_t *payload = NULL;
  for (;;) {
    fread(marker, 1, 2, f);
    printf("%X%X ", marker[0], marker[1]);

    if (marker[0] != 0xFF) {
      printf("Not a marker\n");
      return 1;
    }

    if (marker[1] == SOI | marker[1] == EOI) {
      length = 0;
    } else {
      length = read_2_bytes(f);
      payload = malloc(length - 2);
      if (payload == NULL) {
        printf("Failed to allocate memory\n");
        return 1;
      }
      fread(payload, 1, length - 2, f);
    }
    printf(" length = %5d ", length);

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

    default:
      handle_unknown(payload);
      break;
    }

    if (payload)
      free(payload);
    printf("\n");
  }
}
