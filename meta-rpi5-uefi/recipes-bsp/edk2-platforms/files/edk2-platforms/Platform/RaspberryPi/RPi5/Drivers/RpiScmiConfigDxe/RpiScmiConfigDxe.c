/** @file

  RpiScmiConfigDxe - the "Power Profile" page in Setup's Device Manager,
  and the one-shot SCMI power configuration handoff to OP-TEE.

  Two jobs:

  1. Setup/Redfish surface. A pure efivarstore HII formset
     (RpiScmiConfigHii.vfr) binds the PowerProfile question straight to the
     PowerProfile variable, and RpiScmiConfigDxeMap.uni exposes it as the
     Redfish BIOS attribute PowerProfile. ActiveCoolerScmiDxe reads
     PowerProfile every poll tick to pick the fan curve, so a saved change
     (or a BMC write) applies live. No ConfigAccess callback is needed.

  2. Power configuration to OP-TEE. OP-TEE owns the physical power button
     and executes the press itself through EL3 after a grace window (see
     plat-rpi5 pwr_button.c); it needs the off-vs-reset policy, which lives
     in the bootloader EEPROM config (POWER_OFF_ON_HALT). This driver reads
     that from the VPU DTB's blconfig region - EDK2 can resolve the address
     from the DTB, OP-TEE cannot (it has no device tree) - and delivers it
     over SCMI System Power (a vendor SYSTEM_POWER_STATE_SET) the moment the
     channel is ready. It is a ONE-SHOT: fire, deliver, cancel. There is no
     periodic polling here by design - a TPL_CALLBACK poll competes with the
     Redfish provisioning that runs at TPL_APPLICATION, which is what broke
     BIOS-attribute sync when the retired PowerButtonScmiDxe polled at
     100 ms.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FdtLib.h>
#include <Library/HiiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/RpiScmiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/DevicePath.h>

#include <Guid/RpiScmiConfig.h>

//
// AutoGen emits these from RpiScmiConfigHii.vfr and RpiScmiConfigDxe.uni.
//
extern UINT8  RpiScmiConfigHiiBin[];
extern UINT8  RpiScmiConfigDxeStrings[];

//
// Deliver the SCMI power policy this soon after entry, retrying until the
// channel is up; a one-shot, cancelled on the first success.
//
#define POLICY_DELIVER_INTERVAL  (250 * 10 * 1000)   // 250 ms in 100 ns units

//
// A blconfig region larger than this is not believable, and the config
// text worth scanning fits well inside the staging cap.
//
#define BLCONFIG_REGION_MAX  SIZE_64KB
#define BLCONFIG_STAGE_MAX   SIZE_8KB

STATIC EFI_EVENT  mPolicyTimer;

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH          VendorDevicePath;
  EFI_DEVICE_PATH_PROTOCOL    End;
} HII_VENDOR_DEVICE_PATH;
#pragma pack ()

STATIC HII_VENDOR_DEVICE_PATH  mVendorDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof (VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    RPI_SCMI_CONFIG_FORMSET_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      (UINT8)(END_DEVICE_PATH_LENGTH),
      (UINT8)((END_DEVICE_PATH_LENGTH) >> 8)
    }
  }
};

// ---------------------------------------------------------------------------
// blconfig (bootloader EEPROM config) parsing - moved here from the retired
// PowerButtonScmiDxe: EDK2 resolves the region from the VPU DTB and reads
// the scalar settings OP-TEE needs, then passes them over SCMI.
// ---------------------------------------------------------------------------

/**
  Accumulate Count big-endian 32-bit cells into a UINT64.
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
  address by walking every parent bus's "ranges" up to the root.
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
  the /aliases "blconfig" indirection.
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
  Read POWER_OFF_ON_HALT from the EEPROM bootloader config text. Only an
  exact POWER_OFF_ON_HALT=1 counts (U-Boot semantics). Absent/invalid ->
  FALSE (reset on button press, the compatible default).
**/
STATIC
BOOLEAN
ReadPowerOffOnHalt (
  VOID
  )
{
  STATIC CONST CHAR8    Key[]  = "POWER_OFF_ON_HALT=";
  CONST UINTN           KeyLen = sizeof (Key) - 1;
  CONST VOID            *Fdt;
  INT32                 Node;
  UINT64                RegionAddress;
  UINT64                RegionSize;
  UINTN                 DataLen;
  UINT8                 *Data;
  CONST volatile UINT8  *Region;
  UINTN                 Index;
  UINTN                 Pos;
  BOOLEAN               Result;

  Fdt = (CONST VOID *)(UINTN)FixedPcdGet32 (PcdFdtBaseAddress);
  if ((Fdt == NULL) || (FdtCheckHeader (Fdt) != 0)) {
    return FALSE;
  }

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

// ---------------------------------------------------------------------------
// One-shot SCMI power policy delivery.
// ---------------------------------------------------------------------------

/**
  Once the SCMI channel is up, tell OP-TEE the power-button policy over
  SCMI System Power (vendor SYSTEM_POWER_STATE_SET), then cancel this
  timer. OP-TEE uses the verdict when it executes a latched press through
  EL3 after its grace window.
**/
STATIC
VOID
EFIAPI
DeliverPolicyTick (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT32   In[2];
  UINT32   Out[1];
  BOOLEAN  PowerOff;

  if (!RpiScmiReady ()) {
    return;   // channel not up yet; next tick retries
  }

  PowerOff = ReadPowerOffOnHalt ();

  In[0] = 0;   // flags
  In[1] = PowerOff ? RPI_SCMI_SYS_SET_POLICY_OFF
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
    DEBUG ((
      DEBUG_INFO,
      "RpiScmiConfig: button policy delivered to OP-TEE (%a)\n",
      PowerOff ? "power off" : "reset"
      ));
    gBS->SetTimer (mPolicyTimer, TimerCancel, 0);
    gBS->CloseEvent (mPolicyTimer);
    mPolicyTimer = NULL;
  }
}

/**
  Create PowerProfile with the Balanced default when it is absent or the
  wrong size, so the browser's efivarstore reads have a well-formed
  variable and ActiveCoolerScmiDxe reads a valid profile.
**/
STATIC
VOID
EnsureProfileVariable (
  VOID
  )
{
  RPI_POWER_PROFILE  Profile;
  UINTN              Size;
  EFI_STATUS         Status;
  EFI_GUID               FormSetGuid = RPI_SCMI_CONFIG_FORMSET_GUID;

  Size   = sizeof (Profile);
  Status = gRT->GetVariable (
                  RPI_POWER_PROFILE_VARIABLE_NAME,
                  &FormSetGuid,
                  NULL,
                  &Size,
                  &Profile
                  );
  if (!EFI_ERROR (Status) && (Size == sizeof (Profile))) {
    return;
  }

  Profile.Profile = RPI_POWER_PROFILE_BALANCED;
  Status          = gRT->SetVariable (
                           RPI_POWER_PROFILE_VARIABLE_NAME,
                           &FormSetGuid,
                           EFI_VARIABLE_NON_VOLATILE |
                           EFI_VARIABLE_BOOTSERVICE_ACCESS |
                           EFI_VARIABLE_RUNTIME_ACCESS,
                           sizeof (Profile),
                           &Profile
                           );
  DEBUG ((DEBUG_INFO, "RpiScmiConfig: created default PowerProfile - %r\n", Status));
}

EFI_STATUS
EFIAPI
RpiScmiConfigEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS             Status;
  EFI_HANDLE             DriverHandle;
  EFI_HII_HANDLE         HiiHandle;
  EFI_GUID               FormSetGuid = RPI_SCMI_CONFIG_FORMSET_GUID;

  EnsureProfileVariable ();

  DriverHandle = NULL;
  Status       = gBS->InstallMultipleProtocolInterfaces (
                        &DriverHandle,
                        &gEfiDevicePathProtocolGuid,
                        &mVendorDevicePath,
                        NULL
                        );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  HiiHandle = HiiAddPackages (
                &FormSetGuid,
                DriverHandle,
                RpiScmiConfigDxeStrings,
                RpiScmiConfigHiiBin,
                NULL
                );
  if (HiiHandle == NULL) {
    gBS->UninstallMultipleProtocolInterfaces (
           DriverHandle,
           &gEfiDevicePathProtocolGuid,
           &mVendorDevicePath,
           NULL
           );
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Arm the one-shot SCMI policy delivery. Not fatal if it cannot start:
  // OP-TEE defaults the button to reset, so only the power-off policy is
  // lost, and only if this never runs.
  //
  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  DeliverPolicyTick,
                  NULL,
                  &mPolicyTimer
                  );
  if (!EFI_ERROR (Status)) {
    gBS->SetTimer (mPolicyTimer, TimerPeriodic, POLICY_DELIVER_INTERVAL);
  }

  DEBUG ((DEBUG_INFO, "RpiScmiConfig: Power Profile page published\n"));
  return EFI_SUCCESS;
}
