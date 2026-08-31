/** @file

  CpuConfigDxe - the "CPU Configuration" page in Setup's Device Manager:
  the ARM clock profile, up to the customary 3.0 GHz Pi 5 overclock.

  Why this is a config.txt editor and not a clock driver: the BCM2712's
  ARM ceiling is fixed at power-on by the VPU bootloader from config.txt
  (arm_freq, with over_voltage_delta for headroom above stock), and our
  shipped config.txt pins the cores there with force_turbo=1. Neither is
  changeable at runtime, so the profile IS the config.txt content and a
  change takes effect at the next reset.

  Split of responsibilities:

  1. The questions bind to the CpuClockPolicy efivarstore - the variable
     is the source of truth (a BMC write through the x-UEFI-redfish map
     lands in the same place).

  2. The managed block in config.txt is derived state: a ReadyToBoot
     sync reconverges it to the variable every boot (quiet, write only
     on drift - this also restores the profile after an sdimg reflash
     resets config.txt), and the page's interactive "apply" action runs
     the same sync immediately, then offers the reset.

  The VPU firmware thermal-throttles regardless of what is configured
  here, and the OP-TEE fan governor sees the extra heat like any other;
  an overclock an individual SoC cannot hold can still hang or fail to
  boot, recovered by editing config.txt from any FAT reader (the BMC's
  mass-storage view included).

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HiiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/RpiBootVolumeLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiHiiServicesLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/HiiConfigAccess.h>

#include <Guid/RpiCpuClockPolicy.h>

//
// AutoGen emits these from CpuConfigHii.vfr and CpuConfigDxe.uni.
//
extern UINT8  CpuConfigHiiBin[];
extern UINT8  CpuConfigDxeStrings[];

#define POPUP_ATTRIBUTES  (EFI_LIGHTGRAY | EFI_BACKGROUND_BLUE)

//
// The managed block's markers in config.txt. The BEGIN line carries
// extra prose after the marker; searches match the marker prefix only.
//
#define CPU_BLOCK_BEGIN  "# BEGIN CpuConfigDxe"
#define CPU_BLOCK_END    "# END CpuConfigDxe"

STATIC EFI_HANDLE      mDriverHandle;
STATIC EFI_HII_HANDLE  mHiiHandle;

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
    RPI_CPU_CONFIG_FORMSET_GUID
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

STATIC
VOID
DefaultPolicy (
  OUT RPI_CPU_CLOCK_POLICY  *Policy
  )
{
  Policy->Profile            = RPI_CPU_CLOCK_PROFILE_DEFAULT;
  Policy->CustomMhz          = RPI_CPU_CLOCK_DEFAULT_MHZ;
  Policy->OverVoltageDeltaUv = RPI_CPU_CLOCK_DELTA_DEFAULT_UV;
}

/**
  The frequency a policy asks for, with the Custom value clamped to the
  supported range (the VFR enforces it too; belt and braces for a
  BMC-written variable).
**/
STATIC
UINT32
EffectiveMhz (
  IN CONST RPI_CPU_CLOCK_POLICY  *Policy
  )
{
  UINT32  Mhz;

  switch (Policy->Profile) {
    case RPI_CPU_CLOCK_PROFILE_OC_2800:
      return 2800;
    case RPI_CPU_CLOCK_PROFILE_OC_3000:
      return 3000;
    case RPI_CPU_CLOCK_PROFILE_CUSTOM:
      Mhz = Policy->CustomMhz;
      if (Mhz < RPI_CPU_CLOCK_MIN_MHZ) {
        Mhz = RPI_CPU_CLOCK_MIN_MHZ;
      }

      if (Mhz > RPI_CPU_CLOCK_MAX_MHZ) {
        Mhz = RPI_CPU_CLOCK_MAX_MHZ;
      }

      return Mhz;
    default:
      return RPI_CPU_CLOCK_DEFAULT_MHZ;
  }
}

STATIC
VOID
GetPolicyVariable (
  OUT RPI_CPU_CLOCK_POLICY  *Policy
  )
{
  UINTN       Size;
  EFI_STATUS  Status;

  Size   = sizeof (*Policy);
  Status = gRT->GetVariable (
                  RPI_CPU_CLOCK_POLICY_VARIABLE_NAME,
                  &gRpiCpuConfigFormSetGuid,
                  NULL,
                  &Size,
                  Policy
                  );
  if (EFI_ERROR (Status) || (Size != sizeof (*Policy))) {
    DefaultPolicy (Policy);
  }
}

/**
  Create CpuClockPolicy with stock defaults when it is absent or has
  been written with the wrong size: the browser's efivarstore reads need
  a well-formed variable to edit (the FanConfigDxe idiom).
**/
STATIC
VOID
EnsurePolicyVariable (
  VOID
  )
{
  RPI_CPU_CLOCK_POLICY  Policy;
  UINTN                 Size;
  EFI_STATUS            Status;

  Size   = sizeof (Policy);
  Status = gRT->GetVariable (
                  RPI_CPU_CLOCK_POLICY_VARIABLE_NAME,
                  &gRpiCpuConfigFormSetGuid,
                  NULL,
                  &Size,
                  &Policy
                  );
  if (!EFI_ERROR (Status) && (Size == sizeof (Policy))) {
    return;
  }

  DefaultPolicy (&Policy);
  Status = gRT->SetVariable (
                  RPI_CPU_CLOCK_POLICY_VARIABLE_NAME,
                  &gRpiCpuConfigFormSetGuid,
                  EFI_VARIABLE_NON_VOLATILE |
                  EFI_VARIABLE_BOOTSERVICE_ACCESS |
                  EFI_VARIABLE_RUNTIME_ACCESS,
                  sizeof (Policy),
                  &Policy
                  );
  DEBUG ((DEBUG_INFO, "CpuConfigDxe: created default CpuClockPolicy - %r\n", Status));
}

/**
  Blocking popup: waits for a key.
**/
STATIC
VOID
PopupWait (
  IN CHAR16  *Line1,
  IN CHAR16  *Line2  OPTIONAL
  )
{
  EFI_INPUT_KEY  Key;

  if (Line2 != NULL) {
    CreatePopUp (POPUP_ATTRIBUTES, &Key, Line1, Line2, NULL);
  } else {
    CreatePopUp (POPUP_ATTRIBUTES, &Key, Line1, NULL);
  }
}

//
// -------------------------------------------------------- config.txt --
//

/**
  Last-occurrence value of "Key=<decimal>" in NUL-terminated config.txt
  content, Fallback when absent. Last wins, matching the VPU's parse;
  section filters ([pi5], [all]) are NOT emulated - good enough for the
  files this platform ships, where the managed block sits under [all] at
  EOF and nothing else sets these keys.
**/
STATIC
UINT32
ParseLastValue (
  IN CONST CHAR8  *Content,
  IN CONST CHAR8  *Key,
  IN UINT32       Fallback
  )
{
  CONST CHAR8  *Line;
  CONST CHAR8  *P;
  UINTN        KeyLen;
  UINT32       Value;

  KeyLen = AsciiStrLen (Key);
  Value  = Fallback;

  for (Line = Content; *Line != '\0'; ) {
    P = Line;
    while ((*P == ' ') || (*P == '\t')) {
      P++;
    }

    if ((AsciiStrnCmp (P, Key, KeyLen) == 0) && (P[KeyLen] == '=')) {
      Value = (UINT32)AsciiStrDecimalToUintn (&P[KeyLen + 1]);
    }

    while ((*Line != '\0') && (*Line != '\n')) {
      Line++;
    }

    if (*Line == '\n') {
      Line++;
    }
  }

  return Value;
}

/**
  Render the managed block for a policy; zero length for the Default
  profile (stock speed = no block at all, the file returns pristine).
  The [all] line lives INSIDE the markers so removal cannot strand it,
  and neutralizes whatever section filter precedes the block.
**/
STATIC
UINTN
BuildManagedBlock (
  IN  CONST RPI_CPU_CLOCK_POLICY  *Policy,
  OUT CHAR8                       *Buf,
  IN  UINTN                       BufSize
  )
{
  UINT32  Mhz;
  UINTN   Len;

  if (Policy->Profile == RPI_CPU_CLOCK_PROFILE_DEFAULT) {
    return 0;
  }

  Mhz = EffectiveMhz (Policy);
  Len = AsciiSPrint (
          Buf,
          BufSize,
          CPU_BLOCK_BEGIN " - managed by Device Manager / CPU Configuration; do not hand-edit\n"
                          "[all]\n"
                          "arm_freq=%u\n",
          Mhz
          );

  if ((Mhz > RPI_CPU_CLOCK_DEFAULT_MHZ) && (Policy->OverVoltageDeltaUv > 0)) {
    Len += AsciiSPrint (
             &Buf[Len],
             BufSize - Len,
             "over_voltage_delta=%u\n",
             Policy->OverVoltageDeltaUv
             );
  }

  Len += AsciiSPrint (&Buf[Len], BufSize - Len, CPU_BLOCK_END "\n");
  return Len;
}

/**
  Original content minus any existing managed block, plus the policy's
  block at EOF. A BEGIN marker without its END claims through to EOF
  (self-healing a damaged block). Caller frees *NewContent.
**/
STATIC
EFI_STATUS
BuildDesiredContent (
  IN  CONST CHAR8                 *Content,
  IN  UINTN                       ContentLen,
  IN  CONST RPI_CPU_CLOCK_POLICY  *Policy,
  OUT CHAR8                       **NewContent,
  OUT UINTN                       *NewLen
  )
{
  CHAR8        Block[256];
  UINTN        BlockLen;
  CONST CHAR8  *BlockStart;
  CONST CHAR8  *EndMarker;
  CONST CHAR8  *Tail;
  UINTN        HeadLen;
  UINTN        TailLen;
  CHAR8        *Buf;
  UINTN        Len;

  BlockLen = BuildManagedBlock (Policy, Block, sizeof (Block));

  BlockStart = AsciiStrStr (Content, CPU_BLOCK_BEGIN);
  if (BlockStart != NULL) {
    HeadLen   = (UINTN)(BlockStart - Content);
    EndMarker = AsciiStrStr (BlockStart, CPU_BLOCK_END);
    if (EndMarker != NULL) {
      Tail = EndMarker;
      while ((*Tail != '\0') && (*Tail != '\n')) {
        Tail++;
      }

      if (*Tail == '\n') {
        Tail++;
      }

      TailLen = ContentLen - (UINTN)(Tail - Content);
    } else {
      Tail    = NULL;
      TailLen = 0;
    }
  } else {
    HeadLen = ContentLen;
    Tail    = NULL;
    TailLen = 0;
  }

  Buf = AllocatePool (HeadLen + TailLen + BlockLen + 2);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Buf, Content, HeadLen);
  Len = HeadLen;
  if (TailLen != 0) {
    CopyMem (&Buf[Len], Tail, TailLen);
    Len += TailLen;
  }

  if (BlockLen != 0) {
    if ((Len > 0) && (Buf[Len - 1] != '\n')) {
      Buf[Len++] = '\n';
    }

    CopyMem (&Buf[Len], Block, BlockLen);
    Len += BlockLen;
  }

  *NewContent = Buf;
  *NewLen     = Len;
  return EFI_SUCCESS;
}

/**
  Converge config.txt's managed block to the policy. Quiet and
  idempotent: reads, compares, writes only on drift (in place - see
  RpiRewriteFileInPlace on why the file must never transit through
  nonexistence). No popups here; callers own the UI.

  @retval EFI_SUCCESS   In sync (already, or *Changed says a write ran).
  @retval EFI_NOT_FOUND No boot volume, or it carries no config.txt.
**/
STATIC
EFI_STATUS
SyncConfigTxt (
  IN  CONST RPI_CPU_CLOCK_POLICY  *Policy,
  OUT BOOLEAN                     *Changed
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Root;
  CHAR8              *Content;
  UINTN              ContentLen;
  CHAR8              *Desired;
  UINTN              DesiredLen;

  *Changed = FALSE;

  Status = RpiOpenBootVolume (&Root);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Status = RpiReadFileContent (Root, L"config.txt", (VOID **)&Content, &ContentLen);
  if (EFI_ERROR (Status)) {
    //
    // No config.txt is not a state this driver invents one from: the
    // VPU booted somehow, and a fresh file missing everything else the
    // platform needs would not help.
    //
    Root->Close (Root);
    return EFI_NOT_FOUND;
  }

  Status = BuildDesiredContent (Content, ContentLen, Policy, &Desired, &DesiredLen);
  if (EFI_ERROR (Status)) {
    FreePool (Content);
    Root->Close (Root);
    return Status;
  }

  if ((DesiredLen == ContentLen) && (CompareMem (Desired, Content, ContentLen) == 0)) {
    Status = EFI_SUCCESS;
  } else {
    Status = RpiRewriteFileInPlace (Root, L"config.txt", Desired, DesiredLen);
    if (!EFI_ERROR (Status)) {
      *Changed = TRUE;
    }
  }

  FreePool (Desired);
  FreePool (Content);
  Root->Close (Root);
  return Status;
}

/**
  Refresh the read-only "configured in config.txt" line from the file
  itself, at form open. Deliberately worded as what the file requests,
  not what the cores run at: after an apply (or a ReadyToBoot sync on a
  failed boot attempt) the file is ahead of the silicon until reset.
**/
STATIC
VOID
UpdateConfiguredString (
  VOID
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Root;
  CHAR8              *Content;
  UINTN              ContentLen;
  UINT32             Mhz;
  UINT32             DeltaUv;
  CHAR16             Line[80];

  Status = RpiOpenBootVolume (&Root);
  if (!EFI_ERROR (Status)) {
    Status = RpiReadFileContent (Root, L"config.txt", (VOID **)&Content, &ContentLen);
    Root->Close (Root);
  }

  if (EFI_ERROR (Status)) {
    UnicodeSPrint (Line, sizeof (Line), L"unknown - boot volume not accessible");
  } else {
    Mhz     = ParseLastValue (Content, "arm_freq", RPI_CPU_CLOCK_DEFAULT_MHZ);
    DeltaUv = ParseLastValue (Content, "over_voltage_delta", 0);
    FreePool (Content);

    if (DeltaUv != 0) {
      UnicodeSPrint (Line, sizeof (Line), L"%u MHz, over_voltage_delta %u uV", Mhz, DeltaUv);
    } else {
      UnicodeSPrint (Line, sizeof (Line), L"%u MHz", Mhz);
    }
  }

  HiiSetString (mHiiHandle, STRING_TOKEN (STR_CONFIGURED_VALUE), Line, NULL);
}

/**
  The apply action: commit the browser's edited policy to the variable,
  converge config.txt now, offer the reset. Every early return has
  shown the user a popup (the BootloaderConfigDxe discipline).
**/
STATIC
VOID
ApplyNow (
  VOID
  )
{
  EFI_STATUS            Status;
  RPI_CPU_CLOCK_POLICY  Policy;
  BOOLEAN               Changed;
  CHAR16                Line[96];
  EFI_INPUT_KEY         Key;

  ZeroMem (&Policy, sizeof (Policy));
  if (!HiiGetBrowserData (
         &gRpiCpuConfigFormSetGuid,
         RPI_CPU_CLOCK_POLICY_VARIABLE_NAME,
         sizeof (Policy),
         (UINT8 *)&Policy
         ))
  {
    PopupWait (L"Could not read the edited settings from the form.", NULL);
    return;
  }

  //
  // Commit before writing the file: the variable is the source of
  // truth, and the ReadyToBoot sync would otherwise converge the file
  // straight back if the user discarded the form edits on exit.
  //
  Status = gRT->SetVariable (
                  RPI_CPU_CLOCK_POLICY_VARIABLE_NAME,
                  &gRpiCpuConfigFormSetGuid,
                  EFI_VARIABLE_NON_VOLATILE |
                  EFI_VARIABLE_BOOTSERVICE_ACCESS |
                  EFI_VARIABLE_RUNTIME_ACCESS,
                  sizeof (Policy),
                  &Policy
                  );
  if (EFI_ERROR (Status)) {
    PopupWait (L"Saving the CpuClockPolicy variable failed.", NULL);
    return;
  }

  Status = SyncConfigTxt (&Policy, &Changed);
  if (Status == EFI_NOT_FOUND) {
    PopupWait (
      L"No boot volume with a config.txt found.",
      L"The profile is saved and will be applied on a later boot."
      );
    return;
  }

  if (EFI_ERROR (Status)) {
    PopupWait (L"Rewriting config.txt on the boot volume failed.", NULL);
    return;
  }

  UpdateConfiguredString ();

  if (!Changed) {
    PopupWait (L"config.txt already matches - nothing to apply.", NULL);
    return;
  }

  UnicodeSPrint (
    Line,
    sizeof (Line),
    L"config.txt updated: ARM frequency %u MHz at the next boot.",
    EffectiveMhz (&Policy)
    );
  CreatePopUp (
    POPUP_ATTRIBUTES,
    &Key,
    Line,
    L"Press Y to reset now, any other key to continue.",
    NULL
    );
  if ((Key.UnicodeChar == L'y') || (Key.UnicodeChar == L'Y')) {
    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
  }
}

//
// EFI_HII_CONFIG_ACCESS_PROTOCOL. The questions bind to the
// CpuClockPolicy efivarstore, which the form browser reads and writes
// directly; ExtractConfig/RouteConfig are still implemented over the
// same variable for spec completeness, and Callback carries the apply
// action plus the form-open refresh of the read-only line (the
// BootloaderConfigDxe idiom).
//

STATIC
EFI_STATUS
EFIAPI
CpuExtractConfig (
  IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN  CONST EFI_STRING                      Request,
  OUT EFI_STRING                            *Progress,
  OUT EFI_STRING                            *Results
  )
{
  EFI_STATUS            Status;
  RPI_CPU_CLOCK_POLICY  Policy;
  EFI_STRING            ConfigRequest;
  EFI_STRING            ConfigRequestHdr;
  UINTN                 RequestSize;
  BOOLEAN               AllocatedRequest;

  if ((Progress == NULL) || (Results == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Progress = Request;
  if ((Request != NULL) &&
      !HiiIsConfigHdrMatch (
         Request,
         &gRpiCpuConfigFormSetGuid,
         RPI_CPU_CLOCK_POLICY_VARIABLE_NAME
         ))
  {
    return EFI_NOT_FOUND;
  }

  GetPolicyVariable (&Policy);

  ConfigRequest    = Request;
  AllocatedRequest = FALSE;
  if ((Request == NULL) || (StrStr (Request, L"OFFSET") == NULL)) {
    ConfigRequestHdr = HiiConstructConfigHdr (
                         &gRpiCpuConfigFormSetGuid,
                         RPI_CPU_CLOCK_POLICY_VARIABLE_NAME,
                         mDriverHandle
                         );
    if (ConfigRequestHdr == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    RequestSize   = (StrLen (ConfigRequestHdr) + 32) * sizeof (CHAR16);
    ConfigRequest = AllocateZeroPool (RequestSize);
    if (ConfigRequest == NULL) {
      FreePool (ConfigRequestHdr);
      return EFI_OUT_OF_RESOURCES;
    }

    UnicodeSPrint (
      ConfigRequest,
      RequestSize,
      L"%s&OFFSET=0&WIDTH=%016LX",
      ConfigRequestHdr,
      (UINT64)sizeof (Policy)
      );
    FreePool (ConfigRequestHdr);
    AllocatedRequest = TRUE;
  }

  Status = gHiiConfigRouting->BlockToConfig (
                                gHiiConfigRouting,
                                ConfigRequest,
                                (UINT8 *)&Policy,
                                sizeof (Policy),
                                Results,
                                Progress
                                );

  if (AllocatedRequest) {
    FreePool (ConfigRequest);
    if (Request == NULL) {
      *Progress = NULL;
    } else if (StrStr (Request, L"OFFSET") == NULL) {
      *Progress = Request + StrLen (Request);
    }
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
CpuRouteConfig (
  IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN  CONST EFI_STRING                      Configuration,
  OUT EFI_STRING                            *Progress
  )
{
  EFI_STATUS            Status;
  RPI_CPU_CLOCK_POLICY  Policy;
  UINTN                 Size;

  if (Progress == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Progress = Configuration;
  if (Configuration == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (!HiiIsConfigHdrMatch (
         Configuration,
         &gRpiCpuConfigFormSetGuid,
         RPI_CPU_CLOCK_POLICY_VARIABLE_NAME
         ))
  {
    return EFI_NOT_FOUND;
  }

  GetPolicyVariable (&Policy);

  Size   = sizeof (Policy);
  Status = gHiiConfigRouting->ConfigToBlock (
                                gHiiConfigRouting,
                                Configuration,
                                (UINT8 *)&Policy,
                                &Size,
                                Progress
                                );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return gRT->SetVariable (
                RPI_CPU_CLOCK_POLICY_VARIABLE_NAME,
                &gRpiCpuConfigFormSetGuid,
                EFI_VARIABLE_NON_VOLATILE |
                EFI_VARIABLE_BOOTSERVICE_ACCESS |
                EFI_VARIABLE_RUNTIME_ACCESS,
                sizeof (Policy),
                &Policy
                );
}

STATIC
EFI_STATUS
EFIAPI
CpuCallback (
  IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN  EFI_BROWSER_ACTION                    Action,
  IN  EFI_QUESTION_ID                       QuestionId,
  IN  UINT8                                 Type,
  IN  EFI_IFR_TYPE_VALUE                    *Value,
  OUT EFI_BROWSER_ACTION_REQUEST            *ActionRequest
  )
{
  if (QuestionId != RPI_CPU_CLOCK_KEY_APPLY) {
    return EFI_UNSUPPORTED;
  }

  if (Action == EFI_BROWSER_ACTION_FORM_OPEN) {
    UpdateConfiguredString ();
    return EFI_SUCCESS;
  }

  if (Action == EFI_BROWSER_ACTION_CHANGING) {
    return EFI_SUCCESS;
  }

  if (Action != EFI_BROWSER_ACTION_CHANGED) {
    return EFI_UNSUPPORTED;
  }

  ApplyNow ();
  return EFI_SUCCESS;
}

STATIC EFI_HII_CONFIG_ACCESS_PROTOCOL  mConfigAccess = {
  CpuExtractConfig,
  CpuRouteConfig,
  CpuCallback
};

/**
  Quiet boot-time convergence: config.txt follows the variable wherever
  it was written from (Setup without the apply action, a BMC Redfish
  write, or an sdimg reflash that reset the file). Skips silently when
  the boot volume is not connected (BootNext connects only its own
  path); a later boot catches it.
**/
STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  RPI_CPU_CLOCK_POLICY  Policy;
  BOOLEAN               Changed;
  EFI_STATUS            Status;

  GetPolicyVariable (&Policy);
  Status = SyncConfigTxt (&Policy, &Changed);
  if (Changed) {
    DEBUG ((
      DEBUG_INFO,
      "CpuConfigDxe: config.txt reconverged to %u MHz (next boot)\n",
      EffectiveMhz (&Policy)
      ));
  } else if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
    DEBUG ((DEBUG_WARN, "CpuConfigDxe: config.txt sync failed - %r\n", Status));
  }
}

EFI_STATUS
EFIAPI
CpuConfigEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;

  EnsurePolicyVariable ();

  mDriverHandle = NULL;
  Status        = gBS->InstallMultipleProtocolInterfaces (
                         &mDriverHandle,
                         &gEfiDevicePathProtocolGuid,
                         &mVendorDevicePath,
                         &gEfiHiiConfigAccessProtocolGuid,
                         &mConfigAccess,
                         NULL
                         );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mHiiHandle = HiiAddPackages (
                 &gRpiCpuConfigFormSetGuid,
                 mDriverHandle,
                 CpuConfigDxeStrings,
                 CpuConfigHiiBin,
                 NULL
                 );
  if (mHiiHandle == NULL) {
    gBS->UninstallMultipleProtocolInterfaces (
           mDriverHandle,
           &gEfiDevicePathProtocolGuid,
           &mVendorDevicePath,
           &gEfiHiiConfigAccessProtocolGuid,
           &mConfigAccess,
           NULL
           );
    return EFI_OUT_OF_RESOURCES;
  }

  Status = EfiCreateEventReadyToBootEx (
             TPL_CALLBACK,
             OnReadyToBoot,
             NULL,
             &Event
             );
  DEBUG ((DEBUG_INFO, "CpuConfigDxe: Setup page published - %r\n", Status));
  return EFI_SUCCESS;
}
