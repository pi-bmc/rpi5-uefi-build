/** @file

  Minimal freestanding SHA-256 (FIPS 180-4), written from the published
  specification. Straight-line block processing, no allocation - suitable
  for hashing the 2 MiB EEPROM image once per staging operation.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Sha256.h"

STATIC CONST UINT32  mK[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
  0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
  0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
  0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
  0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
  0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n)  (((x) >> (n)) | ((x) << (32 - (n))))

STATIC
VOID
Sha256Block (
  IN OUT UINT32       State[8],
  IN     CONST UINT8  *Block
  )
{
  UINT32  W[64];
  UINT32  A, B, C, D, E, F, G, H;
  UINT32  T1, T2;
  UINTN   I;

  for (I = 0; I < 16; I++) {
    W[I] = ((UINT32)Block[I * 4] << 24) |
           ((UINT32)Block[I * 4 + 1] << 16) |
           ((UINT32)Block[I * 4 + 2] << 8) |
           (UINT32)Block[I * 4 + 3];
  }

  for (I = 16; I < 64; I++) {
    T1   = ROTR (W[I - 15], 7) ^ ROTR (W[I - 15], 18) ^ (W[I - 15] >> 3);
    T2   = ROTR (W[I - 2], 17) ^ ROTR (W[I - 2], 19) ^ (W[I - 2] >> 10);
    W[I] = W[I - 16] + T1 + W[I - 7] + T2;
  }

  A = State[0];
  B = State[1];
  C = State[2];
  D = State[3];
  E = State[4];
  F = State[5];
  G = State[6];
  H = State[7];

  for (I = 0; I < 64; I++) {
    T1 = H + (ROTR (E, 6) ^ ROTR (E, 11) ^ ROTR (E, 25)) +
         ((E & F) ^ ((~E) & G)) + mK[I] + W[I];
    T2 = (ROTR (A, 2) ^ ROTR (A, 13) ^ ROTR (A, 22)) +
         ((A & B) ^ (A & C) ^ (B & C));
    H = G;
    G = F;
    F = E;
    E = D + T1;
    D = C;
    C = B;
    B = A;
    A = T1 + T2;
  }

  State[0] += A;
  State[1] += B;
  State[2] += C;
  State[3] += D;
  State[4] += E;
  State[5] += F;
  State[6] += G;
  State[7] += H;
}

VOID
BlSha256 (
  IN  CONST UINT8  *Data,
  IN  UINTN        Len,
  OUT UINT8        Digest[BLCFG_SHA256_DIGEST_SIZE]
  )
{
  UINT32  State[8];
  UINT8   Tail[128];
  UINTN   Remain;
  UINTN   TailLen;
  UINT64  BitLen;
  UINTN   I;

  State[0] = 0x6a09e667;
  State[1] = 0xbb67ae85;
  State[2] = 0x3c6ef372;
  State[3] = 0xa54ff53a;
  State[4] = 0x510e527f;
  State[5] = 0x9b05688c;
  State[6] = 0x1f83d9ab;
  State[7] = 0x5be0cd19;

  for (Remain = Len; Remain >= 64; Remain -= 64, Data += 64) {
    Sha256Block (State, Data);
  }

  //
  // Final padding: 0x80, zeros, 64-bit big-endian bit count. Spills into
  // a second block when fewer than 8 pad bytes fit.
  //
  for (I = 0; I < Remain; I++) {
    Tail[I] = Data[I];
  }

  Tail[Remain] = 0x80;
  TailLen      = (Remain < 56) ? 64 : 128;
  for (I = Remain + 1; I < TailLen - 8; I++) {
    Tail[I] = 0;
  }

  BitLen = (UINT64)Len * 8;
  for (I = 0; I < 8; I++) {
    Tail[TailLen - 1 - I] = (UINT8)(BitLen >> (8 * I));
  }

  Sha256Block (State, Tail);
  if (TailLen == 128) {
    Sha256Block (State, Tail + 64);
  }

  for (I = 0; I < 8; I++) {
    Digest[I * 4]     = (UINT8)(State[I] >> 24);
    Digest[I * 4 + 1] = (UINT8)(State[I] >> 16);
    Digest[I * 4 + 2] = (UINT8)(State[I] >> 8);
    Digest[I * 4 + 3] = (UINT8)State[I];
  }
}
