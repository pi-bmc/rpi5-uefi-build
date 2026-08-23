/** @file
 *
 *  Copyright (c) 2023, Mario Bălănică <mariobalanica02@gmail.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <Uefi.h>
#include <Library/BoardRevisionHelperLib.h>

//
// https://www.raspberrypi.com/documentation/computers/raspberry-pi.html#new-style-revision-codes
//
#define RPI_MEMORY_SIZE(Rev)   ((Rev >> 20) & 0x07)
#define RPI_MANUFACTURER(Rev)  ((Rev >> 16) & 0x0F)
#define RPI_PROCESSOR(Rev)     ((Rev >> 12) & 0x0F)
#define RPI_TYPE(Rev)          ((Rev >> 4) & 0xFF)
#define RPI_REVISION(Rev)      (Rev & 0x0F)

UINT64
EFIAPI
BoardRevisionGetMemorySize (
  IN  UINT32  RevisionCode
  )
{
  if (RevisionCode != 0) {
    return SIZE_256MB * 1ULL << RPI_MEMORY_SIZE (RevisionCode);
  }

  return SIZE_256MB; // Smallest possible size
}

UINT32
EFIAPI
BoardRevisionGetModelFamily (
  IN  UINT32  RevisionCode
  )
{
  if (RevisionCode != 0) {
    switch (RPI_TYPE (RevisionCode)) {
      case 0x00:          // Raspberry Pi Model A
      case 0x01:          // Raspberry Pi Model B
      case 0x02:          // Raspberry Pi Model A+
      case 0x03:          // Raspberry Pi Model B+
      case 0x06:          // Raspberry Pi Compute Module 1
      case 0x09:          // Raspberry Pi Zero
      case 0x0C:          // Raspberry Pi Zero W
        return 1;
      case 0x04:          // Raspberry Pi 2 Model B
        return 2;
      case 0x08:          // Raspberry Pi 3 Model B
      case 0x0A:          // Raspberry Pi Compute Module 3
      case 0x0D:          // Raspberry Pi 3 Model B+
      case 0x0E:          // Raspberry Pi 3 Model A+
      case 0x10:          // Raspberry Pi Compute Module 3+
        return 3;
      case 0x11:          // Raspberry Pi 4 Model B
      case 0x13:          // Raspberry Pi 400
      case 0x14:          // Raspberry Pi Computer Module 4
        return 4;
      case 0x17:          // Raspberry Pi 5 Model B
      case 0x18:          // Compute Module 5
      case 0x19:          // Raspberry Pi 500
      case 0x1a:          // Compute Module 5 Lite
        return 5;
    }
  }

  return 0;
}

CHAR8 *
EFIAPI
BoardRevisionGetModelName (
  IN  UINT32  RevisionCode
  )
{
  if (RevisionCode != 0) {
    switch (RPI_TYPE (RevisionCode)) {
      case 0x00:
        return "Raspberry Pi Model A";
      case 0x01:
        return "Raspberry Pi Model B";
      case 0x02:
        return "Raspberry Pi Model A+";
      case 0x03:
        return "Raspberry Pi Model B+";
      case 0x04:
        return "Raspberry Pi 2 Model B";
      case 0x06:
        return "Raspberry Pi Compute Module 1";
      case 0x08:
        return "Raspberry Pi 3 Model B";
      case 0x09:
        return "Raspberry Pi Zero";
      case 0x0A:
        return "Raspberry Pi Compute Module 3";
      case 0x0C:
        return "Raspberry Pi Zero W";
      case 0x0D:
        return "Raspberry Pi 3 Model B+";
      case 0x0E:
        return "Raspberry Pi 3 Model A+";
      case 0x10:
        return "Raspberry Pi Compute Module 3+";
      case 0x11:
        return "Raspberry Pi 4 Model B";
      case 0x13:
        return "Raspberry Pi 400";
      case 0x14:
        return "Raspberry Pi Compute Module 4";
      case 0x17:
        return "Raspberry Pi 5 Model B";
      case 0x18:
        return "Raspberry Pi Compute Module 5";
      case 0x19:
        return "Raspberry Pi 500";
      case 0x1A:
        return "Raspberry Pi Compute Module 5 Lite";
    }
  }

  return "Unknown Raspberry Pi Model";
}

//
// Device tree file names, spelled the way mainline Linux spells them
// (arch/arm64/boot/dts/broadcom), because that is the name an OS installer
// has next to its kernel. The Raspberry Pi firmware's own tree names differ
// for some variants -- it calls the Pi 5 D0 tree bcm2712d0-rpi-5-b -- and are
// deliberately not what this returns.
//
// A model that needs more than one tree gets a list indexed by the board
// revision nibble, exactly as U-Boot's rpi_models_new_scheme[] FDTFILES()
// lists are (board/raspberrypi/rpi/rpi.c): entry 0 is the model default, and
// a revision past the end of the list falls back to it.
//
STATIC CONST CHAR8 *CONST  mFdtNamesRpi5B[] = {
  "bcm2712-rpi-5-b.dtb",       // rev 1.0 -- BCM2712 C0
  "bcm2712-d-rpi-5-b.dtb"      // rev 1.1 -- BCM2712 D0
};

/**
  Pick one entry out of a model's device tree list.

  The list is indexed by board variant. For the Pi 5 B that axis is the SoC
  stepping, and the revision nibble is only a proxy for it -- the nibble
  tracks PCB spins, so a later SKU can carry D0 silicon on a board whose
  nibble is still 0, and then the proxy names the C0 tree for a D0 part. A
  caller that can read the stepping should say so rather than pass
  BOARD_VARIANT_FROM_REVISION and inherit that guess.

  @param  Names         The list, model default first.
  @param  Count         How many entries it has.
  @param  RevisionCode  The board's revision code.
  @param  Variant       The entry to take, or BOARD_VARIANT_FROM_REVISION to
                        take the one RevisionCode implies.

  @return  The named entry, or the model default if the list does not reach
           that far.

**/
STATIC
CHAR8 *
FdtNameByVariant (
  IN  CONST CHAR8 *CONST  *Names,
  IN  UINTN               Count,
  IN  UINT32              RevisionCode,
  IN  UINTN               Variant
  )
{
  if (Variant == BOARD_VARIANT_FROM_REVISION) {
    Variant = RPI_REVISION (RevisionCode);
  }

  if (Variant >= Count) {
    Variant = 0;
  }

  return (CHAR8 *)Names[Variant];
}

/**
  The device tree this board wants, as a bare file name.

  @param  RevisionCode  The board's revision code.

  @return  A static string, or NULL if the code names no board we know a tree
           for. The caller owns nothing.

**/
CHAR8 *
EFIAPI
BoardRevisionGetFdtName (
  IN  UINT32  RevisionCode,
  IN  UINTN   Variant
  )
{
  //
  // Old-scheme codes (bit 23 clear) hold the type in the low byte and index a
  // different table entirely -- original Pi 1 boards, which no platform here
  // builds for. Decline them rather than decode them as new-scheme and name a
  // tree for the wrong board. The functions either side of this one are only
  // producing display strings and can afford to be loose about it; a file
  // name gets loaded.
  //
  if ((RevisionCode == 0) || ((RevisionCode & BIT23) == 0)) {
    return NULL;
  }

  switch (RPI_TYPE (RevisionCode)) {
    case 0x00:
      return "bcm2835-rpi-a.dtb";
    case 0x01:
      return "bcm2835-rpi-b.dtb";
    case 0x02:
      return "bcm2835-rpi-a-plus.dtb";
    case 0x03:
      return "bcm2835-rpi-b-plus.dtb";
    case 0x04:
      return "bcm2836-rpi-2-b.dtb";
    case 0x06:
      return "bcm2835-rpi-cm.dtb";
    case 0x08:
      return "bcm2837-rpi-3-b.dtb";
    case 0x09:
      return "bcm2835-rpi-zero.dtb";
    case 0x0A:
      return "bcm2837-rpi-cm3.dtb";
    case 0x0C:
      return "bcm2835-rpi-zero-w.dtb";
    case 0x0D:
      return "bcm2837-rpi-3-b-plus.dtb";
    case 0x0E:
      return "bcm2837-rpi-3-a-plus.dtb";
    case 0x10:
      return "bcm2837-rpi-cm3.dtb";
    case 0x11:
      return "bcm2711-rpi-4-b.dtb";
    case 0x12:
      return "bcm2837-rpi-zero-2-w.dtb";
    case 0x13:
      return "bcm2711-rpi-400.dtb";
    case 0x14:
      return "bcm2711-rpi-cm4.dtb";
    case 0x17:
      return FdtNameByVariant (
               mFdtNamesRpi5B,
               ARRAY_SIZE (mFdtNamesRpi5B),
               RevisionCode,
               Variant
               );
    case 0x18:
      return "bcm2712-rpi-cm5-cm5io.dtb";
    case 0x19:
      return "bcm2712-rpi-500.dtb";
    case 0x1A:
      return "bcm2712-rpi-cm5l-cm5io.dtb";
  }

  return NULL;
}

CHAR8 *
EFIAPI
BoardRevisionGetManufacturerName (
  IN  UINT32  RevisionCode
  )
{
  if (RevisionCode != 0) {
    switch (RPI_MANUFACTURER (RevisionCode)) {
      case 0x00:
        return "Sony UK";
      case 0x01:
        return "Egoman";
      case 0x02:
      case 0x04:
        return "Embest";
      case 0x03:
        return "Sony Japan";
      case 0x05:
        return "Stadium";
    }
  }

  return "Unknown Manufacturer";
}

CHAR8 *
EFIAPI
BoardRevisionGetProcessorName (
  IN  UINT32  RevisionCode
  )
{
  if (RevisionCode != 0) {
    switch (RPI_PROCESSOR (RevisionCode)) {
      case 0x00:
        return "BCM2835 (ARM11)";
      case 0x01:
        return "BCM2836 (Arm Cortex-A7)";
      case 0x02:
        return "BCM2837 (Arm Cortex-A53)";
      case 0x03:
        return "BCM2711 (Arm Cortex-A72)";
      case 0x04:
        return "BCM2712 (Arm Cortex-A76)";
    }
  }

  return "Unknown CPU Model";
}
