/** @file

  BootloaderConfigDxe Setup page and EEPROM update staging.

  The "Bootloader EEPROM" page in Setup's Device Manager binds its
  questions to the BlCfg efivarstore, which the core re-seeds from the
  live blconfig every boot. The page's one interactive action - "Stage
  EEPROM update and reboot" - is the ONLY path that touches the boot
  EEPROM, and only reads it:

    1. The edited values are fetched from the form browser and compared
       with the running config; no difference means nothing to do.
    2. The live 2 MiB image is read back over the boot SPI (read-only).
    3. bootconf.txt inside the image is patched with the edited values
       (partition A and, on A/B images, partition B), the self-update
       timestamp is refreshed, and the image's SHA-256 is computed.
    4. pieeprom.upd + pieeprom.sig are written to the boot volume (the
       one carrying armstub8-2712.bin / config.txt) - the exact contract
       rpi-eeprom-update uses for staged updates - and the staged values
       are recorded in the BlCfgStaged variable.
    5. The system reboots; the bootloader's self-update flashes the
       image. The following boot's ReadyToBoot handler sees the staged
       values live in blconfig and deletes the staged files.

  A refused signed config (bootconf.sig present) or a config with
  ENABLE_SELF_UPDATE=0 aborts before any file is written.

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
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiHiiServicesLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/HiiConfigAccess.h>
#include <Protocol/SimpleFileSystem.h>

#include "BootloaderConfig.h"
#include "Bcm2712SpiFlash.h"
#include "EepromImage.h"
#include "Sha256.h"

//
// AutoGen emits these from BootloaderConfigHii.vfr and
// BootloaderConfigDxe.uni.
//
extern UINT8  BootloaderConfigHiiBin[];
extern UINT8  BootloaderConfigDxeStrings[];

#define POPUP_ATTRIBUTES  (EFI_LIGHTGRAY | EFI_BACKGROUND_BLUE)

#define STAGED_UPDATE_FILE  L"pieeprom.upd"
#define STAGED_SIG_FILE     L"pieeprom.sig"

//
// New config text budget; the EEPROM tooling caps modifiable files at
// one erase sector anyway.
//
#define NEW_CONFIG_CAP  SIZE_4KB

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
    RPI_BLCFG_FORMSET_GUID
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

STATIC EFI_HANDLE      mDriverHandle;
STATIC EFI_HII_HANDLE  mHiiHandle;

/**
  Blocking popup: waits for a key so the message is actually seen.
**/
STATIC
VOID
PopupWait (
  IN CHAR16  *Line1,
  IN CHAR16  *Line2 OPTIONAL
  )
{
  EFI_INPUT_KEY  Key;

  if (Line2 != NULL) {
    CreatePopUp (POPUP_ATTRIBUTES, &Key, Line1, Line2, NULL);
  } else {
    CreatePopUp (POPUP_ATTRIBUTES, &Key, Line1, NULL);
  }
}

/**
  Non-blocking popup: draws and returns (progress messages).
**/
STATIC
VOID
PopupInfo (
  IN CHAR16  *Line1
  )
{
  CreatePopUp (POPUP_ATTRIBUTES, NULL, Line1, NULL);
}

/**
  Open the boot volume's root: the filesystem carrying our firmware
  (armstub8-2712.bin), falling back to any volume with a config.txt.
  This is the partition the VPU bootloader booted from, so it is where
  it will look for pieeprom.upd.
**/
STATIC
EFI_STATUS
OpenBootVolume (
  OUT EFI_FILE_PROTOCOL  **Root
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Sfs;
  EFI_FILE_PROTOCOL                *Candidate;
  EFI_FILE_PROTOCOL                *File;
  EFI_FILE_PROTOCOL                *Best;
  UINTN                            BestScore;
  UINTN                            Score;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Best      = NULL;
  BestScore = 0;

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Sfs
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    if (EFI_ERROR (Sfs->OpenVolume (Sfs, &Candidate))) {
      continue;
    }

    Score = 0;
    if (!EFI_ERROR (Candidate->Open (
                      Candidate,
                      &File,
                      L"armstub8-2712.bin",
                      EFI_FILE_MODE_READ,
                      0
                      )))
    {
      File->Close (File);
      Score = 2;
    } else if (!EFI_ERROR (Candidate->Open (
                             Candidate,
                             &File,
                             L"config.txt",
                             EFI_FILE_MODE_READ,
                             0
                             )))
    {
      File->Close (File);
      Score = 1;
    }

    if (Score > BestScore) {
      if (Best != NULL) {
        Best->Close (Best);
      }

      Best      = Candidate;
      BestScore = Score;
      if (Score == 2) {
        break;
      }
    } else {
      Candidate->Close (Candidate);
    }
  }

  FreePool (Handles);

  if (Best == NULL) {
    return EFI_NOT_FOUND;
  }

  *Root = Best;
  return EFI_SUCCESS;
}

STATIC
VOID
DeleteFileIfPresent (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CHAR16             *Name
  )
{
  EFI_FILE_PROTOCOL  *File;

  if (!EFI_ERROR (Root->Open (
                    Root,
                    &File,
                    Name,
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                    0
                    )))
  {
    //
    // Delete() closes the handle regardless of the outcome.
    //
    File->Delete (File);
  }
}

/**
  Create Name with exactly the given content, replacing any previous
  file (delete + recreate, so no stale tail can survive).
**/
STATIC
EFI_STATUS
ReplaceFileContent (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CHAR16             *Name,
  IN CONST VOID         *Data,
  IN UINTN              Len
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  UINTN              WriteLen;

  DeleteFileIfPresent (Root, Name);

  Status = Root->Open (
                   Root,
                   &File,
                   Name,
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                   0
                   );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  WriteLen = Len;
  Status   = File->Write (File, &WriteLen, (VOID *)Data);
  if (!EFI_ERROR (Status) && (WriteLen != Len)) {
    Status = EFI_DEVICE_ERROR;
  }

  if (!EFI_ERROR (Status)) {
    Status = File->Flush (File);
  }

  File->Close (File);

  if (EFI_ERROR (Status)) {
    DeleteFileIfPresent (Root, Name);
  }

  return Status;
}

/**
  Days-from-civil conversion (proleptic Gregorian), for EFI_TIME to Unix
  epoch seconds.
**/
STATIC
UINT32
EfiTimeToUnix (
  IN CONST EFI_TIME  *Time
  )
{
  INT64  Year;
  INT64  Era;
  INT64  Yoe;
  INT64  Doy;
  INT64  Doe;
  INT64  Days;

  Year = (INT64)Time->Year - ((Time->Month <= 2) ? 1 : 0);
  Era  = Year / 400;
  Yoe  = Year - Era * 400;
  Doy  = (153 * ((INT64)Time->Month + ((Time->Month > 2) ? -3 : 9)) + 2) / 5 +
         (INT64)Time->Day - 1;
  Doe  = Yoe * 365 + Yoe / 4 - Yoe / 100 + Doy;
  Days = Era * 146097 + Doe - 719468;

  return (UINT32)(Days * 86400 +
                  (INT64)Time->Hour * 3600 +
                  (INT64)Time->Minute * 60 +
                  (INT64)Time->Second);
}

/**
  Timestamp for the staged update: real time when the RTC is plausible,
  otherwise one past the currently installed EEPROM's timestamp - either
  way it differs from the installed one, which is what the bootloader's
  self-update compares.
**/
STATIC
UINT32
StagingTimestamp (
  VOID
  )
{
  EFI_TIME  Time;
  UINT32    Unix;

  if (!EFI_ERROR (gRT->GetTime (&Time, NULL)) &&
      (Time.Year >= 2025) && (Time.Year <= 2100))
  {
    Unix = EfiTimeToUnix (&Time);
    if (Unix > mDtbUpdateTimestamp) {
      return Unix;
    }
  }

  return mDtbUpdateTimestamp + 1;
}

/**
  Does a "key: value" line appear in a .sig file? rpi-eeprom-digest writes
  one directive per line; the key of interest here is "rsa2048".
**/
STATIC
BOOLEAN
BlTextHasLineKey (
  IN CONST CHAR8  *Text,
  IN UINTN        Len,
  IN CONST CHAR8  *Key
  )
{
  UINTN  KeyLen;
  UINTN  Index;
  UINTN  Start;

  if ((Text == NULL) || (Key == NULL)) {
    return FALSE;
  }

  KeyLen = AsciiStrLen (Key);
  Start  = 0;

  for (Index = 0; Index <= Len; Index++) {
    if ((Index == Len) || (Text[Index] == '\n') || (Text[Index] == '\r')) {
      if (((Index - Start) > KeyLen) &&
          (CompareMem (&Text[Start], Key, KeyLen) == 0) &&
          (Text[Start + KeyLen] == ':'))
      {
        return TRUE;
      }

      Start = Index + 1;
    }
  }

  return FALSE;
}

/**
  Rebuild a digest-only bootconf.sig over the replacement config text, in
  the exact shape "rpi-eeprom-digest -i bootconf.txt -o bootconf.sig"
  produces: the hex SHA-256, then a "ts:" line. No target-soc line -- that
  one only appears when the tool is given -c, which the config digest is not.

  Best effort: a config whose sig cannot be rewritten is reported by the
  caller, not silently accepted.
**/
STATIC
EFI_STATUS
SyncConfigDigest (
  IN OUT UINT8        *Image,
  IN     UINTN        Size,
  IN     UINTN        WalkStart,
  IN     UINTN        WinStart,
  IN     UINTN        WinEnd,
  IN     CONST CHAR8  *Config,
  IN     UINTN        ConfigLen,
  IN     UINT32       Timestamp
  )
{
  UINT8  Digest[BLCFG_SHA256_DIGEST_SIZE];
  CHAR8  Sig[128];
  UINTN  SigLen;
  UINTN  Index;

  BlSha256 ((CONST UINT8 *)Config, ConfigLen, Digest);

  SigLen = 0;
  for (Index = 0; Index < BLCFG_SHA256_DIGEST_SIZE; Index++) {
    SigLen += AsciiSPrint (&Sig[SigLen], sizeof (Sig) - SigLen, "%02x", Digest[Index]);
  }

  SigLen += AsciiSPrint (&Sig[SigLen], sizeof (Sig) - SigLen, "\nts: %u\n", Timestamp);

  return EepromReplaceFileIn (
           Image,
           Size,
           WalkStart,
           WinStart,
           WinEnd,
           "bootconf.sig",
           (CONST UINT8 *)Sig,
           SigLen
           );
}

/**
  Patch bootconf.txt (and updatetime) with Values inside a freshly read
  EEPROM image.
**/
STATIC
EFI_STATUS
PatchImage (
  IN OUT UINT8               *Image,
  IN     UINTN               Size,
  IN     CONST BLCFG_VALUES  *Values,
  IN     UINT32              Timestamp,
  OUT    CHAR16              **FailLine
  )
{
  EFI_STATUS       Status;
  EEPROM_FILE_LOC  Loc;
  CHAR8            *NewConfig;
  UINTN            NewLen;
  BOOLEAN          HasSig;

  if (!EepromImageValid (Image, Size)) {
    *FailLine = L"The EEPROM content does not look like a bootloader image.";
    return EFI_VOLUME_CORRUPTED;
  }

  //
  // A bootconf.sig is not by itself a reason to stop. rpi-eeprom-digest
  // writes that file in two quite different shapes:
  //
  //   <sha256>\nts: <epoch>\n                 - integrity digest only
  //   <sha256>\nts: <epoch>\nrsa2048: <hex>\n  - RSA-signed for secure boot
  //
  // Only the second is a signature, and only signatures are reproducible
  // exclusively by whoever holds the private key. The first we can recompute
  // ourselves, which is what SyncConfigDigest below does.
  //
  // A digest-only sig also proves the board is not enforcing signed boot: the
  // bootloader verifies the RSA signature over the config whenever secure
  // boot is active in OTP (or SIGNED_BOOT=1 is set), so a board carrying an
  // unsigned config sig while enforcing would not have booted far enough to
  // run this code.
  //
  HasSig = EepromFindFileIn (
             Image,
             Size,
             0,
             EEPROM_PARTITION_A_START,
             EEPROM_PARTITION_A_END,
             "bootconf.sig",
             &Loc
             );
  if (HasSig &&
      BlTextHasLineKey (
        (CONST CHAR8 *)Image + Loc.ContentOffset,
        Loc.ContentLen,
        "rsa2048"
        ))
  {
    *FailLine = L"The EEPROM config is RSA-signed; only the signing key can change it.";
    return EFI_SECURITY_VIOLATION;
  }

  if (!EepromFindFileIn (
        Image,
        Size,
        0,
        EEPROM_PARTITION_A_START,
        EEPROM_PARTITION_A_END,
        "bootconf.txt",
        &Loc
        ))
  {
    *FailLine = L"No bootconf.txt section in the EEPROM image.";
    return EFI_NOT_FOUND;
  }

  NewConfig = AllocatePool (NEW_CONFIG_CAP);
  if (NewConfig == NULL) {
    *FailLine = L"Out of memory.";
    return EFI_OUT_OF_RESOURCES;
  }

  Status = BlBuildNewConfigText (
             (CONST CHAR8 *)Image + Loc.ContentOffset,
             Loc.ContentLen,
             Values,
             NewConfig,
             NEW_CONFIG_CAP,
             &NewLen
             );
  if (!EFI_ERROR (Status)) {
    Status = EepromReplaceFileIn (
               Image,
               Size,
               0,
               EEPROM_PARTITION_A_START,
               EEPROM_PARTITION_A_END,
               "bootconf.txt",
               (CONST UINT8 *)NewConfig,
               NewLen
               );
  }

  if (EFI_ERROR (Status)) {
    FreePool (NewConfig);
    *FailLine = L"Rewriting bootconf.txt did not fit the EEPROM layout.";
    return Status;
  }

  if (HasSig) {
    Status = SyncConfigDigest (
               Image,
               Size,
               0,
               EEPROM_PARTITION_A_START,
               EEPROM_PARTITION_A_END,
               NewConfig,
               NewLen,
               Timestamp
               );
    if (EFI_ERROR (Status)) {
      FreePool (NewConfig);
      *FailLine = L"The config digest (bootconf.sig) could not be rewritten.";
      return Status;
    }
  }

  //
  // A/B-capable images carry a second partition after a 0xFF gap with
  // its own section chain; keep its config in step when it has one.
  //
  if (EepromFindFileIn (
        Image,
        Size,
        EEPROM_PARTITION_B_START,
        EEPROM_PARTITION_B_START,
        EEPROM_PARTITION_B_END,
        "bootconf.txt",
        &Loc
        ))
  {
    Status = EepromReplaceFileIn (
               Image,
               Size,
               EEPROM_PARTITION_B_START,
               EEPROM_PARTITION_B_START,
               EEPROM_PARTITION_B_END,
               "bootconf.txt",
               (CONST UINT8 *)NewConfig,
               NewLen
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_WARN,
        "BootloaderConfig: partition B config not updated: %r\n",
        Status
        ));
    } else if (EepromFindFileIn (
                 Image,
                 Size,
                 EEPROM_PARTITION_B_START,
                 EEPROM_PARTITION_B_START,
                 EEPROM_PARTITION_B_END,
                 "bootconf.sig",
                 &Loc
                 ))
    {
      Status = SyncConfigDigest (
                 Image,
                 Size,
                 EEPROM_PARTITION_B_START,
                 EEPROM_PARTITION_B_START,
                 EEPROM_PARTITION_B_END,
                 NewConfig,
                 NewLen,
                 Timestamp
                 );
      if (EFI_ERROR (Status)) {
        DEBUG ((
          DEBUG_WARN,
          "BootloaderConfig: partition B config digest not updated: %r\n",
          Status
          ));
      }
    }
  }

  FreePool (NewConfig);

  Status = EepromSetTimestamp (Image, Size, Timestamp);
  if (EFI_ERROR (Status)) {
    *FailLine = L"The EEPROM image's update timestamp could not be set.";
    return Status;
  }

  return EFI_SUCCESS;
}

/**
  Does a config value read as exactly zero ("0", "00", ...)?
**/
STATIC
BOOLEAN
ValueIsZero (
  IN CONST CHAR8  *Val,
  IN UINTN        Len
  )
{
  UINTN    Index;
  BOOLEAN  SawDigit;

  SawDigit = FALSE;
  for (Index = 0; Index < Len; Index++) {
    if ((Val[Index] == ' ') || (Val[Index] == '\t')) {
      continue;
    }

    if (Val[Index] != '0') {
      return FALSE;
    }

    SawDigit = TRUE;
  }

  return SawDigit;
}

/**
  The staging action: read - patch - stage - reboot. Every early return
  has shown the user a popup.
**/
STATIC
VOID
StageUpdate (
  VOID
  )
{
  EFI_STATUS         Status;
  RPI_BLCFG_DATA     Edited;
  BLCFG_VALUES       NewValues;
  BCM2712_BOOT_SPI   Spi;
  BCM2712_BOOT_SPI_RESULT  SpiResult;
  CHAR16             DiagLine1[80];
  CHAR16             DiagLine2[80];
  UINT8              *Image;
  UINT8              Digest[BLCFG_SHA256_DIGEST_SIZE];
  CHAR8              Sig[128];
  UINTN              SigLen;
  UINTN              Index;
  UINT32             Timestamp;
  EFI_FILE_PROTOCOL  *Root;
  CHAR16             *FailLine;
  CONST CHAR8        *Val;
  UINTN              ValLen;

  ZeroMem (&Edited, sizeof (Edited));
  if (!HiiGetBrowserData (
         &gRpiBlCfgFormSetGuid,
         RPI_BLCFG_VARIABLE_NAME,
         sizeof (Edited),
         (UINT8 *)&Edited
         ))
  {
    PopupWait (L"Could not read the edited settings from the form.", NULL);
    return;
  }

  if (!BlValuesFromData (&Edited, &NewValues)) {
    PopupWait (
      L"BOOT_ORDER must be 1-8 hex digits, e.g. 0xf461.",
      L"See the Raspberry Pi bootloader documentation for the codes."
      );
    return;
  }

  if (BlValuesEqual (&NewValues, &mCurrentValues)) {
    PopupWait (L"No pending changes - the EEPROM already matches.", NULL);
    return;
  }

  if (mBlconfigRaw == NULL) {
    PopupWait (L"The firmware provided no bootloader config (blconfig).", NULL);
    return;
  }

  if (BlTextGetValue (
        (CONST CHAR8 *)mBlconfigRaw,
        mBlconfigTextLen,
        "ENABLE_SELF_UPDATE",
        &Val,
        &ValLen
        ) &&
      ValueIsZero (Val, ValLen))
  {
    PopupWait (
      L"ENABLE_SELF_UPDATE=0 is set: the bootloader would ignore",
      L"a staged update. Use recovery.bin from another machine."
      );
    return;
  }

  if (mFdt == NULL) {
    PopupWait (L"No device tree; cannot locate the boot SPI.", NULL);
    return;
  }

  Status = Bcm2712BootSpiLocate (mFdt, &Spi);
  if (EFI_ERROR (Status)) {
    PopupWait (L"Could not determine the boot SPI controller.", NULL);
    return;
  }

  Image = AllocatePool (EEPROM_IMAGE_SIZE_2712);
  if (Image == NULL) {
    PopupWait (L"Out of memory for the 2 MiB EEPROM image.", NULL);
    return;
  }

  PopupInfo (L"Reading the 2 MiB boot EEPROM over SPI...");

  ZeroMem (&SpiResult, sizeof (SpiResult));
  Status = Bcm2712BootSpiReadImage (&Spi, Image, EEPROM_IMAGE_SIZE_2712, &SpiResult);
  if (EFI_ERROR (Status)) {
    FreePool (Image);
    //
    // Show what the probe actually saw. A RELEASE build emits no DEBUG
    // output, and without these numbers "did not respond" is the same
    // message for a wrong address, a wrong chip select, a wrong pin mux and
    // a genuinely dead part. JEDEC 00 00 00 means MISO was never driven;
    // ff ff ff means it floated high with nothing answering.
    //
    UnicodeSPrint (
      DiagLine1,
      sizeof (DiagLine1),
      L"No answer: JEDEC %02x %02x %02x, %a mux %a",
      SpiResult.JedecId[0],
      SpiResult.JedecId[1],
      SpiResult.JedecId[2],
      SpiResult.UsedD0 ? "D0" : "C0",
      SpiResult.Muxed ? "applied" : "unavailable"
      );
    UnicodeSPrint (
      DiagLine2,
      sizeof (DiagLine2),
      L"SPI %Lx CS %Lx/%x pinctrl %Lx",
      Spi.SpiBase,
      Spi.CsBankBase,
      Spi.CsMask,
      Spi.PinctrlBase
      );
    PopupWait (DiagLine1, DiagLine2);
    return;
  }

  Timestamp = StagingTimestamp ();
  FailLine  = L"";
  Status    = PatchImage (
                Image,
                EEPROM_IMAGE_SIZE_2712,
                &NewValues,
                Timestamp,
                &FailLine
                );
  if (EFI_ERROR (Status)) {
    FreePool (Image);
    PopupWait (FailLine, NULL);
    return;
  }

  //
  // pieeprom.sig, as written by rpi-eeprom-digest -c 2712: the image
  // hash, the update timestamp, the target SoC.
  //
  BlSha256 (Image, EEPROM_IMAGE_SIZE_2712, Digest);
  SigLen = 0;
  for (Index = 0; Index < BLCFG_SHA256_DIGEST_SIZE; Index++) {
    SigLen += AsciiSPrint (
                &Sig[SigLen],
                sizeof (Sig) - SigLen,
                "%02x",
                Digest[Index]
                );
  }

  SigLen += AsciiSPrint (
              &Sig[SigLen],
              sizeof (Sig) - SigLen,
              "\nts: %u\ntarget-soc: 2712\n",
              Timestamp
              );

  PopupInfo (L"Writing pieeprom.upd to the boot partition...");

  Status = OpenBootVolume (&Root);
  if (EFI_ERROR (Status)) {
    FreePool (Image);
    PopupWait (L"No boot volume with armstub8-2712.bin or config.txt found.", NULL);
    return;
  }

  Status = ReplaceFileContent (
             Root,
             STAGED_UPDATE_FILE,
             Image,
             EEPROM_IMAGE_SIZE_2712
             );
  if (!EFI_ERROR (Status)) {
    Status = ReplaceFileContent (Root, STAGED_SIG_FILE, Sig, SigLen);
    if (EFI_ERROR (Status)) {
      //
      // Never leave an unsigned .upd behind: without its .sig the
      // bootloader ignores it, but a half-staged pair is confusing.
      //
      DeleteFileIfPresent (Root, STAGED_UPDATE_FILE);
    }
  }

  Root->Close (Root);
  FreePool (Image);

  if (EFI_ERROR (Status)) {
    PopupWait (L"Writing the update files to the boot partition failed.", NULL);
    return;
  }

  //
  // Record what was staged so the next boot can verify and clean up,
  // then reboot into the bootloader's self-update.
  //
  BlValuesToData (&NewValues, &Edited);
  gRT->SetVariable (
         RPI_BLCFG_STAGED_VARIABLE_NAME,
         &gRpiBlCfgFormSetGuid,
         EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
         sizeof (Edited),
         &Edited
         );

  DEBUG ((
    DEBUG_INFO,
    "BootloaderConfig: staged EEPROM update, ts %u - resetting\n",
    Timestamp
    ));

  PopupInfo (L"EEPROM update staged - rebooting to apply...");
  gBS->Stall (2000000);
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
}

VOID
BlStagedMarkerCleanup (
  VOID
  )
{
  EFI_STATUS         Status;
  RPI_BLCFG_DATA     StagedData;
  BLCFG_VALUES       Staged;
  UINTN              Size;
  EFI_FILE_PROTOCOL  *Root;
  BOOLEAN            Applied;

  Size   = sizeof (StagedData);
  Status = gRT->GetVariable (
                  RPI_BLCFG_STAGED_VARIABLE_NAME,
                  &gRpiBlCfgFormSetGuid,
                  NULL,
                  &Size,
                  &StagedData
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  Applied = (Size == sizeof (StagedData)) &&
            BlValuesFromData (&StagedData, &Staged) &&
            BlValuesEqual (&Staged, &mCurrentValues);

  if (Applied) {
    if (!EFI_ERROR (OpenBootVolume (&Root))) {
      DeleteFileIfPresent (Root, STAGED_UPDATE_FILE);
      DeleteFileIfPresent (Root, STAGED_SIG_FILE);
      Root->Close (Root);
    }

    DEBUG ((DEBUG_INFO, "BootloaderConfig: staged EEPROM update applied\n"));
  } else {
    //
    // The bootloader saw the staged files on the way here and chose not
    // to flash them (or the update is from another tool); leave the
    // files for inspection but stop tracking them.
    //
    DEBUG ((DEBUG_WARN, "BootloaderConfig: staged EEPROM update NOT applied\n"));
  }

  gRT->SetVariable (
         RPI_BLCFG_STAGED_VARIABLE_NAME,
         &gRpiBlCfgFormSetGuid,
         0,
         0,
         NULL
         );
}

//
// EFI_HII_CONFIG_ACCESS_PROTOCOL. The questions bind to the BlCfg
// efivarstore, which the form browser reads and writes directly;
// ExtractConfig/RouteConfig are still implemented over the same variable
// for spec completeness (ExportConfig et al.), and Callback carries the
// staging action.
//

STATIC
EFI_STATUS
EFIAPI
BlExtractConfig (
  IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN  CONST EFI_STRING                      Request,
  OUT EFI_STRING                            *Progress,
  OUT EFI_STRING                            *Results
  )
{
  EFI_STATUS      Status;
  RPI_BLCFG_DATA  Data;
  UINTN           Size;
  EFI_STRING      ConfigRequest;
  EFI_STRING      ConfigRequestHdr;
  UINTN           RequestSize;
  BOOLEAN         AllocatedRequest;

  if ((Progress == NULL) || (Results == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Progress = Request;
  if ((Request != NULL) &&
      !HiiIsConfigHdrMatch (
         Request,
         &gRpiBlCfgFormSetGuid,
         RPI_BLCFG_VARIABLE_NAME
         ))
  {
    return EFI_NOT_FOUND;
  }

  Size   = sizeof (Data);
  Status = gRT->GetVariable (
                  RPI_BLCFG_VARIABLE_NAME,
                  &gRpiBlCfgFormSetGuid,
                  NULL,
                  &Size,
                  &Data
                  );
  if (EFI_ERROR (Status) || (Size != sizeof (Data))) {
    BlValuesToData (&mCurrentValues, &Data);
  }

  ConfigRequest    = Request;
  AllocatedRequest = FALSE;
  if ((Request == NULL) || (StrStr (Request, L"OFFSET") == NULL)) {
    //
    // Full-store request: synthesize <ConfigHdr>&OFFSET=0&WIDTH=<size>.
    //
    ConfigRequestHdr = HiiConstructConfigHdr (
                         &gRpiBlCfgFormSetGuid,
                         RPI_BLCFG_VARIABLE_NAME,
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
      (UINT64)sizeof (Data)
      );
    FreePool (ConfigRequestHdr);
    AllocatedRequest = TRUE;
  }

  Status = gHiiConfigRouting->BlockToConfig (
                                gHiiConfigRouting,
                                ConfigRequest,
                                (UINT8 *)&Data,
                                sizeof (Data),
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
BlRouteConfig (
  IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN  CONST EFI_STRING                      Configuration,
  OUT EFI_STRING                            *Progress
  )
{
  EFI_STATUS      Status;
  RPI_BLCFG_DATA  Data;
  UINTN           Size;

  if (Progress == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Progress = Configuration;
  if (Configuration == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (!HiiIsConfigHdrMatch (
         Configuration,
         &gRpiBlCfgFormSetGuid,
         RPI_BLCFG_VARIABLE_NAME
         ))
  {
    return EFI_NOT_FOUND;
  }

  Size   = sizeof (Data);
  Status = gRT->GetVariable (
                  RPI_BLCFG_VARIABLE_NAME,
                  &gRpiBlCfgFormSetGuid,
                  NULL,
                  &Size,
                  &Data
                  );
  if (EFI_ERROR (Status) || (Size != sizeof (Data))) {
    BlValuesToData (&mCurrentValues, &Data);
  }

  Size   = sizeof (Data);
  Status = gHiiConfigRouting->ConfigToBlock (
                                gHiiConfigRouting,
                                Configuration,
                                (UINT8 *)&Data,
                                &Size,
                                Progress
                                );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return gRT->SetVariable (
                RPI_BLCFG_VARIABLE_NAME,
                &gRpiBlCfgFormSetGuid,
                EFI_VARIABLE_NON_VOLATILE |
                EFI_VARIABLE_BOOTSERVICE_ACCESS |
                EFI_VARIABLE_RUNTIME_ACCESS,
                sizeof (Data),
                &Data
                );
}

STATIC
EFI_STATUS
EFIAPI
BlCallback (
  IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN  EFI_BROWSER_ACTION                    Action,
  IN  EFI_QUESTION_ID                       QuestionId,
  IN  UINT8                                 Type,
  IN  EFI_IFR_TYPE_VALUE                    *Value,
  OUT EFI_BROWSER_ACTION_REQUEST            *ActionRequest
  )
{
  if (QuestionId != RPI_BLCFG_KEY_STAGE) {
    return EFI_UNSUPPORTED;
  }

  if (Action == EFI_BROWSER_ACTION_CHANGING) {
    return EFI_SUCCESS;
  }

  if (Action != EFI_BROWSER_ACTION_CHANGED) {
    return EFI_UNSUPPORTED;
  }

  StageUpdate ();
  return EFI_SUCCESS;
}

STATIC EFI_HII_CONFIG_ACCESS_PROTOCOL  mConfigAccess = {
  BlExtractConfig,
  BlRouteConfig,
  BlCallback
};

EFI_STATUS
BlInstallHiiPage (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = gBS->InstallMultipleProtocolInterfaces (
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
                 &gRpiBlCfgFormSetGuid,
                 mDriverHandle,
                 BootloaderConfigDxeStrings,
                 BootloaderConfigHiiBin,
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

  DEBUG ((DEBUG_INFO, "BootloaderConfig: Setup page published\n"));
  return EFI_SUCCESS;
}
