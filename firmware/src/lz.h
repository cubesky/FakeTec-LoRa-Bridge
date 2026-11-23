// Tiny stateless LZ77 for single LoRa frames (no dictionary shared between frames, so a
// lost or repeated frame can never corrupt later ones).
//   0xxxxxxx            : literal run, x+1 bytes follow (1..128)
//   1llllloo oooooooo   : match, length l+3 (3..34), offset o (1..1023) back in the output
#pragma once
#include <Arduino.h>

#define LZ_RAW_MAX 512  // largest block that is compressed into one frame

// returns compressed size, 0 if it does not fit into cap
static inline size_t lzCompress(const uint8_t* in, size_t n, uint8_t* out, size_t cap) {
  static uint16_t head[512];
  memset(head, 0xFF, sizeof(head));
  size_t ip = 0, op = 0, lit = 0;
  while (ip + 3 <= n) {
    uint32_t h = ((in[ip] * 33u + in[ip + 1]) * 33u + in[ip + 2]) & 511u;
    uint16_t cand = head[h];
    head[h] = (uint16_t)ip;
    if (cand != 0xFFFF && ip - cand <= 1023 && in[cand] == in[ip] && in[cand + 1] == in[ip + 1] &&
        in[cand + 2] == in[ip + 2]) {
      size_t len = 3;
      while (ip + len < n && len < 34 && in[cand + len] == in[ip + len]) len++;
      while (lit < ip) {
        size_t k = ip - lit > 128 ? 128 : ip - lit;
        if (op + 1 + k > cap) return 0;
        out[op++] = (uint8_t)(k - 1);
        memcpy(out + op, in + lit, k);
        op += k;
        lit += k;
      }
      if (op + 2 > cap) return 0;
      size_t off = ip - cand;
      out[op++] = (uint8_t)(0x80 | ((len - 3) << 2) | (off >> 8));
      out[op++] = (uint8_t)off;
      ip += len;
      lit = ip;
    } else {
      ip++;
    }
  }
  while (lit < n) {
    size_t k = n - lit > 128 ? 128 : n - lit;
    if (op + 1 + k > cap) return 0;
    out[op++] = (uint8_t)(k - 1);
    memcpy(out + op, in + lit, k);
    op += k;
    lit += k;
  }
  return op;
}

// returns decompressed size, 0 on malformed input
static inline size_t lzDecompress(const uint8_t* in, size_t n, uint8_t* out, size_t cap) {
  size_t ip = 0, op = 0;
  while (ip < n) {
    uint8_t c = in[ip++];
    if (c < 0x80) {
      size_t k = (size_t)c + 1;
      if (ip + k > n || op + k > cap) return 0;
      memcpy(out + op, in + ip, k);
      ip += k;
      op += k;
    } else {
      if (ip >= n) return 0;
      size_t len = ((c >> 2) & 0x1F) + 3;
      size_t off = ((size_t)(c & 3) << 8) | in[ip++];
      if (off == 0 || off > op || op + len > cap) return 0;
      for (size_t i = 0; i < len; i++, op++) out[op] = out[op - off];
    }
  }
  return op;
}