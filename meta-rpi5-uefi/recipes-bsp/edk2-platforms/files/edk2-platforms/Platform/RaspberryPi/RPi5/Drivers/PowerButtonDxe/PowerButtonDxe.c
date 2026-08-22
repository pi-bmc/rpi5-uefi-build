/** @file

  PowerButtonDxe - poll the Pi 5 power button and power off or reset,
  honoring POWER_OFF_ON_HALT from the bootloader EEPROM config.

  The RPi 5 power button goes through the PMIC, which signals the VPU
  firmware, which drives SoC GIO GPIO20 low. The pulse can be very short
  (~100-200 ms from KVM devices), so plain level-polling misses it: the
  GIO block is instead configured for falling-edge detection, whose STAT
  register latches the edge (write-1-to-clear) until this driver's 50 ms
  periodic timer samples it. A level check backs the edge path up for
  sustained holds.

  The press action mirrors U-Boot's rpi_power_btn_poll(): power off when
  the EEPROM bootloader config (blconfig nvmem-rmem region in the VPU DTB)
  contains POWER_OFF_ON_HALT=1, reset otherwise. Both actions go through
  gRT->ResetSystem - NOT the bare ResetSystemLib - so drivers registered
  with EFI_RESET_NOTIFICATION_PROTOCOL get to flush state first, which a
  direct PSCI call would skip. EfiResetShutdown reaches PSCI SYSTEM_OFF in TF-A,
  which parks the SoC via the PM watchdog RSTS halt partition; the VPU
  then honors POWER_OFF_ON_HALT just as it does for an OS shutdown.

  Everything (button GPIO, controller base, bank widths, polarity) is
  parsed from the VPU DTB at PcdFdtBaseAddress, so board variants that
  move the button keep working. The blconfig region is a VPU carve-out
  mapped as device memory: it is staged with single-byte volatile reads
  before parsing (wide/unaligned accesses fault - see BootloaderConfigDxe).

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

//
// brcmstb GIO per-bank register offsets (each bank is 0x20 bytes), per
// Linux gpio-brcmstb.c and U-Boot's power-button code.
//
#define GIO_BANK_SIZE  0x20
#define GIO_REG_DATA   0x04              // pin level
#define GIO_REG_IODIR  0x08              // direction: 1 = input
#define GIO_REG_EC     0x0c              // edge config: 0 = falling, 1 = rising
#define GIO_REG_EI     0x10              // edge-insensitive: set = both edges
#define GIO_REG_MASK   0x14              // must be set for STAT to latch edges
#define GIO_REG_STAT   0x1c              // latched edge status, write-1-to-clear

//
// 50 ms poll, same as U-Boot's RPI_PWR_BTN_POLL_US, in 100 ns units.
//
#define POWER_BUTTON_POLL_INTERVAL  (50 * 10 * 1000)

#define GPIO_ACTIVE_LOW  BIT0

//
// A blconfig region larger than this is not believable - treat as invalid -
// and the config text worth scanning fits well inside the staging cap.
//
#define BLCONFIG_REGION_MAX  SIZE_64KB
#define BLCONFIG_STAGE_MAX   SIZE_8KB

STATIC EFI_EVENT  mPollTimer;
STATIC UINT64     mBankBase;
STATIC UINT32     mBitMask;
STATIC BOOLEAN    mActiveLow;
STATIC BOOLEAN    mPowerOffOnHalt;
STATIC BOOLEAN    mTriggered;

/**
  Accumulate Count big-endian 32-bit cells into a UINT64.

  @param[in] Cell   First cell.
  @param[in] Count  Number of cells (1 or 2).

  @return The combined value.
**/
STATIC
UINT64
ReadCells (
  IN CONST UINT32  *Cell,
  IN INT32         Count
  )
{
  UINT64  Value;
  INT32   Index;

  Value = 0;
  for (Index = 0; Index < Count; Index++) {
    Value = LShiftU64 (Value, 32) | Fdt32ToCpu (Cell[Index]);
  }

  return Value;
}

/**
  Read a node's first "reg" entry and translate it to a CPU physical
  address by walking every parent bus's "ranges" up to the root - the
  equivalent of U-Boot's dev_remap_addr(). A bus with an empty "ranges"
  (or, leniently, none at all - the firmware DTBs always carry one) maps
  1:1.

  @param[in]  Fdt         The FDT blob.
  @param[in]  NodeOffset  Node whose "reg" to read.
  @param[out] Address     CPU physical address of the first reg entry.
  @param[out] Size        First reg entry's size (bus-invariant).

  @retval TRUE   Address/Size extracted and translated.
  @retval FALSE  Missing/short properties or unsupported cell counts.
**/
STATIC
BOOLEAN
GetTranslatedRegAddress (
  IN  CONST VOID  *Fdt,
  IN  INT32       NodeOffset,
  OUT UINT64      *Address,
  OUT UINT64      *Size
  )
{
  INT32         Bus;
  INT32         BusParent;
  INT32         AddressCells;
  INT32         SizeCells;
  INT32         ParentCells;
  INT32         Length;
  INT32         EntryCells;
  CONST UINT32  *Cell;
  UINT64        Addr;
  UINT64        ChildStart;
  UINT64        ParentStart;
  UINT64        RangeSize;
  BOOLEAN       Matched;

  Bus = FdtParentOffset (Fdt, NodeOffset);
  if (Bus < 0) {
    return FALSE;
  }

  AddressCells = FdtAddressCells (Fdt, Bus);
  SizeCells    = FdtSizeCells (Fdt, Bus);
  if ((AddressCells < 1) || (AddressCells > 2) ||
      (SizeCells < 0) || (SizeCells > 2))
  {
    return FALSE;
  }

  Cell = FdtGetProp (Fdt, NodeOffset, "reg", &Length);
  if ((Cell == NULL) ||
      (Length < (AddressCells + SizeCells) * (INT32)sizeof (UINT32)))
  {
    return FALSE;
  }

  Addr  = ReadCells (Cell, AddressCells);
  *Size = ReadCells (Cell + AddressCells, SizeCells);

  //
  // Root node offset is 0: once the bus IS the root, Addr is CPU-physical.
  //
  while (Bus > 0) {
    BusParent = FdtParentOffset (Fdt, Bus);
    if (BusParent < 0) {
      return FALSE;
    }

    ParentCells = FdtAddressCells (Fdt, BusParent);
    if ((ParentCells < 1) || (ParentCells > 2)) {
      return FALSE;
    }

    Cell = FdtGetProp (Fdt, Bus, "ranges", &Length);
    if ((Cell != NULL) && (Length > 0)) {
      EntryCells = AddressCells + ParentCells + SizeCells;
      Matched    = FALSE;
      while (Length >= EntryCells * (INT32)sizeof (UINT32)) {
        ChildStart  = ReadCells (Cell, AddressCells);
        ParentStart = ReadCells (Cell + AddressCells, ParentCells);
        RangeSize   = ReadCells (Cell + AddressCells + ParentCells, SizeCells);
        if ((Addr >= ChildStart) && (Addr - ChildStart < RangeSize)) {
          Addr    = ParentStart + (Addr - ChildStart);
          Matched = TRUE;
          break;
        }

        Cell   += EntryCells;
        Length -= EntryCells * (INT32)sizeof (UINT32);
      }

      if (!Matched) {
        return FALSE;
      }
    }

    Bus          = BusParent;
    AddressCells = ParentCells;
    SizeCells    = FdtSizeCells (Fdt, Bus);
    if ((SizeCells < 0) || (SizeCells > 2)) {
      return FALSE;
    }
  }

  *Address = Addr;
  return TRUE;
}

/**
  Locate the blconfig nvmem-rmem node: by compatible first, then through
  the /aliases "blconfig" indirection, like U-Boot's rpi_find_blconfig().

  @param[in] Fdt  The FDT blob.

  @return Node offset, or negative if not found.
**/
STATIC
INT32
FindBlconfigNode (
  IN CONST VOID  *Fdt
  )
{
  INT32  Node;

  Node = FdtNodeOffsetByCompatible (Fdt, -1, "raspberrypi,bootloader-config");
  if (Node >= 0) {
    return Node;
  }

  return FdtPathOffset (Fdt, "blconfig");
}

/**
  Read POWER_OFF_ON_HALT from the EEPROM bootloader config text the VPU
  copied into the blconfig region. The config is plain key=value lines;
  only an exact POWER_OFF_ON_HALT=1 counts (U-Boot semantics).

  @param[in] Fdt  The FDT blob.

  @retval TRUE   POWER_OFF_ON_HALT=1 found.
  @retval FALSE  Absent, 0, or the region is missing/invalid.
**/
STATIC
BOOLEAN
ReadPowerOffOnHalt (
  IN CONST VOID  *Fdt
  )
{
  STATIC CONST CHAR8    Key[] = "POWER_OFF_ON_HALT=";
  CONST UINTN           KeyLen = sizeof (Key) - 1;
  INT32                 Node;
  UINT64                RegionAddress;
  UINT64                RegionSize;
  UINTN                 DataLen;
  UINT8                 *Data;
  CONST volatile UINT8  *Region;
  UINTN                 Index;
  UINTN                 Pos;
  BOOLEAN               Result;

  Node = FindBlconfigNode (Fdt);
  if (Node < 0) {
    return FALSE;
  }

  if (!GetTranslatedRegAddress (Fdt, Node, &RegionAddress, &RegionSize)) {
    return FALSE;
  }

  if ((RegionAddress == 0) || (RegionSize == 0) ||
      (RegionSize >= BLCONFIG_REGION_MAX))
  {
    return FALSE;
  }

  //
  // Device-memory staging discipline: single-byte volatile reads only.
  //
  DataLen = MIN ((UINTN)RegionSize, (UINTN)BLCONFIG_STAGE_MAX);
  Data    = AllocatePool (DataLen);
  if (Data == NULL) {
    return FALSE;
  }

  Region = (CONST volatile UINT8 *)(UINTN)RegionAddress;
  for (Index = 0; Index < DataLen; Index++) {
    Data[Index] = Region[Index];
  }

  Result = FALSE;
  for (Pos = 0; (Pos < DataLen) && (Data[Pos] != '\0'); ) {
    if ((DataLen - Pos > KeyLen) &&
        (CompareMem (&Data[Pos], Key, KeyLen) == 0))
    {
      Result = (Data[Pos + KeyLen] == '1');
      break;
    }

    while ((Pos < DataLen) && (Data[Pos] != '\0') && (Data[Pos] != '\n')) {
      Pos++;
    }

    if ((Pos < DataLen) && (Data[Pos] == '\n')) {
      Pos++;
    }
  }

  FreePool (Data);
  return Result;
}

/**
  Find the power button in the VPU DTB: any "gpio-keys" node's subnode
  labeled "pwr_button", per the firmware DTBs and U-Boot.

  @param[in]  Fdt         The FDT blob.
  @param[out] Controller  GPIO controller node (gpios phandle target).
  @param[out] GpioNumber  Controller-relative GPIO number.
  @param[out] Flags       GPIO specifier flags cell (0 if absent).

  @retval TRUE   Button found and its gpios specifier parsed.
  @retval FALSE  No pwr_button node, or unparseable gpios.
**/
STATIC
BOOLEAN
FindPowerButton (
  IN  CONST VOID  *Fdt,
  OUT INT32       *Controller,
  OUT UINT32      *GpioNumber,
  OUT UINT32      *Flags
  )
{
  INT32         Keys;
  INT32         Button;
  INT32         Length;
  CONST CHAR8   *Label;
  CONST UINT32  *Gpios;
  CONST UINT32  *Prop;
  INT32         GpioCells;

  for (Keys = FdtNodeOffsetByCompatible (Fdt, -1, "gpio-keys");
       Keys >= 0;
       Keys = FdtNodeOffsetByCompatible (Fdt, Keys, "gpio-keys"))
  {
    for (Button = FdtFirstSubnode (Fdt, Keys);
         Button >= 0;
         Button = FdtNextSubnode (Fdt, Button))
    {
      Label = FdtGetProp (Fdt, Button, "label", &Length);
      if ((Label == NULL) || (AsciiStrCmp (Label, "pwr_button") != 0)) {
        continue;
      }

      Gpios = FdtGetProp (Fdt, Button, "gpios", &Length);
      if ((Gpios == NULL) || (Length < 2 * (INT32)sizeof (UINT32))) {
        return FALSE;
      }

      *Controller = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Gpios[0]));
      if (*Controller < 0) {
        return FALSE;
      }

      Prop = FdtGetProp (Fdt, *Controller, "#gpio-cells", &Length);
      if ((Prop == NULL) || (Length != sizeof (UINT32))) {
        return FALSE;
      }

      GpioCells = (INT32)Fdt32ToCpu (*Prop);
      if ((GpioCells < 1) || (GpioCells > 3) ||
          (FdtGetProp (Fdt, Button, "gpios", &Length) == NULL) ||
          (Length < (1 + GpioCells) * (INT32)sizeof (UINT32)))
      {
        return FALSE;
      }

      *GpioNumber = Fdt32ToCpu (Gpios[1]);
      *Flags      = (GpioCells >= 2) ? Fdt32ToCpu (Gpios[GpioCells]) : 0;
      return TRUE;
    }
  }

  return FALSE;
}

/**
  50 ms poll: latched-edge check first (catches short pulses), level
  check as fallback for sustained holds. On a press, power off or reset
  per POWER_OFF_ON_HALT - through gRT->ResetSystem so reset notifications
  (the BMC EEPROM variable sync among them) run first.

  @param[in] Event    The periodic timer event.
  @param[in] Context  Unused.
**/
STATIC
VOID
EFIAPI
PowerButtonPoll (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT32   Data;
  BOOLEAN  Pressed;

  if (mTriggered) {
    return;
  }

  Pressed = FALSE;
  if ((MmioRead32 (mBankBase + GIO_REG_STAT) & mBitMask) != 0) {
    MmioWrite32 (mBankBase + GIO_REG_STAT, mBitMask);
    Pressed = TRUE;
  } else {
    Data = MmioRead32 (mBankBase + GIO_REG_DATA);
    if (mActiveLow ? ((Data & mBitMask) == 0) : ((Data & mBitMask) != 0)) {
      Pressed = TRUE;
    }
  }

  if (!Pressed) {
    return;
  }

  mTriggered = TRUE;
  gBS->SetTimer (mPollTimer, TimerCancel, 0);

  DEBUG ((
    DEBUG_INFO,
    "PowerButton: pressed, %a\n",
    mPowerOffOnHalt ? "powering off" : "resetting"
    ));

  gRT->ResetSystem (
         mPowerOffOnHalt ? EfiResetShutdown : EfiResetCold,
         EFI_SUCCESS,
         0,
         NULL
         );
}

/**
  Entry point: parse the VPU DTB for the button and its GIO controller,
  arm falling-edge detection, and start the poll timer.

  @param[in] ImageHandle  The image handle.
  @param[in] SystemTable  The system table.

  @retval EFI_SUCCESS    Poller armed.
  @retval EFI_NOT_FOUND  No usable power button in the DTB (driver unloads).
  @return                Error from CreateEvent/SetTimer otherwise.
**/
EFI_STATUS
EFIAPI
PowerButtonEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS    Status;
  CONST VOID    *Fdt;
  INT32         Controller;
  UINT32        GpioNumber;
  UINT32        Flags;
  UINT64        GioBase;
  UINT64        GioSize;
  CONST UINT32  *Widths;
  INT32         Length;
  UINT32        Bank;
  UINT32        Offset;
  UINT32        Width;

  Fdt = (CONST VOID *)(UINTN)FixedPcdGet32 (PcdFdtBaseAddress);
  if ((Fdt == NULL) || (FdtCheckHeader (Fdt) != 0)) {
    DEBUG ((DEBUG_INFO, "PowerButton: no valid VPU DTB\n"));
    return EFI_NOT_FOUND;
  }

  if (!FindPowerButton (Fdt, &Controller, &GpioNumber, &Flags)) {
    DEBUG ((DEBUG_INFO, "PowerButton: no pwr_button node\n"));
    return EFI_NOT_FOUND;
  }

  if (!GetTranslatedRegAddress (Fdt, Controller, &GioBase, &GioSize)) {
    DEBUG ((DEBUG_WARN, "PowerButton: cannot resolve GIO base\n"));
    return EFI_NOT_FOUND;
  }

  //
  // Map the GPIO number to its bank and bit. Absent bank widths (the
  // firmware DTBs always carry them) fall back to uniform 32-bit banks.
  //
  Bank   = 0;
  Offset = GpioNumber;
  Widths = FdtGetProp (Fdt, Controller, "brcm,gpio-bank-widths", &Length);
  if ((Widths != NULL) && (Length >= (INT32)sizeof (UINT32))) {
    while (Length >= (INT32)sizeof (UINT32)) {
      Width = Fdt32ToCpu (*Widths);
      if (Offset < Width) {
        break;
      }

      Offset -= Width;
      Bank++;
      Widths++;
      Length -= sizeof (UINT32);
    }

    if (Length < (INT32)sizeof (UINT32)) {
      DEBUG ((DEBUG_WARN, "PowerButton: GPIO%u out of range\n", GpioNumber));
      return EFI_NOT_FOUND;
    }
  } else {
    Bank   = GpioNumber / 32;
    Offset = GpioNumber % 32;
  }

  mBankBase  = GioBase + Bank * GIO_BANK_SIZE;
  mBitMask   = 1U << Offset;
  mActiveLow = (Flags & GPIO_ACTIVE_LOW) != 0;

  //
  // POWER_OFF_ON_HALT decides press behavior (poweroff vs reset), read
  // once: the EEPROM config cannot change without a reboot.
  //
  mPowerOffOnHalt = ReadPowerOffOnHalt (Fdt);

  //
  // Input direction, then edge detect toward the pressed level: EC clear
  // = falling (active-low press), EI clear = honor EC, MASK set so STAT
  // latches, then clear any stale edge (STAT is write-1-to-clear).
  //
  MmioOr32 (mBankBase + GIO_REG_IODIR, mBitMask);
  if (mActiveLow) {
    MmioAnd32 (mBankBase + GIO_REG_EC, ~mBitMask);
  } else {
    MmioOr32 (mBankBase + GIO_REG_EC, mBitMask);
  }

  MmioAnd32 (mBankBase + GIO_REG_EI, ~mBitMask);
  MmioOr32 (mBankBase + GIO_REG_MASK, mBitMask);
  MmioWrite32 (mBankBase + GIO_REG_STAT, mBitMask);

  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  PowerButtonPoll,
                  NULL,
                  &mPollTimer
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->SetTimer (mPollTimer, TimerPeriodic, POWER_BUTTON_POLL_INTERVAL);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mPollTimer);
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "PowerButton: GPIO%u bank %u bit %u at 0x%lx, %a on press\n",
    GpioNumber,
    Bank,
    Offset,
    mBankBase,
    mPowerOffOnHalt ? "power-off" : "reset"
    ));

  return EFI_SUCCESS;
}
