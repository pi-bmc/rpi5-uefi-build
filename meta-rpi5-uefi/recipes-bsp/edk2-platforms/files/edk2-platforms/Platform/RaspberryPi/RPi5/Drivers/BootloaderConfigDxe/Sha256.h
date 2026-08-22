/** @file

  Minimal freestanding SHA-256 (FIPS 180-4) for hashing the staged EEPROM
  image. Local implementation because this platform build carries no
  CryptoPkg and the Hash2 protocol producer is not in the flash device.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef BLCFG_SHA256_H_
#define BLCFG_SHA256_H_

#include <Uefi.h>

#define BLCFG_SHA256_DIGEST_SIZE  32

/**
  Compute the SHA-256 digest of a buffer in one shot.

  @param[in]  Data    Bytes to hash.
  @param[in]  Len     Number of bytes.
  @param[out] Digest  32-byte digest.
**/
VOID
BlSha256 (
  IN  CONST UINT8  *Data,
  IN  UINTN        Len,
  OUT UINT8        Digest[BLCFG_SHA256_DIGEST_SIZE]
  );

#endif // BLCFG_SHA256_H_
