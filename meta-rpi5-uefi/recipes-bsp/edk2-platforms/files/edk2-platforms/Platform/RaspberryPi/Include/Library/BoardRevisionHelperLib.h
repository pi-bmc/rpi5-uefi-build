/** @file
 *
 *  Copyright (c) 2023, Mario Bălănică <mariobalanica02@gmail.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#ifndef __BOARD_REVISION_HELPER_LIB_H__
#define __BOARD_REVISION_HELPER_LIB_H__

UINT64
EFIAPI
BoardRevisionGetMemorySize (
  IN  UINT32  RevisionCode
  );

UINT32
EFIAPI
BoardRevisionGetModelFamily (
  IN  UINT32  RevisionCode
  );

CHAR8 *
EFIAPI
BoardRevisionGetModelName (
  IN  UINT32  RevisionCode
  );

//
// Passed as Variant when the caller has nothing better than the revision
// code to go on.
//
#define BOARD_VARIANT_FROM_REVISION  MAX_UINTN

/**
  The device tree this board wants, as a bare file name.

  @param  RevisionCode  The board's revision code.
  @param  Variant       Which entry of a multi-tree model to take, or
                        BOARD_VARIANT_FROM_REVISION to take the one the
                        revision code implies. A model with a single tree
                        ignores this.

  @return  A static string, or NULL if the code names no board we know a tree
           for. The caller owns nothing.

**/
CHAR8 *
EFIAPI
BoardRevisionGetFdtName (
  IN  UINT32  RevisionCode,
  IN  UINTN   Variant
  );

CHAR8 *
EFIAPI
BoardRevisionGetManufacturerName (
  IN  UINT32  RevisionCode
  );

CHAR8 *
EFIAPI
BoardRevisionGetProcessorName (
  IN  UINT32  RevisionCode
  );

#endif /* __BOARD_REVISION_HELPER_LIB_H__ */
