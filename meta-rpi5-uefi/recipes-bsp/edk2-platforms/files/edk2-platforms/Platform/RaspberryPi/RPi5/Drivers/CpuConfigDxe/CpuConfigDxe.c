/** @file

  CpuConfigDxe - the "CPU Configuration" page in Setup's Device Manager:
  an explicit ARM frequency cap, up to the customary 3.0 GHz Pi 5
  overclock, in Processor-schema terms (SpeedLimitMHz / SpeedLocked).

  Why this is (mostly) a config.txt editor and not a clock driver: the
  BCM2712's ARM *ceiling* - the DVFS operating-point table and its
  voltage curve - is fixed at power-on by the VPU bootloader from
  config.txt (arm_freq, with over_voltage_delta for headroom above
  stock) and is not changeable at runtime, so the persistent policy IS
  the config.txt content and a ceiling change takes effect at the next
  reset. Below the ceiling, though, the clock is a mailbox call away:
  this driver also sets the ARM clock for the CURRENT boot through
  RASPBERRY_PI_FIRMWARE_PROTOCOL (min(cap, this boot's ceiling)), which
  is what lets the shipped config.txt carry no clock lines at all - the
  firmware requests its own speed instead of a static force_turbo=1
  doing it (the RPi4 ConfigDxe model).

  Split of responsibilities:

  1. The questions bind to the CpuClockPolicy efivarstore - the variable
     is the source of truth. The speed questions carry the standard
     Processor.v1_14_0 configure language, so a BMC PATCH of
     /Systems/1/Processors/{id} lands here through RedfishProcessorDxe;
     the over-voltage delta remains the CpuOverVoltageDeltaUv BIOS
     attribute.

  2. The managed block in config.txt is derived state: a ReadyToBoot
     sync reconverges it to the variable every boot (quiet, write only
     on drift - this also restores the policy after an sdimg reflash
     resets config.txt), and the page's interactive "apply" action runs
     the same sync immediately, then offers the reset. The block now
     carries the whole clock policy, force_turbo included - it also
     overrides the force_turbo=1 that older shipped images still set
     outside the block (last assignment wins).

  3. The current boot's clock is converged to the same policy via the
     mailbox at driver entry, after an interactive apply, and at
     ReadyToBoot (picking up a BMC write that landed this boot) -
     capping applies immediately; raising above this boot's power-on
     ceiling still needs the reset.

  The VPU firmware thermal-throttles regardless of what is configured
  here, and the OP-TEE fan governor sees the extra heat like any other;
  an overclock an individual SoC cannot hold can still hang or fail to
  boot, recovered by editing config.txt from any FAT reader (the BMC's
  mass-storage view included).

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <IndustryStandard/RpiMbox.h>

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
#include <Protocol/RpiFirmware.h>

#include <Guid/RpiCpuClockPolicy.h>

//
// AutoGen emits these from CpuConfigHii.vfr and CpuConfigDxe.uni.
//
extern UINT8  CpuConfigHiiBin[];
extern UINT8  CpuConfigDxeStrings[];

#define POPUP_ATTRIBUTES  (EFI_LIGHTGRAY | EFI_BACKGROUND_BLUE)

//
// The managed block's markers. The BEGIN line carries extra prose
// after the marker; searches match the marker prefix only.
//
#define CPU_BLOCK_BEGIN  "# BEGIN CpuConfigDxe"
#define CPU_BLOCK_END    "# END CpuConfigDxe"

//
// The override file this driver owns. config.txt includes it as its
// very last directive, so everything in it wins over anything the
// shipped file sets (last assignment rules the VPU's parse) - and
// config.txt itself stays untouched in steady state. Its absence is
// benign to the VPU (a missing include is skipped), so creating it on
// first use is safe.
//
#define CPU_OVERRIDE_FILE  L"uefi-cfg.txt"
#define CPU_INCLUDE_LINE   "include uefi-cfg.txt"

//
// Appended once to a config.txt that predates the include (an image
// flashed before uefi-cfg.txt existed, kept current by capsule updates
// that never touch the boot volume's files): without it the VPU would
// never read the override file and the clock policy would silently
// stop applying on exactly those boards.
//
STATIC CONST CHAR8  mIncludeStanza[] =
  "[all]\n"
  "# Load the EDK2-managed override file (see uefi-cfg.txt).\n"
  CPU_INCLUDE_LINE "\n";

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
  Policy->SpeedLimitMhz      = 0;
  Policy->SpeedLocked        = 1;
  Policy->Reserved           = 0;
  Policy->OverVoltageDeltaUv = RPI_CPU_CLOCK_DELTA_DEFAULT_UV;
}

/**
  The frequency cap a policy asks for: 0 for "no override" (stock,
  managed block removed), otherwise the limit clamped to the supported
  range (the VFR bounds it too; belt and braces for a BMC-written
  variable - a limit below the hardware floor is best-effort = floor).
**/
STATIC
UINT32
EffectiveMhz (
  IN CONST RPI_CPU_CLOCK_POLICY  *Policy
  )
{
  UINT32  Mhz;

  Mhz = Policy->SpeedLimitMhz;
  if (Mhz == 0) {
    return 0;
  }

  if (Mhz < RPI_CPU_CLOCK_MIN_MHZ) {
    Mhz = RPI_CPU_CLOCK_MIN_MHZ;
  }

  if (Mhz > RPI_CPU_CLOCK_MAX_MHZ) {
    Mhz = RPI_CPU_CLOCK_MAX_MHZ;
  }

  return Mhz;
}

/**
  The over-voltage delta with the DELTA_MAX cap enforced in C - the old
  layout only bounded it in VFR, so a raw BMC write could put an
  arbitrary voltage request straight into config.txt.
**/
STATIC
UINT32
EffectiveDeltaUv (
  IN CONST RPI_CPU_CLOCK_POLICY  *Policy
  )
{
  if (Policy->OverVoltageDeltaUv > RPI_CPU_CLOCK_DELTA_MAX_UV) {
    return RPI_CPU_CLOCK_DELTA_MAX_UV;
  }

  return Policy->OverVoltageDeltaUv;
}

/**
  Converge the CURRENT boot's ARM clock to the policy through the VPU
  mailbox: min(cap, this boot's power-on ceiling), or the ceiling when
  no cap is set. This is what replaces the force_turbo=1 the shipped
  config.txt used to carry - the firmware phase runs at the allowed
  maximum because this driver asked for it, not because a static file
  pinned it. SpeedLocked deliberately plays no part here: it governs
  OS-phase behavior through the managed block; under firmware there is
  no DVFS governor to leave in charge, so not requesting a clock would
  just mean booting slowly.

  Fail-open everywhere: no mailbox protocol yet (dispatch order) or a
  refused call leaves the clock as the VPU set it, and the ReadyToBoot
  pass retries once the protocol certainly exists.
**/
STATIC
VOID
ApplyClockThisBoot (
  IN CONST RPI_CPU_CLOCK_POLICY  *Policy
  )
{
  EFI_STATUS                      Status;
  RASPBERRY_PI_FIRMWARE_PROTOCOL  *FwProtocol;
  UINT32                          CeilingHz;
  UINT32                          TargetHz;
  UINT32                          CapMhz;

  Status = gBS->LocateProtocol (
                  &gRaspberryPiFirmwareProtocolGuid,
                  NULL,
                  (VOID **)&FwProtocol
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  Status = FwProtocol->GetMaxClockRate (RPI_MBOX_CLOCK_RATE_ARM, &CeilingHz);
  if (EFI_ERROR (Status) || (CeilingHz == 0)) {
    DEBUG ((DEBUG_WARN, "CpuConfigDxe: ARM max clock query failed - %r\n", Status));
    return;
  }

  TargetHz = CeilingHz;
  CapMhz   = EffectiveMhz (Policy);
  if ((CapMhz != 0) && (CapMhz < (CeilingHz / 1000000U))) {
    TargetHz = CapMhz * 1000000U;
  }

  Status = FwProtocol->SetClockRate (RPI_MBOX_CLOCK_RATE_ARM, TargetHz, FALSE);
  DEBUG ((
    DEBUG_INFO,
    "CpuConfigDxe: ARM clock for this boot: %u MHz (ceiling %u MHz) - %r\n",
    TargetHz / 1000000U,
    CeilingHz / 1000000U,
    Status
    ));
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

//
// The retired profile-based layout (7 bytes, packed): Profile 0..3
// selected Default/2800/3000/Custom, CustomMhz fed the Custom profile.
// Read only to migrate an existing variable in place.
//
#pragma pack (1)
typedef struct {
  UINT8     Profile;
  UINT16    CustomMhz;
  UINT32    OverVoltageDeltaUv;
} CPU_LEGACY_CLOCK_POLICY;
#pragma pack ()

#define CPU_LEGACY_PROFILE_OC_2800  1
#define CPU_LEGACY_PROFILE_OC_3000  2
#define CPU_LEGACY_PROFILE_CUSTOM   3

/**
  Create CpuClockPolicy with stock defaults when it is absent or
  malformed, migrating the retired 7-byte profile layout in place (the
  layouts are distinguishable by size alone): the profile becomes the
  equivalent explicit cap, locked, keeping the stored delta.
**/
STATIC
VOID
EnsurePolicyVariable (
  VOID
  )
{
  RPI_CPU_CLOCK_POLICY     Policy;
  CPU_LEGACY_CLOCK_POLICY  Legacy;
  UINT8                    Raw[16];
  UINTN                    Size;
  EFI_STATUS               Status;

  Size   = sizeof (Raw);
  Status = gRT->GetVariable (
                  RPI_CPU_CLOCK_POLICY_VARIABLE_NAME,
                  &gRpiCpuConfigFormSetGuid,
                  NULL,
                  &Size,
                  Raw
                  );
  if (!EFI_ERROR (Status) && (Size == sizeof (Policy))) {
    return;
  }

  DefaultPolicy (&Policy);

  if (!EFI_ERROR (Status) && (Size == sizeof (Legacy))) {
    CopyMem (&Legacy, Raw, sizeof (Legacy));
    Policy.OverVoltageDeltaUv = Legacy.OverVoltageDeltaUv;
    switch (Legacy.Profile) {
      case CPU_LEGACY_PROFILE_OC_2800:
        Policy.SpeedLimitMhz = 2800;
        break;
      case CPU_LEGACY_PROFILE_OC_3000:
        Policy.SpeedLimitMhz = 3000;
        break;
      case CPU_LEGACY_PROFILE_CUSTOM:
        Policy.SpeedLimitMhz = Legacy.CustomMhz;
        break;
      default:
        //
        // Default profile (or garbage): no override, matching the old
        // "block removed" semantics.
        //
        break;
    }

    DEBUG ((
      DEBUG_INFO,
      "CpuConfigDxe: migrated profile %u to SpeedLimitMhz %u\n",
      Legacy.Profile,
      Policy.SpeedLimitMhz
      ));
  }

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
  Render the managed block for a policy. Always present: the block now
  carries the WHOLE clock policy - the shipped config.txt sets no clock
  keys at all, and the block must also override the force_turbo=1 that
  images flashed before this change still carry outside the markers
  (the last assignment wins in the VPU's parse). The [all] line lives
  INSIDE the markers so removal cannot strand it, and neutralizes
  whatever section filter precedes the block.
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
  UINT32  DeltaUv;
  UINTN   Len;

  Mhz = EffectiveMhz (Policy);

  Len = AsciiSPrint (
          Buf,
          BufSize,
          CPU_BLOCK_BEGIN " - managed by Device Manager / CPU Configuration; do not hand-edit\n"
                          "[all]\n"
          );

  if (Mhz != 0) {
    Len += AsciiSPrint (&Buf[Len], BufSize - Len, "arm_freq=%u\n", Mhz);
  }

  //
  // SpeedLocked pins the cores at the ceiling for the OS phase too;
  // unlocked leaves DVFS free to scale below it. The firmware phase is
  // not affected either way - ApplyClockThisBoot() requests its own
  // speed through the mailbox.
  //
  Len += AsciiSPrint (
           &Buf[Len],
           BufSize - Len,
           "force_turbo=%u\n",
           (Policy->SpeedLocked != 0) ? 1u : 0u
           );

  DeltaUv = EffectiveDeltaUv (Policy);
  if ((Mhz > RPI_CPU_CLOCK_STOCK_MHZ) && (DeltaUv > 0)) {
    Len += AsciiSPrint (
             &Buf[Len],
             BufSize - Len,
             "over_voltage_delta=%u\n",
             DeltaUv
             );
  }

  Len += AsciiSPrint (&Buf[Len], BufSize - Len, CPU_BLOCK_END "\n");
  return Len;
}

/**
  Original content minus any existing managed block, plus Append (when
  AppendLen is nonzero) at EOF. A BEGIN marker without its END claims
  through to EOF (self-healing a damaged block). AppendLen of zero
  strips only - used to retire a legacy managed block from config.txt.
  Caller frees *NewContent.
**/
STATIC
EFI_STATUS
BuildDesiredContent (
  IN  CONST CHAR8  *Content,
  IN  UINTN        ContentLen,
  IN  CONST CHAR8  *Append OPTIONAL,
  IN  UINTN        AppendLen,
  OUT CHAR8        **NewContent,
  OUT UINTN        *NewLen
  )
{
  CONST CHAR8  *BlockStart;
  CONST CHAR8  *EndMarker;
  CONST CHAR8  *Tail;
  UINTN        HeadLen;
  UINTN        TailLen;
  CHAR8        *Buf;
  UINTN        Len;

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

  Buf = AllocatePool (HeadLen + TailLen + AppendLen + 2);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Buf, Content, HeadLen);
  Len = HeadLen;
  if (TailLen != 0) {
    CopyMem (&Buf[Len], Tail, TailLen);
    Len += TailLen;
  }

  if ((Append != NULL) && (AppendLen != 0)) {
    if ((Len > 0) && (Buf[Len - 1] != '\n')) {
      Buf[Len++] = '\n';
    }

    CopyMem (&Buf[Len], Append, AppendLen);
    Len += AppendLen;
  }

  *NewContent = Buf;
  *NewLen     = Len;
  return EFI_SUCCESS;
}

/**
  Make config.txt ready for the override file, touching it at most
  once per lifetime: retire a legacy in-config.txt managed block (from
  firmware that predates uefi-cfg.txt) and append the include stanza
  when it is missing (an old flashed image kept current by capsule
  updates - those never touch the boot volume's files). In steady
  state this reads, matches, and writes nothing.

  @retval EFI_SUCCESS   config.txt carries the include (already, or
                        *Changed says a write ran).
  @retval EFI_NOT_FOUND No config.txt - not a state this driver invents
                        one from: the VPU booted somehow, and a fresh
                        file missing everything else the platform needs
                        would not help.
**/
STATIC
EFI_STATUS
EnsureConfigTxtIncludes (
  IN  EFI_FILE_PROTOCOL  *Root,
  OUT BOOLEAN            *Changed
  )
{
  EFI_STATUS  Status;
  CHAR8       *Content;
  UINTN       ContentLen;
  CHAR8       *Desired;
  UINTN       DesiredLen;
  BOOLEAN     NeedInclude;

  *Changed = FALSE;

  Status = RpiReadFileContent (Root, L"config.txt", (VOID **)&Content, &ContentLen);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  NeedInclude = (AsciiStrStr (Content, CPU_INCLUDE_LINE) == NULL);
  if (!NeedInclude && (AsciiStrStr (Content, CPU_BLOCK_BEGIN) == NULL)) {
    FreePool (Content);
    return EFI_SUCCESS;
  }

  Status = BuildDesiredContent (
             Content,
             ContentLen,
             NeedInclude ? mIncludeStanza : NULL,
             NeedInclude ? (sizeof (mIncludeStanza) - 1) : 0,
             &Desired,
             &DesiredLen
             );
  if (EFI_ERROR (Status)) {
    FreePool (Content);
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
  return Status;
}

/**
  Converge uefi-cfg.txt's managed block to the policy. Quiet and
  idempotent: reads, compares, writes only on drift - in place when the
  file exists (a mid-write power cut garbles a tail, never loses the
  file), created outright when it does not (the sdimg ships a skeleton,
  but its absence is benign: the VPU skips a missing include). Content
  outside the markers is a human's and survives.
**/
STATIC
EFI_STATUS
SyncOverrideFile (
  IN  EFI_FILE_PROTOCOL           *Root,
  IN  CONST RPI_CPU_CLOCK_POLICY  *Policy,
  OUT BOOLEAN                     *Changed
  )
{
  EFI_STATUS  Status;
  CHAR8       Block[256];
  UINTN       BlockLen;
  CHAR8       *Content;
  UINTN       ContentLen;
  BOOLEAN     Existed;
  CHAR8       *Desired;
  UINTN       DesiredLen;

  *Changed = FALSE;

  BlockLen = BuildManagedBlock (Policy, Block, sizeof (Block));

  Existed = TRUE;
  Status  = RpiReadFileContent (Root, CPU_OVERRIDE_FILE, (VOID **)&Content, &ContentLen);
  if (EFI_ERROR (Status)) {
    Existed    = FALSE;
    Content    = AllocateZeroPool (1);
    ContentLen = 0;
    if (Content == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
  }

  Status = BuildDesiredContent (Content, ContentLen, Block, BlockLen, &Desired, &DesiredLen);
  if (EFI_ERROR (Status)) {
    FreePool (Content);
    return Status;
  }

  if (Existed && (DesiredLen == ContentLen) && (CompareMem (Desired, Content, ContentLen) == 0)) {
    Status = EFI_SUCCESS;
  } else {
    Status = Existed ?
             RpiRewriteFileInPlace (Root, CPU_OVERRIDE_FILE, Desired, DesiredLen) :
             RpiReplaceFileContent (Root, CPU_OVERRIDE_FILE, Desired, DesiredLen);
    if (!EFI_ERROR (Status)) {
      *Changed = TRUE;
    }
  }

  FreePool (Desired);
  FreePool (Content);
  return Status;
}

/**
  Converge the boot volume to the policy: config.txt readied (include
  present, legacy block retired), then the override file's managed
  block rebuilt. No popups here; callers own the UI.

  @retval EFI_SUCCESS   In sync (already, or *Changed says a write ran).
  @retval EFI_NOT_FOUND No boot volume, or it carries no config.txt.
**/
STATIC
EFI_STATUS
SyncClockOverrides (
  IN  CONST RPI_CPU_CLOCK_POLICY  *Policy,
  OUT BOOLEAN                     *Changed
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Root;
  BOOLEAN            CfgChanged;
  BOOLEAN            OverrideChanged;

  *Changed = FALSE;

  Status = RpiOpenBootVolume (&Root);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Status = EnsureConfigTxtIncludes (Root, &CfgChanged);
  if (!EFI_ERROR (Status)) {
    Status   = SyncOverrideFile (Root, Policy, &OverrideChanged);
    *Changed = CfgChanged || OverrideChanged;
  }

  Root->Close (Root);
  return Status;
}

/**
  Refresh the read-only "configured for the next boot" line from the
  files themselves, at form open: config.txt first, then - when it
  includes the override file - uefi-cfg.txt on top, matching the VPU's
  last-assignment-wins order (the include is config.txt's last
  directive). Deliberately worded as what the files request, not what
  the cores run at: after an apply the files are ahead of the DVFS
  table until reset.
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
  CHAR8              *Override;
  UINTN              OverrideLen;
  UINT32             Mhz;
  UINT32             DeltaUv;
  UINT32             Locked;
  CHAR16             Line[80];

  Root   = NULL;
  Status = RpiOpenBootVolume (&Root);
  if (!EFI_ERROR (Status)) {
    Status = RpiReadFileContent (Root, L"config.txt", (VOID **)&Content, &ContentLen);
  }

  if (EFI_ERROR (Status)) {
    if (Root != NULL) {
      Root->Close (Root);
    }

    UnicodeSPrint (Line, sizeof (Line), L"unknown - boot volume not accessible");
  } else {
    Mhz     = ParseLastValue (Content, "arm_freq", RPI_CPU_CLOCK_STOCK_MHZ);
    DeltaUv = ParseLastValue (Content, "over_voltage_delta", 0);
    //
    // No force_turbo line anywhere means DVFS rules (the VPU default);
    // config.txt no longer sets it, the managed block always does, and
    // images from before this change carry their own line for the
    // parse to find.
    //
    Locked = ParseLastValue (Content, "force_turbo", 0);

    if (AsciiStrStr (Content, CPU_INCLUDE_LINE) != NULL) {
      Status = RpiReadFileContent (Root, CPU_OVERRIDE_FILE, (VOID **)&Override, &OverrideLen);
      if (!EFI_ERROR (Status)) {
        Mhz     = ParseLastValue (Override, "arm_freq", Mhz);
        DeltaUv = ParseLastValue (Override, "over_voltage_delta", DeltaUv);
        Locked  = ParseLastValue (Override, "force_turbo", Locked);
        FreePool (Override);
      }
    }

    Root->Close (Root);
    FreePool (Content);

    if (DeltaUv != 0) {
      UnicodeSPrint (
        Line,
        sizeof (Line),
        L"%u MHz%s, over_voltage_delta %u uV",
        Mhz,
        (Locked == 0) ? L" (DVFS enabled)" : L"",
        DeltaUv
        );
    } else {
      UnicodeSPrint (
        Line,
        sizeof (Line),
        L"%u MHz%s",
        Mhz,
        (Locked == 0) ? L" (DVFS enabled)" : L""
        );
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

  //
  // The current boot honors the new policy immediately, up to this
  // boot's power-on ceiling; only raising the ceiling needs the reset.
  //
  ApplyClockThisBoot (&Policy);

  Status = SyncClockOverrides (&Policy, &Changed);
  if (Status == EFI_NOT_FOUND) {
    PopupWait (
      L"No boot volume with a config.txt found.",
      L"The profile is saved and will be applied on a later boot."
      );
    return;
  }

  if (EFI_ERROR (Status)) {
    PopupWait (L"Rewriting uefi-cfg.txt on the boot volume failed.", NULL);
    return;
  }

  UpdateConfiguredString ();

  if (!Changed) {
    PopupWait (L"uefi-cfg.txt already matches - nothing to apply.", NULL);
    return;
  }

  if (EffectiveMhz (&Policy) == 0) {
    UnicodeSPrint (
      Line,
      sizeof (Line),
      L"uefi-cfg.txt updated: stock configuration at the next boot."
      );
  } else {
    UnicodeSPrint (
      Line,
      sizeof (Line),
      L"uefi-cfg.txt updated: ARM frequency %u MHz at the next boot.",
      EffectiveMhz (&Policy)
      );
  }

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
  //
  // Converge the running clock too: a BMC Redfish write that landed
  // during this boot lowers the cap right here, and the entry-time
  // attempt is retried in case the mailbox protocol dispatched late.
  //
  ApplyClockThisBoot (&Policy);
  Status = SyncClockOverrides (&Policy, &Changed);
  if (Changed) {
    DEBUG ((
      DEBUG_INFO,
      "CpuConfigDxe: uefi-cfg.txt reconverged (limit %u MHz, locked %u; next boot)\n",
      EffectiveMhz (&Policy),
      Policy.SpeedLocked
      ));
  } else if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
    DEBUG ((DEBUG_WARN, "CpuConfigDxe: uefi-cfg.txt sync failed - %r\n", Status));
  }
}

EFI_STATUS
EFIAPI
CpuConfigEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS            Status;
  EFI_EVENT             Event;
  RPI_CPU_CLOCK_POLICY  Policy;

  EnsurePolicyVariable ();

  //
  // Ask for this boot's clock as early as possible - the shipped
  // config.txt no longer pins the cores, so until this call the SoC
  // runs at whatever the VPU left it (fail-open when the mailbox
  // protocol has not dispatched yet; ReadyToBoot retries).
  //
  GetPolicyVariable (&Policy);
  ApplyClockThisBoot (&Policy);

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
