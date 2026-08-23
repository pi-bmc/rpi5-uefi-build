/** @file
  EFI_MM_COMMUNICATION2_PROTOCOL over the OP-TEE StandaloneMM (StMM) SP.

  Relays the MM communication buffer to OP-TEE's StMM secure partition, which
  OP-TEE runs as an ordinary pseudo-TA under the opteed dispatcher (no FF-A
  SPMC). Session setup reuses ArmPkg/OpteeLib (proven on this board by
  RpiOpteeSensorDxe); the InvokeCommand call is done here so the OP-TEE
  normal-world RPC loop -- shared-memory alloc/free and the RPMB command
  family that StMM's variable service needs to reach the RPMB store -- can be
  serviced. Without that loop a GetVariable/SetVariable that touches storage
  would never complete.

  Copyright (c) 2026, pi-bmc.  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/OpteeLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/MmCommunication2.h>

#include "MmCommunicationOptee.h"

//
// RPMB RPC servicer (Rpmb.c). Given the OPTEE_MSG_ARG of an
// OPTEE_RPC_CMD_RPMB* request, performs the frame transport and fills the
// result. Returns a TEE_* code composed into MsgArg->Ret.
//
UINT32
OpteeRpmbServiceCmd (
  IN OUT OPTEE_MSG_ARG  *MsgArg
  );

//
// REE-FS RPC servicer (ReeFs.c). Services OPTEE_RPC_CMD_FS file ops for the
// REE filesystem secure-storage backend, against the boot FAT volume.
//
UINT32
OpteeReeFsServiceCmd (
  IN OUT OPTEE_MSG_ARG  *MsgArg
  );

//
// Cached OP-TEE static shared memory window (from GET_SHM_CONFIG) and the
// per-call bump allocator over it. The window is the plat-rpi5 reserved SHM
// (0x1F000000); OpteeInit() already remapped it WB and validated it.
//
STATIC UINTN    mShmBase   = 0;
STATIC UINTN    mShmSize   = 0;
STATIC UINTN    mShmNext   = 0;   // bump cursor, reset at each Communicate
STATIC UINT32   mSession   = 0;
STATIC BOOLEAN  mHaveSession = FALSE;

#define SHM_ALIGN_UP(x)  (((x) + 0xFUL) & ~0xFUL)

/**
  Read the OP-TEE static SHM window base/size (matches what OpteeInit mapped).
**/
STATIC
EFI_STATUS
MmOpteeGetShm (
  VOID
  )
{
  ARM_SMC_ARGS  Args;
  UINTN         Start;
  UINTN         End;

  if (mShmBase != 0) {
    return EFI_SUCCESS;
  }

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = OPTEE_SMC_GET_SHM_CONFIG;
  ArmCallSmc (&Args);
  if (Args.Arg0 != OPTEE_SMC_RETURN_OK) {
    return EFI_UNSUPPORTED;
  }

  Start = (Args.Arg1 + SIZE_4KB - 1) & ~(UINTN)(SIZE_4KB - 1);
  End   = (Args.Arg1 + Args.Arg2) & ~(UINTN)(SIZE_4KB - 1);
  if (End <= Start) {
    return EFI_BUFFER_TOO_SMALL;
  }

  mShmBase = Start;
  mShmSize = End - Start;
  return EFI_SUCCESS;
}

/**
  Bump-allocate Size bytes from the SHM window. Returns 0 on exhaustion.
  Cookies handed to OP-TEE are the buffer's physical address (identity mapped).
**/
STATIC
UINTN
MmOpteeShmAlloc (
  IN UINTN  Size
  )
{
  UINTN  Addr;

  Addr = mShmNext;
  if ((Size == 0) || (SHM_ALIGN_UP (Size) > (mShmSize - (Addr - mShmBase)))) {
    return 0;
  }

  mShmNext = Addr + SHM_ALIGN_UP (Size);
  return Addr;
}

/**
  Service one OPTEE_RPC_CMD_* request (delivered via OPTEE_SMC_RPC_FUNC_CMD).
  MsgArg lives in SHM and is updated in place with the result.
**/
STATIC
VOID
MmOpteeRpcCmd (
  IN OUT OPTEE_MSG_ARG  *MsgArg
  )
{
  UINTN  Buf;
  UINT64 Size;

  switch (MsgArg->Cmd) {
    case OPTEE_RPC_CMD_SHM_ALLOC:
      //
      // [in] value[0].b = size, value[0].c = align; [out] memref[0] = buffer.
      //
      if (MsgArg->NumParams != 1) {
        MsgArg->Ret = TEE_ERROR_GENERIC;
        break;
      }

      Size = MsgArg->Params[0].U.Value.B;
      Buf  = MmOpteeShmAlloc ((UINTN)Size);
      if (Buf == 0) {
        MsgArg->Ret = TEE_ERROR_OUT_OF_MEMORY;
        break;
      }

      MsgArg->Params[0].Attr        = OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT;
      MsgArg->Params[0].U.TMem.BufPtr = (UINT64)Buf;
      MsgArg->Params[0].U.TMem.Size   = Size;
      MsgArg->Params[0].U.TMem.ShmRef = (UINT64)Buf;   // cookie = PA
      MsgArg->Ret = TEE_SUCCESS;
      break;

    case OPTEE_RPC_CMD_SHM_FREE:
      //
      // Bump allocator reclaims wholesale at the next Communicate; nothing to
      // do per free.
      //
      MsgArg->Ret = TEE_SUCCESS;
      break;

    case OPTEE_RPC_CMD_RPMB:
    case OPTEE_RPC_CMD_RPMB_PROBE_RESET:
    case OPTEE_RPC_CMD_RPMB_PROBE_NEXT:
    case OPTEE_RPC_CMD_RPMB_FRAMES:
      MsgArg->Ret = OpteeRpmbServiceCmd (MsgArg);
      break;

    case OPTEE_RPC_CMD_FS:
      //
      // REE-FS backend (CFG_REE_FS): OP-TEE stores its encrypted, integrity-
      // protected objects as files in the normal world. Which of RPMB / FS
      // arrives is decided by how OP-TEE was built (RPI5_OPTEE_STMM_BACKEND);
      // the transport services whichever it receives.
      //
      MsgArg->Ret = OpteeReeFsServiceCmd (MsgArg);
      break;

    default:
      DEBUG ((DEBUG_WARN, "%a: unhandled OP-TEE RPC cmd %u\n", __func__, MsgArg->Cmd));
      MsgArg->Ret = TEE_ERROR_NOT_SUPPORTED;
      break;
  }
}

/**
  Issue OPTEE_SMC_CALL_WITH_ARG for the message at ArgPa and run the OP-TEE
  normal-world RPC loop until the call completes. Returns the OP-TEE SMC
  return value (0 = OK).
**/
STATIC
UINT32
MmOpteeCallWithRpc (
  IN UINTN  ArgPa
  )
{
  ARM_SMC_ARGS  Args;

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = OPTEE_SMC_CALL_WITH_ARG;
  Args.Arg1 = (UINT32)(((UINT64)ArgPa) >> 32);
  Args.Arg2 = (UINT32)ArgPa;

  while (TRUE) {
    ArmCallSmc (&Args);

    if (!OPTEE_SMC_RETURN_IS_RPC (Args.Arg0)) {
      break;
    }

    switch (OPTEE_SMC_RETURN_GET_RPC_FUNC (Args.Arg0)) {
      case OPTEE_SMC_RPC_FUNC_ALLOC:
      {
        UINTN  Buf;

        Buf = MmOpteeShmAlloc ((UINTN)Args.Arg1);
        //
        // a1:a2 = 64-bit physical pointer, a4:a5 = cookie (= PA). All zero
        // signals allocation failure, which OP-TEE handles gracefully.
        //
        Args.Arg1 = (UINT32)(((UINT64)Buf) >> 32);
        Args.Arg2 = (UINT32)Buf;
        Args.Arg4 = (UINT32)(((UINT64)Buf) >> 32);
        Args.Arg5 = (UINT32)Buf;
        break;
      }

      case OPTEE_SMC_RPC_FUNC_FREE:
        //
        // Cookie in a1:a2; bump allocator reclaims at the next Communicate.
        //
        break;

      case OPTEE_SMC_RPC_FUNC_FOREIGN_INTR:
        //
        // A foreign (non-secure) interrupt fired while OP-TEE ran. It is
        // taken through the normal vector by returning to OP-TEE; nothing to
        // do here.
        //
        break;

      case OPTEE_SMC_RPC_FUNC_CMD:
      {
        UINT64  Cookie;

        //
        // a1:a2 = cookie of a shared-memory buffer holding an OPTEE_MSG_ARG.
        // The cookie is the buffer PA (see MmOpteeShmAlloc), identity mapped.
        //
        Cookie = (((UINT64)Args.Arg1) << 32) | (UINT32)Args.Arg2;
        MmOpteeRpcCmd ((OPTEE_MSG_ARG *)(UINTN)Cookie);
        break;
      }

      default:
        DEBUG ((DEBUG_WARN, "%a: unhandled RPC func %x\n", __func__, Args.Arg0));
        break;
    }

    Args.Arg0 = OPTEE_SMC_CALL_RETURN_FROM_RPC;
  }

  return (UINT32)Args.Arg0;
}

/**
  EFI_MM_COMMUNICATION2_PROTOCOL.Communicate: hand the MM buffer to StMM.
**/
STATIC
EFI_STATUS
EFIAPI
MmCommunicationOpteeCommunicate (
  IN CONST EFI_MM_COMMUNICATION2_PROTOCOL  *This,
  IN OUT VOID                              *CommBufferPhysical,
  IN OUT VOID                              *CommBufferVirtual,
  IN OUT UINTN                             *CommSize OPTIONAL
  )
{
  EFI_MM_COMMUNICATE_HEADER  *Header;
  OPTEE_MSG_ARG              *MsgArg;
  UINTN                      BufferSize;
  UINTN                      ArgSize;
  UINTN                      ShmComm;
  UINT32                     Rc;

  if ((CommBufferPhysical == NULL) || (CommBufferVirtual == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Header     = (EFI_MM_COMMUNICATE_HEADER *)CommBufferVirtual;
  BufferSize = OFFSET_OF (EFI_MM_COMMUNICATE_HEADER, Data) +
               (UINTN)Header->MessageLength;
  if ((CommSize != NULL) && (*CommSize != 0)) {
    BufferSize = *CommSize;
  }

  if (!mHaveSession) {
    return EFI_NOT_STARTED;
  }

  //
  // Per-call SHM layout: [invoke arg][comm buffer copy][RPC scratch...].
  //
  mShmNext = mShmBase;
  ArgSize  = SHM_ALIGN_UP (OPTEE_MSG_ARG_SIZE (2));
  MsgArg   = (OPTEE_MSG_ARG *)MmOpteeShmAlloc (ArgSize);
  ShmComm  = MmOpteeShmAlloc (BufferSize);
  if ((MsgArg == NULL) || (ShmComm == 0)) {
    DEBUG ((DEBUG_ERROR, "%a: comm buffer (%lu B) exceeds OP-TEE SHM\n", __func__, (UINT64)BufferSize));
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem ((VOID *)ShmComm, (VOID *)CommBufferPhysical, BufferSize);

  ZeroMem (MsgArg, ArgSize);
  MsgArg->Cmd       = OPTEE_MSG_CMD_INVOKE_COMMAND;
  MsgArg->Func      = PTA_STMM_CMD_COMMUNICATE;
  MsgArg->Session   = mSession;
  MsgArg->NumParams = 2;
  //
  // [in/out] memref[0] = EFI MM communication buffer.
  //
  MsgArg->Params[0].Attr          = OPTEE_MSG_ATTR_TYPE_TMEM_INOUT;
  MsgArg->Params[0].U.TMem.BufPtr = (UINT64)ShmComm;
  MsgArg->Params[0].U.TMem.Size   = BufferSize;
  MsgArg->Params[0].U.TMem.ShmRef = (UINT64)ShmComm;
  //
  // [out] value[1].a = EFI return code from StMM.
  //
  MsgArg->Params[1].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT;

  Rc = MmOpteeCallWithRpc ((UINTN)MsgArg);
  if ((Rc != 0) || (MsgArg->Ret != TEE_SUCCESS)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: StMM communicate failed smc=0x%x ret=0x%x\n",
      __func__, Rc, MsgArg->Ret
      ));
    return EFI_DEVICE_ERROR;
  }

  //
  // StMM edited the buffer in place; copy it back to the caller.
  //
  CopyMem ((VOID *)CommBufferPhysical, (VOID *)ShmComm, BufferSize);

  return (EFI_STATUS)MsgArg->Params[1].U.Value.A;
}

STATIC EFI_MM_COMMUNICATION2_PROTOCOL  mMmCommunication2 = {
  MmCommunicationOpteeCommunicate
};

/**
  Open the StMM pseudo-TA session through OUR RPC-aware loop.

  LOAD-BEARING: StMM runs its entire initialisation -- including the
  OpTeeRpmbFvb store reads that drive OP-TEE's REE-FS / RPMB RPCs -- inside
  stmm_complete_session(), which OP-TEE executes during OPEN_SESSION, NOT during
  the later invoke/Communicate. OpteeLib's OpteeOpenSession() drives OPEN_SESSION
  with OpteeCallWithArg(), whose RPC loop services only foreign interrupts and
  drops every OPTEE_RPC_CMD_* ("default: Do nothing"). That silently fails
  StMM's load-time storage, so StMM panics before Communicate ever runs and our
  ReeFs/RPMB servicer is never reached. We must therefore issue OPEN_SESSION
  ourselves and pump MmOpteeCallWithRpc(), so those bring-up RPCs reach
  MmOpteeRpcCmd() -> OpteeReeFsServiceCmd()/OpteeRpmbServiceCmd().

  @param[in]   Uuid      StMM pseudo-TA UUID.
  @param[out]  Session   Opened session id on success.

  @retval EFI_SUCCESS         Session opened.
  @retval EFI_OUT_OF_RESOURCES SHM too small for the arg.
  @retval EFI_NOT_FOUND       OP-TEE refused the session (StMM absent / panicked).
**/
STATIC
EFI_STATUS
MmOpteeOpenStmmSession (
  IN  CONST EFI_GUID  *Uuid,
  OUT UINT32          *Session
  )
{
  OPTEE_MSG_ARG  *MsgArg;
  UINT8          *Val;
  UINT32         D1;
  UINT16         D2;
  UINT16         D3;
  UINTN          ArgSize;
  UINT32         Rc;

  //
  // Reset the bump allocator; StMM's bring-up SHM_ALLOC RPCs allocate more
  // from the same window during the call below.
  //
  mShmNext = mShmBase;
  ArgSize  = SHM_ALIGN_UP (OPTEE_MSG_ARG_SIZE (2));
  MsgArg   = (OPTEE_MSG_ARG *)MmOpteeShmAlloc (ArgSize);
  if (MsgArg == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (MsgArg, ArgSize);
  MsgArg->Cmd       = OPTEE_MSG_CMD_OPEN_SESSION;
  MsgArg->NumParams = 2;

  //
  // Two meta params: [0] = TA UUID, [1] = client UUID (nil) + login method.
  // The UUID is packed big-endian (RFC4122) into value.a/value.b, byte-for-byte
  // as OpteeLib's EfiGuidToRfc4122Uuid does, so OP-TEE's UUID match succeeds.
  //
  MsgArg->Params[0].Attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT | OPTEE_MSG_ATTR_META;
  Val = (UINT8 *)&MsgArg->Params[0].U.Value;
  D1  = SwapBytes32 (Uuid->Data1);
  D2  = SwapBytes16 (Uuid->Data2);
  D3  = SwapBytes16 (Uuid->Data3);
  CopyMem (Val + 0, &D1, sizeof (D1));
  CopyMem (Val + 4, &D2, sizeof (D2));
  CopyMem (Val + 6, &D3, sizeof (D3));
  CopyMem (Val + 8, (VOID *)Uuid->Data4, sizeof (Uuid->Data4));

  MsgArg->Params[1].Attr      = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT | OPTEE_MSG_ATTR_META;
  MsgArg->Params[1].U.Value.C = OPTEE_MSG_LOGIN_PUBLIC;

  Rc = MmOpteeCallWithRpc ((UINTN)MsgArg);
  if ((Rc != 0) || (MsgArg->Ret != TEE_SUCCESS)) {
    DEBUG ((
      DEBUG_ERROR,
      "REEFS: OPEN_SESSION failed smc=0x%x ret=0x%x origin=0x%x\n",
      Rc, MsgArg->Ret, MsgArg->RetOrigin
      ));
    return EFI_NOT_FOUND;
  }

  *Session = MsgArg->Session;
  return EFI_SUCCESS;
}

/**
  Driver entry: init OP-TEE, open the StMM session, publish MM_COMMUNICATION2.
**/
EFI_STATUS
EFIAPI
MmCommunicationOpteeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS              Status;
  EFI_HANDLE              Handle;
  STATIC CONST EFI_GUID   StmmUuid = PTA_STMM_UUID;

  if (!IsOpteePresent ()) {
    DEBUG ((DEBUG_WARN, "%a: OP-TEE not present; MM/variable store unavailable\n", __func__));
    return EFI_UNSUPPORTED;
  }

  Status = OpteeInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: OpteeInit failed - %r\n", __func__, Status));
    return Status;
  }

  Status = MmOpteeGetShm ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: no OP-TEE shared memory - %r\n", __func__, Status));
    return Status;
  }

  //
  // Open via our own RPC-aware loop, NOT OpteeLib's OpteeOpenSession: StMM does
  // its storage-touching bring-up during OPEN_SESSION and OpteeLib drops the
  // RPCs (see MmOpteeOpenStmmSession). The store is served from memory (ReeFs.c),
  // so it is available here with no device dependency -- no ordering wait.
  //
  Status = MmOpteeOpenStmmSession (&StmmUuid, &mSession);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: open StMM session failed - %r (is CFG_STMM_PATH set in OP-TEE?)\n",
      __func__, Status
      ));
    return EFI_NOT_FOUND;
  }

  mHaveSession = TRUE;

  Handle = NULL;
  Status = gBS->InstallProtocolInterface (
                  &Handle,
                  &gEfiMmCommunication2ProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mMmCommunication2
                  );
  if (EFI_ERROR (Status)) {
    OpteeCloseSession (mSession);
    mHaveSession = FALSE;
    return Status;
  }

  DEBUG ((DEBUG_INFO, "%a: StMM MM transport up (session 0x%x)\n", __func__, mSession));
  return EFI_SUCCESS;
}
