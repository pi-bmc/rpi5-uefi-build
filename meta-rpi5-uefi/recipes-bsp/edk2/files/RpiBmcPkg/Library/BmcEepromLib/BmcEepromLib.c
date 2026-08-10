/** @file

  EEPROM region accessors for the BMC shared 24c256 on RP1 I2C1.

  Implements BmcEepromLib.h over EFI_I2C_MASTER_PROTOCOL:

    - 2-byte big-endian data addressing, as both a real 24c256 and the
      BMC's Linux i2c-slave-eeprom emulation expect;
    - reads chunked to a controller-friendly size, each chunk a
      write[2-byte address] + read[N] packet with a repeated start;
    - writes chunked so no chunk crosses a PcdBmcEepromPageSize page
      boundary, each page-write followed by an ACK-poll (a real 24c256
      needs ~5-10 ms of internal write time during which it NACKs its
      address; the BMC's emulated slave ACKs immediately, so the first
      probe succeeds and the poll costs nothing there);
    - offsets/lengths bounds-checked against PcdBmcEepromCapacity.

  EEPROM paging/ACK-poll behavior modeled on Marvell's MvEepromDxe.

  Copyright (C) 2016 Marvell International Ltd.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/BmcEepromLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/I2cMaster.h>

//
// Read chunk size. Reads have no page-boundary constraint (the EEPROM's
// address counter rolls across pages), so this is purely a
// controller-friendly transaction size.
//
#define BMC_EEPROM_READ_CHUNK  128

//
// ACK-poll: probe every 1 ms until ACK or 15 ms timeout. A real 24c256
// finishes its internal page-write cycle in ~5-10 ms; the emulated slave
// ACKs the very first probe.
//
#define BMC_EEPROM_ACK_POLL_INTERVAL_US  1000
#define BMC_EEPROM_ACK_POLL_TRIES        15

//
// EFI_I2C_REQUEST_PACKET has a single-element operation array; this
// provides storage for the two-operation (write + read) form.
//
typedef struct {
  UINTN                OperationCount;
  EFI_I2C_OPERATION    Operation[2];
} BMC_EEPROM_I2C_PACKET;

/**
  Bounds-check an EEPROM access against PcdBmcEepromCapacity.

  @param[in] Offset   First byte of the access.
  @param[in] Length   Number of bytes accessed.

  @retval EFI_SUCCESS             The access lies within the EEPROM.
  @retval EFI_INVALID_PARAMETER   The access overruns the EEPROM.
**/
STATIC
EFI_STATUS
BmcEepromCheckBounds (
  IN UINT32  Offset,
  IN UINTN   Length
  )
{
  UINT32  Capacity;

  Capacity = PcdGet32 (PcdBmcEepromCapacity);

  if ((Length > Capacity) || (Offset > Capacity - (UINT32)Length)) {
    DEBUG ((
      DEBUG_ERROR,
      "BmcEeprom: access 0x%x+0x%x overruns capacity 0x%x\n",
      Offset,
      (UINT32)Length,
      Capacity
      ));
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

/**
  Issue a 2-byte address write probe: START, slave address, two data
  address bytes, STOP. Doubles as the ACK-poll probe (the slave NACKs
  while an internal write cycle is in progress) and as the address-set
  cost of a write chunk.

  @param[in] I2cMaster   The I2C master to probe through.
  @param[in] Offset      EEPROM data address to load, big-endian on the wire.

  @retval EFI_SUCCESS   The slave ACKed the probe.
  @retval other         The slave NACKed or the transfer failed.
**/
STATIC
EFI_STATUS
BmcEepromProbe (
  IN EFI_I2C_MASTER_PROTOCOL  *I2cMaster,
  IN UINT32                   Offset
  )
{
  BMC_EEPROM_I2C_PACKET  Packet;
  UINT8                  Address[2];

  Address[0] = (UINT8)(Offset >> 8);
  Address[1] = (UINT8)Offset;

  Packet.OperationCount             = 1;
  Packet.Operation[0].Flags         = 0;
  Packet.Operation[0].LengthInBytes = sizeof (Address);
  Packet.Operation[0].Buffer        = Address;

  return I2cMaster->StartRequest (
                      I2cMaster,
                      PcdGet8 (PcdBmcEepromSlaveAddress),
                      (EFI_I2C_REQUEST_PACKET *)&Packet,
                      NULL,
                      NULL
                      );
}

/**
  Wait for the EEPROM to finish its internal write cycle by ACK-polling.

  @param[in] I2cMaster   The I2C master to poll through.
  @param[in] Offset      EEPROM data address used for the probe writes.

  @retval EFI_SUCCESS   The slave ACKed within the poll window.
  @retval EFI_TIMEOUT   No ACK within BMC_EEPROM_ACK_POLL_TRIES ms.
**/
STATIC
EFI_STATUS
BmcEepromAckPoll (
  IN EFI_I2C_MASTER_PROTOCOL  *I2cMaster,
  IN UINT32                   Offset
  )
{
  EFI_STATUS  Status;
  UINTN       Try;

  for (Try = 0; Try <= BMC_EEPROM_ACK_POLL_TRIES; Try++) {
    Status = BmcEepromProbe (I2cMaster, Offset);
    if (!EFI_ERROR (Status)) {
      return EFI_SUCCESS;
    }

    gBS->Stall (BMC_EEPROM_ACK_POLL_INTERVAL_US);
  }

  DEBUG ((DEBUG_ERROR, "BmcEeprom: ACK-poll timeout at offset 0x%x\n", Offset));
  return EFI_TIMEOUT;
}

/**
  Locate the (sole) I2C master and prepare it for EEPROM access.

  Convenience wrapper: LocateProtocol (gEfiI2cMasterProtocolGuid) and set
  the bus to 100 kHz.

  @param[out] I2cMaster   The located master.

  @retval EFI_SUCCESS     Master located and configured.
  @retval EFI_NOT_FOUND   No I2C master present (yet).
**/
EFI_STATUS
EFIAPI
BmcEepromLocateI2cMaster (
  OUT EFI_I2C_MASTER_PROTOCOL  **I2cMaster
  )
{
  EFI_STATUS  Status;
  UINTN       BusClockHertz;

  if (I2cMaster == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->LocateProtocol (
                  &gEfiI2cMasterProtocolGuid,
                  NULL,
                  (VOID **)I2cMaster
                  );
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  BusClockHertz = 100000;
  return (*I2cMaster)->SetBusFrequency (*I2cMaster, &BusClockHertz);
}

/**
  Read Length bytes at EEPROM Offset into Data.

  @param[in]  I2cMaster   The I2C master to use.
  @param[in]  Offset      EEPROM byte offset to read from.
  @param[in]  Length      Number of bytes to read.
  @param[out] Data        Destination buffer.

  @retval EFI_SUCCESS             All bytes read.
  @retval EFI_INVALID_PARAMETER   NULL pointer or out-of-bounds access.
  @retval other                   Propagated I2C transfer failure.
**/
EFI_STATUS
EFIAPI
BmcEepromRead (
  IN  EFI_I2C_MASTER_PROTOCOL  *I2cMaster,
  IN  UINT32                   Offset,
  IN  UINTN                    Length,
  OUT VOID                     *Data
  )
{
  BMC_EEPROM_I2C_PACKET  Packet;
  EFI_STATUS             Status;
  UINT8                  Address[2];
  UINT8                  *Buffer;
  UINTN                  Chunk;

  if ((I2cMaster == NULL) || (Data == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = BmcEepromCheckBounds (Offset, Length);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Buffer = Data;

  while (Length > 0) {
    Chunk = MIN (Length, (UINTN)BMC_EEPROM_READ_CHUNK);

    Address[0] = (UINT8)(Offset >> 8);
    Address[1] = (UINT8)Offset;

    Packet.OperationCount             = 2;
    Packet.Operation[0].Flags         = 0;
    Packet.Operation[0].LengthInBytes = sizeof (Address);
    Packet.Operation[0].Buffer        = Address;
    Packet.Operation[1].Flags         = I2C_FLAG_READ;
    Packet.Operation[1].LengthInBytes = (UINT32)Chunk;
    Packet.Operation[1].Buffer        = Buffer;

    Status = I2cMaster->StartRequest (
                          I2cMaster,
                          PcdGet8 (PcdBmcEepromSlaveAddress),
                          (EFI_I2C_REQUEST_PACKET *)&Packet,
                          NULL,
                          NULL
                          );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "BmcEeprom: read of 0x%x bytes at 0x%x failed - %r\n",
        (UINT32)Chunk,
        Offset,
        Status
        ));
      return Status;
    }

    Offset += (UINT32)Chunk;
    Buffer += Chunk;
    Length -= Chunk;
  }

  return EFI_SUCCESS;
}

/**
  Write Length bytes from Data at EEPROM Offset, page-chunked with an
  ACK-poll after each page.

  @param[in] I2cMaster   The I2C master to use.
  @param[in] Offset      EEPROM byte offset to write to.
  @param[in] Length      Number of bytes to write.
  @param[in] Data        Source buffer.

  @retval EFI_SUCCESS             All bytes written.
  @retval EFI_INVALID_PARAMETER   NULL pointer or out-of-bounds access.
  @retval EFI_OUT_OF_RESOURCES    Page buffer allocation failed.
  @retval EFI_TIMEOUT             The EEPROM never ACKed after a page write.
  @retval other                   Propagated I2C transfer failure.
**/
EFI_STATUS
EFIAPI
BmcEepromWrite (
  IN EFI_I2C_MASTER_PROTOCOL  *I2cMaster,
  IN UINT32                   Offset,
  IN UINTN                    Length,
  IN CONST VOID               *Data
  )
{
  BMC_EEPROM_I2C_PACKET  Packet;
  EFI_STATUS             Status;
  CONST UINT8            *Buffer;
  UINT8                  *PageBuffer;
  UINT32                 PageSize;
  UINTN                  Chunk;

  if ((I2cMaster == NULL) || (Data == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = BmcEepromCheckBounds (Offset, Length);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Length == 0) {
    return EFI_SUCCESS;
  }

  PageSize = PcdGet32 (PcdBmcEepromPageSize);
  ASSERT (PageSize != 0);

  PageBuffer = AllocatePool ((UINTN)PageSize + 2);
  if (PageBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Buffer = Data;
  Status = EFI_SUCCESS;

  while (Length > 0) {
    //
    // Never cross a write-page boundary within one transaction: the
    // EEPROM's address counter wraps inside the page, corrupting data.
    //
    Chunk = MIN (Length, (UINTN)(PageSize - (Offset % PageSize)));

    PageBuffer[0] = (UINT8)(Offset >> 8);
    PageBuffer[1] = (UINT8)Offset;
    CopyMem (&PageBuffer[2], Buffer, Chunk);

    Packet.OperationCount             = 1;
    Packet.Operation[0].Flags         = 0;
    Packet.Operation[0].LengthInBytes = (UINT32)(Chunk + 2);
    Packet.Operation[0].Buffer        = PageBuffer;

    Status = I2cMaster->StartRequest (
                          I2cMaster,
                          PcdGet8 (PcdBmcEepromSlaveAddress),
                          (EFI_I2C_REQUEST_PACKET *)&Packet,
                          NULL,
                          NULL
                          );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "BmcEeprom: write of 0x%x bytes at 0x%x failed - %r\n",
        (UINT32)Chunk,
        Offset,
        Status
        ));
      break;
    }

    Status = BmcEepromAckPoll (I2cMaster, Offset);
    if (EFI_ERROR (Status)) {
      break;
    }

    Offset += (UINT32)Chunk;
    Buffer += Chunk;
    Length -= Chunk;
  }

  FreePool (PageBuffer);
  return Status;
}

/**
  Write only if the stored bytes differ (read + compare first). Spares
  EEPROM wear and needless BMC-slave pokes on unchanged content.

  @param[in]  I2cMaster   The I2C master to use.
  @param[in]  Offset      EEPROM byte offset to write to.
  @param[in]  Length      Number of bytes to write.
  @param[in]  Data        Source buffer.
  @param[out] Wrote       Optional: set to TRUE if a write was performed.

  @retval EFI_SUCCESS             Content matches, or was written.
  @retval EFI_INVALID_PARAMETER   NULL pointer or out-of-bounds access.
  @retval EFI_OUT_OF_RESOURCES    Compare buffer allocation failed.
  @retval other                   Propagated read or write failure.
**/
EFI_STATUS
EFIAPI
BmcEepromWriteIfChanged (
  IN  EFI_I2C_MASTER_PROTOCOL  *I2cMaster,
  IN  UINT32                   Offset,
  IN  UINTN                    Length,
  IN  CONST VOID               *Data,
  OUT BOOLEAN                  *Wrote  OPTIONAL
  )
{
  EFI_STATUS  Status;
  UINT8       *Existing;

  if (Wrote != NULL) {
    *Wrote = FALSE;
  }

  if ((I2cMaster == NULL) || (Data == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = BmcEepromCheckBounds (Offset, Length);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Length == 0) {
    return EFI_SUCCESS;
  }

  Existing = AllocatePool (Length);
  if (Existing == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = BmcEepromRead (I2cMaster, Offset, Length, Existing);
  if (!EFI_ERROR (Status) && (CompareMem (Existing, Data, Length) == 0)) {
    FreePool (Existing);
    return EFI_SUCCESS;
  }

  FreePool (Existing);

  //
  // Unreadable or different: (re)write. A failed read is not fatal here -
  // the write below decides the final status.
  //
  Status = BmcEepromWrite (I2cMaster, Offset, Length, Data);
  if (!EFI_ERROR (Status) && (Wrote != NULL)) {
    *Wrote = TRUE;
  }

  return Status;
}
