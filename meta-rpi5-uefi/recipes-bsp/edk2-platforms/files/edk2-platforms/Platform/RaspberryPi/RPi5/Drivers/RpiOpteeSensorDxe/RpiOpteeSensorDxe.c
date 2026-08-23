/** @file
 *
 *  The EDK2 -> OP-TEE late-initialization handshake for the BMC sensor
 *  push service.
 *
 *  OP-TEE (BL32) boots before this firmware (BL33) and its sensor pTA
 *  needs the RP1's DesignWare I2C1 controller -- but the RP1 is a PCIe
 *  endpoint, so its registers have no address until PciHostBridgeDxe has
 *  enumerated segment 2 and assigned the peripheral BAR. This driver
 *  waits for Rp1BusDxe (which binds the RP1 by VID/DID and reads BAR1),
 *  muxes GPIO2/3 to I2C1, and hands the BAR address to the pTA over the
 *  OP-TEE static shared memory (PTA_BMC_SENSOR_CMD_INIT). From then on
 *  the secure world samples and pushes on its own timer.
 *
 *  If the OS ever moves the BAR, the normal-world daemon re-runs the
 *  same INIT command with the address Linux sees (see
 *  docs/optee-sensor/ in the build repo).
 *
 *  Copyright (c) 2026, pi-bmc contributors
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/OpteeLib.h>
#include <Library/Rp1GpioLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/Rp1Bus.h>
#include <Rp1.h>

//
// Mirror of the pTA ABI in optee-os files/plat-rpi5/pta_bmc_sensor.h --
// keep the two in step.
//
#define PTA_BMC_SENSOR_UUID \
  { 0x575d6607, 0x5a2b, 0x4384, \
    { 0x82, 0x7e, 0xc1, 0x6a, 0x25, 0xac, 0x4f, 0xa5 } }

#define PTA_BMC_SENSOR_CMD_INIT          0
#define PTA_BMC_SENSOR_CMD_MBOX_HANDOFF  3

//
// The pi-bmc EEPROM wire contract: BMC-emulated 24c256 at 0x50 on RP1
// I2C1 (GPIO2/3, 100 kHz); the sensor record goes to the spare region
// at 0x7800 of the EEPROM map.
//
#define BMC_EEPROM_SLAVE_ADDRESS  0x50
#define BMC_SENSOR_EEPROM_OFFSET  0x7800

//
// I2C1 SDA/SCL are GPIO2/GPIO3 as alt3 (same mux the retired
// Rp1DwI2cDxe proved on hardware).
//
#define RP1_I2C1_GPIO_SDA  2
#define RP1_I2C1_GPIO_SCL  3

STATIC CONST EFI_GUID  mPtaBmcSensorGuid = PTA_BMC_SENSOR_UUID;

STATIC EFI_EVENT  mRp1BusEvent;
STATIC VOID       *mRp1BusRegistration;
STATIC EFI_EVENT  mExitBootServicesEvent;

STATIC
EFI_STATUS
OpteeSensorInvokeInit (
  IN EFI_PHYSICAL_ADDRESS  PeripheralBase
  )
{
  EFI_STATUS                 Status;
  OPTEE_OPEN_SESSION_ARG     OpenArg;
  OPTEE_INVOKE_FUNCTION_ARG  InvokeArg;

  Status = OpteeInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "RpiOpteeSensor: OpteeInit failed - %r (no static SHM?)\n",
      Status
      ));
    return Status;
  }

  ZeroMem (&OpenArg, sizeof (OpenArg));
  CopyGuid (&OpenArg.Uuid, &mPtaBmcSensorGuid);
  Status = OpteeOpenSession (&OpenArg);
  if (EFI_ERROR (Status) || (OpenArg.Return != OPTEE_SUCCESS)) {
    DEBUG ((
      DEBUG_ERROR,
      "RpiOpteeSensor: pTA session failed - %r (TEE ret %x origin %x)\n",
      Status,
      OpenArg.Return,
      OpenArg.ReturnOrigin
      ));
    return EFI_ERROR (Status) ? Status : EFI_PROTOCOL_ERROR;
  }

  ZeroMem (&InvokeArg, sizeof (InvokeArg));
  InvokeArg.Function = PTA_BMC_SENSOR_CMD_INIT;
  InvokeArg.Session  = OpenArg.Session;

  InvokeArg.Params[0].Attribute     = OPTEE_MESSAGE_ATTRIBUTE_TYPE_VALUE_INPUT;
  InvokeArg.Params[0].Union.Value.A = (UINT32)PeripheralBase;
  InvokeArg.Params[0].Union.Value.B = (UINT32)RShiftU64 (PeripheralBase, 32);
  InvokeArg.Params[1].Attribute     = OPTEE_MESSAGE_ATTRIBUTE_TYPE_VALUE_INPUT;
  InvokeArg.Params[1].Union.Value.A = RP1_I2C1_BASE;
  InvokeArg.Params[1].Union.Value.B = BMC_EEPROM_SLAVE_ADDRESS;
  InvokeArg.Params[2].Attribute     = OPTEE_MESSAGE_ATTRIBUTE_TYPE_VALUE_INPUT;
  InvokeArg.Params[2].Union.Value.A = BMC_SENSOR_EEPROM_OFFSET;
  InvokeArg.Params[2].Union.Value.B = 0;  // keep the pTA's sample period

  Status = OpteeInvokeFunction (&InvokeArg);
  if (EFI_ERROR (Status) || (InvokeArg.Return != OPTEE_SUCCESS)) {
    DEBUG ((
      DEBUG_ERROR,
      "RpiOpteeSensor: CMD_INIT failed - %r (TEE ret %x origin %x)\n",
      Status,
      InvokeArg.Return,
      InvokeArg.ReturnOrigin
      ));
    if (!EFI_ERROR (Status)) {
      Status = EFI_PROTOCOL_ERROR;
    }
  } else {
    DEBUG ((
      DEBUG_INFO,
      "RpiOpteeSensor: sensor push armed (RP1 BAR %lx, I2C1 +%x, "
      "slave %x, EEPROM +%x)\n",
      PeripheralBase,
      RP1_I2C1_BASE,
      BMC_EEPROM_SLAVE_ADDRESS,
      BMC_SENSOR_EEPROM_OFFSET
      ));
  }

  OpteeCloseSession (OpenArg.Session);

  return Status;
}

STATIC
VOID
EFIAPI
OnRp1BusInstalled (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS            Status;
  EFI_HANDLE            Handle;
  UINTN                 BufferSize;
  RP1_BUS_PROTOCOL      *Rp1Bus;
  EFI_PHYSICAL_ADDRESS  PeripheralBase;

  BufferSize = sizeof (Handle);
  Status     = gBS->LocateHandle (
                      ByRegisterNotify,
                      NULL,
                      mRp1BusRegistration,
                      &BufferSize,
                      &Handle
                      );
  if (EFI_ERROR (Status)) {
    // Spurious signal before Rp1BusDxe is up; wait for the next one.
    return;
  }

  Status = gBS->HandleProtocol (
                  Handle,
                  &gRp1BusProtocolGuid,
                  (VOID **)&Rp1Bus
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RpiOpteeSensor: RP1 bus protocol - %r\n", Status));
    return;
  }

  PeripheralBase = Rp1Bus->GetPeripheralBase (Rp1Bus);

  //
  // One-time mux of GPIO2/GPIO3 to I2C1 (alt3) with pull-ups.
  // Idempotent even if the VPU firmware already muxed them.
  //
  Rp1GpioSetFunction (PeripheralBase, RP1_I2C1_GPIO_SDA, RP1_GPIO_FUNC_ALT3);
  Rp1GpioSetFunction (PeripheralBase, RP1_I2C1_GPIO_SCL, RP1_GPIO_FUNC_ALT3);
  Rp1GpioSetPull (PeripheralBase, RP1_I2C1_GPIO_SDA, Rp1GpioPullUp);
  Rp1GpioSetPull (PeripheralBase, RP1_I2C1_GPIO_SCL, Rp1GpioPullUp);

  OpteeSensorInvokeInit (PeripheralBase);

  //
  // One-shot either way: on failure the secure world simply keeps
  // caching samples without an I2C transport, and the normal-world
  // daemon can still complete the handshake later.
  //
  gBS->CloseEvent (Event);
  mRp1BusEvent = NULL;
}

/**
  ExitBootServices: hand the VPU mailbox to OP-TEE. Until this moment the
  normal world (RpiFirmwareDxe) drives the mailbox natively; from here on
  OP-TEE is its ONLY user -- the OS device tree disables the mailbox and
  firmware nodes and consumes the firmware services (clocks, later PMIC
  telemetry) over SCMI. Best-effort: on failure OP-TEE simply keeps
  refusing mailbox-backed SCMI requests, which the agents treat as
  "not available" rather than an error.
**/
STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS                 Status;
  OPTEE_OPEN_SESSION_ARG     OpenArg;
  OPTEE_INVOKE_FUNCTION_ARG  InvokeArg;

  ZeroMem (&OpenArg, sizeof (OpenArg));
  CopyGuid (&OpenArg.Uuid, &mPtaBmcSensorGuid);
  Status = OpteeOpenSession (&OpenArg);
  if (EFI_ERROR (Status) || (OpenArg.Return != OPTEE_SUCCESS)) {
    DEBUG ((DEBUG_ERROR, "RpiOpteeSensor: handoff session failed - %r\n", Status));
    return;
  }

  ZeroMem (&InvokeArg, sizeof (InvokeArg));
  InvokeArg.Function = PTA_BMC_SENSOR_CMD_MBOX_HANDOFF;
  InvokeArg.Session  = OpenArg.Session;

  Status = OpteeInvokeFunction (&InvokeArg);
  DEBUG ((
    DEBUG_INFO,
    "RpiOpteeSensor: VPU mailbox handed to OP-TEE - %r (TEE ret %x)\n",
    Status,
    InvokeArg.Return
    ));

  OpteeCloseSession (OpenArg.Session);
}

EFI_STATUS
EFIAPI
RpiOpteeSensorInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  if (!IsOpteePresent ()) {
    DEBUG ((
      DEBUG_WARN,
      "RpiOpteeSensor: no OP-TEE at S-EL1 (TF-A built without "
      "SPD=opteed?), sensor handshake disabled\n"
      ));
    return EFI_SUCCESS;
  }

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnRp1BusInstalled,
                  NULL,
                  &mRp1BusEvent
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->RegisterProtocolNotify (
                  &gRp1BusProtocolGuid,
                  mRp1BusEvent,
                  &mRp1BusRegistration
                  );
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mRp1BusEvent);
    return Status;
  }

  //
  // Cover the case where Rp1BusDxe already started before us.
  //
  gBS->SignalEvent (mRp1BusEvent);

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  OnExitBootServices,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &mExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "RpiOpteeSensor: no EBS event - %r; "
      "VPU mailbox will not be handed to OP-TEE\n",
      Status
      ));
  }

  return EFI_SUCCESS;
}
