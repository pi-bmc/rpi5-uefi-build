/** @file
  SMBIOS- and protocol-sourced host inventory for the Redfish Host Interface
  client.

  The BMC has no in-band view of the managed host: over the USB link it can
  see a NIC and nothing else. Everything it reports about the system has to
  come from the host itself. SMBIOS is where this firmware already publishes
  identity (type 0/1, from PlatformSmbiosDxe) and memory (type 17); drives
  come from the boot-services protocol stack, exactly the data the old
  BlkInfoMirrorDxe wrote to the blkinfo EEPROM region.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "RpiRedfishSyncDxe.h"

/**
  Return the Nth string of an SMBIOS structure.

  SMBIOS keeps a structure's strings in a NUL-separated list that starts at the
  end of its formatted area and ends with a double NUL; fields reference them by
  1-based index, with 0 meaning "no string".

  @param[in] Header  SMBIOS structure header.
  @param[in] Index   1-based string number. 0 returns NULL.

  @retval Pointer to the string, or NULL when the index is 0 or out of range.
**/
STATIC
CHAR8 *
SmbiosGetString (
  IN EFI_SMBIOS_TABLE_HEADER  *Header,
  IN SMBIOS_TABLE_STRING      Index
  )
{
  CHAR8  *Walker;
  UINT8  Current;

  if ((Header == NULL) || (Index == 0)) {
    return NULL;
  }

  Walker  = (CHAR8 *)Header + Header->Length;
  Current = 1;

  //
  // A zero-length string at the head of the list is the terminating double NUL,
  // i.e. the structure has fewer strings than requested.
  //
  while (*Walker != '\0') {
    if (Current == Index) {
      return Walker;
    }

    Walker += AsciiStrLen (Walker) + 1;
    Current++;
  }

  return NULL;
}

/**
  Copy an SMBIOS string into a fixed-size field, truncating if necessary and
  always NUL-terminating.

  @param[out] Dest      Destination buffer.
  @param[in]  DestSize  Size of Dest in bytes.
  @param[in]  Source    Source string, may be NULL.
**/
STATIC
VOID
CopyInventoryString (
  OUT CHAR8        *Dest,
  IN  UINTN        DestSize,
  IN  CONST CHAR8  *Source
  )
{
  UINTN  Length;

  if ((Dest == NULL) || (DestSize == 0)) {
    return;
  }

  Dest[0] = '\0';
  if (Source == NULL) {
    return;
  }

  Length = AsciiStrLen (Source);
  if (Length >= DestSize) {
    Length = DestSize - 1;
  }

  CopyMem (Dest, Source, Length);
  Dest[Length] = '\0';
}

/**
  Render an SMBIOS type 1 UUID field as a Redfish-style UUID string.

  SMBIOS 2.6 and later store the first three fields little-endian (the same
  layout as EFI_GUID), so they are emitted in reverse byte order to produce the
  canonical text form.

  @param[in]  Uuid  16-byte SMBIOS UUID field.
  @param[out] Text  Receives 37 bytes (36 characters plus NUL).

  @retval TRUE   A usable UUID was rendered.
  @retval FALSE  The field was all-zero or all-0xFF, i.e. "not present".
**/
STATIC
BOOLEAN
RenderSmbiosUuid (
  IN  UINT8  *Uuid,
  OUT CHAR8  *Text
  )
{
  UINTN  Index;
  UINT8  Or;
  UINT8  And;

  Or  = 0x00;
  And = 0xFF;
  for (Index = 0; Index < 16; Index++) {
    Or  |= Uuid[Index];
    And &= Uuid[Index];
  }

  if ((Or == 0x00) || (And == 0xFF)) {
    return FALSE;
  }

  AsciiSPrint (
    Text,
    37,
    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    Uuid[3],
    Uuid[2],
    Uuid[1],
    Uuid[0],
    Uuid[5],
    Uuid[4],
    Uuid[7],
    Uuid[6],
    Uuid[8],
    Uuid[9],
    Uuid[10],
    Uuid[11],
    Uuid[12],
    Uuid[13],
    Uuid[14],
    Uuid[15]
    );

  return TRUE;
}

EFI_STATUS
RpiRedfishCollectInventory (
  OUT RPI_REDFISH_HOST_INVENTORY  *Inventory
  )
{
  EFI_STATUS               Status;
  EFI_SMBIOS_PROTOCOL      *Smbios;
  EFI_SMBIOS_HANDLE        Handle;
  EFI_SMBIOS_TABLE_HEADER  *Record;
  SMBIOS_TABLE_TYPE0       *Type0;
  SMBIOS_TABLE_TYPE1       *Type1;

  if (Inventory == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Inventory, sizeof (*Inventory));

  Status = gBS->LocateProtocol (&gEfiSmbiosProtocolGuid, NULL, (VOID **)&Smbios);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RpiRedfishSync: SMBIOS protocol not found - %r\n", Status));
    return EFI_NOT_FOUND;
  }

  Handle = SMBIOS_HANDLE_PI_RESERVED;
  while (!EFI_ERROR (Smbios->GetNext (Smbios, &Handle, NULL, &Record, NULL))) {
    switch (Record->Type) {
      case EFI_SMBIOS_TYPE_BIOS_INFORMATION:
        Type0 = (SMBIOS_TABLE_TYPE0 *)Record;
        CopyInventoryString (
          Inventory->BiosVersion,
          sizeof (Inventory->BiosVersion),
          SmbiosGetString (Record, Type0->BiosVersion)
          );
        break;

      case EFI_SMBIOS_TYPE_SYSTEM_INFORMATION:
        Type1 = (SMBIOS_TABLE_TYPE1 *)Record;
        CopyInventoryString (
          Inventory->Manufacturer,
          sizeof (Inventory->Manufacturer),
          SmbiosGetString (Record, Type1->Manufacturer)
          );
        CopyInventoryString (
          Inventory->Model,
          sizeof (Inventory->Model),
          SmbiosGetString (Record, Type1->ProductName)
          );
        CopyInventoryString (
          Inventory->SerialNumber,
          sizeof (Inventory->SerialNumber),
          SmbiosGetString (Record, Type1->SerialNumber)
          );
        Inventory->UuidValid = RenderSmbiosUuid ((UINT8 *)&Type1->Uuid, Inventory->Uuid);
        break;

      default:
        break;
    }
  }

  DEBUG ((
    DEBUG_ERROR,
    "RpiRedfishSync: inventory bios='%a' mfr='%a' model='%a' sn='%a' uuid='%a'\n",
    Inventory->BiosVersion,
    Inventory->Manufacturer,
    Inventory->Model,
    Inventory->SerialNumber,
    Inventory->UuidValid ? Inventory->Uuid : "(none)"
    ));

  return EFI_SUCCESS;
}

/**
  Map an SMBIOS type 17 MemoryType to the Redfish MemoryDeviceType enumeration.

  Returns NULL for values with no Redfish equivalent, which the caller omits
  rather than guessing -- a wrong enum is worse than an absent property.
**/
STATIC
CONST CHAR8 *
RedfishMemoryDeviceType (
  IN UINT8  SmbiosMemoryType
  )
{
  switch (SmbiosMemoryType) {
    case MemoryTypeSdram:    return "SDRAM";
    case MemoryTypeDdr:      return "DDR";
    case MemoryTypeDdr2:     return "DDR2";
    case MemoryTypeDdr3:     return "DDR3";
    case MemoryTypeDdr4:     return "DDR4";
    case MemoryTypeLpddr:    return "LPDDR_SDRAM";
    case MemoryTypeLpddr2:   return "LPDDR2_SDRAM";
    case MemoryTypeLpddr3:   return "LPDDR3_SDRAM";
    case MemoryTypeLpddr4:   return "LPDDR4_SDRAM";
    default:                 return NULL;
  }
}

/**
  Map an SMBIOS type 17 FormFactor to the Redfish BaseModuleType enumeration.
**/
STATIC
CONST CHAR8 *
RedfishBaseModuleType (
  IN UINT8  FormFactor
  )
{
  switch (FormFactor) {
    case MemoryFormFactorSodimm: return "SO_DIMM";
    case MemoryFormFactorDimm:   return "UDIMM";
    case MemoryFormFactorRimm:   return "RDIMM";
    case MemoryFormFactorFbDimm: return "LRDIMM";
    default:                     return NULL;
  }
}

/**
  Decode the type 17 size fields into MiB.

  SMBIOS overloads one 16-bit field: bit 15 selects KiB rather than MiB, 0
  means the socket is empty, 0xFFFF means unknown, and 0x7FFF means "too large
  to express here, see ExtendedSize". Returns 0 for empty or unknown, which the
  caller treats as "do not report this socket".
**/
STATIC
UINT32
RedfishMemoryCapacityMiB (
  IN SMBIOS_TABLE_TYPE17  *Type17
  )
{
  if ((Type17->Size == 0) || (Type17->Size == 0xFFFF)) {
    return 0;
  }

  if (Type17->Size == 0x7FFF) {
    //
    // ExtendedSize arrived in SMBIOS 2.7; only read it if the record is long
    // enough to contain it.
    //
    if (Type17->Hdr.Length < OFFSET_OF (SMBIOS_TABLE_TYPE17, ExtendedSize) + sizeof (Type17->ExtendedSize)) {
      return 0;
    }

    return Type17->ExtendedSize & 0x7FFFFFFF;
  }

  if ((Type17->Size & BIT15) != 0) {
    //
    // Value is in KiB. Round down; a sub-MiB module is not a thing this needs
    // to represent precisely.
    //
    return (UINT32)(Type17->Size & 0x7FFF) / 1024;
  }

  return Type17->Size;
}

EFI_STATUS
RpiRedfishCollectMemory (
  OUT RPI_REDFISH_MEMORY_MODULE  *Modules,
  IN  UINTN                      Max,
  OUT UINTN                      *Count
  )
{
  EFI_STATUS                 Status;
  EFI_SMBIOS_PROTOCOL        *Smbios;
  EFI_SMBIOS_HANDLE          Handle;
  EFI_SMBIOS_TABLE_HEADER    *Record;
  SMBIOS_TABLE_TYPE17        *Type17;
  RPI_REDFISH_MEMORY_MODULE  *Module;
  UINT32                     Capacity;

  if ((Modules == NULL) || (Count == NULL) || (Max == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  *Count = 0;
  ZeroMem (Modules, Max * sizeof (*Modules));

  Status = gBS->LocateProtocol (&gEfiSmbiosProtocolGuid, NULL, (VOID **)&Smbios);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Handle = SMBIOS_HANDLE_PI_RESERVED;
  while (!EFI_ERROR (Smbios->GetNext (Smbios, &Handle, NULL, &Record, NULL))) {
    if (Record->Type != EFI_SMBIOS_TYPE_MEMORY_DEVICE) {
      continue;
    }

    if (*Count >= Max) {
      DEBUG ((DEBUG_ERROR, "RpiRedfishSync: more than %d memory devices, truncating\n", Max));
      break;
    }

    Type17 = (SMBIOS_TABLE_TYPE17 *)Record;

    //
    // SMBIOS emits a record per socket whether or not it is populated.
    // Reporting an empty one would claim hardware that is not there.
    //
    Capacity = RedfishMemoryCapacityMiB (Type17);
    if (Capacity == 0) {
      continue;
    }

    Module              = &Modules[*Count];
    Module->CapacityMiB = Capacity;

    CopyInventoryString (
      Module->DeviceLocator,
      sizeof (Module->DeviceLocator),
      SmbiosGetString (Record, Type17->DeviceLocator)
      );
    CopyInventoryString (
      Module->BankLocator,
      sizeof (Module->BankLocator),
      SmbiosGetString (Record, Type17->BankLocator)
      );
    CopyInventoryString (
      Module->Manufacturer,
      sizeof (Module->Manufacturer),
      SmbiosGetString (Record, Type17->Manufacturer)
      );
    CopyInventoryString (
      Module->SerialNumber,
      sizeof (Module->SerialNumber),
      SmbiosGetString (Record, Type17->SerialNumber)
      );
    CopyInventoryString (
      Module->PartNumber,
      sizeof (Module->PartNumber),
      SmbiosGetString (Record, Type17->PartNumber)
      );

    Module->MemoryDeviceType = RedfishMemoryDeviceType (Type17->MemoryType);
    Module->BaseModuleType   = RedfishBaseModuleType (Type17->FormFactor);
    Module->DataWidthBits    = Type17->DataWidth;
    Module->BusWidthBits     = Type17->TotalWidth;
    Module->RatedSpeedMhz    = Type17->Speed;

    //
    // The configured speed is what the module is actually running at, and is
    // what Redfish calls OperatingSpeedMhz. It arrived in SMBIOS 2.7; fall back
    // to the rated speed on an older record.
    //
    if (Type17->Hdr.Length >= OFFSET_OF (SMBIOS_TABLE_TYPE17, ConfiguredMemoryClockSpeed) +
        sizeof (Type17->ConfiguredMemoryClockSpeed))
    {
      Module->OperatingSpeedMhz = Type17->ConfiguredMemoryClockSpeed;
    }

    if (Module->OperatingSpeedMhz == 0) {
      Module->OperatingSpeedMhz = Type17->Speed;
    }

    DEBUG ((
      DEBUG_ERROR,
      "RpiRedfishSync: memory[%d] '%a' %d MiB %a %d MHz mfr='%a' pn='%a'\n",
      *Count,
      Module->DeviceLocator,
      Module->CapacityMiB,
      Module->MemoryDeviceType != NULL ? Module->MemoryDeviceType : "(type?)",
      Module->OperatingSpeedMhz,
      Module->Manufacturer,
      Module->PartNumber
      ));

    (*Count)++;
  }

  return EFI_SUCCESS;
}

/**
  Map an SMBIOS type 4 ProcessorType to the Redfish ProcessorType enumeration.

  Returns NULL for values with no Redfish equivalent, which the caller omits
  rather than guessing -- same discipline as the type 17 mappers above.
**/
STATIC
CONST CHAR8 *
RedfishProcessorType (
  IN UINT8  SmbiosProcessorType
  )
{
  switch (SmbiosProcessorType) {
    case CentralProcessor: return "CPU";
    case DspProcessor:     return "DSP";
    case VideoProcessor:   return "GPU";
    default:               return NULL;
  }
}

/**
  Resolve a type 4 core/thread count across the SMBIOS 3.0 field widening.

  SMBIOS 3.0 added 16-bit companions for the core and thread counts: the
  original 8-bit field reads 0xFF when the real value does not fit, and the
  wide field carries it. 0 means "unknown" in both, which the JSON layer
  omits rather than reporting a CPU with no cores.

  @param[in] Narrow     The 8-bit count.
  @param[in] Wide       The 16-bit companion.
  @param[in] WideValid  Whether the record is long enough to hold Wide.

  @return The resolved count, or 0 when unknown.
**/
STATIC
UINT32
ProcessorCount (
  IN UINT8    Narrow,
  IN UINT16   Wide,
  IN BOOLEAN  WideValid
  )
{
  if ((Narrow == 0xFF) && WideValid) {
    return Wide;
  }

  return Narrow;
}

EFI_STATUS
RpiRedfishCollectProcessors (
  OUT RPI_REDFISH_PROCESSOR  *Processors,
  IN  UINTN                  Max,
  OUT UINTN                  *Count
  )
{
  EFI_STATUS               Status;
  EFI_SMBIOS_PROTOCOL      *Smbios;
  EFI_SMBIOS_HANDLE        Handle;
  EFI_SMBIOS_TABLE_HEADER  *Record;
  SMBIOS_TABLE_TYPE4       *Type4;
  RPI_REDFISH_PROCESSOR    *Processor;
  BOOLEAN                  HasCounts;
  BOOLEAN                  HasWideCounts;
  UINT64                   IdRegisters;

  if ((Processors == NULL) || (Count == NULL) || (Max == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  *Count = 0;
  ZeroMem (Processors, Max * sizeof (*Processors));

  Status = gBS->LocateProtocol (&gEfiSmbiosProtocolGuid, NULL, (VOID **)&Smbios);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Handle = SMBIOS_HANDLE_PI_RESERVED;
  while (!EFI_ERROR (Smbios->GetNext (Smbios, &Handle, NULL, &Record, NULL))) {
    if (Record->Type != EFI_SMBIOS_TYPE_PROCESSOR_INFORMATION) {
      continue;
    }

    if (*Count >= Max) {
      DEBUG ((DEBUG_ERROR, "RpiRedfishSync: more than %d processors, truncating\n", Max));
      break;
    }

    Type4 = (SMBIOS_TABLE_TYPE4 *)Record;

    //
    // Status bit 6 is "CPU Socket Populated"; bits 2:0 are the CPU status,
    // where 1 is enabled. An empty socket is not a processor.
    //
    if ((Type4->Status & 0x40) == 0) {
      continue;
    }

    Processor          = &Processors[*Count];
    Processor->Enabled = (BOOLEAN)((Type4->Status & 0x07) == 1);

    CopyInventoryString (
      Processor->Socket,
      sizeof (Processor->Socket),
      SmbiosGetString (Record, Type4->Socket)
      );
    CopyInventoryString (
      Processor->Manufacturer,
      sizeof (Processor->Manufacturer),
      SmbiosGetString (Record, Type4->ProcessorManufacturer)
      );
    CopyInventoryString (
      Processor->Model,
      sizeof (Processor->Model),
      SmbiosGetString (Record, Type4->ProcessorVersion)
      );

    Processor->ProcessorType     = RedfishProcessorType (Type4->ProcessorType);
    Processor->MaxSpeedMhz       = Type4->MaxSpeed;
    Processor->OperatingSpeedMhz = Type4->CurrentSpeed;

    //
    // The raw identification registers. PlatformSmbiosDxe writes MIDR_EL1
    // over this field on AArch64, which is exactly what Redfish's
    // ProcessorId/IdentificationRegisters is for. Copied out rather than
    // cast: the field is a struct and need not be 8-byte aligned.
    //
    CopyMem (&IdRegisters, &Type4->ProcessorId, sizeof (IdRegisters));
    if (IdRegisters != 0) {
      AsciiSPrint (
        Processor->IdRegisters,
        sizeof (Processor->IdRegisters),
        "0x%016Lx",
        IdRegisters
        );
    }

    //
    // Serial and part number arrived in SMBIOS 2.3, the core/thread counts in
    // 2.5, and their 16-bit companions in 3.0. Guard each block on the record
    // length so a shorter table is read as "absent" rather than as garbage.
    //
    if (Type4->Hdr.Length >= OFFSET_OF (SMBIOS_TABLE_TYPE4, PartNumber) +
        sizeof (Type4->PartNumber))
    {
      CopyInventoryString (
        Processor->SerialNumber,
        sizeof (Processor->SerialNumber),
        SmbiosGetString (Record, Type4->SerialNumber)
        );
      CopyInventoryString (
        Processor->PartNumber,
        sizeof (Processor->PartNumber),
        SmbiosGetString (Record, Type4->PartNumber)
        );
    }

    HasCounts = (BOOLEAN)(Type4->Hdr.Length >=
                          OFFSET_OF (SMBIOS_TABLE_TYPE4, ThreadCount) +
                          sizeof (Type4->ThreadCount));
    HasWideCounts = (BOOLEAN)(Type4->Hdr.Length >=
                              OFFSET_OF (SMBIOS_TABLE_TYPE4, ThreadCount2) +
                              sizeof (Type4->ThreadCount2));

    if (HasCounts) {
      Processor->TotalCores = ProcessorCount (
                                Type4->CoreCount,
                                Type4->CoreCount2,
                                HasWideCounts
                                );
      Processor->TotalEnabledCores = ProcessorCount (
                                       Type4->EnabledCoreCount,
                                       Type4->EnabledCoreCount2,
                                       HasWideCounts
                                       );
      Processor->TotalThreads = ProcessorCount (
                                  Type4->ThreadCount,
                                  Type4->ThreadCount2,
                                  HasWideCounts
                                  );
    }

    DEBUG ((
      DEBUG_ERROR,
      "RpiRedfishSync: processor[%d] '%a' mfr='%a' model='%a' %d/%d cores %d threads %d MHz id=%a\n",
      *Count,
      Processor->Socket,
      Processor->Manufacturer,
      Processor->Model,
      Processor->TotalEnabledCores,
      Processor->TotalCores,
      Processor->TotalThreads,
      Processor->OperatingSpeedMhz,
      (Processor->IdRegisters[0] != '\0') ? Processor->IdRegisters : "(none)"
      ));

    (*Count)++;
  }

  return EFI_SUCCESS;
}

/**
  Append a "Name": "Value" member when Value is non-empty.

  Values come from SMBIOS strings, which may legitimately contain a double quote
  or backslash; those are escaped rather than dropped so the body stays valid
  JSON. Control characters are replaced with spaces for the same reason.

  @param[in,out] Json      Buffer being built.
  @param[in]     JsonSize  Size of Json.
  @param[in]     Name      Member name.
  @param[in]     Value     Member value; skipped when NULL or empty.
**/
STATIC
VOID
AppendJsonString (
  IN OUT CHAR8        *Json,
  IN     UINTN        JsonSize,
  IN     CONST CHAR8  *Name,
  IN     CONST CHAR8  *Value
  )
{
  CHAR8  Escaped[RPI_REDFISH_STR_MAX * 2];
  UINTN  In;
  UINTN  Out;

  if ((Value == NULL) || (Value[0] == '\0')) {
    return;
  }

  for (In = 0, Out = 0; (Value[In] != '\0') && (Out < sizeof (Escaped) - 2); In++) {
    if ((Value[In] == '"') || (Value[In] == '\\')) {
      Escaped[Out++] = '\\';
      Escaped[Out++] = Value[In];
    } else if ((UINT8)Value[In] < 0x20) {
      Escaped[Out++] = ' ';
    } else {
      Escaped[Out++] = Value[In];
    }
  }

  Escaped[Out] = '\0';

  AsciiStrCatS (Json, JsonSize, ",\"");
  AsciiStrCatS (Json, JsonSize, Name);
  AsciiStrCatS (Json, JsonSize, "\":\"");
  AsciiStrCatS (Json, JsonSize, Escaped);
  AsciiStrCatS (Json, JsonSize, "\"");
}

EFI_STATUS
RpiRedfishBuildSystemPatch (
  IN  RPI_REDFISH_HOST_INVENTORY  *Inventory,
  OUT CHAR8                       **Json
  )
{
  CHAR8  *Body;

  if ((Inventory == NULL) || (Json == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Body = AllocateZeroPool (RPI_REDFISH_JSON_MAX);
  if (Body == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // BootProgress leads so the object is never empty: every other member is
  // conditional on SMBIOS actually having supplied it. Subsequent members are
  // emitted with a leading comma by AppendJsonString.
  //
  AsciiSPrint (
    Body,
    RPI_REDFISH_JSON_MAX,
    "{\"BootProgress\":{\"LastState\":\"%a\"}",
    RPI_REDFISH_BOOT_PROGRESS
    );

  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "BiosVersion", Inventory->BiosVersion);
  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "Manufacturer", Inventory->Manufacturer);
  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "Model", Inventory->Model);
  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "SerialNumber", Inventory->SerialNumber);
  if (Inventory->UuidValid) {
    AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "UUID", Inventory->Uuid);
  }

  AsciiStrCatS (Body, RPI_REDFISH_JSON_MAX, "}");

  *Json = Body;
  return EFI_SUCCESS;
}

/**
  Append a "Name": <number> member when Value is non-zero.

  Zero is treated as "SMBIOS did not say" throughout type 17 -- an unknown speed
  or width is encoded as 0 -- so omitting it is more honest than reporting a
  module that runs at 0 MHz.
**/
STATIC
VOID
AppendJsonNumber (
  IN OUT CHAR8        *Json,
  IN     UINTN        JsonSize,
  IN     CONST CHAR8  *Name,
  IN     UINT32       Value
  )
{
  CHAR8  Buffer[32];

  if (Value == 0) {
    return;
  }

  AsciiSPrint (Buffer, sizeof (Buffer), ",\"%a\":%d", Name, Value);
  AsciiStrCatS (Json, JsonSize, Buffer);
}

EFI_STATUS
RpiRedfishBuildMemoryPost (
  IN  RPI_REDFISH_MEMORY_MODULE  *Module,
  OUT CHAR8                      **Json
  )
{
  CHAR8  *Body;

  if ((Module == NULL) || (Json == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Body = AllocateZeroPool (RPI_REDFISH_JSON_MAX);
  if (Body == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // @odata.type leads so the object is never empty and the BMC can tell which
  // schema version this is; everything after it is conditional and emitted with
  // a leading comma.
  //
  // DeviceLocator doubles as the member's identity: the BMC uses it as the
  // resource Id, so re-reporting the same socket on a later boot updates that
  // member rather than creating a second one.
  //
  AsciiSPrint (
    Body,
    RPI_REDFISH_JSON_MAX,
    "{\"@odata.type\":\"#Memory.v1_7_1.Memory\""
    ",\"Status\":{\"State\":\"Enabled\",\"Health\":\"OK\"}"
    ",\"MemoryType\":\"DRAM\""
    );

  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "DeviceLocator", Module->DeviceLocator);
  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "Name", Module->DeviceLocator);
  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "Manufacturer", Module->Manufacturer);
  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "SerialNumber", Module->SerialNumber);
  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "PartNumber", Module->PartNumber);

  if (Module->MemoryDeviceType != NULL) {
    AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "MemoryDeviceType", Module->MemoryDeviceType);
  }

  if (Module->BaseModuleType != NULL) {
    AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "BaseModuleType", Module->BaseModuleType);
  }

  AppendJsonNumber (Body, RPI_REDFISH_JSON_MAX, "CapacityMiB", Module->CapacityMiB);
  AppendJsonNumber (Body, RPI_REDFISH_JSON_MAX, "OperatingSpeedMhz", Module->OperatingSpeedMhz);
  AppendJsonNumber (Body, RPI_REDFISH_JSON_MAX, "DataWidthBits", Module->DataWidthBits);
  AppendJsonNumber (Body, RPI_REDFISH_JSON_MAX, "BusWidthBits", Module->BusWidthBits);

  //
  // AllowedSpeedsMHz is an array of what the module itself supports, as opposed
  // to OperatingSpeedMhz which is what it was configured to.
  //
  if (Module->RatedSpeedMhz != 0) {
    CHAR8  Speeds[48];

    AsciiSPrint (Speeds, sizeof (Speeds), ",\"AllowedSpeedsMHz\":[%d]", Module->RatedSpeedMhz);
    AsciiStrCatS (Body, RPI_REDFISH_JSON_MAX, Speeds);
  }

  AsciiStrCatS (Body, RPI_REDFISH_JSON_MAX, "}");

  *Json = Body;
  return EFI_SUCCESS;
}

EFI_STATUS
RpiRedfishBuildProcessorPost (
  IN  RPI_REDFISH_PROCESSOR  *Processor,
  OUT CHAR8                  **Json
  )
{
  CHAR8  *Body;

  if ((Processor == NULL) || (Json == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Body = AllocateZeroPool (RPI_REDFISH_JSON_MAX);
  if (Body == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // @odata.type and Status lead so the object is never empty; everything after
  // is conditional and emitted with a leading comma.
  //
  // The architecture pair is stated rather than derived from ProcessorFamily2:
  // this driver only builds for AARCH64, so any processor it can describe is
  // an A64 part. Deriving it from the SMBIOS family would add a mapping table
  // whose every entry is the same answer.
  //
  // Socket doubles as the member's identity, the way DeviceLocator does for
  // memory: re-reporting the same socket on a later boot should update that
  // member rather than create a second one.
  //
  AsciiSPrint (
    Body,
    RPI_REDFISH_JSON_MAX,
    "{\"@odata.type\":\"#Processor.v1_16_0.Processor\""
    ",\"Status\":{\"State\":\"%a\",\"Health\":\"OK\"}"
    ",\"ProcessorArchitecture\":\"ARM\""
    ",\"InstructionSet\":\"ARM-A64\"",
    Processor->Enabled ? "Enabled" : "Disabled"
    );

  if (Processor->ProcessorType != NULL) {
    AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "ProcessorType", Processor->ProcessorType);
  }

  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "Socket", Processor->Socket);
  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "Name", Processor->Socket);
  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "Manufacturer", Processor->Manufacturer);
  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "Model", Processor->Model);
  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "SerialNumber", Processor->SerialNumber);
  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "PartNumber", Processor->PartNumber);

  AppendJsonNumber (Body, RPI_REDFISH_JSON_MAX, "MaxSpeedMHz", Processor->MaxSpeedMhz);
  AppendJsonNumber (Body, RPI_REDFISH_JSON_MAX, "OperatingSpeedMHz", Processor->OperatingSpeedMhz);
  AppendJsonNumber (Body, RPI_REDFISH_JSON_MAX, "TotalCores", Processor->TotalCores);
  AppendJsonNumber (Body, RPI_REDFISH_JSON_MAX, "TotalEnabledCores", Processor->TotalEnabledCores);
  AppendJsonNumber (Body, RPI_REDFISH_JSON_MAX, "TotalThreads", Processor->TotalThreads);

  if (Processor->IdRegisters[0] != '\0') {
    CHAR8  IdObject[80];

    AsciiSPrint (
      IdObject,
      sizeof (IdObject),
      ",\"ProcessorId\":{\"IdentificationRegisters\":\"%a\"}",
      Processor->IdRegisters
      );
    AsciiStrCatS (Body, RPI_REDFISH_JSON_MAX, IdObject);
  }

  AsciiStrCatS (Body, RPI_REDFISH_JSON_MAX, "}");

  *Json = Body;
  return EFI_SUCCESS;
}

/**
  Copy a fixed-width identify-data string field, undoing its encoding.

  ATA and NVMe identify strings are fixed-width, space-padded and not
  NUL-terminated. ATA additionally stores them byte-swapped within each 16-bit
  word ("oMedM l" for "Model M"), which SwapPairs undoes. The result is
  NUL-terminated with the leading and trailing padding removed.

  @param[out] Dest       Destination buffer.
  @param[in]  DestSize   Size of Dest in bytes.
  @param[in]  Source     Fixed-width source field.
  @param[in]  SourceLen  Width of the source field; even when SwapPairs.
  @param[in]  SwapPairs  Undo ATA's per-word byte swap.
**/
STATIC
VOID
CopyIdentifyString (
  OUT CHAR8        *Dest,
  IN  UINTN        DestSize,
  IN  CONST UINT8  *Source,
  IN  UINTN        SourceLen,
  IN  BOOLEAN      SwapPairs
  )
{
  UINTN  Length;
  UINTN  Index;
  UINTN  Start;

  if (DestSize == 0) {
    return;
  }

  Length = SourceLen;
  if (Length >= DestSize) {
    Length = DestSize - 1;
  }

  for (Index = 0; Index < Length; Index++) {
    Dest[Index] = (CHAR8)Source[SwapPairs ? (Index ^ 1) : Index];
  }

  //
  // Trim the padding: trailing first (spaces, and NULs from a device that
  // pads with them instead), then leading.
  //
  while ((Length > 0) && ((UINT8)Dest[Length - 1] <= ' ')) {
    Length--;
  }

  Dest[Length] = '\0';

  Start = 0;
  while (Dest[Start] == ' ') {
    Start++;
  }

  if (Start > 0) {
    CopyMem (Dest, Dest + Start, Length - Start + 1);
  }
}

/**
  Fill in a drive's identity from its NVMe controller.

  The model, serial and firmware revision live in the *controller* identify
  data, which EFI_DISK_INFO_PROTOCOL does not expose for NVMe -- its Identify()
  returns the namespace data, which has neither. So reach the controller's
  pass-thru and send ADMIN_IDENTIFY ourselves, the same way UefiBootManagerLib
  builds its "NVMe: <model>" boot descriptions (BmGetNvmeDescription).

  @param[in]  Handle  Handle carrying the NVMe-interface DiskInfo.
  @param[out] Drive   Receives model, serial, revision, protocol, media type.

  @retval EFI_SUCCESS  Drive identity was filled in.
**/
STATIC
EFI_STATUS
CollectNvmeDrive (
  IN  EFI_HANDLE         Handle,
  OUT RPI_REDFISH_DRIVE  *Drive
  )
{
  EFI_STATUS                                Status;
  EFI_DEVICE_PATH_PROTOCOL                  *DevicePath;
  EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL        *Passthru;
  EFI_NVM_EXPRESS_PASS_THRU_COMMAND_PACKET  Packet;
  EFI_NVM_EXPRESS_COMMAND                   Command;
  EFI_NVM_EXPRESS_COMPLETION                Completion;
  NVME_ADMIN_CONTROLLER_DATA                Data;

  Status = gBS->HandleProtocol (Handle, &gEfiDevicePathProtocolGuid, (VOID **)&DevicePath);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->LocateDevicePath (&gEfiNvmExpressPassThruProtocolGuid, &DevicePath, &Handle);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->HandleProtocol (Handle, &gEfiNvmExpressPassThruProtocolGuid, (VOID **)&Passthru);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Packet, sizeof (Packet));
  ZeroMem (&Command, sizeof (Command));
  ZeroMem (&Completion, sizeof (Completion));
  ZeroMem (&Data, sizeof (Data));

  Command.Cdw0.Opcode = NVME_ADMIN_IDENTIFY_CMD;
  //
  // CNS 1 identifies the controller; NSID is unused for that structure and
  // stays 0.
  //
  Command.Cdw10 = 1;
  Command.Flags = CDW10_VALID;

  Packet.NvmeCmd        = &Command;
  Packet.NvmeCompletion = &Completion;
  Packet.TransferBuffer = &Data;
  Packet.TransferLength = sizeof (Data);
  Packet.CommandTimeout = EFI_TIMER_PERIOD_SECONDS (5);
  Packet.QueueType      = NVME_ADMIN_QUEUE;

  Status = Passthru->PassThru (Passthru, 0, &Packet, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CopyIdentifyString (Drive->Model, sizeof (Drive->Model), Data.Mn, sizeof (Data.Mn), FALSE);
  CopyIdentifyString (Drive->SerialNumber, sizeof (Drive->SerialNumber), Data.Sn, sizeof (Data.Sn), FALSE);
  CopyIdentifyString (Drive->Revision, sizeof (Drive->Revision), Data.Fr, sizeof (Data.Fr), FALSE);

  Drive->Protocol  = "NVMe";
  Drive->MediaType = "SSD";

  return EFI_SUCCESS;
}

/**
  Fill in a drive's identity from its ATA IDENTIFY data.

  @param[in]  DiskInfo  AHCI- or IDE-interface DiskInfo instance.
  @param[out] Drive     Receives model, serial, revision, protocol, media type.

  @retval EFI_SUCCESS  Drive identity was filled in.
**/
STATIC
EFI_STATUS
CollectAtaDrive (
  IN  EFI_DISK_INFO_PROTOCOL  *DiskInfo,
  OUT RPI_REDFISH_DRIVE       *Drive
  )
{
  EFI_STATUS         Status;
  ATA_IDENTIFY_DATA  Data;
  UINT32             Size;

  ZeroMem (&Data, sizeof (Data));
  Size = sizeof (Data);

  Status = DiskInfo->Identify (DiskInfo, &Data, &Size);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CopyIdentifyString (Drive->Model, sizeof (Drive->Model), (CONST UINT8 *)Data.ModelName, sizeof (Data.ModelName), TRUE);
  CopyIdentifyString (Drive->SerialNumber, sizeof (Drive->SerialNumber), (CONST UINT8 *)Data.SerialNo, sizeof (Data.SerialNo), TRUE);
  CopyIdentifyString (Drive->Revision, sizeof (Drive->Revision), (CONST UINT8 *)Data.FirmwareVer, sizeof (Data.FirmwareVer), TRUE);

  Drive->Protocol = "SATA";

  //
  // Word 217: 1 means non-rotating, 0x0401-0xFFFE is an RPM figure. Everything
  // else is "not reported", left NULL rather than guessed.
  //
  if (Data.nominal_media_rotation_rate == 1) {
    Drive->MediaType = "SSD";
  } else if ((Data.nominal_media_rotation_rate >= 0x0401) && (Data.nominal_media_rotation_rate <= 0xFFFE)) {
    Drive->MediaType = "HDD";
  }

  return EFI_SUCCESS;
}

EFI_STATUS
RpiRedfishCollectDrives (
  OUT RPI_REDFISH_DRIVE  *Drives,
  IN  UINTN              Max,
  OUT UINTN              *Count
  )
{
  EFI_STATUS              Status;
  EFI_HANDLE              *Handles;
  UINTN                   HandleCount;
  UINTN                   Index;
  UINTN                   Existing;
  EFI_DISK_INFO_PROTOCOL  *DiskInfo;
  EFI_BLOCK_IO_PROTOCOL   *BlockIo;
  RPI_REDFISH_DRIVE       *Drive;

  if ((Drives == NULL) || (Count == NULL) || (Max == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  *Count = 0;
  ZeroMem (Drives, Max * sizeof (*Drives));

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiDiskInfoProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    //
    // A diskless boot (netboot with no local media connected) is a normal
    // outcome, not a failure.
    //
    return EFI_SUCCESS;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    if (*Count >= Max) {
      DEBUG ((DEBUG_ERROR, "RpiRedfishSync: more than %d drives, truncating\n", Max));
      break;
    }

    Status = gBS->HandleProtocol (Handles[Index], &gEfiDiskInfoProtocolGuid, (VOID **)&DiskInfo);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Drive = &Drives[*Count];

    if (CompareGuid (&DiskInfo->Interface, &gEfiDiskInfoNvmeInterfaceGuid)) {
      Status = CollectNvmeDrive (Handles[Index], Drive);
    } else if (CompareGuid (&DiskInfo->Interface, &gEfiDiskInfoAhciInterfaceGuid) ||
               CompareGuid (&DiskInfo->Interface, &gEfiDiskInfoIdeInterfaceGuid))
    {
      Status = CollectAtaDrive (DiskInfo, Drive);
    } else {
      //
      // Everything else is skipped deliberately. The only USB mass storage
      // this board ever sees is the BMC's own virtual-media gadget --
      // reporting that would hand the BMC's mounted image back to it as host
      // hardware. The SD card has no DiskInfo identity worth reporting.
      //
      continue;
    }

    if (EFI_ERROR (Status) || ((Drive->Model[0] == '\0') && (Drive->SerialNumber[0] == '\0'))) {
      ZeroMem (Drive, sizeof (*Drive));
      continue;
    }

    //
    // An NVMe controller with several namespaces yields one DiskInfo handle
    // per namespace, each answering with the controller's identity. Report
    // the device once.
    //
    for (Existing = 0; Existing < *Count; Existing++) {
      if ((AsciiStrCmp (Drives[Existing].SerialNumber, Drive->SerialNumber) == 0) &&
          (AsciiStrCmp (Drives[Existing].Model, Drive->Model) == 0))
      {
        break;
      }
    }

    if (Existing < *Count) {
      ZeroMem (Drive, sizeof (*Drive));
      continue;
    }

    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (!EFI_ERROR (Status) && (BlockIo->Media != NULL) && BlockIo->Media->MediaPresent) {
      Drive->CapacityBytes = MultU64x32 (BlockIo->Media->LastBlock + 1, BlockIo->Media->BlockSize);
    }

    DEBUG ((
      DEBUG_ERROR,
      "RpiRedfishSync: drive[%d] %a '%a' sn='%a' fw='%a' %ld bytes\n",
      *Count,
      Drive->Protocol,
      Drive->Model,
      Drive->SerialNumber,
      Drive->Revision,
      Drive->CapacityBytes
      ));

    (*Count)++;
  }

  FreePool (Handles);
  return EFI_SUCCESS;
}

EFI_STATUS
RpiRedfishBuildDrivePost (
  IN  RPI_REDFISH_DRIVE  *Drive,
  OUT CHAR8              **Json
  )
{
  CHAR8  *Body;

  if ((Drive == NULL) || (Json == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Body = AllocateZeroPool (RPI_REDFISH_JSON_MAX);
  if (Body == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // SerialNumber doubles as the member's identity: the BMC uses it as the
  // resource Id, so re-reporting the same drive on a later boot updates that
  // member rather than creating a second one.
  //
  AsciiSPrint (
    Body,
    RPI_REDFISH_JSON_MAX,
    "{\"@odata.type\":\"#Drive.v1_4_0.Drive\""
    ",\"Status\":{\"State\":\"Enabled\",\"Health\":\"OK\"}"
    );

  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "Name", Drive->Model);
  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "Model", Drive->Model);
  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "SerialNumber", Drive->SerialNumber);
  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "Revision", Drive->Revision);
  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "Protocol", Drive->Protocol);

  if (Drive->MediaType != NULL) {
    AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "MediaType", Drive->MediaType);
  }

  //
  // Not AppendJsonNumber: its UINT32 tops out two orders of magnitude below a
  // routine disk size.
  //
  if (Drive->CapacityBytes != 0) {
    CHAR8  Capacity[48];

    AsciiSPrint (Capacity, sizeof (Capacity), ",\"CapacityBytes\":%ld", Drive->CapacityBytes);
    AsciiStrCatS (Body, RPI_REDFISH_JSON_MAX, Capacity);
  }

  AsciiStrCatS (Body, RPI_REDFISH_JSON_MAX, "}");

  *Json = Body;
  return EFI_SUCCESS;
}

/**
  Does this device path pass through USB?

  @param[in] DevicePath  Device path to inspect; NULL is treated as "yes" so
                         an unknown path is excluded rather than mistaken for
                         the onboard NIC.

  @retval TRUE   A USB node is present (or the path is unknowable).
  @retval FALSE  No USB node.
**/
STATIC
BOOLEAN
NicPathHasUsbNode (
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Node;

  if (DevicePath == NULL) {
    return TRUE;
  }

  for (Node = DevicePath; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if ((DevicePathType (Node) == MESSAGING_DEVICE_PATH) &&
        (DevicePathSubType (Node) == MSG_USB_DP))
    {
      return TRUE;
    }
  }

  return FALSE;
}

EFI_STATUS
RpiRedfishCollectNics (
  OUT RPI_REDFISH_NIC  *Nics,
  IN  UINTN            Max,
  OUT UINTN            *Count
  )
{
  EFI_STATUS                   Status;
  EFI_HANDLE                   *Handles;
  UINTN                        HandleCount;
  UINTN                        Index;
  EFI_SIMPLE_NETWORK_PROTOCOL  *Snp;
  EFI_DEVICE_PATH_PROTOCOL     *DevicePath;
  RPI_REDFISH_NIC              *Nic;

  if ((Nics == NULL) || (Count == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Count = 0;

  Handles     = NULL;
  HandleCount = 0;
  Status      = gBS->LocateHandleBuffer (
                       ByProtocol,
                       &gEfiSimpleNetworkProtocolGuid,
                       NULL,
                       &HandleCount,
                       &Handles
                       );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; (Index < HandleCount) && (*Count < Max); Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiSimpleNetworkProtocolGuid,
                    (VOID **)&Snp
                    );
    if (EFI_ERROR (Status) || (Snp->Mode == NULL)) {
      continue;
    }

    //
    // Ethernet only (IfType 1, RFC 1700), with the address size that
    // implies. SNP publishes nothing else on this platform, but say so
    // rather than assume so.
    //
    if ((Snp->Mode->IfType != 1) || (Snp->Mode->HwAddressSize != 6)) {
      continue;
    }

    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiDevicePathProtocolGuid,
                    (VOID **)&DevicePath
                    );
    if (EFI_ERROR (Status) || NicPathHasUsbNode (DevicePath)) {
      continue;
    }

    Nic = &Nics[*Count];
    ZeroMem (Nic, sizeof (*Nic));
    CopyMem (Nic->Mac, Snp->Mode->CurrentAddress.Addr, sizeof (Nic->Mac));
    CopyMem (Nic->PermanentMac, Snp->Mode->PermanentAddress.Addr, sizeof (Nic->PermanentMac));
    Nic->MediaPresentSupported = Snp->Mode->MediaPresentSupported;
    Nic->MediaPresent          = Snp->Mode->MediaPresent;
    (*Count)++;
  }

  FreePool (Handles);
  return EFI_SUCCESS;
}

EFI_STATUS
RpiRedfishBuildNicPost (
  IN  RPI_REDFISH_NIC  *Nic,
  OUT CHAR8            **Json
  )
{
  CHAR8        *Body;
  CHAR8        Id[32];
  CHAR8        Mac[24];
  CHAR8        PermanentMac[24];
  CONST UINT8  *KeyMac;
  BOOLEAN      PermanentKnown;
  UINTN        Index;

  if ((Nic == NULL) || (Json == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Body = AllocateZeroPool (RPI_REDFISH_JSON_MAX);
  if (Body == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // The Id doubles as the member's identity on the BMC, so it must be stable
  // across boots regardless of enumeration order: derive it from the
  // permanent MAC, falling back to the current one when the permanent
  // address is unpopulated (all zero).
  //
  PermanentKnown = FALSE;
  for (Index = 0; Index < sizeof (Nic->PermanentMac); Index++) {
    if (Nic->PermanentMac[Index] != 0) {
      PermanentKnown = TRUE;
      break;
    }
  }

  KeyMac = PermanentKnown ? Nic->PermanentMac : Nic->Mac;
  AsciiSPrint (
    Id,
    sizeof (Id),
    "NIC-%02X%02X%02X%02X%02X%02X",
    KeyMac[0], KeyMac[1], KeyMac[2], KeyMac[3], KeyMac[4], KeyMac[5]
    );

  AsciiSPrint (
    Mac,
    sizeof (Mac),
    "%02X:%02X:%02X:%02X:%02X:%02X",
    Nic->Mac[0], Nic->Mac[1], Nic->Mac[2], Nic->Mac[3], Nic->Mac[4], Nic->Mac[5]
    );

  AsciiSPrint (
    Body,
    RPI_REDFISH_JSON_MAX,
    "{\"@odata.type\":\"#EthernetInterface.v1_8_0.EthernetInterface\""
    ",\"Status\":{\"State\":\"Enabled\",\"Health\":\"OK\"}"
    ",\"InterfaceEnabled\":true"
    );

  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "Id", Id);
  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "Name", "Ethernet Interface");
  AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "MACAddress", Mac);

  if (PermanentKnown) {
    AsciiSPrint (
      PermanentMac,
      sizeof (PermanentMac),
      "%02X:%02X:%02X:%02X:%02X:%02X",
      Nic->PermanentMac[0], Nic->PermanentMac[1], Nic->PermanentMac[2],
      Nic->PermanentMac[3], Nic->PermanentMac[4], Nic->PermanentMac[5]
      );
    AppendJsonString (Body, RPI_REDFISH_JSON_MAX, "PermanentMACAddress", PermanentMac);
  }

  //
  // LinkStatus only when the SNP can actually see media; a NIC that cannot
  // report it gets no claim rather than an invented one.
  //
  if (Nic->MediaPresentSupported) {
    AppendJsonString (
      Body,
      RPI_REDFISH_JSON_MAX,
      "LinkStatus",
      Nic->MediaPresent ? "LinkUp" : "LinkDown"
      );
  }

  AsciiStrCatS (Body, RPI_REDFISH_JSON_MAX, "}");

  *Json = Body;
  return EFI_SUCCESS;
}
