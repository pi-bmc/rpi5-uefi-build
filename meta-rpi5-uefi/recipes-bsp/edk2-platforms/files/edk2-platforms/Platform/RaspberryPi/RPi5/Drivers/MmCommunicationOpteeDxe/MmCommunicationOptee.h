/** @file
  OP-TEE StandaloneMM (StMM) transport for EDK2 MM communication on RPi5.

  Publishes EFI_MM_COMMUNICATION2_PROTOCOL and relays the MM communication
  buffer to the OP-TEE StMM secure partition. OP-TEE runs StMM as an ordinary
  pseudo-TA under the opteed dispatcher (no FF-A SPMC): normal world opens a
  session to PTA_STMM_UUID and invokes PTA_STMM_CMD_COMMUNICATE, servicing
  the OP-TEE normal-world RPC loop (shared-memory alloc/free, foreign
  interrupts) throughout.

  Storage is not RPC-serviced: StMM's variable stack works directly on the
  VPU-loaded FD NV window OP-TEE maps into the SP (CFG_STMM_VARSTORE_*, FVB
  = RpiNvMemFvb). This driver persists that window back to the boot FAT
  (VarStoreSync.c).

  Copyright (c) 2026, pi-bmc.  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef MM_COMMUNICATION_OPTEE_H_
#define MM_COMMUNICATION_OPTEE_H_

#include <Uefi.h>

//
// Persistence engine (VarStoreSync.c): writes the FD NV window back into
// armstub8-2712.bin / RPI_EFI.fd on the boot FAT (VarBlockServiceDxe model).
//

/**
  Arm the persistence engine: geometry from the RPi5.fdf NV PCDs, protocol
  notifies (SimpleFileSystem, ResetNotification) and the ReadyToBoot dump.
**/
EFI_STATUS
VarStoreSyncInit (
  VOID
  );

/**
  Mark the store dirty; called for every successful SetVariable communicate.
**/
VOID
VarStoreSyncMarkDirty (
  VOID
  );

//
// PTA_STMM: the OP-TEE pseudo-TA that fronts the StMM secure partition.
// UUID ed32d533-99e6-4209-9cc0-2d72cdd998a7 (core/arch/arm/include/pta_stmm.h).
//
#define PTA_STMM_UUID \
  { 0xed32d533, 0x99e6, 0x4209, { 0x9c, 0xc0, 0x2d, 0x72, 0xcd, 0xd9, 0x98, 0xa7 } }

#define PTA_STMM_CMD_COMMUNICATE  0

//
// OP-TEE SMC fastcall/std-call function IDs and RPC return encoding
// (core/arch/arm/include/sm/optee_smc.h). Only the subset this transport
// needs is reproduced here.
//
#define OPTEE_SMC_CALL_WITH_ARG         0x32000004
#define OPTEE_SMC_CALL_RETURN_FROM_RPC  0x32000003
#define OPTEE_SMC_GET_SHM_CONFIG        0xB2000007

#define OPTEE_SMC_RETURN_OK                0x00000000
#define OPTEE_SMC_RETURN_ETHREAD_LIMIT     0x00000001
#define OPTEE_SMC_RETURN_UNKNOWN_FUNCTION  0xFFFFFFFF

#define OPTEE_SMC_RETURN_RPC_PREFIX_MASK  0xFFFF0000
#define OPTEE_SMC_RETURN_RPC_PREFIX       0xFFFF0000
#define OPTEE_SMC_RETURN_RPC_FUNC_MASK    0x0000FFFF

#define OPTEE_SMC_RPC_FUNC_ALLOC         0
#define OPTEE_SMC_RPC_FUNC_FREE          2
#define OPTEE_SMC_RPC_FUNC_FOREIGN_INTR  4
#define OPTEE_SMC_RPC_FUNC_CMD           5

#define OPTEE_SMC_RETURN_IS_RPC(Ret) \
  (((Ret) != OPTEE_SMC_RETURN_UNKNOWN_FUNCTION) && \
   (((Ret) & OPTEE_SMC_RETURN_RPC_PREFIX_MASK) == OPTEE_SMC_RETURN_RPC_PREFIX))

#define OPTEE_SMC_RETURN_GET_RPC_FUNC(Ret)  ((Ret) & OPTEE_SMC_RETURN_RPC_FUNC_MASK)

#define OPTEE_SMC_SHM_CACHED  1

//
// OPTEE_MSG_ARG / param attributes (core/include/optee_msg.h).
//
#define OPTEE_MSG_ATTR_TYPE_NONE          0x0
#define OPTEE_MSG_ATTR_TYPE_VALUE_INPUT   0x1
#define OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT  0x2
#define OPTEE_MSG_ATTR_TYPE_VALUE_INOUT   0x3
#define OPTEE_MSG_ATTR_TYPE_RMEM_INPUT    0x5
#define OPTEE_MSG_ATTR_TYPE_RMEM_OUTPUT   0x6
#define OPTEE_MSG_ATTR_TYPE_RMEM_INOUT    0x7
#define OPTEE_MSG_ATTR_TYPE_TMEM_INPUT    0x9
#define OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT   0xa
#define OPTEE_MSG_ATTR_TYPE_TMEM_INOUT    0xb
#define OPTEE_MSG_ATTR_TYPE_MASK          0xff
#define OPTEE_MSG_ATTR_META               0x100

#define OPTEE_MSG_CMD_OPEN_SESSION    0
#define OPTEE_MSG_CMD_INVOKE_COMMAND  1
#define OPTEE_MSG_CMD_CLOSE_SESSION   2

#define OPTEE_MSG_LOGIN_PUBLIC  0x0

#define OPTEE_MSG_MAX_NUM_PARAMS  4

#pragma pack(1)
typedef struct {
  UINT64    Attr;
  union {
    struct {
      UINT64    BufPtr;
      UINT64    Size;
      UINT64    ShmRef;
    } TMem;
    struct {
      UINT64    Offs;
      UINT64    Size;
      UINT64    ShmRef;
    } RMem;
    struct {
      UINT64    A;
      UINT64    B;
      UINT64    C;
    } Value;
  } U;
} OPTEE_MSG_PARAM;

typedef struct {
  UINT32             Cmd;
  UINT32             Func;
  UINT32             Session;
  UINT32             CancelId;
  UINT32             Pad;
  UINT32             Ret;
  UINT32             RetOrigin;
  UINT32             NumParams;
  OPTEE_MSG_PARAM    Params[];
} OPTEE_MSG_ARG;
#pragma pack()

//
// Byte size of an OPTEE_MSG_ARG that carries NumParams params.
//
#define OPTEE_MSG_ARG_SIZE(NumParams) \
  (sizeof (OPTEE_MSG_ARG) + sizeof (OPTEE_MSG_PARAM) * (NumParams))

//
// OP-TEE RPC command IDs serviced during the call (core/include/optee_rpc_cmd.h).
// Only the shared-memory pair: storage never RPCs in this design.
//
#define OPTEE_RPC_CMD_SHM_ALLOC  6
#define OPTEE_RPC_CMD_SHM_FREE   7

//
// TEE return codes used when composing RPC results (tee_api_defines.h).
//
#define TEE_SUCCESS              0x00000000
#define TEE_ERROR_GENERIC        0xFFFF0000
#define TEE_ERROR_NOT_SUPPORTED  0xFFFF000A
#define TEE_ERROR_OUT_OF_MEMORY  0xFFFF000C
#define TEE_ERROR_SHORT_BUFFER   0xFFFF0010

#endif // MM_COMMUNICATION_OPTEE_H_
