/** @file

  PowerButtonScmiDxe - act on the Pi 5 power button through SCMI, honoring
  POWER_OFF_ON_HALT from the bootloader EEPROM config.

  Successor to PowerButtonDxe (kept in the tree, unbuilt, as the
  direct-GIO reference for OP-TEE-less configurations). The button's edge
  latch in the brcmstb GIO block is single-owner hardware - polling and
  write-1-clearing STAT from two worlds loses presses - and that owner is
  now OP-TEE (plat-rpi5 pwr_button.c), which latches presses and surfaces
  them on the SCMI System Power protocol's SYSTEM_POWER_STATE_GET as the
  vendor state RPI_SCMI_SYS_STATE_BUTTON_SHUTDOWN. This driver polls that
  state every 100 ms and performs the action.

  The press action mirrors U-Boot's rpi_power_btn_poll(): power off when
  the EEPROM bootloader config (blconfig nvmem-rmem region in the VPU DTB)
  contains POWER_OFF_ON_HALT=1, reset otherwise. Both actions go through
  gRT->ResetSystem - NOT a bare PSCI call - so drivers registered with
  EFI_RESET_NOTIFICATION_PROTOCOL flush first (the variable-store file
  sync among them). EfiResetShutdown reaches PSCI SYSTEM_OFF in TF-A,
  which parks the SoC via the PM watchdog RSTS halt partition; the VPU
  then honors POWER_OFF_ON_HALT just as it does for an OS shutdown.

  Policy stays in the normal world by design: OP-TEE only reports (TF-A
  rejects PSCI from the secure world, and acting from there would skip
  the reset-notification flush). At OS runtime, when nothing polls SCMI,
  the same latched press reaches the BMC through the sensor record's
  POWER_BUTTON status bit and the BMC orchestrates a graceful shutdown.

  The blconfig region is a VPU carve-out mapped as device memory: it is
  staged with single-byte volatile reads before parsing (wide/unaligned
  accesses fault - see BootloaderConfigDxe).

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/RpiScmiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

//
// 100 ms poll: the press is latched in OP-TEE, so nothing is lost to the
// interval - it only bounds reaction time.
//
#define POWER_BUTTON_POLL_INTERVAL  (100 * 10 * 1000)

//
// A blconfig region larger than this is not believable - treat as invalid -
// and the config text worth scanning fits well inside the staging cap.
//
#define BLCONFIG_REGION_MAX  SIZE_64KB
#define BLCONFIG_STAGE_MAX   SIZE_8KB

STATIC EFI_EVENT  mPollTimer;
STATIC BOOLEAN    mPowerOffOnHalt;
STATIC BOOLEAN    mTriggered;
STATIC BOOLEAN    mPolicySent;

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
  STATIC CONST CHAR8    Key[]  = "POWER_OFF_ON_HALT=";
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
  100 ms poll of SYSTEM_POWER_STATE_GET. On a latched press, power off or
  reset per POWER_OFF_ON_HALT - through gRT->ResetSystem so reset
  notifications (the variable-store file sync among them) run first.

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
  UINT32  In[2];
  UINT32  Out[2];

  if (mTriggered || !RpiScmiReady ()) {
    return;
  }

  //
  // Deliver the press policy to OP-TEE once, on the first ready tick:
  // OP-TEE watches the button itself and, after a grace window in which
  // this driver's flush-first action (below) normally wins, executes the
  // press through EL3 - it needs the off-vs-reset verdict to do so. This
  // is what makes the button work at OS runtime, where nothing here polls.
  //
  if (!mPolicySent) {
    In[0] = 0;   // flags
    In[1] = mPowerOffOnHalt ? RPI_SCMI_SYS_SET_POLICY_OFF
                            : RPI_SCMI_SYS_SET_POLICY_RESET;
    if (!EFI_ERROR (
           RpiScmiCall (
             RPI_SCMI_PROTOCOL_SYS_POWER,
             RPI_SCMI_SYS_POWER_STATE_SET,
             In,
             2,
             Out,
             1
             )
           ) &&
        ((INT32)Out[0] == 0))
    {
      mPolicySent = TRUE;
    }
  }

  if (EFI_ERROR (
        RpiScmiCall (
          RPI_SCMI_PROTOCOL_SYS_POWER,
          RPI_SCMI_SYS_POWER_STATE_GET,
          NULL,
          0,
          Out,
          2
          )
        ) ||
      ((INT32)Out[0] != 0))
  {
    return;    // server not answering yet; next tick retries
  }

  if (Out[1] != RPI_SCMI_SYS_STATE_BUTTON_SHUTDOWN) {
    return;
  }

  mTriggered = TRUE;
  gBS->SetTimer (mPollTimer, TimerCancel, 0);

  DEBUG ((
    DEBUG_INFO,
    "PowerButtonScmi: pressed, %a\n",
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
  Entry point: read the POWER_OFF_ON_HALT policy from the VPU DTB's
  blconfig region and start polling the SCMI system power state.

  @param[in] ImageHandle  The image handle.
  @param[in] SystemTable  The system table.

  @retval EFI_SUCCESS  Poller armed.
  @return              Error from CreateEvent/SetTimer otherwise.
**/
EFI_STATUS
EFIAPI
PowerButtonScmiEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  CONST VOID  *Fdt;

  //
  // POWER_OFF_ON_HALT decides press behavior (poweroff vs reset), read
  // once: the EEPROM config cannot change without a reboot. No DTB or no
  // blconfig degrades to reset-on-press, matching the U-Boot fallback.
  //
  mPowerOffOnHalt = FALSE;
  Fdt             = (CONST VOID *)(UINTN)FixedPcdGet32 (PcdFdtBaseAddress);
  if ((Fdt != NULL) && (FdtCheckHeader (Fdt) == 0)) {
    mPowerOffOnHalt = ReadPowerOffOnHalt (Fdt);
  }

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
    "PowerButtonScmi: polling SCMI system power, %a on press\n",
    mPowerOffOnHalt ? "power-off" : "reset"
    ));

  return EFI_SUCCESS;
}
