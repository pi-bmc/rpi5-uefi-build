/** @file
  SCMI agent client for the OP-TEE SCMI server. See RpiScmiLib.h.

  The SMT slot layout is field-for-field OP-TEE's
  core/drivers/scmi-msg/smt.c struct smt_header (the SCMI platform design
  document's shared-memory transfer). The page must be accessed uncached:
  OP-TEE maps it MEM_AREA_IO_NSEC and Linux ioremaps it (no-map), so a
  cached view here would exchange stale lines instead of messages.

  Copyright (c) 2026, pi-bmc contributors

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/ArmSmcLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/PcdLib.h>
#include <Library/RpiScmiLib.h>

//
// SiP fast SMC the SMT channel is rung with (RPI_SIP_SCMI_AGENT0).
//
#define SCMI_DOORBELL_SMC_FID  0x82000010

#pragma pack(1)
typedef struct {
  UINT32    Reserved0;
  UINT32    Status;         // bit0 FREE, bit1 ERROR
  UINT64    Reserved1;
  UINT32    Flags;          // bit1 interrupt completion; 0 = polled
  UINT32    Length;         // MessageHeader + payload, in bytes
  UINT32    MessageHeader;  // msg id [7:0], type [9:8], protocol [17:10]
  UINT32    Payload[30];    // rest of the 128-byte slot
} SCMI_SMT_SLOT;
#pragma pack()

#define SMT_STATUS_FREE   BIT0
#define SMT_STATUS_ERROR  BIT1

#define SCMI_MSG_HEADER(Protocol, MsgId)  \
  (((UINT32)(Protocol) << 10) | (UINT32)(MsgId))

STATIC volatile SCMI_SMT_SLOT  *mSmtSlot;   // NULL until the page is UC-mapped

BOOLEAN
EFIAPI
RpiScmiReady (
  VOID
  )
{
  EFI_PHYSICAL_ADDRESS  Base;
  EFI_STATUS            Status;

  if (mSmtSlot != NULL) {
    return TRUE;
  }

  //
  // The SMT slot: top 4 KB page of the OP-TEE reserved SHM window,
  // derived from the same carve-out PCDs RaspberryPiMem.c reserves it
  // with (= 0x1F3FF000).
  //
  Base = PcdGet64 (PcdOpteeTzdramBase) + PcdGet32 (PcdOpteeTzdramSize) +
         PcdGet32 (PcdOpteeShmSize) - SIZE_4KB;

  Status = gDS->SetMemorySpaceAttributes (Base, SIZE_4KB, EFI_MEMORY_UC);
  if (EFI_ERROR (Status)) {
    return FALSE;      // CPU arch protocol not up yet; caller retries
  }

  mSmtSlot = (volatile SCMI_SMT_SLOT *)(UINTN)Base;
  DEBUG ((DEBUG_INFO, "RpiScmiLib: SMT channel at 0x%lx\n", Base));

  return TRUE;
}

EFI_STATUS
EFIAPI
RpiScmiCall (
  IN  UINT8         Protocol,
  IN  UINT8         MessageId,
  IN  CONST UINT32  *In OPTIONAL,
  IN  UINTN         InCount,
  OUT UINT32        *Out,
  IN  UINTN         OutCount
  )
{
  ARM_SMC_ARGS  Args;
  UINTN         Index;

  if (mSmtSlot == NULL) {
    return EFI_NOT_READY;
  }

  ASSERT (InCount <= ARRAY_SIZE (mSmtSlot->Payload));
  ASSERT (OutCount <= ARRAY_SIZE (mSmtSlot->Payload));

  //
  // The channel must be free: the platform sets FREE after every message,
  // and DXE is the only agent before the OS. Anything else means the
  // server side is wedged - do not write over an in-flight slot.
  //
  if ((mSmtSlot->Status & SMT_STATUS_FREE) == 0) {
    return EFI_NOT_READY;
  }

  for (Index = 0; Index < InCount; Index++) {
    mSmtSlot->Payload[Index] = In[Index];
  }

  mSmtSlot->Flags         = 0;
  mSmtSlot->Length        = (UINT32)(sizeof (UINT32) * (1 + InCount));
  mSmtSlot->MessageHeader = SCMI_MSG_HEADER (Protocol, MessageId);
  mSmtSlot->Status        = 0;                     // claim the channel

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = SCMI_DOORBELL_SMC_FID;
  ArmCallSmc (&Args);

  if (Args.Arg0 != 0) {
    //
    // TF-A refused the forward (OP-TEE not up?): release our claim so
    // the next attempt can try again.
    //
    mSmtSlot->Status = SMT_STATUS_FREE;
    return EFI_DEVICE_ERROR;
  }

  if (((mSmtSlot->Status & SMT_STATUS_FREE) == 0) ||
      ((mSmtSlot->Status & SMT_STATUS_ERROR) != 0))
  {
    return EFI_DEVICE_ERROR;
  }

  for (Index = 0; Index < OutCount; Index++) {
    Out[Index] = mSmtSlot->Payload[Index];
  }

  return EFI_SUCCESS;
}
