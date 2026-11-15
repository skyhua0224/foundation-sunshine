#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rs.h"

static double now(void) {
  struct timespec t;
  timespec_get(&t, TIME_UTC);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(void) {
  const int ds = 40, ps = 8, bs = 1408, total = ds + ps;
  reed_solomon_init();
  reed_solomon *rs = reed_solomon_new(ds, ps);
  uint8_t *block = malloc((size_t) total * bs);
  uint8_t *shards[255];
  uint8_t marks[255];
  for (int i = 0; i < total * bs; ++i) block[i] = (uint8_t) (i * 2654435761u >> 24);
  for (int i = 0; i < total; ++i) shards[i] = block + (size_t) i * bs;

  double best_enc = 0, best_dec = 0;
  for (int round = 0; round < 5; ++round) {
    long n = 0;
    double t0 = now();
    while (now() - t0 < 0.4) {
      for (int k = 0; k < 50; ++k) reed_solomon_encode(rs, shards, total, bs);
      n += 50;
    }
    double enc = (double) n * ds * bs / (now() - t0) / 1e6;
    if (enc > best_enc) best_enc = enc;

    n = 0;
    t0 = now();
    while (now() - t0 < 0.4) {
      for (int k = 0; k < 20; ++k) {
        memset(marks, 0, sizeof marks);
        marks[1] = marks[7] = marks[20] = marks[33] = 1;
        reed_solomon_decode(rs, shards, marks, total, bs);
      }
      n += 20;
    }
    double dec = (double) n * ds * bs / (now() - t0) / 1e6;
    if (dec > best_dec) best_dec = dec;
  }
  printf("FEC encode %.1f MB/s decode(4 lost) %.1f MB/s\n", best_enc, best_dec);
  reed_solomon_release(rs);
  free(block);
  return 0;
}
