/** @file

  RP1 DesignWare I2C master driver.

  Binds to the NON_DISCOVERABLE_DEVICE that Rp1BusDxe registers for the
  RP1's I2C1 block (type gRp1DwI2cNonDiscoverableDeviceGuid, one MMIO
  resource) and produces EFI_I2C_MASTER_PROTOCOL on the same handle,
  driving the DW_apb_i2c controller in polled master mode.

  On first start it also muxes GPIO2/GPIO3 to alt3 (I2C1 SDA/SCL) with
  pull-ups through Rp1GpioLib - idempotent if the VPU firmware already
  did so.

  DesignWare register programming adapted from Ampere's DwI2cLib;
  driver-binding shape after Socionext's SynQuacerI2cDxe.

  Copyright (c) 2020 - 2021, Ampere Computing LLC. All rights reserved.<BR>
  Copyright (c) 2017, Linaro, Ltd. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Rp1DwI2cDxe.h"

STATIC BOOLEAN  mPinmuxDone = FALSE;

//
// No hardware transfer-length limit in polled mode.
//
STATIC CONST EFI_I2C_CONTROLLER_CAPABILITIES  mI2cControllerCapabilities = {
  sizeof (EFI_I2C_CONTROLLER_CAPABILITIES), // StructureSizeInBytes
  MAX_UINT32,                               // MaximumReceiveBytes
  MAX_UINT32,                               // MaximumTransmitBytes
  MAX_UINT32                                // MaximumTotalBytes
};

/**
  Enable or disable the controller, waiting for IC_ENABLE_STATUS to
  follow.

  @param[in] I2c      Controller context.
  @param[in] Enable   TRUE to enable, FALSE to disable.

  @retval EFI_SUCCESS   The controller reached the requested state.
  @retval EFI_TIMEOUT   IC_ENABLE_STATUS never followed IC_ENABLE.
**/
STATIC
EFI_STATUS
Rp1DwI2cEnable (
  IN RP1_DW_I2C_MASTER  *I2c,
  IN BOOLEAN            Enable
  )
{
  UINT32  Target;
  UINTN   Elapsed;

  Target = Enable ? DW_IC_ENABLE_ENABLE : 0;
  MmioWrite32 ((UINTN)(I2c->MmioBase + DW_IC_ENABLE), Target);

  for (Elapsed = 0; Elapsed < DW_I2C_ENABLE_TIMEOUT_US;
       Elapsed += DW_I2C_ENABLE_INTERVAL_US)
  {
    if ((MmioRead32 ((UINTN)(I2c->MmioBase + DW_IC_ENABLE_STATUS)) &
         DW_IC_ENABLE_ENABLE) == Target)
    {
      return EFI_SUCCESS;
    }

    gBS->Stall (DW_I2C_ENABLE_INTERVAL_US);
  }

  DEBUG ((
    DEBUG_ERROR,
    "Rp1DwI2c: %a timeout\n",
    Enable ? "enable" : "disable"
    ));
  return EFI_TIMEOUT;
}

/**
  Check for and acknowledge a transmit abort.

  @param[in] I2c   Controller context.

  @retval EFI_SUCCESS        No abort pending.
  @retval EFI_NO_RESPONSE    The slave did not ACK its address.
  @retval EFI_DEVICE_ERROR   Any other abort source.
**/
STATIC
EFI_STATUS
Rp1DwI2cCheckAbort (
  IN RP1_DW_I2C_MASTER  *I2c
  )
{
  UINT32  AbortSource;

  if ((MmioRead32 ((UINTN)(I2c->MmioBase + DW_IC_RAW_INTR_STAT)) &
       DW_IC_INTR_TX_ABRT) == 0)
  {
    return EFI_SUCCESS;
  }

  AbortSource = MmioRead32 ((UINTN)(I2c->MmioBase + DW_IC_TX_ABRT_SOURCE));
  MmioRead32 ((UINTN)(I2c->MmioBase + DW_IC_CLR_TX_ABRT));

  DEBUG ((
    DEBUG_VERBOSE,
    "Rp1DwI2c: TX abort, IC_TX_ABRT_SOURCE=0x%x\n",
    AbortSource
    ));

  if ((AbortSource & DW_IC_ABRT_7B_ADDR_NOACK) != 0) {
    return EFI_NO_RESPONSE;
  }

  return EFI_DEVICE_ERROR;
}

/**
  Wait until an IC_STATUS flag has the wanted state, checking for aborts.

  @param[in] I2c        Controller context.
  @param[in] StatusBit  IC_STATUS bit to test.
  @param[in] WantSet    TRUE to wait for the bit to set, FALSE to clear.

  @retval EFI_SUCCESS   The flag reached the wanted state.
  @retval EFI_TIMEOUT   ~50 ms elapsed without it.
  @retval other         A transmit abort occurred (Rp1DwI2cCheckAbort).
**/
STATIC
EFI_STATUS
Rp1DwI2cWaitStatus (
  IN RP1_DW_I2C_MASTER  *I2c,
  IN UINT32             StatusBit,
  IN BOOLEAN            WantSet
  )
{
  EFI_STATUS  Status;
  UINT32      Value;
  UINTN       Elapsed;

  for (Elapsed = 0; Elapsed < DW_I2C_BYTE_TIMEOUT_US;
       Elapsed += DW_I2C_POLL_INTERVAL_US)
  {
    Status = Rp1DwI2cCheckAbort (I2c);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Value = MmioRead32 ((UINTN)(I2c->MmioBase + DW_IC_STATUS)) & StatusBit;
    if ((WantSet && (Value != 0)) || (!WantSet && (Value == 0))) {
      return EFI_SUCCESS;
    }

    gBS->Stall (DW_I2C_POLL_INTERVAL_US);
  }

  return EFI_TIMEOUT;
}

/**
  Program speed-dependent configuration: IC_CON, SCL counts, spike
  suppression, SDA hold, and mask all interrupts (polled operation).
  The controller must be disabled on entry.

  @param[in] I2c        Controller context.
  @param[in] FastMode   TRUE for 400 kHz fast mode, FALSE for 100 kHz.
**/
STATIC
VOID
Rp1DwI2cConfigure (
  IN RP1_DW_I2C_MASTER  *I2c,
  IN BOOLEAN            FastMode
  )
{
  UINT32  IcCon;

  IcCon = DW_IC_CON_MASTER | DW_IC_CON_SLAVE_DISABLE | DW_IC_CON_RESTART_EN;
  IcCon |= FastMode ? DW_IC_CON_SPEED_FAST : DW_IC_CON_SPEED_STD;

  MmioWrite32 ((UINTN)(I2c->MmioBase + DW_IC_CON), IcCon);
  MmioWrite32 (
    (UINTN)(I2c->MmioBase + DW_IC_SS_SCL_HCNT),
    DW_I2C_SS_SCL_HCNT_VALUE
    );
  MmioWrite32 (
    (UINTN)(I2c->MmioBase + DW_IC_SS_SCL_LCNT),
    DW_I2C_SS_SCL_LCNT_VALUE
    );
  MmioWrite32 (
    (UINTN)(I2c->MmioBase + DW_IC_FS_SCL_HCNT),
    DW_I2C_FS_SCL_HCNT_VALUE
    );
  MmioWrite32 (
    (UINTN)(I2c->MmioBase + DW_IC_FS_SCL_LCNT),
    DW_I2C_FS_SCL_LCNT_VALUE
    );
  MmioWrite32 (
    (UINTN)(I2c->MmioBase + DW_IC_FS_SPKLEN),
    DW_I2C_FS_SPKLEN_VALUE
    );
  MmioWrite32 (
    (UINTN)(I2c->MmioBase + DW_IC_SDA_HOLD),
    DW_I2C_SDA_HOLD_VALUE
    );
  MmioWrite32 ((UINTN)(I2c->MmioBase + DW_IC_INTR_MASK), 0);
}

/**
  Run one synchronous transaction: an optional write phase, then an
  optional read phase separated by a repeated start, then a stop.

  @param[in] I2c            Controller context.
  @param[in] SlaveAddress   7-bit slave address.
  @param[in] WriteOp        Write operation, or NULL.
  @param[in] ReadOp         Read operation, or NULL.

  @retval EFI_SUCCESS        Transaction complete.
  @retval EFI_NO_RESPONSE    The slave did not ACK its address.
  @retval EFI_DEVICE_ERROR   Transmit abort (NACKed data, lost
                             arbitration, ...).
  @retval EFI_TIMEOUT        The controller stopped making progress.
**/
STATIC
EFI_STATUS
Rp1DwI2cTransfer (
  IN RP1_DW_I2C_MASTER  *I2c,
  IN UINTN              SlaveAddress,
  IN EFI_I2C_OPERATION  *WriteOp  OPTIONAL,
  IN EFI_I2C_OPERATION  *ReadOp   OPTIONAL
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  FinishStatus;
  UINT32      Cmd;
  UINT32      Batch;
  UINT32      TxSpace;
  UINT32      RxSpace;
  UINT32      Index;
  UINT32      Issued;
  UINT32      Received;
  UINTN       Stalled;

  //
  // Wait for any previous activity to drain; not fatal if it does not,
  // the new transfer will fail cleanly on its own.
  //
  Status = Rp1DwI2cWaitStatus (I2c, DW_IC_STATUS_MST_ACTIVITY, FALSE);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "Rp1DwI2c: bus busy before transfer - %r\n", Status));
  }

  //
  // IC_TAR may only change while the controller is disabled.
  //
  Rp1DwI2cEnable (I2c, FALSE);
  MmioWrite32 ((UINTN)(I2c->MmioBase + DW_IC_TAR), (UINT32)SlaveAddress);

  Status = Rp1DwI2cEnable (I2c, TRUE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioRead32 ((UINTN)(I2c->MmioBase + DW_IC_CLR_INTR));

  //
  // Write phase: push bytes as TX FIFO space allows; STOP on the last
  // byte only when no read phase follows.
  //
  if (WriteOp != NULL) {
    for (Index = 0; Index < WriteOp->LengthInBytes; Index++) {
      Status = Rp1DwI2cWaitStatus (I2c, DW_IC_STATUS_TFNF, TRUE);
      if (EFI_ERROR (Status)) {
        goto Done;
      }

      Cmd = WriteOp->Buffer[Index] & DW_IC_DATA_CMD_DAT_MASK;
      if ((ReadOp == NULL) && (Index == WriteOp->LengthInBytes - 1)) {
        Cmd |= DW_IC_DATA_CMD_STOP;
      }

      MmioWrite32 ((UINTN)(I2c->MmioBase + DW_IC_DATA_CMD), Cmd);
    }
  }

  //
  // Read phase: pump read commands and drain the RX FIFO in batches
  // sized to the FIFO room, RESTART on the first command when a write
  // phase precedes, STOP on the last.
  //
  if (ReadOp != NULL) {
    Issued   = 0;
    Received = 0;
    Stalled  = 0;

    while (Received < ReadOp->LengthInBytes) {
      TxSpace = I2c->TxFifoDepth -
                MmioRead32 ((UINTN)(I2c->MmioBase + DW_IC_TXFLR));
      RxSpace = I2c->RxFifoDepth -
                MmioRead32 ((UINTN)(I2c->MmioBase + DW_IC_RXFLR));

      Batch = ReadOp->LengthInBytes - Issued;
      Batch = MIN (Batch, TxSpace);
      Batch = MIN (Batch, RxSpace);

      if (Batch == 0) {
        Status = Rp1DwI2cCheckAbort (I2c);
        if (EFI_ERROR (Status)) {
          goto Done;
        }

        Stalled += DW_I2C_POLL_INTERVAL_US;
        if (Stalled >= DW_I2C_BYTE_TIMEOUT_US) {
          Status = EFI_TIMEOUT;
          goto Done;
        }

        gBS->Stall (DW_I2C_POLL_INTERVAL_US);
        continue;
      }

      Stalled = 0;

      for (Index = 0; Index < Batch; Index++) {
        Cmd = DW_IC_DATA_CMD_CMD;
        if ((Issued == 0) && (WriteOp != NULL)) {
          Cmd |= DW_IC_DATA_CMD_RESTART;
        }

        if (Issued == ReadOp->LengthInBytes - 1) {
          Cmd |= DW_IC_DATA_CMD_STOP;
        }

        MmioWrite32 ((UINTN)(I2c->MmioBase + DW_IC_DATA_CMD), Cmd);
        Issued++;
      }

      for (Index = 0; Index < Batch; Index++) {
        Status = Rp1DwI2cWaitStatus (I2c, DW_IC_STATUS_RFNE, TRUE);
        if (EFI_ERROR (Status)) {
          goto Done;
        }

        ReadOp->Buffer[Received] =
          (UINT8)(MmioRead32 ((UINTN)(I2c->MmioBase + DW_IC_DATA_CMD)) &
                  DW_IC_DATA_CMD_DAT_MASK);
        Received++;
      }
    }
  }

  //
  // Wait for the TX FIFO to drain and catch a late abort (a NACK on the
  // final byte is only reported after the byte leaves the FIFO).
  //
  Status = Rp1DwI2cWaitStatus (I2c, DW_IC_STATUS_TFE, TRUE);
  if (!EFI_ERROR (Status)) {
    Status = Rp1DwI2cWaitStatus (I2c, DW_IC_STATUS_MST_ACTIVITY, FALSE);
  }

Done:
  //
  // A final abort check: the wait helpers may have exited on TFE/idle in
  // the same poll window an abort was raised.
  //
  FinishStatus = Rp1DwI2cCheckAbort (I2c);
  if (EFI_ERROR (FinishStatus)) {
    Status = FinishStatus;
  }

  if ((MmioRead32 ((UINTN)(I2c->MmioBase + DW_IC_RAW_INTR_STAT)) &
       DW_IC_INTR_STOP_DET) != 0)
  {
    MmioRead32 ((UINTN)(I2c->MmioBase + DW_IC_CLR_STOP_DET));
  }

  Rp1DwI2cEnable (I2c, FALSE);

  return Status;
}

/**
  Set the frequency for the I2C clock line.

  Supports 100 kHz standard mode and 400 kHz fast mode; a request at or
  above 400 kHz selects fast mode, at or above 100 kHz standard mode,
  below that EFI_UNSUPPORTED (per protocol: never exceed the request).

  @param[in]     This           EFI_I2C_MASTER_PROTOCOL instance.
  @param[in,out] BusClockHertz  Requested frequency in Hertz; on return
                                the actual frequency in use.

  @retval EFI_SUCCESS             Frequency set.
  @retval EFI_INVALID_PARAMETER   BusClockHertz is NULL.
  @retval EFI_UNSUPPORTED         No supported frequency at or below the
                                  request.
**/
STATIC
EFI_STATUS
EFIAPI
Rp1DwI2cSetBusFrequency (
  IN CONST EFI_I2C_MASTER_PROTOCOL  *This,
  IN OUT UINTN                      *BusClockHertz
  )
{
  RP1_DW_I2C_MASTER  *I2c;
  BOOLEAN            FastMode;

  if ((This == NULL) || (BusClockHertz == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  I2c = RP1_DW_I2C_FROM_THIS (This);

  if (*BusClockHertz >= 400000) {
    FastMode       = TRUE;
    *BusClockHertz = 400000;
  } else if (*BusClockHertz >= 100000) {
    FastMode       = FALSE;
    *BusClockHertz = 100000;
  } else {
    return EFI_UNSUPPORTED;
  }

  Rp1DwI2cEnable (I2c, FALSE);
  Rp1DwI2cConfigure (I2c, FastMode);
  I2c->BusClockHertz = *BusClockHertz;

  return EFI_SUCCESS;
}

/**
  Reset the I2C controller: disable it, discard pending state and
  interrupts. The caller must call SetBusFrequency() afterwards, per the
  protocol contract.

  @param[in] This   EFI_I2C_MASTER_PROTOCOL instance.

  @retval EFI_SUCCESS   The reset completed successfully.
**/
STATIC
EFI_STATUS
EFIAPI
Rp1DwI2cReset (
  IN CONST EFI_I2C_MASTER_PROTOCOL  *This
  )
{
  RP1_DW_I2C_MASTER  *I2c;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  I2c = RP1_DW_I2C_FROM_THIS (This);

  Rp1DwI2cEnable (I2c, FALSE);
  MmioRead32 ((UINTN)(I2c->MmioBase + DW_IC_CLR_INTR));
  MmioWrite32 ((UINTN)(I2c->MmioBase + DW_IC_INTR_MASK), 0);

  return EFI_SUCCESS;
}

/**
  Start a synchronous I2C transaction: a single write, a single read, or
  a write followed by a read separated by a repeated start.

  @param[in]  This           EFI_I2C_MASTER_PROTOCOL instance.
  @param[in]  SlaveAddress   7-bit address of the device on the bus.
  @param[in]  RequestPacket  Describes the transaction.
  @param[in]  Event          Must be NULL: asynchronous requests are not
                             supported.
  @param[out] I2cStatus      Optional transaction completion status.

  @retval EFI_SUCCESS             Transaction complete.
  @retval EFI_INVALID_PARAMETER   RequestPacket is NULL or malformed.
  @retval EFI_NOT_FOUND           Reserved bits set in SlaveAddress.
  @retval EFI_UNSUPPORTED         Async request, 10-bit address, SMBus
                                  flags, zero-length (ping) operation, or
                                  an unsupported operation combination.
  @retval EFI_NO_RESPONSE         The slave did not ACK its address.
  @retval EFI_DEVICE_ERROR        The transfer failed on the bus.
**/
STATIC
EFI_STATUS
EFIAPI
Rp1DwI2cStartRequest (
  IN CONST EFI_I2C_MASTER_PROTOCOL  *This,
  IN UINTN                          SlaveAddress,
  IN EFI_I2C_REQUEST_PACKET         *RequestPacket,
  IN EFI_EVENT                      Event      OPTIONAL,
  OUT EFI_STATUS                    *I2cStatus OPTIONAL
  )
{
  RP1_DW_I2C_MASTER  *I2c;
  EFI_I2C_OPERATION  *WriteOp;
  EFI_I2C_OPERATION  *ReadOp;
  EFI_STATUS         Status;
  UINTN              Index;

  if ((This == NULL) || (RequestPacket == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Event != NULL) {
    return EFI_UNSUPPORTED;
  }

  if ((SlaveAddress & I2C_ADDRESSING_10_BIT) != 0) {
    //
    // The controller is configured for 7-bit addressing only.
    //
    return EFI_UNSUPPORTED;
  }

  if (SlaveAddress > 0x7F) {
    return EFI_NOT_FOUND;
  }

  if ((RequestPacket->OperationCount == 0) ||
      (RequestPacket->OperationCount > 2))
  {
    return EFI_UNSUPPORTED;
  }

  for (Index = 0; Index < RequestPacket->OperationCount; Index++) {
    if ((RequestPacket->Operation[Index].Flags & ~(UINT32)I2C_FLAG_READ) != 0) {
      return EFI_UNSUPPORTED;
    }

    if (RequestPacket->Operation[Index].LengthInBytes == 0) {
      return EFI_UNSUPPORTED;
    }

    if (RequestPacket->Operation[Index].Buffer == NULL) {
      return EFI_INVALID_PARAMETER;
    }
  }

  WriteOp = NULL;
  ReadOp  = NULL;

  if (RequestPacket->OperationCount == 1) {
    if ((RequestPacket->Operation[0].Flags & I2C_FLAG_READ) != 0) {
      ReadOp = &RequestPacket->Operation[0];
    } else {
      WriteOp = &RequestPacket->Operation[0];
    }
  } else {
    //
    // Two operations: only write-then-read (repeated start) is
    // supported.
    //
    if (((RequestPacket->Operation[0].Flags & I2C_FLAG_READ) != 0) ||
        ((RequestPacket->Operation[1].Flags & I2C_FLAG_READ) == 0))
    {
      return EFI_UNSUPPORTED;
    }

    WriteOp = &RequestPacket->Operation[0];
    ReadOp  = &RequestPacket->Operation[1];
  }

  I2c = RP1_DW_I2C_FROM_THIS (This);

  Status = Rp1DwI2cTransfer (I2c, SlaveAddress, WriteOp, ReadOp);
  if (Status == EFI_TIMEOUT) {
    //
    // Not part of the StartRequest contract; the controller wedged.
    //
    Status = EFI_DEVICE_ERROR;
  }

  if (I2cStatus != NULL) {
    *I2cStatus = Status;
  }

  return Status;
}

/**
  One-time mux of GPIO2/GPIO3 to I2C1 (alt3) with pull-ups.

  Idempotent even if the VPU firmware already muxed them; if Rp1BusDxe's
  protocol is absent the mux is assumed done and this quietly proceeds.
**/
STATIC
VOID
Rp1DwI2cSetupPinmux (
  VOID
  )
{
  EFI_STATUS            Status;
  RP1_BUS_PROTOCOL      *Rp1Bus;
  EFI_PHYSICAL_ADDRESS  PeripheralBase;

  if (mPinmuxDone) {
    return;
  }

  Status = gBS->LocateProtocol (&gRp1BusProtocolGuid, NULL, (VOID **)&Rp1Bus);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "Rp1DwI2c: no RP1 bus protocol (%r); assuming GPIO2/3 already muxed\n",
      Status
      ));
    return;
  }

  PeripheralBase = Rp1Bus->GetPeripheralBase (Rp1Bus);

  Rp1GpioSetFunction (PeripheralBase, RP1_I2C1_GPIO_SDA, RP1_GPIO_FUNC_ALT3);
  Rp1GpioSetFunction (PeripheralBase, RP1_I2C1_GPIO_SCL, RP1_GPIO_FUNC_ALT3);
  Rp1GpioSetPull (PeripheralBase, RP1_I2C1_GPIO_SDA, Rp1GpioPullUp);
  Rp1GpioSetPull (PeripheralBase, RP1_I2C1_GPIO_SCL, Rp1GpioPullUp);

  mPinmuxDone = TRUE;

  DEBUG ((DEBUG_INFO, "Rp1DwI2c: GPIO2/3 muxed to I2C1 (alt3, pull-up)\n"));
}

/**
  Tests to see if this driver supports a given controller.

  @param[in] This                  EFI_DRIVER_BINDING_PROTOCOL instance.
  @param[in] ControllerHandle      The handle of the controller to test.
  @param[in] RemainingDevicePath   Ignored - not a bus driver.

  @retval EFI_SUCCESS       The controller is the RP1 DW I2C device.
  @retval EFI_UNSUPPORTED   The controller is something else.
  @retval other             OpenProtocol failure (already started, ...).
**/
STATIC
EFI_STATUS
EFIAPI
Rp1DwI2cDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS               Status;
  NON_DISCOVERABLE_DEVICE  *Dev;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEdkiiNonDiscoverableDeviceProtocolGuid,
                  (VOID **)&Dev,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (CompareGuid (Dev->Type, &gRp1DwI2cNonDiscoverableDeviceGuid)) {
    Status = EFI_SUCCESS;
  } else {
    Status = EFI_UNSUPPORTED;
  }

  gBS->CloseProtocol (
         ControllerHandle,
         &gEdkiiNonDiscoverableDeviceProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

  return Status;
}

/**
  Starts the RP1 DW I2C controller: pinmux, controller init, and
  EFI_I2C_MASTER_PROTOCOL installation on the same handle.

  @param[in] This                  EFI_DRIVER_BINDING_PROTOCOL instance.
  @param[in] ControllerHandle      The handle of the device to start.
  @param[in] RemainingDevicePath   Ignored - not a bus driver.

  @retval EFI_SUCCESS            The device was started.
  @retval EFI_DEVICE_ERROR       Unexpected MMIO resource layout.
  @retval EFI_OUT_OF_RESOURCES   Context allocation failed.
  @retval other                  OpenProtocol/install failure.
**/
STATIC
EFI_STATUS
EFIAPI
Rp1DwI2cDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS               Status;
  NON_DISCOVERABLE_DEVICE  *Dev;
  RP1_DW_I2C_MASTER        *I2c;
  UINT32                   Param;
  UINT32                   CompType;
  UINTN                    BusClockHertz;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEdkiiNonDiscoverableDeviceProtocolGuid,
                  (VOID **)&Dev,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Rp1BusDxe registers this device with exactly one MMIO resource: the
  // I2C1 register block.
  //
  if ((Dev->Resources == NULL) ||
      (Dev->Resources[0].Desc != ACPI_ADDRESS_SPACE_DESCRIPTOR) ||
      (Dev->Resources[0].ResType != ACPI_ADDRESS_SPACE_TYPE_MEM))
  {
    DEBUG ((DEBUG_ERROR, "Rp1DwI2c: unexpected MMIO resource layout\n"));
    Status = EFI_DEVICE_ERROR;
    goto CloseProtocol;
  }

  Rp1DwI2cSetupPinmux ();

  I2c = AllocateZeroPool (sizeof (RP1_DW_I2C_MASTER));
  if (I2c == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto CloseProtocol;
  }

  I2c->Signature                          = RP1_DW_I2C_SIGNATURE;
  I2c->ControllerHandle                   = ControllerHandle;
  I2c->Dev                                = Dev;
  I2c->MmioBase                           = Dev->Resources[0].AddrRangeMin;
  I2c->I2cMaster.SetBusFrequency          = Rp1DwI2cSetBusFrequency;
  I2c->I2cMaster.Reset                    = Rp1DwI2cReset;
  I2c->I2cMaster.StartRequest             = Rp1DwI2cStartRequest;
  I2c->I2cMaster.I2cControllerCapabilities = &mI2cControllerCapabilities;

  CompType = MmioRead32 ((UINTN)(I2c->MmioBase + DW_IC_COMP_TYPE));
  if (CompType != DW_IC_COMP_TYPE_VALUE) {
    DEBUG ((
      DEBUG_WARN,
      "Rp1DwI2c: unexpected IC_COMP_TYPE 0x%x at 0x%lx\n",
      CompType,
      I2c->MmioBase
      ));
  }

  Param            = MmioRead32 ((UINTN)(I2c->MmioBase + DW_IC_COMP_PARAM_1));
  I2c->TxFifoDepth = DW_IC_COMP_PARAM_1_TX_BUFFER_DEPTH (Param);
  I2c->RxFifoDepth = DW_IC_COMP_PARAM_1_RX_BUFFER_DEPTH (Param);

  //
  // Initialize at 100 kHz standard speed; EEPROM consumers re-request
  // this through SetBusFrequency anyway.
  //
  BusClockHertz = 100000;
  Status        = Rp1DwI2cSetBusFrequency (&I2c->I2cMaster, &BusClockHertz);
  if (EFI_ERROR (Status)) {
    goto FreeContext;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ControllerHandle,
                  &gEfiI2cMasterProtocolGuid,
                  &I2c->I2cMaster,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Rp1DwI2c: failed to install I2C master protocol - %r\n",
      Status
      ));
    goto FreeContext;
  }

  DEBUG ((
    DEBUG_INFO,
    "Rp1DwI2c: I2C master at 0x%lx (TX/RX FIFO %u/%u)\n",
    I2c->MmioBase,
    I2c->TxFifoDepth,
    I2c->RxFifoDepth
    ));

  return EFI_SUCCESS;

FreeContext:
  FreePool (I2c);

CloseProtocol:
  gBS->CloseProtocol (
         ControllerHandle,
         &gEdkiiNonDiscoverableDeviceProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

  return Status;
}

/**
  Stops the RP1 DW I2C controller.

  @param[in] This                A pointer to the
                                 EFI_DRIVER_BINDING_PROTOCOL instance.
  @param[in] ControllerHandle    A handle to the device being stopped.
  @param[in] NumberOfChildren    Zero - not a bus driver.
  @param[in] ChildHandleBuffer   Ignored.

  @retval EFI_SUCCESS        The device was stopped.
  @retval EFI_DEVICE_ERROR   The I2C master protocol was not found or
                             could not be uninstalled.
**/
STATIC
EFI_STATUS
EFIAPI
Rp1DwI2cDriverBindingStop (
  IN  EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN  EFI_HANDLE                   ControllerHandle,
  IN  UINTN                        NumberOfChildren,
  IN  EFI_HANDLE                   *ChildHandleBuffer OPTIONAL
  )
{
  EFI_STATUS               Status;
  EFI_I2C_MASTER_PROTOCOL  *I2cMaster;
  RP1_DW_I2C_MASTER        *I2c;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiI2cMasterProtocolGuid,
                  (VOID **)&I2cMaster,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  I2c = RP1_DW_I2C_FROM_THIS (I2cMaster);

  Rp1DwI2cEnable (I2c, FALSE);

  Status = gBS->UninstallMultipleProtocolInterfaces (
                  ControllerHandle,
                  &gEfiI2cMasterProtocolGuid,
                  I2cMaster,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  gBS->CloseProtocol (
         ControllerHandle,
         &gEdkiiNonDiscoverableDeviceProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

  FreePool (I2c);

  return EFI_SUCCESS;
}

STATIC EFI_DRIVER_BINDING_PROTOCOL  mRp1DwI2cDriverBinding = {
  Rp1DwI2cDriverBindingSupported,
  Rp1DwI2cDriverBindingStart,
  Rp1DwI2cDriverBindingStop,
  0x10,
  NULL,
  NULL
};

/**
  The entry point of the RP1 DW I2C UEFI driver.

  @param[in] ImageHandle   The image handle of the UEFI driver.
  @param[in] SystemTable   A pointer to the EFI system table.

  @retval EFI_SUCCESS   The driver binding was installed.
  @retval other         Installation failure.
**/
EFI_STATUS
EFIAPI
Rp1DwI2cDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiLibInstallDriverBindingComponentName2 (
           ImageHandle,
           SystemTable,
           &mRp1DwI2cDriverBinding,
           ImageHandle,
           &gRp1DwI2cComponentName,
           &gRp1DwI2cComponentName2
           );
}
