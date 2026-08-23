/** @file
  SCMI agent client for the OP-TEE SCMI server on the Raspberry Pi 5.

  OP-TEE serves SCMI over the standard arm,scmi-smc wire: a 128-byte SMT
  message slot in the top page of the OP-TEE reserved SHM window, rung by
  a SiP fast SMC that TF-A forwards into OP-TEE's fast-SMC vector. This
  library is that wire for DXE drivers - the same one Linux's SCMI stack
  uses at runtime - so every consumer (fan control, power button) reaches
  the platform through one channel with one owner behind it.

  Contract values are kept in step across TF-A (rpi_scmi_svc.c), OP-TEE
  (plat-rpi5 scmi_server.c), the bcm2712-scmi DTB overlay and here.

  Copyright (c) 2026, pi-bmc contributors

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef RPI_SCMI_LIB_H_
#define RPI_SCMI_LIB_H_

#include <Uefi.h>

//
// Protocols and messages this platform serves (ids per the SCMI spec,
// verified against OP-TEE's scmi-msg headers).
//
#define RPI_SCMI_PROTOCOL_SYS_POWER   0x12
#define RPI_SCMI_SYS_POWER_STATE_SET  0x3
#define RPI_SCMI_SYS_POWER_STATE_GET  0x4

#define RPI_SCMI_PROTOCOL_PERF   0x13
#define RPI_SCMI_PERF_LEVEL_SET  0x7
#define RPI_SCMI_PERF_LEVEL_GET  0x8

#define RPI_SCMI_PROTOCOL_CLOCK  0x14

#define RPI_SCMI_PROTOCOL_SENSOR     0x15
#define RPI_SCMI_SENSOR_READING_GET  0x6

//
// This platform's SCMI ids.
//
#define RPI_SCMI_SENSOR_ID_SOC_TEMP  0
#define RPI_SCMI_PERF_DOMAIN_FAN     0

//
// Vendor system-power states OP-TEE's SYSTEM_POWER_STATE_GET reports
// (scmi_server.c): the quiescent state, and a latched power-button press
// awaiting a normal-world agent's action.
//
#define RPI_SCMI_SYS_STATE_RUNNING          0x80000000
#define RPI_SCMI_SYS_STATE_BUTTON_SHUTDOWN  0x80000001

//
// Vendor SYSTEM_POWER_STATE_SET states that deliver the power-button
// policy to OP-TEE (which executes the press through EL3 after a grace
// window). PowerButtonScmiDxe sends one at boot from the blconfig
// POWER_OFF_ON_HALT verdict.
//
#define RPI_SCMI_SYS_SET_POLICY_OFF    0x80000002
#define RPI_SCMI_SYS_SET_POLICY_RESET  0x80000003

/**
  Bring up the channel if needed and report readiness. Maps the SMT page
  uncached, which requires the CPU arch protocol - callers poll this from
  a timer until it turns TRUE rather than treating FALSE as fatal.

  @retval TRUE   Channel usable; RpiScmiCall may be invoked.
  @retval FALSE  Not yet (or never, if the platform has no OP-TEE).
**/
BOOLEAN
EFIAPI
RpiScmiReady (
  VOID
  );

/**
  One synchronous SCMI command. The doorbell fastcall is served inside
  the SMC, so the response is available on return.

  @param[in]  Protocol  SCMI protocol id.
  @param[in]  MessageId Message id within the protocol.
  @param[in]  In        Payload words to send (NULL if none).
  @param[in]  InCount   Number of payload words to send.
  @param[out] Out       Response payload words; Out[0] is the int32 SCMI
                        status, negative on failure.
  @param[in]  OutCount  Number of response words to read back.

  @retval EFI_SUCCESS       Transport round trip completed; check Out[0].
  @retval EFI_NOT_READY     Channel not initialized or not free.
  @retval EFI_DEVICE_ERROR  Doorbell SMC or channel-level error.
**/
EFI_STATUS
EFIAPI
RpiScmiCall (
  IN  UINT8         Protocol,
  IN  UINT8         MessageId,
  IN  CONST UINT32  *In OPTIONAL,
  IN  UINTN         InCount,
  OUT UINT32        *Out,
  IN  UINTN         OutCount
  );

#endif // RPI_SCMI_LIB_H_
