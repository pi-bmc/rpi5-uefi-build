/** @file

  EEPROM-backed UEFI variable restore and sync-back.

  Bridges the volatile-on-this-platform UEFI variable store and the BMC
  shared EEPROM's UbEfiVa region (see RpiBmcEeprom.h):

    - RESTORE: when the RP1 I2C master appears (during BDS, before boot
      options are evaluated), read the UbEfiVa blob at
      PcdBmcEepromVarStoreOffset, validate magic/length/CRC32 and
      SetVariable every non-volatile, non-time-authenticated entry that
      is missing or different. A blank or invalid blob is a first boot,
      not an error.

    - SYNC-BACK: at ReadyToBoot and from EFI_RESET_NOTIFICATION_PROTOCOL
      (both cheap to repeat: the EEPROM write is compare-skipped),
      serialize all NV non-time-auth variables into a UbEfiVa blob -
      BMC-relevant variables first so they always fit - and write it
      through BmcEepromWriteIfChanged.

  The blob wire format is defined by RpiBmcEeprom.h; the restore/sync
  behavior mirrors the pi-bmc U-Boot port's efi_var_i2c semantics.
  ReadyToBoot + reset-notification pairing after edk2-platforms'
  Platform/RaspberryPi/Drivers/VarBlockServiceDxe.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Guid/GlobalVariable.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BmcEepromLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/I2cMaster.h>
#include <Protocol/ResetNotification.h>
#include <RpiBmcEeprom.h>

//
// Serialization priority classes, emitted in ascending order so the
// BMC-relevant variables always land inside PcdBmcEepromVarStoreSize.
//
#define VAR_PRIORITY_BOOT_ORDER        0
#define VAR_PRIORITY_BOOT_NEXT         1
#define VAR_PRIORITY_TIMEOUT           2
#define VAR_PRIORITY_BOOT_OPTION       3
#define VAR_PRIORITY_BL_CONFIG         4
#define VAR_PRIORITY_BL_TIMESTAMP      5
#define VAR_PRIORITY_OTHER             6
#define VAR_PRIORITY_LEVELS            7

typedef struct EEPROM_VAR_RECORD_ {
  struct EEPROM_VAR_RECORD_    *Next;
  CHAR16                       *Name;
  EFI_GUID                     Guid;
  UINT32                       Attributes;
  UINTN                        DataSize;
  UINT8                        *Data;
  BOOLEAN                      Emitted;
} EEPROM_VAR_RECORD;

STATIC EFI_I2C_MASTER_PROTOCOL  *mI2cMaster              = NULL;
STATIC EFI_EVENT                mI2cNotifyEvent          = NULL;
STATIC VOID                     *mI2cNotifyRegistration  = NULL;
STATIC VOID                     *mResetNotifyRegistration = NULL;
STATIC BOOLEAN                  mRestoreDone             = FALSE;
STATIC BOOLEAN                  mResetNotifyRegistered   = FALSE;
STATIC BOOLEAN                  mSyncInProgress          = FALSE;

/**
  Test whether Name is a Boot#### load option name: "Boot" followed by
  exactly four hex digits.

  @param[in] Name   Variable name.

  @retval TRUE    Name is Boot####.
  @retval FALSE   It is not.
**/
STATIC
BOOLEAN
EepromVarIsBootOption (
  IN CONST CHAR16  *Name
  )
{
  UINTN   Index;
  CHAR16  Char;

  if ((StrLen (Name) != 8) || (StrnCmp (Name, L"Boot", 4) != 0)) {
    return FALSE;
  }

  for (Index = 4; Index < 8; Index++) {
    Char = Name[Index];
    if (!(((Char >= L'0') && (Char <= L'9')) ||
          ((Char >= L'A') && (Char <= L'F')) ||
          ((Char >= L'a') && (Char <= L'f'))))
    {
      return FALSE;
    }
  }

  return TRUE;
}

/**
  Classify a variable into its serialization priority.

  @param[in] Record   The collected variable.

  @return  The priority class, 0 (first) .. VAR_PRIORITY_LEVELS - 1.
**/
STATIC
UINTN
EepromVarPriority (
  IN CONST EEPROM_VAR_RECORD  *Record
  )
{
  if (CompareGuid (&Record->Guid, &gEfiGlobalVariableGuid)) {
    if (StrCmp (Record->Name, L"BootOrder") == 0) {
      return VAR_PRIORITY_BOOT_ORDER;
    }

    if (StrCmp (Record->Name, L"BootNext") == 0) {
      return VAR_PRIORITY_BOOT_NEXT;
    }

    if (StrCmp (Record->Name, L"Timeout") == 0) {
      return VAR_PRIORITY_TIMEOUT;
    }

    if (EepromVarIsBootOption (Record->Name)) {
      return VAR_PRIORITY_BOOT_OPTION;
    }
  } else if (CompareGuid (&Record->Guid, &gRpiBmcBootloaderVendorGuid)) {
    if (StrCmp (Record->Name, BMC_VAR_BOOTLOADER_CONFIG) == 0) {
      return VAR_PRIORITY_BL_CONFIG;
    }

    if (StrCmp (Record->Name, BMC_VAR_BOOTLOADER_TIMESTAMP) == 0) {
      return VAR_PRIORITY_BL_TIMESTAMP;
    }
  }

  return VAR_PRIORITY_OTHER;
}

/**
  Restore one blob entry into the variable store, unless an identical
  variable already exists (spares fault-tolerant-write churn).

  @param[in] Name         NUL-terminated variable name.
  @param[in] Guid         Vendor GUID.
  @param[in] Attributes   Variable attributes from the entry.
  @param[in] DataSize     Entry data size in bytes.
  @param[in] Data         Entry data.
**/
STATIC
VOID
EepromVarRestoreOne (
  IN CONST CHAR16    *Name,
  IN CONST EFI_GUID  *Guid,
  IN UINT32          Attributes,
  IN UINTN           DataSize,
  IN CONST VOID      *Data
  )
{
  EFI_STATUS  Status;
  VOID        *Existing;
  UINTN       ExistingSize;
  UINT32      ExistingAttributes;
  BOOLEAN     Matches;

  if (DataSize == 0) {
    //
    // SetVariable with no data would delete; nothing useful to restore.
    //
    return;
  }

  Matches      = FALSE;
  ExistingSize = 0;
  Status       = gRT->GetVariable (
                        (CHAR16 *)Name,
                        (EFI_GUID *)Guid,
                        NULL,
                        &ExistingSize,
                        NULL
                        );
  if ((Status == EFI_BUFFER_TOO_SMALL) && (ExistingSize == DataSize)) {
    Existing = AllocatePool (ExistingSize);
    if (Existing != NULL) {
      Status = gRT->GetVariable (
                      (CHAR16 *)Name,
                      (EFI_GUID *)Guid,
                      &ExistingAttributes,
                      &ExistingSize,
                      Existing
                      );
      if (!EFI_ERROR (Status) &&
          (ExistingAttributes == Attributes) &&
          (ExistingSize == DataSize) &&
          (CompareMem (Existing, Data, DataSize) == 0))
      {
        Matches = TRUE;
      }

      FreePool (Existing);
    }
  }

  if (Matches) {
    return;
  }

  Status = gRT->SetVariable (
                  (CHAR16 *)Name,
                  (EFI_GUID *)Guid,
                  Attributes,
                  DataSize,
                  (VOID *)Data
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "EepromVarStore: restore of %g:%s failed - %r\n",
      Guid,
      Name,
      Status
      ));
  } else {
    DEBUG ((DEBUG_INFO, "EepromVarStore: restored %g:%s\n", Guid, Name));
  }
}

/**
  Read and validate the UbEfiVa blob from the EEPROM and restore its
  entries. Absent/blank/corrupt blobs are logged and skipped - the
  sync-back path creates the blob on first boot.
**/
STATIC
VOID
EepromVarStoreRestore (
  VOID
  )
{
  UB_EFI_VAR_FILE_HEADER  Header;
  UB_EFI_VAR_ENTRY        Entry;
  EFI_STATUS              Status;
  EFI_GUID                Guid;
  UINT8                   *Blob;
  CONST CHAR16            *Name;
  CONST UINT8             *Data;
  UINT32                  StoreOffset;
  UINT32                  StoreSize;
  UINT32                  EntryOffset;
  UINTN                   NameChars;
  UINTN                   MaxNameChars;
  UINTN                   NameBytes;
  UINTN                   DataSize;
  UINT32                  Crc;

  StoreOffset = PcdGet32 (PcdBmcEepromVarStoreOffset);
  StoreSize   = PcdGet32 (PcdBmcEepromVarStoreSize);

  Status = BmcEepromRead (mI2cMaster, StoreOffset, sizeof (Header), &Header);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "EepromVarStore: EEPROM header read failed - %r\n",
      Status
      ));
    return;
  }

  if ((Header.Magic != UB_EFI_VAR_FILE_MAGIC) ||
      (Header.Length < sizeof (Header)) ||
      (Header.Length > StoreSize))
  {
    DEBUG ((
      DEBUG_INFO,
      "EepromVarStore: no valid variable blob (first boot?) - "
      "magic 0x%lx length 0x%x\n",
      Header.Magic,
      Header.Length
      ));
    return;
  }

  Blob = AllocatePool (Header.Length);
  if (Blob == NULL) {
    DEBUG ((DEBUG_WARN, "EepromVarStore: out of memory for blob read\n"));
    return;
  }

  Status = BmcEepromRead (mI2cMaster, StoreOffset, Header.Length, Blob);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "EepromVarStore: EEPROM blob read failed - %r\n",
      Status
      ));
    FreePool (Blob);
    return;
  }

  Crc = CalculateCrc32 (
          Blob + sizeof (Header),
          Header.Length - sizeof (Header)
          );
  if (Crc != Header.Crc32) {
    DEBUG ((
      DEBUG_INFO,
      "EepromVarStore: blob CRC mismatch (stored 0x%x, computed 0x%x); "
      "ignoring\n",
      Header.Crc32,
      Crc
      ));
    FreePool (Blob);
    return;
  }

  //
  // Walk the entries. Per RpiBmcEeprom.h, an entry's Length field is the
  // DATA length in bytes (not the entry length) and the next entry
  // starts at ALIGN8 (offset-of-Data + Length) - the same stride math as
  // u-boot's efi_var_mem.c and the BMC's efivars/blob.go. Entry starts
  // stay 8-aligned: the 24-byte header and every stride are multiples
  // of 8.
  //
  EntryOffset = sizeof (Header);
  while (EntryOffset < Header.Length) {
    if (Header.Length - EntryOffset < sizeof (Entry)) {
      DEBUG ((
        DEBUG_WARN,
        "EepromVarStore: trailing garbage at blob offset 0x%x\n",
        EntryOffset
        ));
      break;
    }

    CopyMem (&Entry, Blob + EntryOffset, sizeof (Entry));

    Name         = (CONST CHAR16 *)(Blob + EntryOffset + sizeof (Entry));
    MaxNameChars = (Header.Length - EntryOffset - sizeof (Entry)) /
                   sizeof (CHAR16);
    for (NameChars = 0; NameChars < MaxNameChars; NameChars++) {
      if (Name[NameChars] == L'\0') {
        break;
      }
    }

    if (NameChars == MaxNameChars) {
      DEBUG ((
        DEBUG_WARN,
        "EepromVarStore: unterminated entry name at blob offset 0x%x; "
        "stopping\n",
        EntryOffset
        ));
      break;
    }

    NameBytes = (NameChars + 1) * sizeof (CHAR16);
    Data      = (CONST UINT8 *)Name + NameBytes;
    DataSize  = Entry.Length;

    if (DataSize >
        Header.Length - EntryOffset - sizeof (Entry) - NameBytes)
    {
      DEBUG ((
        DEBUG_WARN,
        "EepromVarStore: entry data (0x%x bytes) overruns blob at offset "
        "0x%x; stopping\n",
        Entry.Length,
        EntryOffset
        ));
      break;
    }

    if (((Entry.Attributes & EFI_VARIABLE_NON_VOLATILE) != 0) &&
        ((Entry.Attributes &
          EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS) == 0))
    {
      Guid = Entry.VendorGuid;
      EepromVarRestoreOne (Name, &Guid, Entry.Attributes, DataSize, Data);
    }

    EntryOffset +=
      (UINT32)ALIGN_VALUE (sizeof (Entry) + NameBytes + DataSize, 8);
  }

  FreePool (Blob);
}

/**
  Free a collected variable list.

  @param[in] Head   List head, may be NULL.
**/
STATIC
VOID
EepromVarFreeList (
  IN EEPROM_VAR_RECORD  *Head
  )
{
  EEPROM_VAR_RECORD  *Next;

  while (Head != NULL) {
    Next = Head->Next;
    if (Head->Name != NULL) {
      FreePool (Head->Name);
    }

    if (Head->Data != NULL) {
      FreePool (Head->Data);
    }

    FreePool (Head);
    Head = Next;
  }
}

/**
  Enumerate the variable store and collect every non-volatile variable
  without EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS.

  @param[out] Head   The collected list (enumeration order).

  @retval EFI_SUCCESS            Collection complete (possibly empty).
  @retval EFI_OUT_OF_RESOURCES   An allocation failed.
**/
STATIC
EFI_STATUS
EepromVarCollect (
  OUT EEPROM_VAR_RECORD  **Head
  )
{
  EEPROM_VAR_RECORD  *Record;
  EEPROM_VAR_RECORD  *Tail;
  EFI_STATUS         Status;
  EFI_GUID           Guid;
  CHAR16             *Name;
  CHAR16             *NewName;
  UINT8              *Data;
  UINTN              NameBufferSize;
  UINTN              NameSize;
  UINTN              DataSize;
  UINT32             Attributes;

  *Head = NULL;
  Tail  = NULL;

  NameBufferSize = 64 * sizeof (CHAR16);
  Name           = AllocateZeroPool (NameBufferSize);
  if (Name == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (&Guid, sizeof (Guid));

  for ( ; ; ) {
    NameSize = NameBufferSize;
    Status   = gRT->GetNextVariableName (&NameSize, Name, &Guid);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      NewName = ReallocatePool (NameBufferSize, NameSize, Name);
      if (NewName == NULL) {
        FreePool (Name);
        EepromVarFreeList (*Head);
        *Head = NULL;
        return EFI_OUT_OF_RESOURCES;
      }

      Name           = NewName;
      NameBufferSize = NameSize;
      Status         = gRT->GetNextVariableName (&NameSize, Name, &Guid);
    }

    if (Status == EFI_NOT_FOUND) {
      break;
    }

    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_WARN,
        "EepromVarStore: GetNextVariableName failed - %r\n",
        Status
        ));
      break;
    }

    DataSize = 0;
    Status   = gRT->GetVariable (Name, &Guid, NULL, &DataSize, NULL);
    if (Status != EFI_BUFFER_TOO_SMALL) {
      continue;
    }

    Data = AllocatePool (DataSize);
    if (Data == NULL) {
      FreePool (Name);
      EepromVarFreeList (*Head);
      *Head = NULL;
      return EFI_OUT_OF_RESOURCES;
    }

    Status = gRT->GetVariable (Name, &Guid, &Attributes, &DataSize, Data);
    if (EFI_ERROR (Status) ||
        ((Attributes & EFI_VARIABLE_NON_VOLATILE) == 0) ||
        ((Attributes &
          EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS) != 0))
    {
      FreePool (Data);
      continue;
    }

    Record = AllocateZeroPool (sizeof (EEPROM_VAR_RECORD));
    if (Record == NULL) {
      FreePool (Data);
      FreePool (Name);
      EepromVarFreeList (*Head);
      *Head = NULL;
      return EFI_OUT_OF_RESOURCES;
    }

    Record->Name = AllocateCopyPool (StrSize (Name), Name);
    if (Record->Name == NULL) {
      FreePool (Record);
      FreePool (Data);
      FreePool (Name);
      EepromVarFreeList (*Head);
      *Head = NULL;
      return EFI_OUT_OF_RESOURCES;
    }

    Record->Guid       = Guid;
    Record->Attributes = Attributes;
    Record->DataSize   = DataSize;
    Record->Data       = Data;

    if (Tail == NULL) {
      *Head = Record;
    } else {
      Tail->Next = Record;
    }

    Tail = Record;
  }

  FreePool (Name);
  return EFI_SUCCESS;
}

/**
  Serialize all collected NV variables into a UbEfiVa blob and write it
  to the EEPROM if it differs from what is stored.

  Safe to call repeatedly (ReadyToBoot, reset notification): unchanged
  content costs one EEPROM read.
**/
STATIC
VOID
EepromVarStoreSync (
  VOID
  )
{
  UB_EFI_VAR_FILE_HEADER  Header;
  UB_EFI_VAR_ENTRY        Entry;
  EEPROM_VAR_RECORD       *List;
  EEPROM_VAR_RECORD       *Record;
  EFI_STATUS              Status;
  UINT8                   *Blob;
  UINT32                  StoreSize;
  UINTN                   Used;
  UINTN                   EntryBytes;
  UINTN                   NameBytes;
  UINTN                   Priority;
  UINTN                   Dropped;
  BOOLEAN                 Full;
  BOOLEAN                 Wrote;

  if (mSyncInProgress) {
    return;
  }

  mSyncInProgress = TRUE;

  if (mI2cMaster == NULL) {
    Status = BmcEepromLocateI2cMaster (&mI2cMaster);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_WARN,
        "EepromVarStore: no I2C master, cannot sync variables - %r\n",
        Status
        ));
      mI2cMaster      = NULL;
      mSyncInProgress = FALSE;
      return;
    }
  }

  List = NULL;
  Blob = NULL;

  Status = EepromVarCollect (&List);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "EepromVarStore: variable collection failed - %r\n",
      Status
      ));
    goto Out;
  }

  StoreSize = PcdGet32 (PcdBmcEepromVarStoreSize);
  Blob      = AllocateZeroPool (StoreSize);
  if (Blob == NULL) {
    DEBUG ((DEBUG_WARN, "EepromVarStore: out of memory for blob\n"));
    goto Out;
  }

  //
  // Emit entries in priority order; once one no longer fits, drop
  // everything still unemitted so the important set survives intact.
  //
  Used    = sizeof (Header);
  Dropped = 0;
  Full    = FALSE;

  for (Priority = 0; Priority < VAR_PRIORITY_LEVELS; Priority++) {
    for (Record = List; Record != NULL; Record = Record->Next) {
      if (Record->Emitted || (EepromVarPriority (Record) != Priority)) {
        continue;
      }

      if (Full) {
        Dropped++;
        continue;
      }

      //
      // EntryBytes is the 8-byte-aligned stride the entry occupies in
      // the blob; the wire Length field carries only the data size.
      //
      NameBytes  = StrSize (Record->Name);
      EntryBytes = ALIGN_VALUE (
                     sizeof (Entry) + NameBytes + Record->DataSize,
                     8
                     );
      if ((EntryBytes > StoreSize) || (Used > StoreSize - EntryBytes)) {
        Full = TRUE;
        Dropped++;
        continue;
      }

      ZeroMem (&Entry, sizeof (Entry));
      Entry.Length     = (UINT32)Record->DataSize;
      Entry.Attributes = Record->Attributes;
      Entry.Time       = 0;
      Entry.VendorGuid = Record->Guid;

      CopyMem (Blob + Used, &Entry, sizeof (Entry));
      CopyMem (Blob + Used + sizeof (Entry), Record->Name, NameBytes);
      CopyMem (
        Blob + Used + sizeof (Entry) + NameBytes,
        Record->Data,
        Record->DataSize
        );

      Used            += EntryBytes;
      Record->Emitted  = TRUE;
    }
  }

  if (Dropped != 0) {
    DEBUG ((
      DEBUG_WARN,
      "EepromVarStore: variable store full, dropped %u variable(s)\n",
      (UINT32)Dropped
      ));
  }

  ZeroMem (&Header, sizeof (Header));
  Header.Magic  = UB_EFI_VAR_FILE_MAGIC;
  Header.Length = (UINT32)Used;
  Header.Crc32  = CalculateCrc32 (
                    Blob + sizeof (Header),
                    Used - sizeof (Header)
                    );
  CopyMem (Blob, &Header, sizeof (Header));

  Wrote  = FALSE;
  Status = BmcEepromWriteIfChanged (
             mI2cMaster,
             PcdGet32 (PcdBmcEepromVarStoreOffset),
             Used,
             Blob,
             &Wrote
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "EepromVarStore: EEPROM sync failed - %r\n",
      Status
      ));
  } else {
    DEBUG ((
      DEBUG_INFO,
      "EepromVarStore: %a (blob 0x%x bytes)\n",
      Wrote ? "variables synced to EEPROM" : "EEPROM already current",
      (UINT32)Used
      ));
  }

Out:
  if (Blob != NULL) {
    FreePool (Blob);
  }

  EepromVarFreeList (List);
  mSyncInProgress = FALSE;
}

/**
  Protocol notification: an EFI_I2C_MASTER_PROTOCOL instance appeared.
  Runs the one-time restore (Rp1DwI2cDxe starts during BDS, before boot
  options are evaluated).

  @param[in] Event     The notification event.
  @param[in] Context   Unused.
**/
STATIC
VOID
EFIAPI
OnI2cMasterAvailable (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;

  if (mRestoreDone) {
    return;
  }

  Status = BmcEepromLocateI2cMaster (&mI2cMaster);
  if (EFI_ERROR (Status)) {
    mI2cMaster = NULL;
    return;
  }

  mRestoreDone = TRUE;
  EepromVarStoreRestore ();

  gBS->CloseEvent (Event);
  mI2cNotifyEvent = NULL;
}

/**
  ReadyToBoot: sync variables back to the EEPROM.

  @param[in] Event     The ReadyToBoot event.
  @param[in] Context   Unused.
**/
STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EepromVarStoreSync ();
}

/**
  Reset notification: last-chance sync before the platform resets (only
  invoked pre-runtime by ResetSystemRuntimeDxe, so boot services are
  available).

  @param[in] ResetType     Unused.
  @param[in] ResetStatus   Unused.
  @param[in] DataSize      Unused.
  @param[in] ResetData     Unused.
**/
STATIC
VOID
EFIAPI
OnResetNotification (
  IN EFI_RESET_TYPE  ResetType,
  IN EFI_STATUS      ResetStatus,
  IN UINTN           DataSize,
  IN VOID            *ResetData OPTIONAL
  )
{
  EepromVarStoreSync ();
}

/**
  Protocol notification: EFI_RESET_NOTIFICATION_PROTOCOL appeared;
  register the sync-back reset handler once.

  @param[in] Event     The notification event.
  @param[in] Context   Unused.
**/
STATIC
VOID
EFIAPI
OnResetNotificationAvailable (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS                       Status;
  EFI_RESET_NOTIFICATION_PROTOCOL  *ResetNotify;

  if (mResetNotifyRegistered) {
    return;
  }

  Status = gBS->LocateProtocol (
                  &gEfiResetNotificationProtocolGuid,
                  NULL,
                  (VOID **)&ResetNotify
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  Status = ResetNotify->RegisterResetNotify (ResetNotify, OnResetNotification);
  if (!EFI_ERROR (Status)) {
    mResetNotifyRegistered = TRUE;
    DEBUG ((DEBUG_INFO, "EepromVarStore: reset-notify sync registered\n"));
  }
}

/**
  Driver entry point: arm the restore trigger (I2C master appearance)
  and both sync-back triggers (ReadyToBoot, reset notification).

  @param[in] ImageHandle   The image handle.
  @param[in] SystemTable   The system table.

  @retval EFI_SUCCESS   All notifications registered.
  @retval other         Event creation failed.
**/
EFI_STATUS
EFIAPI
EepromVarStoreDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   ReadyToBootEvent;
  EFI_EVENT   ResetNotifyEvent;

  //
  // RESTORE path: run when the RP1 I2C master shows up. The helper also
  // signals the event once so an already-present master is handled.
  //
  mI2cNotifyEvent = EfiCreateProtocolNotifyEvent (
                      &gEfiI2cMasterProtocolGuid,
                      TPL_CALLBACK,
                      OnI2cMasterAvailable,
                      NULL,
                      &mI2cNotifyRegistration
                      );
  if (mI2cNotifyEvent == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // SYNC-BACK path 1: ReadyToBoot, after BDS finished editing boot
  // options.
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnReadyToBoot,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &ReadyToBootEvent
                  );
  ASSERT_EFI_ERROR (Status);

  //
  // SYNC-BACK path 2: reset notification, catching resets requested
  // before or after ReadyToBoot.
  //
  ResetNotifyEvent = EfiCreateProtocolNotifyEvent (
                       &gEfiResetNotificationProtocolGuid,
                       TPL_CALLBACK,
                       OnResetNotificationAvailable,
                       NULL,
                       &mResetNotifyRegistration
                       );
  ASSERT (ResetNotifyEvent != NULL);

  return EFI_SUCCESS;
}
