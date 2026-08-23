/** @file
  Empty OpensslLib instance.

  BaseCryptLibMbedTls's SmmCryptLib.inf names the OpensslLib class, but the
  StMM variable stack's crypto is served entirely by mbedtls: the linked
  VariableStandaloneMm image contains zero OpenSSL symbols (verified by
  symbol-table sweep), so the class reference is satisfied without pulling a
  single archive member. Building the real OpensslLibCrypto here is not just
  waste -- it is impossible under -mgeneral-regs-only, which every module in
  the FP/SIMD-trapping StMM secure partition must use and which gcc refuses
  to combine with OpenSSL's double-typed OSSL_PARAM API. Hence this empty
  instance. If a future BaseCryptLibMbedTls change starts referencing real
  OpenSSL symbols, the build fails at link -- loudly, which is the intent.

  Copyright (c) 2026, pi-bmc contributors

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

typedef int OpensslLibNullPlaceholder;
